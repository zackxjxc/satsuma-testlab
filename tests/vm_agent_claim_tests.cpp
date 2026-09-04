// VM Agent 主动续租、失权取消、恢复和并发领取集成测试。
#include <atomic>
#include <chrono>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

#include <windows.h>

#include <nlohmann/json.hpp>

#include "agent.hpp"
#include "satsuma/core/claim_store.hpp"
#include "satsuma/core/errors.hpp"
#include "satsuma/core/id.hpp"
#include "satsuma/core/json_io.hpp"
#include "satsuma/core/path.hpp"
#include "satsuma/core/sha256.hpp"
#include "satsuma/core/task.hpp"

namespace {

using namespace std::chrono_literals;

// 在条件不满足时终止当前测试。
void expect(const bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

// 在有限时间内等待异步进程树收敛。
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

// 读取一份 UTF-8 测试文本。
[[nodiscard]] std::string read_text(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>(),
    };
}

// 判断测试夹具记录的子进程是否已退出。
[[nodiscard]] bool process_has_exited(const DWORD process_id) {
    const HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, process_id);
    if (process == nullptr) {
        if (GetLastError() == ERROR_INVALID_PARAMETER) {
            return true;
        }
        throw std::runtime_error(
            "cannot open fixture child process " + std::to_string(process_id));
    }
    const DWORD wait_result = WaitForSingleObject(process, 0);
    CloseHandle(process);
    if (wait_result == WAIT_OBJECT_0) {
        return true;
    }
    if (wait_result == WAIT_TIMEOUT) {
        return false;
    }
    throw std::runtime_error("cannot query fixture child process termination state");
}

// 返回毫秒级 Agent claim 测试策略。
[[nodiscard]] satsuma::vm::ClaimLeasePolicy test_policy() {
    return {
        300ms,
        50ms,
        20ms,
        60ms,
    };
}

// 成功与恢复路径使用宽租约，容忍共享 CI 主机的长时间调度停顿。
[[nodiscard]] satsuma::vm::ClaimLeasePolicy stable_execution_policy() {
    return {
        30'000ms,
        100ms,
        30ms,
        3'000ms,
    };
}

// 失败注入路径保留有限租约，同时保证续租回调能在慢速 CI 上实际执行。
[[nodiscard]] satsuma::vm::ClaimLeasePolicy failure_injection_policy() {
    return {
        5'000ms,
        50ms,
        20ms,
        1'000ms,
    };
}

// 创建指向专用镜像目录和本地工作目录的 Agent 配置。
[[nodiscard]] satsuma::AgentConfig make_config(
    const std::filesystem::path& mirror_root,
    const std::filesystem::path& local_work_root) {
    satsuma::AgentConfig config;
    config.lab_id = "vm_agent_claim_test";
    config.vm_id = "vm_01";
    config.agent_version = "0.1.0";
    config.mirror_root = mirror_root;
    config.local_work_root = local_work_root;
    config.poll_interval_ms = 30'000;
    config.reconnect_interval_ms = 30'000;
    return config;
}

