// VM Agent claim 文件事务、并发领取和结果 fencing 测试。
#include <atomic>
#include <chrono>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

#include "claim_store.hpp"
#include "satsuma/core/claim.hpp"
#include "satsuma/core/errors.hpp"
#include "satsuma/core/id.hpp"
#include "satsuma/core/json_io.hpp"
#include "satsuma/core/path.hpp"

namespace {

using namespace std::chrono_literals;

// 在条件不满足时终止当前测试。
void expect(const bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

// 验证操作按预期抛出 Satsuma 错误。
void expect_error(const std::function<void()>& operation, const std::string& message) {
    try {
        operation();
    } catch (const satsuma::Error&) {
        return;
    }
    throw std::runtime_error(message);
}

// 写入一份测试证据文本。
void write_text(const std::filesystem::path& path, const std::string& content) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary);
    output << content;
    if (!output) {
        throw std::runtime_error("could not write claim store test evidence");
    }
}

// 读取一份测试证据文本。
[[nodiscard]] std::string read_text(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>(),
    };
}

// 创建由事务层刷新领取时间和 attempt 的初始 v3 claim。
[[nodiscard]] satsuma::StepClaimLease make_proposed_claim(
    const std::string& job_id,
    const std::string& boot_id,
    const std::chrono::milliseconds lease_duration,
    const bool retry_safe) {
    return satsuma::make_step_claim_lease(
        "run_claim_store",
        "client",
        "execute",
        job_id,
        "session_claim_store",
        boot_id,
        satsuma::unix_time_ms(),
        lease_duration.count(),
        retry_safe);
}

// 创建只包含稳定所有权字段的测试结果。
[[nodiscard]] nlohmann::json make_result(const satsuma::StepClaimLease& claim) {
    return {
        {"schema_version", 1},
        {"run_id", claim.run_id},
        {"vm_id", claim.vm_id},
        {"step_id", claim.step_id},
        {"job_id", claim.job_id},
        {"status", "exited"},
    };
}

// 等待系统时间越过指定租约截止点。
void wait_until_expired(const satsuma::StepClaimLease& claim) {
    while (satsuma::unix_time_ms() < claim.lease_expires_unix_ms) {
        std::this_thread::sleep_for(1ms);
    }
}

// 验证锁文件名对同一步骤保持稳定且不包含 JSON 扩展名。
void test_lock_path(const std::filesystem::path& root) {
    const std::filesystem::path claim_path = root / L"execute.claim.json";
    expect(
        satsuma::vm::step_claim_lock_path(claim_path).filename() == L"execute.claim.lock",
        "claim lock path did not use the stable step-level name");
}

