// Host 权威 claim 租约、恢复和结果 fencing 测试。
#include <array>
#include <atomic>
#include <chrono>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "satsuma/core/claim.hpp"
#include "satsuma/core/claim_store.hpp"
#include "satsuma/core/id.hpp"
#include "satsuma/core/json_io.hpp"
#include "satsuma/core/path.hpp"

namespace {

using namespace std::chrono_literals;

void expect(const bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

template <typename Operation>
void expect_state_error(Operation operation, const std::string& message) {
    try {
        operation();
    } catch (const satsuma::vm::StepClaimStateError&) {
        return;
    }
    throw std::runtime_error(message);
}

struct StepPaths {
    std::filesystem::path claim;
    std::filesystem::path result;
};

[[nodiscard]] StepPaths make_paths(
    const std::filesystem::path& root,
    const std::string& name) {
    return {
        root / satsuma::path_from_utf8(name) / L"state" / L"execute.claim.json",
        root / satsuma::path_from_utf8(name) / L"results" / L"execution.json",
    };
}

[[nodiscard]] satsuma::StepClaimLease make_claim(
    const std::string& run_id,
    const std::string& job_id,
    const bool retry_safe = true,
    const std::int64_t claimed_unix_ms = satsuma::unix_time_ms(),
    const std::int64_t lease_duration_ms = 5'000) {
    return satsuma::make_step_claim_lease(
        run_id,
        "vm_01",
        "execute",
        job_id,
        "session_01",
        "boot_01",
        claimed_unix_ms,
        lease_duration_ms,
        retry_safe);
}

[[nodiscard]] nlohmann::json make_result(const satsuma::StepClaimLease& owner) {
    return {
        {"schema_version", 2},
        {"run_id", owner.run_id},
        {"vm_id", owner.vm_id},
        {"step_id", owner.step_id},
        {"job_id", owner.job_id},
        {"status", "exited"},
        {"exit_code", 0},
    };
}

void write_text(const std::filesystem::path& path, const std::string& text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << text;
    if (!output) {
        throw std::runtime_error("cannot write claim test evidence");
    }
}

// 续租直接替换单份 Host claim，并将重复请求作为幂等成功处理。
void test_acquire_and_renew(const std::filesystem::path& root) {
    const StepPaths paths = make_paths(root, "renew");
    const satsuma::StepClaimLease proposed = make_claim("run_renew", "job_renew");
    const satsuma::vm::StepClaimAcquireResult acquired =
        satsuma::vm::acquire_step_claim_transaction(paths.claim, paths.result, proposed);
    expect(
        acquired.status == satsuma::vm::StepClaimAcquireStatus::Acquired &&
            acquired.claim.has_value(),
        "fresh claim was not acquired");

    while (satsuma::unix_time_ms() <= acquired.claim->last_renewed_unix_ms) {
        std::this_thread::sleep_for(1ms);
    }
    const satsuma::vm::StepClaimRenewResult renewed =
        satsuma::vm::renew_step_claim_transaction(paths.claim, *acquired.claim, 5'000);
    expect(
        renewed.status == satsuma::vm::StepClaimRenewStatus::Renewed &&
            renewed.claim.has_value() && renewed.claim->renewal_sequence == 1 &&
            nlohmann::json(satsuma::load_step_claim_lease(paths.claim)) ==
                nlohmann::json(*renewed.claim),
        "claim renewal did not replace the authoritative record");

    const satsuma::vm::StepClaimRenewResult repeated =
        satsuma::vm::renew_step_claim_transaction(paths.claim, *acquired.claim, 5'000);
    expect(
        repeated.status == satsuma::vm::StepClaimRenewStatus::Renewed &&
            repeated.claim.has_value() && repeated.claim->renewal_sequence == 1,
        "repeated VMCI renewal request was not idempotent");

    std::size_t protocol_files = 0;
    for (const auto& entry : std::filesystem::directory_iterator(paths.claim.parent_path())) {
        if (entry.is_regular_file()) {
            ++protocol_files;
        }
    }
    expect(protocol_files == 1, "claim renewal created obsolete lock or sidecar files");
}

// 安全步骤可在租约过期后递增 attempt，且保留确定性的旧 claim 证据。
void test_safe_recovery(const std::filesystem::path& root) {
    const StepPaths paths = make_paths(root, "safe-recovery");
    const satsuma::StepClaimLease expired = make_claim(
        "run_safe_recovery",
        "job_expired",
        true,
        satsuma::unix_time_ms() - 5'000,
        1'000);
    satsuma::write_json_atomic(paths.claim, expired);

    const satsuma::StepClaimLease proposed =
        make_claim("run_safe_recovery", "job_recovered");
    const satsuma::vm::StepClaimAcquireResult recovered =
        satsuma::vm::acquire_step_claim_transaction(paths.claim, paths.result, proposed);
    std::filesystem::path archived = paths.claim;
    archived += L".attempt-1.json";
    expect(
        recovered.status == satsuma::vm::StepClaimAcquireStatus::Acquired &&
            recovered.claim.has_value() && recovered.claim->attempt == 2 &&
            satsuma::load_step_claim_lease(archived).job_id == expired.job_id,
        "safe expired claim did not recover with preserved evidence");
}

// 非幂等步骤过期后必须停在人工门禁，不能换 owner。
void test_unsafe_recovery_gate(const std::filesystem::path& root) {
    const StepPaths paths = make_paths(root, "unsafe-recovery");
    const satsuma::StepClaimLease expired = make_claim(
        "run_unsafe_recovery",
        "job_unsafe",
        false,
        satsuma::unix_time_ms() - 5'000,
        1'000);
    satsuma::write_json_atomic(paths.claim, expired);

    const satsuma::vm::StepClaimAcquireResult blocked =
        satsuma::vm::acquire_step_claim_transaction(
            paths.claim,
            paths.result,
            make_claim("run_unsafe_recovery", "job_other", false));
    expect(
        blocked.status ==
                satsuma::vm::StepClaimAcquireStatus::ManualInterventionRequired &&
            blocked.claim.has_value() && blocked.claim->job_id == expired.job_id &&
            satsuma::load_step_claim_lease(paths.claim).job_id == expired.job_id,
        "unsafe expired claim bypassed the manual recovery gate");
}

// 结果和证据只能由有效 owner 发布，完成后的重复领取直接返回完成。
void test_result_fencing(const std::filesystem::path& root) {
    const StepPaths paths = make_paths(root, "result");
    const satsuma::StepClaimLease proposed = make_claim("run_result", "job_owner");
    const satsuma::vm::StepClaimAcquireResult acquired =
        satsuma::vm::acquire_step_claim_transaction(paths.claim, paths.result, proposed);
    expect(acquired.claim.has_value(), "result test did not acquire its claim");

    std::filesystem::create_directories(paths.result.parent_path());
    const std::filesystem::path staged = paths.result.parent_path() / L".jobs" / L"log.part";
    const std::filesystem::path canonical = paths.result.parent_path() / L"stdout.log";
    write_text(staged, "claim output\n");
    const satsuma::vm::StepResultPublishStatus published =
        satsuma::vm::publish_step_result_if_owned(
            paths.claim,
            *acquired.claim,
            paths.result,
            make_result(*acquired.claim),
            {{staged, canonical}});
    expect(
        published == satsuma::vm::StepResultPublishStatus::Published &&
            std::filesystem::is_regular_file(paths.result) &&
            std::filesystem::is_regular_file(canonical) &&
            !std::filesystem::exists(staged),
        "owning claim did not atomically publish its result evidence");

    satsuma::StepClaimLease stale = *acquired.claim;
    stale.job_id = "job_stale";
    expect(
        satsuma::vm::publish_step_result_if_owned(
            paths.claim,
            stale,
            paths.result,
            make_result(stale)) == satsuma::vm::StepResultPublishStatus::OwnershipLost,
        "stale owner was allowed to publish a result");
    expect(
        satsuma::vm::acquire_step_claim_transaction(
            paths.claim,
            paths.result,
            make_claim("run_result", "job_late")).status ==
            satsuma::vm::StepClaimAcquireStatus::Completed,
        "completed step was offered to another Agent");
}

// 损坏和跨步骤状态会提升为稳定错误，而不是被静默覆盖。
void test_state_validation(const std::filesystem::path& root) {
    const StepPaths corrupt = make_paths(root, "corrupt");
    write_text(corrupt.claim, "{not-json");
    expect_state_error(
        [&] {
            static_cast<void>(satsuma::vm::acquire_step_claim_transaction(
                corrupt.claim,
                corrupt.result,
                make_claim("run_corrupt", "job_new")));
        },
        "corrupt claim was silently replaced");

    const StepPaths mismatch = make_paths(root, "mismatch");
    satsuma::write_json_atomic(mismatch.claim, make_claim("run_original", "job_original"));
    expect_state_error(
        [&] {
            static_cast<void>(satsuma::vm::acquire_step_claim_transaction(
                mismatch.claim,
                mismatch.result,
                make_claim("run_different", "job_new")));
        },
        "claim from another run was silently reused");
}

// 即使测试绕过 VMCI 网关，并发首次领取也只能产生一个 owner。
void test_concurrent_acquisition(const std::filesystem::path& root) {
    constexpr std::size_t count = 8;
    const StepPaths paths = make_paths(root, "concurrent");
    std::array<satsuma::vm::StepClaimAcquireResult, count> results;
    std::array<std::exception_ptr, count> errors;
    std::atomic<bool> start{false};
    std::vector<std::jthread> workers;
    workers.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        workers.emplace_back([&, index] {
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            try {
                results[index] = satsuma::vm::acquire_step_claim_transaction(
                    paths.claim,
                    paths.result,
                    make_claim("run_concurrent", "job_" + std::to_string(index)));
            } catch (...) {
                errors[index] = std::current_exception();
            }
        });
    }
    start.store(true, std::memory_order_release);
    workers.clear();

    std::size_t acquired_count = 0;
    std::size_t waiting_count = 0;
    for (std::size_t index = 0; index < count; ++index) {
        if (errors[index]) {
            std::rethrow_exception(errors[index]);
        }
        acquired_count += results[index].status ==
            satsuma::vm::StepClaimAcquireStatus::Acquired;
        waiting_count += results[index].status ==
            satsuma::vm::StepClaimAcquireStatus::Wait;
    }
    expect(
        acquired_count == 1 && waiting_count == count - 1,
        "concurrent claim requests produced more than one owner");
}

}  // namespace

int main() {
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        satsuma::path_from_utf8(satsuma::make_id("claim-store-test"));
    try {
        test_acquire_and_renew(root);
        test_safe_recovery(root);
        test_unsafe_recovery_gate(root);
        test_result_fencing(root);
        test_state_validation(root);
        test_concurrent_acquisition(root);
        std::filesystem::remove_all(root);
        std::cout << "SatsumaVmClaimStoreTests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "SatsumaVmClaimStoreTests failed: " << error.what() << '\n';
        std::error_code cleanup_error;
        std::filesystem::remove_all(root, cleanup_error);
        return 1;
    }
}