// 创建一个可跨越多个短租约的 SYSTEM execute 任务。
void write_execute_run(
    const std::filesystem::path& mirror_root,
    const std::filesystem::path& fixture,
    const std::string& run_id,
    const int sleep_ms,
    const bool include_child_probe,
    const int child_delay_ms = 1'000) {
    const std::filesystem::path run_directory =
        mirror_root / L"runs" / satsuma::path_from_utf8(run_id);
    const std::filesystem::path artifact =
        run_directory / L"artifacts" / L"vm_01" / L"fixture.exe";
    std::filesystem::create_directories(artifact.parent_path());
    std::filesystem::copy_file(
        fixture,
        artifact,
        std::filesystem::copy_options::overwrite_existing);

    satsuma::RunManifest manifest;
    manifest.lab_id = "vm_agent_claim_test";
    manifest.run_id = run_id;
    manifest.name = "active-claim-renewal";
    manifest.created_at = satsuma::utc_timestamp();
    manifest.artifacts.push_back({
        "vm_01",
        satsuma::path_from_utf8("artifacts/vm_01/fixture.exe"),
        satsuma::sha256_file(artifact),
    });
    satsuma::TaskStep step;
    step.id = "execute";
    step.vm = "vm_01";
    step.type = "execute";
    step.program = satsuma::path_from_utf8("artifacts/vm_01/fixture.exe");
    step.run_as = satsuma::TaskRunAs::System;
    step.arguments = {
        "--ready-file", "ready.marker",
        "--sleep-ms", std::to_string(sleep_ms),
        "--message", "claim renewal execution",
    };
    if (include_child_probe) {
        step.arguments.insert(
            step.arguments.end(),
            {
                "--child-pid-file", "child.pid",
                "--child-marker", "child-survived.marker",
                "--child-delay-ms", std::to_string(child_delay_ms),
            });
    }
    step.timeout_seconds = 5;
    step.retry_safe = true;
    manifest.steps.push_back(std::move(step));
    satsuma::write_json_atomic(
        run_directory / L"task.json",
        nlohmann::json(manifest));
}

// 创建一个用于并发首次领取的 echo 任务。
void write_echo_run(
    const std::filesystem::path& mirror_root,
    const std::string& run_id) {
    satsuma::RunManifest manifest;
    manifest.lab_id = "vm_agent_claim_test";
    manifest.run_id = run_id;
    manifest.name = "concurrent-claim";
    manifest.created_at = satsuma::utc_timestamp();
    satsuma::TaskStep step;
    step.id = "echo";
    step.vm = "vm_01";
    step.type = "echo";
    step.message = "single owner";
    step.run_as = satsuma::TaskRunAs::System;
    step.retry_safe = true;
    manifest.steps.push_back(std::move(step));
    satsuma::write_json_atomic(
        mirror_root / L"runs" / satsuma::path_from_utf8(run_id) / L"task.json",
        nlohmann::json(manifest));
}

// 返回当前步骤的共享状态和结果根目录。
[[nodiscard]] std::filesystem::path step_root(
    const std::filesystem::path& mirror_root,
    const std::string& run_id) {
    return mirror_root / L"runs" / satsuma::path_from_utf8(run_id);
}

