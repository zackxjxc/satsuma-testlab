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
#include <windows.h>

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

// 等待系统时间足以生成严格递增的下一次续租时间戳。
void wait_for_next_renewal_timestamp(const satsuma::StepClaimLease& claim) {
    while (satsuma::unix_time_ms() <= claim.last_renewed_unix_ms) {
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
void test_acquire_renew_and_publish(
    const std::filesystem::path& root,
    const std::chrono::milliseconds second_renewal_delay) {
    const std::filesystem::path claim_path = root / L"state" / L"execute.claim.json";
    const std::filesystem::path result_path = root / L"results" / L"execution.json";
    std::filesystem::create_directories(result_path.parent_path());

    const satsuma::StepClaimLease proposed = make_proposed_claim(
        "job_initial",
        "boot_initial",
        5s,
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

    wait_for_next_renewal_timestamp(*acquired.claim);
    const satsuma::vm::StepClaimRenewResult renewal =
        satsuma::vm::renew_step_claim_transaction(
            claim_path,
            *acquired.claim,
            5'000);
    expect(
        renewal.status == satsuma::vm::StepClaimRenewStatus::Renewed &&
            renewal.claim.has_value(),
        "owning job could not renew its active claim");
    const satsuma::StepClaimLease& renewed = *renewal.claim;
    const std::filesystem::path first_renewal_path =
        satsuma::step_claim_renewal_path(claim_path, renewed);
    const satsuma::StepClaimLease effective =
        satsuma::vm::load_effective_step_claim(claim_path);
    expect(
        renewed.renewal_sequence == 1 &&
            effective.renewal_sequence == renewed.renewal_sequence &&
            effective.lease_expires_unix_ms == renewed.lease_expires_unix_ms &&
            std::filesystem::is_regular_file(first_renewal_path),
        "claim renewal sidecar was not selected as the effective lease");
    wait_for_next_renewal_timestamp(effective);
    if (second_renewal_delay.count() > 0) {
        std::this_thread::sleep_for(second_renewal_delay);
    }
    const satsuma::vm::StepClaimRenewResult second_renewal =
        satsuma::vm::renew_step_claim_transaction(
            claim_path,
            effective,
            5'000);
    expect(
        second_renewal.status == satsuma::vm::StepClaimRenewStatus::Renewed &&
            second_renewal.claim.has_value() &&
            second_renewal.claim->renewal_sequence == 2,
        "owning job could not publish its second immutable renewal sidecar");
    const std::filesystem::path second_renewal_path =
        satsuma::step_claim_renewal_path(claim_path, *second_renewal.claim);
    expect(
        first_renewal_path != second_renewal_path &&
            std::filesystem::is_regular_file(first_renewal_path) &&
            std::filesystem::is_regular_file(second_renewal_path) &&
            satsuma::vm::load_effective_step_claim(claim_path).renewal_sequence == 2,
        "immutable claim renewal sequence was replaced or not selected");

    const satsuma::StepClaimLease contender = make_proposed_claim(
        "job_contender",
        "boot_contender",
        5s,
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
    HANDLE blocking_reader = CreateFileW( // 模拟共享目录延迟释放的 claim 读取 lease
        claim_path.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    expect(blocking_reader != INVALID_HANDLE_VALUE, "safe recovery could not open its blocking reader");
    std::jthread release_reader([blocking_reader] {
        std::this_thread::sleep_for(250ms);
        CloseHandle(blocking_reader);
    });

    const satsuma::StepClaimLease second_proposed = make_proposed_claim(
        "job_recovered",
        "boot_recovered",
        2s,
        true);
    const auto recovery_started = std::chrono::steady_clock::now(); // 验证归档确实等待占用释放
    const satsuma::vm::StepClaimAcquireResult second =
        satsuma::vm::acquire_step_claim_transaction(
            claim_path,
            result_path,
            second_proposed,
            "boot_recovered");
    const auto recovery_elapsed = std::chrono::steady_clock::now() - recovery_started;
    expect(
        second.status == satsuma::vm::StepClaimAcquireStatus::Acquired &&
            second.claim.has_value() && second.claim->attempt == 2 &&
            second.archived_claim_path.has_value() &&
            std::filesystem::is_regular_file(*second.archived_claim_path),
        "expired safe claim was not atomically archived and reacquired");
    release_reader.join();
    expect(recovery_elapsed >= 100ms, "expired claim archive skipped its retry wait");

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
    satsuma::StepClaimLease invalid_path_claim = *acquired.claim; // 损坏内容使用的合法序号路径
    invalid_path_claim.renewal_sequence = 1;
    satsuma::write_json_atomic(
        satsuma::step_claim_renewal_path(claim_path, invalid_path_claim),
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

// 验证旧版单 sidecar 可作为 checkpoint，并继续发布下一份不可变记录。
void test_legacy_renewal_checkpoint(const std::filesystem::path& root) {
    const std::filesystem::path claim_path = root / L"state" / L"execute.claim.json";
    const std::filesystem::path result_path = root / L"results" / L"execution.json";
    const satsuma::StepClaimLease proposed = make_proposed_claim(
        "job_legacy_sidecar",
        "boot_legacy_sidecar",
        5s,
        true);
    const satsuma::vm::StepClaimAcquireResult acquired =
        satsuma::vm::acquire_step_claim_transaction(
            claim_path,
            result_path,
            proposed,
            proposed.boot_id);
    expect(acquired.claim.has_value(), "legacy-sidecar fixture did not acquire its claim");
    wait_for_next_renewal_timestamp(*acquired.claim);
    const satsuma::StepClaimLease legacy = satsuma::renew_step_claim_lease(
        *acquired.claim,
        satsuma::unix_time_ms(),
        5'000);
    const std::filesystem::path legacy_path = claim_path.parent_path() /
        satsuma::path_from_utf8(
            legacy.step_id + ".claim-renewal-" + legacy.job_id + ".json");
    satsuma::write_json_atomic(legacy_path, legacy);
    expect(
        satsuma::vm::load_effective_step_claim(claim_path).renewal_sequence == 1,
        "legacy renewal checkpoint was not selected");

    wait_for_next_renewal_timestamp(legacy);
    const satsuma::vm::StepClaimRenewResult advanced =
        satsuma::vm::renew_step_claim_transaction(
            claim_path,
            legacy,
            5'000);
    expect(
        advanced.status == satsuma::vm::StepClaimRenewStatus::Renewed &&
            advanced.claim.has_value() &&
            advanced.claim->renewal_sequence == 2 &&
            std::filesystem::is_regular_file(legacy_path) &&
            std::filesystem::is_regular_file(
                satsuma::step_claim_renewal_path(claim_path, *advanced.claim)),
        "legacy renewal checkpoint did not advance to an immutable sidecar");
}

// 验证不可变续租序列出现缺口时进入持久化状态错误。
void test_renewal_sequence_gap(const std::filesystem::path& root) {
    const std::filesystem::path claim_path = root / L"state" / L"execute.claim.json";
    const std::filesystem::path result_path = root / L"results" / L"execution.json";
    const satsuma::StepClaimLease proposed = make_proposed_claim(
        "job_sequence_gap",
        "boot_sequence_gap",
        5s,
        true);
    const satsuma::vm::StepClaimAcquireResult acquired =
        satsuma::vm::acquire_step_claim_transaction(
            claim_path,
            result_path,
            proposed,
            proposed.boot_id);
    expect(acquired.claim.has_value(), "sequence-gap fixture did not acquire its claim");
    wait_for_next_renewal_timestamp(*acquired.claim);
    const satsuma::StepClaimLease first = satsuma::renew_step_claim_lease(
        *acquired.claim,
        satsuma::unix_time_ms(),
        5'000);
    wait_for_next_renewal_timestamp(first);
    const satsuma::StepClaimLease second = satsuma::renew_step_claim_lease(
        first,
        satsuma::unix_time_ms(),
        5'000);
    satsuma::write_json_atomic(
        satsuma::step_claim_renewal_path(claim_path, second),
        second);

    bool rejected_as_state = false;
    try {
        static_cast<void>(satsuma::vm::load_effective_step_claim(claim_path));
    } catch (const satsuma::vm::StepClaimStateError&) {
        rejected_as_state = true;
    }
    expect(rejected_as_state, "claim renewal sequence gap was not rejected");
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

// 运行 claim 事务测试并清理本机或调用方指定根目录下的临时目录。
int main(const int argc, char* argv[]) {
    if (argc > 2) {
        std::cerr << "Usage: SatsumaVmClaimStoreTests [test-root]\n";
        return 2;
    }
    const std::filesystem::path base_root = argc == 2 // 本机临时目录或实机共享根目录
        ? satsuma::path_from_utf8(argv[1])
        : std::filesystem::temp_directory_path();
    const std::filesystem::path root =
        base_root / satsuma::path_from_utf8(satsuma::make_id("claim-store-test"));
    try {
        test_lock_path(root / L"lock-path");
        try {
            test_acquire_renew_and_publish(
                root / L"renew",
                argc == 2 ? 2s : 0ms); // 实机复现后台续租的稳定文件间隔
        } catch (const std::exception& error) {
            throw std::runtime_error("renew failed: " + std::string(error.what()));
        }
        test_safe_recovery_and_fencing(root / L"safe-recovery");
        test_unsafe_recovery_gate(root / L"unsafe-recovery");
        test_completed_result_validation(root / L"completed-result");
        test_invalid_renewal_state(root / L"invalid-renewal");
        test_legacy_renewal_checkpoint(root / L"legacy-renewal");
        test_renewal_sequence_gap(root / L"renewal-gap");
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