// 验证首次领取、续租 sidecar、有效期等待和结果发布。
void test_acquire_renew_and_publish(const std::filesystem::path& root) {
    const std::filesystem::path claim_path = root / L"state" / L"execute.claim.json";
    const std::filesystem::path result_path = root / L"results" / L"execution.json";
    std::filesystem::create_directories(result_path.parent_path());

    const satsuma::StepClaimLease proposed = make_proposed_claim(
        "job_initial",
        "boot_initial",
        2s,
        true);
    const satsuma::vm::StepClaimAcquireResult acquired =
        satsuma::vm::acquire_step_claim_transaction(
            claim_path,
            result_path,
            proposed,
            "boot_initial");
    expect(
        acquired.status == satsuma::vm::StepClaimAcquireStatus::Acquired &&
            acquired.claim.has_value() && acquired.claim->attempt == 1,
        "initial claim transaction did not acquire attempt 1");

    while (satsuma::unix_time_ms() <= acquired.claim->last_renewed_unix_ms) {
        std::this_thread::sleep_for(1ms);
    }
    const satsuma::vm::StepClaimRenewResult renewal =
        satsuma::vm::renew_step_claim_transaction(
            claim_path,
            *acquired.claim,
            2'000);
    expect(
        renewal.status == satsuma::vm::StepClaimRenewStatus::Renewed &&
            renewal.claim.has_value(),
        "owning job could not renew its active claim");
    const satsuma::StepClaimLease& renewed = *renewal.claim;
    const satsuma::StepClaimLease effective =
        satsuma::vm::load_effective_step_claim(claim_path);
    expect(
        renewed.renewal_sequence == 1 &&
            effective.renewal_sequence == renewed.renewal_sequence &&
            effective.lease_expires_unix_ms == renewed.lease_expires_unix_ms,
        "claim renewal sidecar was not selected as the effective lease");

    const satsuma::StepClaimLease contender = make_proposed_claim(
        "job_contender",
        "boot_contender",
        2s,
        true);
    const satsuma::vm::StepClaimAcquireResult waiting =
        satsuma::vm::acquire_step_claim_transaction(
            claim_path,
            result_path,
            contender,
            "boot_contender");
    expect(
        waiting.status == satsuma::vm::StepClaimAcquireStatus::Wait,
        "active renewed claim was reclaimed");

    const std::filesystem::path staged_log =
        root / L"jobs" / L"job_initial" / L"stdout.log";
    const std::filesystem::path canonical_log =
        result_path.parent_path() / L"stdout.log";
    write_text(staged_log, "owned evidence\n");
    expect(
        satsuma::vm::publish_step_result_if_owned(
            claim_path,
            *acquired.claim,
            result_path,
            make_result(*acquired.claim),
            {{staged_log, canonical_log}}) == satsuma::vm::StepResultPublishStatus::Published,
        "owning job could not publish its canonical result");
    expect(
        !std::filesystem::exists(staged_log) &&
            read_text(canonical_log) == "owned evidence\n",
        "owning job did not atomically publish its staged evidence");
    const satsuma::vm::StepClaimAcquireResult completed =
        satsuma::vm::acquire_step_claim_transaction(
            claim_path,
            result_path,
            contender,
            "boot_contender");
    expect(
        completed.status == satsuma::vm::StepClaimAcquireStatus::Completed,
        "published canonical result did not close the claim transaction");
}

// 验证安全 claim 到期后递增 attempt，并隔离旧 job 的续租和结果。
void test_safe_recovery_and_fencing(const std::filesystem::path& root) {
    const std::filesystem::path claim_path = root / L"state" / L"execute.claim.json";
    const std::filesystem::path result_path = root / L"results" / L"execution.json";
    std::filesystem::create_directories(result_path.parent_path());

    const satsuma::StepClaimLease first_proposed = make_proposed_claim(
        "job_expired",
        "boot_expired",
        40ms,
        true);
    const satsuma::vm::StepClaimAcquireResult first =
        satsuma::vm::acquire_step_claim_transaction(
            claim_path,
            result_path,
            first_proposed,
            "boot_expired");
    expect(first.claim.has_value(), "safe recovery fixture did not acquire its first claim");
    wait_until_expired(*first.claim);

    const satsuma::StepClaimLease second_proposed = make_proposed_claim(
        "job_recovered",
        "boot_recovered",
        2s,
        true);
    const satsuma::vm::StepClaimAcquireResult second =
        satsuma::vm::acquire_step_claim_transaction(
            claim_path,
            result_path,
            second_proposed,
            "boot_recovered");
    expect(
        second.status == satsuma::vm::StepClaimAcquireStatus::Acquired &&
            second.claim.has_value() && second.claim->attempt == 2 &&
            second.archived_claim_path.has_value() &&
            std::filesystem::is_regular_file(*second.archived_claim_path),
        "expired safe claim was not atomically archived and reacquired");

    expect(
        satsuma::vm::renew_step_claim_transaction(
            claim_path,
            *first.claim,
            2'000).status == satsuma::vm::StepClaimRenewStatus::OwnershipLost,
        "old job renewed after ownership moved to attempt 2");
    const std::filesystem::path stale_staged_log =
        root / L"jobs" / L"job_expired" / L"stdout.log";
    const std::filesystem::path stale_canonical_log =
        result_path.parent_path() / L"stdout.log";
    write_text(stale_staged_log, "stale evidence\n");
    expect(
        satsuma::vm::publish_step_result_if_owned(
            claim_path,
            *first.claim,
            result_path,
            make_result(*first.claim),
            {{stale_staged_log, stale_canonical_log}}) ==
                satsuma::vm::StepResultPublishStatus::OwnershipLost,
        "old job published a canonical result after recovery");
    expect(
        std::filesystem::is_regular_file(stale_staged_log) &&
            !std::filesystem::exists(stale_canonical_log),
        "old job moved staged evidence after losing ownership");
    expect(
        satsuma::vm::publish_step_result_if_owned(
            claim_path,
            *second.claim,
            result_path,
            make_result(*second.claim)) == satsuma::vm::StepResultPublishStatus::Published,
        "recovered job could not publish its canonical result");
    expect(
        satsuma::load_json(result_path).value("job_id", std::string{}) == second.claim->job_id,
        "canonical result retained the stale job identity");
}

// 验证危险步骤到期后只能进入人工门禁。
void test_unsafe_recovery_gate(const std::filesystem::path& root) {
    const std::filesystem::path claim_path = root / L"state" / L"execute.claim.json";
    const std::filesystem::path result_path = root / L"results" / L"execution.json";
    const satsuma::StepClaimLease proposed = make_proposed_claim(
        "job_unsafe",
        "boot_unsafe",
        40ms,
        false);
    const satsuma::vm::StepClaimAcquireResult first =
        satsuma::vm::acquire_step_claim_transaction(
            claim_path,
            result_path,
            proposed,
            "boot_unsafe");
    expect(first.claim.has_value(), "unsafe recovery fixture did not acquire its claim");
    wait_until_expired(*first.claim);

    const satsuma::StepClaimLease contender = make_proposed_claim(
        "job_unsafe_contender",
        "boot_unsafe_contender",
        2s,
        false);
    const satsuma::vm::StepClaimAcquireResult gated =
        satsuma::vm::acquire_step_claim_transaction(
            claim_path,
            result_path,
            contender,
            "boot_unsafe_contender");
    expect(
        gated.status == satsuma::vm::StepClaimAcquireStatus::ManualInterventionRequired &&
            gated.claim.has_value() && gated.claim->job_id == first.claim->job_id,
        "expired unsafe claim did not preserve its manual recovery gate");
}

// 验证存在的 canonical 结果必须匹配当前步骤和持久化 claim owner。
void test_completed_result_validation(const std::filesystem::path& root) {
    const std::filesystem::path claim_path = root / L"state" / L"execute.claim.json";
    const std::filesystem::path result_path = root / L"results" / L"execution.json";
    std::filesystem::create_directories(result_path.parent_path());
    const satsuma::StepClaimLease proposed = make_proposed_claim(
        "job_completed",
        "boot_completed",
        2s,
        true);

    nlohmann::json mismatched_step = make_result(proposed);
    mismatched_step["step_id"] = "other_step";
    satsuma::write_json_atomic(result_path, mismatched_step);
    expect_error(
        [&] {
            static_cast<void>(satsuma::vm::acquire_step_claim_transaction(
                claim_path,
                result_path,
                proposed,
                "boot_completed"));
        },
        "claim transaction accepted a result for another step");

    std::filesystem::remove(result_path);
    const satsuma::vm::StepClaimAcquireResult acquired =
        satsuma::vm::acquire_step_claim_transaction(
            claim_path,
            result_path,
            proposed,
            "boot_completed");
    expect(acquired.claim.has_value(), "completed-result fixture did not acquire its claim");
    nlohmann::json mismatched_job = make_result(*acquired.claim);
    mismatched_job["job_id"] = "job_other";
    satsuma::write_json_atomic(result_path, mismatched_job);
    const satsuma::StepClaimLease contender = make_proposed_claim(
        "job_completed_contender",
        "boot_completed_contender",
        2s,
        true);
    expect_error(
        [&] {
            static_cast<void>(satsuma::vm::acquire_step_claim_transaction(
                claim_path,
                result_path,
                contender,
                "boot_completed_contender"));
        },
        "claim transaction accepted a result owned by another job");
}

// 验证损坏或串属的 renewal sidecar 使用专门的状态错误拒绝。
void test_invalid_renewal_state(const std::filesystem::path& root) {
    const std::filesystem::path claim_path = root / L"state" / L"execute.claim.json";
    const std::filesystem::path result_path = root / L"results" / L"execution.json";
    const satsuma::StepClaimLease proposed = make_proposed_claim(
        "job_invalid_sidecar",
        "boot_invalid_sidecar",
        2s,
        true);
    const satsuma::vm::StepClaimAcquireResult acquired =
        satsuma::vm::acquire_step_claim_transaction(
            claim_path,
            result_path,
            proposed,
            proposed.boot_id);
    expect(acquired.claim.has_value(), "invalid-sidecar fixture did not acquire its claim");
    satsuma::write_json_atomic(
        satsuma::step_claim_renewal_path(claim_path, *acquired.claim),
        {{"schema_version", 3}, {"job_id", "job_other"}});

    bool rejected_as_state = false;
    try {
        static_cast<void>(satsuma::vm::load_effective_step_claim(claim_path));
    } catch (const satsuma::vm::StepClaimStateError&) {
        rejected_as_state = true;
    }
    expect(
        rejected_as_state,
        "invalid renewal sidecar was not classified as a persisted state error");
}

// 验证两个并发首次领取事务只能产生一个 owner。
void test_concurrent_initial_acquire(const std::filesystem::path& root) {
    const std::filesystem::path claim_path = root / L"state" / L"execute.claim.json";
    const std::filesystem::path result_path = root / L"results" / L"execution.json";
    const satsuma::StepClaimLease proposed[] = {
        make_proposed_claim("job_race_a", "boot_race_a", 2s, true),
        make_proposed_claim("job_race_b", "boot_race_b", 2s, true),
    };
    satsuma::vm::StepClaimAcquireResult results[2]; // 两个竞争线程的事务结果
    std::exception_ptr errors[2]; // 两个竞争线程的异常
    std::atomic<bool> start{false}; // 同时释放两个事务
    std::thread workers[2];
    for (int index = 0; index < 2; ++index) {
        workers[index] = std::thread([&, index] {
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            try {
                results[index] = satsuma::vm::acquire_step_claim_transaction(
                    claim_path,
                    result_path,
                    proposed[index],
                    proposed[index].boot_id);
            } catch (...) {
                errors[index] = std::current_exception();
            }
        });
    }
    start.store(true, std::memory_order_release);
    for (std::thread& worker : workers) {
        worker.join();
    }
    for (const std::exception_ptr& error : errors) {
        if (error != nullptr) {
            std::rethrow_exception(error);
        }
    }

    int acquired_count = 0;
    int waiting_count = 0;
    std::string acquired_job;
    for (const satsuma::vm::StepClaimAcquireResult& result : results) {
        if (result.status == satsuma::vm::StepClaimAcquireStatus::Acquired) {
            ++acquired_count;
            acquired_job = result.claim->job_id;
        } else if (result.status == satsuma::vm::StepClaimAcquireStatus::Wait) {
            ++waiting_count;
        }
    }
    expect(
        acquired_count == 1 && waiting_count == 1,
        "concurrent claim transactions did not elect exactly one owner");
    expect(
        satsuma::vm::load_effective_step_claim(claim_path).job_id == acquired_job,
        "persisted claim owner differs from the concurrent transaction winner");
}

}  // namespace

// 运行 claim 事务测试并清理专用临时目录。
int main() {
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        satsuma::path_from_utf8(satsuma::make_id("claim-store-test"));
    try {
        test_lock_path(root / L"lock-path");
        test_acquire_renew_and_publish(root / L"renew");
        test_safe_recovery_and_fencing(root / L"safe-recovery");
        test_unsafe_recovery_gate(root / L"unsafe-recovery");
        test_completed_result_validation(root / L"completed-result");
        test_invalid_renewal_state(root / L"invalid-renewal");
        test_concurrent_initial_acquire(root / L"concurrent-acquire");
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