// 验证长任务连续续租、成功发布并停止后台线程。
void test_long_execution_renews_claim(
    const std::filesystem::path& root,
    const std::filesystem::path& fixture) {
    const std::filesystem::path mirror_root = root / L"mirror";
    const std::filesystem::path local_work_root = root / L"work";
    const std::string run_id = "run_long_renewal";
    write_execute_run(mirror_root, fixture, run_id, 700, false);

    satsuma::vm::AgentRuntimeOptions options;
    options.claim_lease_policy = stable_execution_policy();
    std::atomic<bool> observed_local_logs{false}; // 续租时是否只看到 Guest 本地运行日志
    options.claim_renew_operation = [&local_work_root, &mirror_root, &run_id, &observed_local_logs](
        const std::filesystem::path& claim_path,
        const satsuma::StepClaimLease& claim,
        const std::int64_t renewed_unix_ms) {
        const std::filesystem::path local_log =
            local_work_root / L"staging" / satsuma::path_from_utf8(claim.job_id) /
            L"stdout.log.partial";
        const std::filesystem::path mirrored_log =
            mirror_root / L"runs" / satsuma::path_from_utf8(run_id) / L"results" /
            L"vm_01" / L"execute" / L".jobs" / satsuma::path_from_utf8(claim.job_id) /
            L"stdout.log.partial";
        if (std::filesystem::is_regular_file(local_log) && !std::filesystem::exists(mirrored_log)) {
            observed_local_logs.store(true, std::memory_order_release);
        }
        return satsuma::vm::renew_step_claim_transaction(
            claim_path,
            claim,
            renewed_unix_ms);
    };
    satsuma::vm::Agent agent(
        make_config(mirror_root, local_work_root),
        {},
        options);
    expect(agent.run_once() == 1, "Agent did not execute the long renewal step");

    const std::filesystem::path run_directory = step_root(mirror_root, run_id);
    const std::filesystem::path claim_path =
        run_directory / L"state" / L"vm_01" / L"execute.claim.json";
    const satsuma::StepClaimLease effective =
        satsuma::load_step_claim_lease(claim_path);
    expect(
        effective.renewal_sequence >= 2,
        "long Agent execution did not cross two renewal intervals");
    expect(
        observed_local_logs.load(std::memory_order_acquire),
        "running process logs were not isolated from the VMCI mirror");
    const std::uint32_t stopped_sequence = effective.renewal_sequence;
    std::this_thread::sleep_for(options.claim_lease_policy.renewal_interval * 2);
    expect(
        satsuma::load_step_claim_lease(claim_path).renewal_sequence ==
            stopped_sequence,
        "Agent renewal thread continued after canonical result publication");

    const std::filesystem::path result_directory =
        run_directory / L"results" / L"vm_01" / L"execute";
    const satsuma::ExecutionResult result = satsuma::load_json(
        result_directory / L"execution.json").get<satsuma::ExecutionResult>();
    expect(
        result.status == "exited" && result.exit_code == 0 &&
            result.job_id == effective.job_id,
        "long Agent execution did not publish the owning job result");
    const std::string stdout_text = read_text(result_directory / L"stdout.log");
    expect(
        stdout_text == "claim renewal execution\n" ||
            stdout_text == "claim renewal execution\r\n",
        "canonical stdout did not contain the completed job output");
    expect(
        !std::filesystem::exists(
            local_work_root / L"staging" / satsuma::path_from_utf8(result.job_id)),
        "successful job staging directory was not cleaned up");
    expect(
        !std::filesystem::exists(
            local_work_root / L"vm_agent_claim_test" / satsuma::path_from_utf8(run_id) /
            L"vm_01" / L".satsuma" / L"jobs" / satsuma::path_from_utf8(result.job_id)),
        "successful local job log directory was not cleaned up");
}

