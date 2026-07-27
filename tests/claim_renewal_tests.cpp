// VM Agent claim 后台续租、重试和安全截止测试。
#include <atomic>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

#include "claim_renewal.hpp"
#include "claim_store.hpp"
#include "satsuma/core/claim.hpp"
#include "satsuma/core/errors.hpp"
#include "satsuma/core/id.hpp"
#include "satsuma/core/path.hpp"

namespace {

using namespace std::chrono_literals;

// 在条件不满足时终止当前测试。
void expect(const bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

// 在有限时间内等待测试条件成立。
template <typename Predicate>
[[nodiscard]] bool wait_for_condition(
    Predicate predicate,
    const std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(5ms);
    }
    return predicate();
}

// 返回适合毫秒级测试的固定租约策略。
[[nodiscard]] satsuma::vm::ClaimLeasePolicy test_policy() {
    return {
        600ms,
        80ms,
        20ms,
        80ms,
    };
}

// 创建并领取一份供续租会话使用的 claim。
[[nodiscard]] satsuma::StepClaimLease acquire_claim(
    const std::filesystem::path& claim_path,
    const std::filesystem::path& result_path,
    const satsuma::vm::ClaimLeasePolicy& policy,
    const std::string& job_id) {
    const satsuma::StepClaimLease proposed = satsuma::make_step_claim_lease(
        "run_claim_renewal",
        "client",
        "execute",
        job_id,
        "session_claim_renewal",
        "boot_claim_renewal",
        satsuma::unix_time_ms(),
        policy.lease_duration.count(),
        true);
    const satsuma::vm::StepClaimAcquireResult acquired =
        satsuma::vm::acquire_step_claim_transaction(
            claim_path,
            result_path,
            proposed,
            proposed.boot_id);
    if (!acquired.claim.has_value()) {
        throw std::runtime_error("renewal test could not acquire its claim");
    }
    return *acquired.claim;
}

// 验证策略拒绝不足以完成主动续租和安全取消的时间窗口。
void test_policy_validation() {
    satsuma::vm::ClaimLeasePolicy invalid = test_policy();
    invalid.renewal_interval = 250ms;
    bool rejected = false;
    try {
        satsuma::vm::validate_claim_lease_policy(invalid);
    } catch (const satsuma::Error&) {
        rejected = true;
    }
    expect(rejected, "claim lease policy accepted fewer than three renewal intervals");
}

// 验证后台会话连续续租，并在正常 finish 后不再写 sidecar。
void test_continuous_renewal(const std::filesystem::path& root) {
    const satsuma::vm::ClaimLeasePolicy policy = test_policy();
    const std::filesystem::path claim_path = root / L"state" / L"execute.claim.json";
    const std::filesystem::path result_path = root / L"results" / L"execution.json";
    const satsuma::StepClaimLease owner = acquire_claim(
        claim_path,
        result_path,
        policy,
        "job_continuous");
    satsuma::vm::ClaimRenewalSession session(claim_path, owner, policy);
    expect(
        wait_for_condition(
            [&] {
                return satsuma::vm::load_effective_step_claim(claim_path)
                    .renewal_sequence >= 2;
            },
            2s),
        "claim renewal session did not publish two renewals");
    expect(
        !session.lease_loss_token().stop_requested(),
        "healthy claim renewal session reported lease loss");
    session.finish();
    const std::uint32_t stopped_sequence =
        satsuma::vm::load_effective_step_claim(claim_path).renewal_sequence;
    std::this_thread::sleep_for(policy.renewal_interval * 2);
    expect(
        satsuma::vm::load_effective_step_claim(claim_path).renewal_sequence ==
            stopped_sequence,
        "claim renewal session continued after finish");
}

// 验证瞬时错误按短周期重试，恢复后不触发 lease-loss。
void test_transient_failure_recovery(const std::filesystem::path& root) {
    const satsuma::vm::ClaimLeasePolicy policy = test_policy();
    const std::filesystem::path claim_path = root / L"state" / L"execute.claim.json";
    const std::filesystem::path result_path = root / L"results" / L"execution.json";
    const satsuma::StepClaimLease owner = acquire_claim(
        claim_path,
        result_path,
        policy,
        "job_transient");
    std::atomic<int> attempts{0}; // 注入的续租调用次数
    satsuma::vm::ClaimRenewalSession session(
        claim_path,
        owner,
        policy,
        [&attempts](
            const std::filesystem::path& path,
            const satsuma::StepClaimLease& expected,
            const std::int64_t duration_ms) {
            if (attempts.fetch_add(1) < 2) {
                throw satsuma::Error("injected transient renewal failure");
            }
            return satsuma::vm::renew_step_claim_transaction(
                path,
                expected,
                duration_ms);
        });
    expect(
        wait_for_condition(
            [&] {
                return satsuma::vm::load_effective_step_claim(claim_path)
                    .renewal_sequence >= 1;
            },
            2s),
        "claim renewal session did not recover from transient failures");
    expect(
        attempts.load() >= 3 && !session.lease_loss_token().stop_requested(),
        "recovered transient renewal failure triggered lease loss");
    session.finish();
}

// 验证显式所有权丢失会及时传播为停止信号和稳定原因。
void test_ownership_loss_signal(const std::filesystem::path& root) {
    const satsuma::vm::ClaimLeasePolicy policy = test_policy();
    const std::filesystem::path claim_path = root / L"state" / L"execute.claim.json";
    const std::filesystem::path result_path = root / L"results" / L"execution.json";
    const satsuma::StepClaimLease owner = acquire_claim(
        claim_path,
        result_path,
        policy,
        "job_lost");
    satsuma::vm::ClaimRenewalSession session(
        claim_path,
        owner,
        policy,
        [](const std::filesystem::path&, const satsuma::StepClaimLease&, std::int64_t) {
            return satsuma::vm::StepClaimRenewResult{
                satsuma::vm::StepClaimRenewStatus::OwnershipLost,
                std::nullopt,
            };
        });
    expect(
        wait_for_condition(
            [&] { return session.lease_loss_token().stop_requested(); },
            1s),
        "claim ownership loss did not stop the renewal session");
    expect(
        session.loss_reason() == "Step claim ownership was lost before renewal",
        "claim ownership loss did not preserve its stable reason");
    session.finish();
}

// 验证持续 I/O 失败会在持久化租约到期前进入安全取消。
void test_safety_deadline(const std::filesystem::path& root) {
    satsuma::vm::ClaimLeasePolicy policy = test_policy();
    policy.lease_duration = 300ms;
    policy.renewal_interval = 50ms;
    policy.retry_interval = 20ms;
    policy.safety_margin = 60ms;
    const std::filesystem::path claim_path = root / L"state" / L"execute.claim.json";
    const std::filesystem::path result_path = root / L"results" / L"execution.json";
    const satsuma::StepClaimLease owner = acquire_claim(
        claim_path,
        result_path,
        policy,
        "job_deadline");
    satsuma::vm::ClaimRenewalSession session(
        claim_path,
        owner,
        policy,
        [](const std::filesystem::path&, const satsuma::StepClaimLease&, std::int64_t)
            -> satsuma::vm::StepClaimRenewResult {
            throw satsuma::Error("injected persistent renewal failure");
        });
    expect(
        wait_for_condition(
            [&] { return session.lease_loss_token().stop_requested(); },
            1s),
        "persistent claim renewal failure did not reach the safety deadline");
    expect(
        satsuma::unix_time_ms() < owner.lease_expires_unix_ms + 50 &&
            session.loss_reason().find("injected persistent renewal failure") !=
                std::string::npos,
        "claim renewal safety deadline was late or lost the failure reason");
    session.finish();
}

}  // namespace

// 运行 claim 后台续租测试并清理专用临时目录。
int main() {
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        satsuma::path_from_utf8(satsuma::make_id("claim-renewal-test"));
    try {
        test_policy_validation();
        test_continuous_renewal(root / L"continuous");
        test_transient_failure_recovery(root / L"transient");
        test_ownership_loss_signal(root / L"ownership-loss");
        test_safety_deadline(root / L"safety-deadline");
        std::filesystem::remove_all(root);
        std::cout << "SatsumaVmClaimRenewalTests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "SatsumaVmClaimRenewalTests failed: " << error.what() << '\n';
        std::error_code cleanup_error;
        std::filesystem::remove_all(root, cleanup_error);
        return 1;
    }
}