// 验证持续续租失败取消进程树，旧 job 隔离后由 attempt 2 恢复。
void test_renewal_failure_and_recovery(
    const std::filesystem::path& root,
    const std::filesystem::path& fixture) {
    const std::filesystem::path mirror_root = root / L"mirror";
    const std::filesystem::path local_work_root = root / L"work";
    const std::string run_id = "run_renewal_failure";
    write_execute_run(mirror_root, fixture, run_id, 6'000, true, -1);

    satsuma::vm::AgentRuntimeOptions failing_options;
    failing_options.claim_lease_policy = failure_injection_policy();
    failing_options.claim_renew_operation = [](
        const std::filesystem::path&,
        const satsuma::StepClaimLease&,
        std::int64_t) -> satsuma::vm::StepClaimRenewResult {
        throw satsuma::Error("injected persistent Agent renewal failure");
    };
    satsuma::vm::Agent failing_agent(
        make_config(mirror_root, local_work_root),
        {},
        failing_options);
    expect(failing_agent.run_once() == 1, "Agent did not enter the failing renewal step");

    const std::filesystem::path run_directory = step_root(mirror_root, run_id);
    const std::filesystem::path claim_path =
        run_directory / L"state" / L"vm_01" / L"execute.claim.json";
    const satsuma::StepClaimLease first_claim =
        satsuma::load_step_claim_lease(claim_path);
    const std::filesystem::path result_directory =
        run_directory / L"results" / L"vm_01" / L"execute";
    const std::filesystem::path stale_result =
        local_work_root / L"staging" / satsuma::path_from_utf8(first_claim.job_id) /
        L"stale-execution.json";
    expect(
        !std::filesystem::exists(result_directory / L"execution.json") &&
            std::filesystem::is_regular_file(stale_result),
        "lease-lost job published canonical output or omitted stale evidence");
    const nlohmann::json stale = satsuma::load_json(stale_result);
    expect(
        stale.value("claim_status", std::string{}) == "ownership_lost" &&
            stale.value("claim_error", std::string{}).find("persistent Agent") !=
                std::string::npos,
        "stale Agent evidence omitted the renewal failure reason");

    const std::filesystem::path child_pid_path =
        local_work_root / L"vm_agent_claim_test" / satsuma::path_from_utf8(run_id) /
        L"vm_01" / L"child.pid";
    expect(
        std::filesystem::is_regular_file(child_pid_path),
        "lease-lost job did not record its child process ID");
    const DWORD first_child_process_id = static_cast<DWORD>(
        std::stoul(read_text(child_pid_path)));
    expect(
        wait_for_condition([&] { return process_has_exited(first_child_process_id); }, 5s),
        "lease loss did not terminate the first attempt's child process");

    // attempt 2 验证恢复发布，不重复运行故意跨越安全截止的长任务。
    write_execute_run(mirror_root, fixture, run_id, 400, true, -1);
    while (satsuma::unix_time_ms() < first_claim.lease_expires_unix_ms) {
        std::this_thread::sleep_for(5ms);
    }
    satsuma::vm::AgentRuntimeOptions recovery_options;
    recovery_options.claim_lease_policy = stable_execution_policy();
    satsuma::vm::Agent recovery_agent(
        make_config(mirror_root, local_work_root),
        {},
        recovery_options);
    expect(recovery_agent.run_once() == 1, "attempt 2 did not recover the expired safe claim");

    const satsuma::StepClaimLease recovered_claim =
        satsuma::load_step_claim_lease(claim_path);
    const satsuma::ExecutionResult recovered_result = satsuma::load_json(
        result_directory / L"execution.json").get<satsuma::ExecutionResult>();
    expect(
        recovered_claim.attempt == 2 &&
            recovered_claim.job_id != first_claim.job_id &&
            recovered_result.job_id == recovered_claim.job_id &&
            recovered_result.status == "exited" && recovered_result.exit_code == 0,
        "attempt 2 did not own the recovered canonical result");
    expect(
        std::filesystem::is_regular_file(stale_result),
        "recovered job removed the old job forensic evidence");

    expect(
        std::filesystem::is_regular_file(child_pid_path),
        "recovered job did not record its child process ID");
    const DWORD recovered_child_process_id = static_cast<DWORD>(
        std::stoul(read_text(child_pid_path)));
    expect(
        wait_for_condition(
            [&] {
                return process_has_exited(recovered_child_process_id);
            },
            5s),
        "normal completion did not terminate the recovered attempt's child process");
}

// 验证两个 Agent 同时扫描时只有一个 job 能领取和发布结果。
void test_concurrent_agents_execute_once(const std::filesystem::path& root) {
    const std::filesystem::path mirror_root = root / L"mirror";
    const std::filesystem::path local_work_root = root / L"work";
    const std::string run_id = "run_concurrent_agents";
    write_echo_run(mirror_root, run_id);

    satsuma::vm::AgentRuntimeOptions options;
    options.claim_lease_policy = stable_execution_policy();
    satsuma::vm::Agent first(
        make_config(mirror_root, local_work_root / L"first"),
        {},
        options);
    satsuma::vm::Agent second(
        make_config(mirror_root, local_work_root / L"second"),
        {},
        options);
    std::atomic<bool> start{false}; // 同时释放两个 Agent 扫描
    int executed[2]{}; // 两个 Agent 各自执行的步骤数
    std::exception_ptr errors[2]; // 两个 Agent 的工作线程异常
    std::thread workers[2] = {
        std::thread([&] {
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            try {
                executed[0] = first.run_once();
            } catch (...) {
                errors[0] = std::current_exception();
            }
        }),
        std::thread([&] {
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            try {
                executed[1] = second.run_once();
            } catch (...) {
                errors[1] = std::current_exception();
            }
        }),
    };
    start.store(true, std::memory_order_release);
    for (std::thread& worker : workers) {
        worker.join();
    }
    for (const std::exception_ptr& error : errors) {
        if (error != nullptr) {
            std::rethrow_exception(error);
        }
    }
    expect(
        executed[0] + executed[1] == 1,
        "concurrent Agents did not execute exactly one step");

    const std::filesystem::path run_directory = step_root(mirror_root, run_id);
    const satsuma::StepClaimLease claim = satsuma::load_step_claim_lease(
        run_directory / L"state" / L"vm_01" / L"echo.claim.json");
    const satsuma::ExecutionResult result = satsuma::load_json(
        run_directory / L"results" / L"vm_01" / L"echo" / L"execution.json")
            .get<satsuma::ExecutionResult>();
    expect(
        result.job_id == claim.job_id && result.status == "exited",
        "concurrent Agent result does not belong to the elected claim owner");
}

// 验证损坏 claim 立即发布人工门禁，而不是执行任务或无限重连。
void test_invalid_claim_enters_manual_gate(const std::filesystem::path& root) {
    const std::filesystem::path mirror_root = root / L"mirror";
    const std::filesystem::path local_work_root = root / L"work";
    const std::string run_id = "run_invalid_claim";
    write_echo_run(mirror_root, run_id);
    const std::filesystem::path run_directory = step_root(mirror_root, run_id);
    const std::filesystem::path claim_path =
        run_directory / L"state" / L"vm_01" / L"echo.claim.json";
    satsuma::write_json_atomic(
        claim_path,
        {{"schema_version", 3}, {"job_id", "job_incomplete"}});

    satsuma::vm::AgentRuntimeOptions options;
    options.claim_lease_policy = test_policy();
    satsuma::vm::Agent agent(
        make_config(mirror_root, local_work_root),
        {},
        options);
    expect(agent.run_once() == 0, "Agent executed a step with an invalid persisted claim");

    const std::filesystem::path recovery_path =
        run_directory / L"state" / L"vm_01" / L"echo.claim-recovery.json";
    const nlohmann::json recovery = satsuma::load_json(recovery_path);
    expect(
        recovery.value("status", std::string{}) == "manual_intervention_required" &&
            recovery.value("reason", std::string{}) == "claim state failed validation" &&
            !std::filesystem::exists(
                run_directory / L"results" / L"vm_01" / L"echo" / L"execution.json"),
        "invalid claim did not preserve the manual recovery gate");
}

// 上传失败只补发摘要，保留退出码和本地证据；彻底断线仍禁止重跑不安全步骤。
void test_evidence_failure_reporting(
    const std::filesystem::path& root,
    const std::filesystem::path& fixture,
    const bool offline) {
    const auto mirror = root / L"mirror";
    const auto work = root / L"work";
    const std::string run_id = offline ? "run_offline_evidence" : "run_failed_evidence";
    write_execute_run(mirror, fixture, run_id, 10, false);
    const auto run = step_root(mirror, run_id);
    auto manifest = satsuma::load_run_manifest(run / L"task.json");
    manifest.steps.front().retry_safe = false;
    if (!offline) {
        auto later = manifest.steps.front();
        later.id = "later_claim_error";
        manifest.steps.push_back(later);
    }
    satsuma::write_json_atomic(run / L"task.json", manifest);
    satsuma::vm::AgentRuntimeOptions options;
    options.claim_lease_policy = stable_execution_policy();
    int publish_calls = 0;
    options.claim_acquire_operation = [](const auto& claim_path, const auto& result_path, const auto& claim) {
        if (claim.step_id == "later_claim_error") throw satsuma::Error("injected later claim error");
        return satsuma::vm::acquire_step_claim_transaction(claim_path, result_path, claim);
    };
    options.result_publish_operation = [&](const auto& claim_path, const auto& claim,
        const auto& result_path, const nlohmann::json& result, const auto& evidence) {
        ++publish_calls;
        const auto staging = work / L"staging" / satsuma::path_from_utf8(claim.job_id);
        const auto attempt = satsuma::load_json(staging / L"attempt.json");
        expect(attempt.at("run_id") == run_id && attempt.at("job_id") == claim.job_id &&
            attempt.at("attempt") == claim.attempt, "flat staging lost attempt identity");
        if (publish_calls == 1) {
            expect(evidence.size() == 2 && evidence.front().staged_path.parent_path() == staging,
                "evidence staging retained redundant run/VM/step layers");
            throw satsuma::Error("injected upload failure for stdout.log");
        }
        expect(evidence.empty() && result.at("stdout") == "" && result.at("files").empty(),
            "summary-only publication claimed unavailable evidence");
        if (offline) throw satsuma::Error("injected disconnected Host");
        return satsuma::vm::publish_step_result_if_owned(claim_path, claim, result_path, result, evidence);
    };
    satsuma::vm::Agent agent(make_config(mirror, work), {}, options);
    expect(agent.run_once() == 1 && publish_calls == 2, "publication fallback did not execute exactly once");
    const auto claim_path = run / L"state" / L"vm_01" / L"execute.claim.json";
    auto claim = satsuma::load_step_claim_lease(claim_path);
    const auto staging = work / L"staging" / satsuma::path_from_utf8(claim.job_id);
    const auto diagnostic = satsuma::load_json(run / L"state" / L"vm_01-agent-error.json");
    const auto& step_error = diagnostic.at("step_errors").at(0);
    expect(diagnostic.at("status") == "execution_error", "later run error invalidated prior step diagnostics");
    expect(step_error.at("process_status") == "exited" && step_error.at("exit_code") == 0 &&
        step_error.at("evidence_status") == "publication_failed" &&
        step_error.at("publication_status") == (offline ? "unreported" : "published"),
        "upload failure lost the independent process/evidence outcome");
    expect(std::filesystem::is_regular_file(staging / L"stdout.log") &&
        std::filesystem::is_regular_file(staging / L"execution.json"), "failed upload deleted forensic evidence");
    const auto canonical = run / L"results" / L"vm_01" / L"execute" / L"execution.json";
    if (offline) {
        expect(!std::filesystem::exists(canonical), "disconnected publication fabricated a canonical result");
        claim.lease_expires_unix_ms = satsuma::unix_time_ms() - 1;
        claim.last_renewed_unix_ms = claim.lease_expires_unix_ms - 1;
        satsuma::write_json_atomic(claim_path, claim);
        expect(agent.run_once() == 0 && publish_calls == 2, "unsafe offline step was executed again");
        expect(satsuma::load_json(run / L"state" / L"vm_01" / L"execute.claim-recovery.json").at("status") ==
            "manual_intervention_required", "offline unsafe claim did not retain its recovery gate");
    } else {
        const auto result = satsuma::load_json(canonical).get<satsuma::ExecutionResult>();
        expect(result.status == "failed" && result.exit_code == 0 && result.stdout_path.empty(),
            "failure summary lost process exit status or claimed uploaded stdout");
        expect(agent.run_once() == 0 && publish_calls == 2, "published failed step was executed again");
    }
}

// 收集缺失文件仍发布执行摘要，已产生的日志保留在短暂存目录。
void test_collection_failure_reporting(const std::filesystem::path& root, const std::filesystem::path& fixture) {
    const auto mirror = root / L"mirror";
    const auto work = root / L"work";
    const std::string run_id = "run_missing_collection";
    write_execute_run(mirror, fixture, run_id, 10, false);
    const auto run = step_root(mirror, run_id);
    auto manifest = satsuma::load_run_manifest(run / L"task.json");
    manifest.steps.front().collect_files.push_back(L"missing.json");
    satsuma::write_json_atomic(run / L"task.json", manifest);
    satsuma::vm::Agent agent(make_config(mirror, work));
    expect(agent.run_once() == 1, "collection failure prevented completion reporting");
    const auto result = satsuma::load_json(run / L"results" / L"vm_01" / L"execute" / L"execution.json")
        .get<satsuma::ExecutionResult>();
    const auto diagnostic = satsuma::load_json(run / L"state" / L"vm_01-agent-error.json").at("step_errors").at(0);
    expect(result.status == "failed" && result.exit_code == 0 &&
        diagnostic.at("process_status") == "exited" && diagnostic.at("evidence_status") == "collection_failed",
        "collection failure was confused with process failure");
    expect(std::filesystem::is_regular_file(work / L"staging" / satsuma::path_from_utf8(result.job_id) /
        L"stdout.log.partial"), "collection failure deleted original logs");
}

// 预置同名目录不能被接管；准备失败也给出未启动的规范失败摘要。
void test_attempt_collision(const std::filesystem::path& root) {
    const auto mirror = root / L"mirror";
    const auto work = root / L"work";
    const std::string run_id = "run_attempt_collision";
    write_echo_run(mirror, run_id);
    satsuma::vm::AgentRuntimeOptions options;
    options.claim_lease_policy = stable_execution_policy();
    std::filesystem::path occupied;
    options.claim_acquire_operation = [&](const auto& claim_path, const auto& result_path, const auto& claim) {
        auto acquired = satsuma::vm::acquire_step_claim_transaction(claim_path, result_path, claim);
        if (acquired.claim) {
            occupied = work / L"staging" / satsuma::path_from_utf8(acquired.claim->job_id);
            satsuma::write_json_atomic(occupied / L"unrelated.json", {{"preserved", true}});
        }
        return acquired;
    };
    satsuma::vm::Agent agent(make_config(mirror, work), {}, options);
    expect(agent.run_once() == 1, "attempt collision did not publish its failure summary");
    const auto run = step_root(mirror, run_id);
    const auto result = satsuma::load_json(run / L"results" / L"vm_01" / L"echo" / L"execution.json");
    const auto diagnostic = satsuma::load_json(run / L"state" / L"vm_01-agent-error.json").at("step_errors").at(0);
    expect(result.at("status") == "failed" && result.at("exit_code").is_null() &&
        diagnostic.at("process_status") == "not_started" &&
        diagnostic.at("evidence_status") == "preparation_failed", "preparation error lost its step identity");
    expect(satsuma::load_json(occupied / L"unrelated.json").at("preserved") == true &&
        std::distance(std::filesystem::directory_iterator(occupied), std::filesystem::directory_iterator{}) == 1,
        "attempt collision modified unrelated files");
}

}  // namespace

// 运行 Agent claim 集成测试并清理专用临时目录。
int main(const int argc, char* argv[]) {
    if (argc != 2) {
        std::cerr << "Usage: SatsumaVmAgentClaimTests <fixture.exe>\n";
        return 2;
    }
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        satsuma::path_from_utf8(satsuma::make_id("vm-agent-claim-test"));
    try {
        const std::filesystem::path fixture = std::filesystem::absolute(argv[1]);
        test_long_execution_renews_claim(root / L"long-renewal", fixture);
        test_renewal_failure_and_recovery(root / L"failure-recovery", fixture);
        test_concurrent_agents_execute_once(root / L"concurrent-agents");
        test_invalid_claim_enters_manual_gate(root / L"invalid-claim");
        test_evidence_failure_reporting(root / L"publication-failed", fixture, false);
        test_evidence_failure_reporting(root / L"publication-offline", fixture, true);
        test_collection_failure_reporting(root / L"collection-failed", fixture);
        test_attempt_collision(root / L"attempt-collision");
        std::filesystem::remove_all(root);
        std::cout << "SatsumaVmAgentClaimTests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "SatsumaVmAgentClaimTests failed: " << error.what() << '\n';
        std::cerr << "Failure evidence retained at: " << satsuma::path_to_utf8(root) << '\n';
        return 1;
    }
}
