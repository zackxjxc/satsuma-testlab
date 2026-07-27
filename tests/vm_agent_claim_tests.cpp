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

#include <nlohmann/json.hpp>

#include "agent.hpp"
#include "claim_store.hpp"
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

// 读取一份 UTF-8 测试文本。
[[nodiscard]] std::string read_text(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>(),
    };
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

// 创建指向专用共享目录和本地工作目录的 Agent 配置。
[[nodiscard]] satsuma::AgentConfig make_config(
    const std::filesystem::path& shared_root,
    const std::filesystem::path& local_work_root) {
    satsuma::AgentConfig config;
    config.lab_id = "vm_agent_claim_test";
    config.vm_id = "client";
    config.agent_version = "0.1.0";
    config.host = "192.0.2.1:9";
    config.shared_root = shared_root;
    config.local_work_root = local_work_root;
    config.poll_interval_ms = 30'000;
    config.reconnect_interval_ms = 30'000;
    return config;
}

// 创建一个可跨越多个短租约的 SYSTEM execute 任务。
void write_execute_run(
    const std::filesystem::path& shared_root,
    const std::filesystem::path& fixture,
    const std::string& run_id,
    const int sleep_ms,
    const bool include_child_probe) {
    const std::filesystem::path run_directory =
        shared_root / L"runs" / satsuma::path_from_utf8(run_id);
    const std::filesystem::path artifact =
        run_directory / L"artifacts" / L"client" / L"fixture.exe";
    std::filesystem::create_directories(artifact.parent_path());
    std::filesystem::copy_file(
        fixture,
        artifact,
        std::filesystem::copy_options::overwrite_existing);

    satsuma::RunManifest manifest;
    manifest.lab_id = "vm_agent_claim_test";
    manifest.run_id = run_id;
    manifest.request_id = satsuma::make_id("request");
    manifest.name = "active-claim-renewal";
    manifest.created_at = satsuma::utc_timestamp();
    manifest.artifacts.push_back({
        "client",
        satsuma::path_from_utf8("artifacts/client/fixture.exe"),
        satsuma::sha256_file(artifact),
    });
    satsuma::TaskStep step;
    step.id = "execute";
    step.vm = "client";
    step.type = "execute";
    step.program = satsuma::path_from_utf8("artifacts/client/fixture.exe");
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
                "--child-delay-ms", "1000",
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
    const std::filesystem::path& shared_root,
    const std::string& run_id) {
    satsuma::RunManifest manifest;
    manifest.lab_id = "vm_agent_claim_test";
    manifest.run_id = run_id;
    manifest.request_id = satsuma::make_id("request");
    manifest.name = "concurrent-claim";
    manifest.created_at = satsuma::utc_timestamp();
    satsuma::TaskStep step;
    step.id = "echo";
    step.vm = "client";
    step.type = "echo";
    step.message = "single owner";
    step.retry_safe = true;
    manifest.steps.push_back(std::move(step));
    satsuma::write_json_atomic(
        shared_root / L"runs" / satsuma::path_from_utf8(run_id) / L"task.json",
        nlohmann::json(manifest));
}

// 返回当前步骤的共享状态和结果根目录。
[[nodiscard]] std::filesystem::path step_root(
    const std::filesystem::path& shared_root,
    const std::string& run_id) {
    return shared_root / L"runs" / satsuma::path_from_utf8(run_id);
}

// 验证长任务连续续租、成功发布并停止后台线程。
void test_long_execution_renews_claim(
    const std::filesystem::path& root,
    const std::filesystem::path& fixture) {
    const std::filesystem::path shared_root = root / L"share";
    const std::filesystem::path local_work_root = root / L"work";
    const std::string run_id = "run_long_renewal";
    write_execute_run(shared_root, fixture, run_id, 700, false);

    satsuma::vm::AgentRuntimeOptions options;
    options.claim_lease_policy = test_policy();
    satsuma::vm::Agent agent(
        make_config(shared_root, local_work_root),
        {},
        options);
    expect(agent.run_once() == 1, "Agent did not execute the long renewal step");

    const std::filesystem::path run_directory = step_root(shared_root, run_id);
    const std::filesystem::path claim_path =
        run_directory / L"state" / L"client" / L"execute.claim.json";
    const satsuma::StepClaimLease effective =
        satsuma::vm::load_effective_step_claim(claim_path);
    expect(
        effective.renewal_sequence >= 2,
        "long Agent execution did not cross two renewal intervals");
    const std::uint32_t stopped_sequence = effective.renewal_sequence;
    std::this_thread::sleep_for(options.claim_lease_policy.renewal_interval * 2);
    expect(
        satsuma::vm::load_effective_step_claim(claim_path).renewal_sequence ==
            stopped_sequence,
        "Agent renewal thread continued after canonical result publication");

    const std::filesystem::path result_directory =
        run_directory / L"results" / L"client" / L"execute";
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
            result_directory / L".jobs" / satsuma::path_from_utf8(result.job_id)),
        "successful job staging directory was not cleaned up");
}

// 验证持续续租失败取消进程树，旧 job 隔离后由 attempt 2 恢复。
void test_renewal_failure_and_recovery(
    const std::filesystem::path& root,
    const std::filesystem::path& fixture) {
    const std::filesystem::path shared_root = root / L"share";
    const std::filesystem::path local_work_root = root / L"work";
    const std::string run_id = "run_renewal_failure";
    write_execute_run(shared_root, fixture, run_id, 400, true);

    satsuma::vm::AgentRuntimeOptions failing_options;
    failing_options.claim_lease_policy = test_policy();
    failing_options.claim_renew_operation = [](
        const std::filesystem::path&,
        const satsuma::StepClaimLease&,
        std::int64_t) -> satsuma::vm::StepClaimRenewResult {
        throw satsuma::Error("injected persistent Agent renewal failure");
    };
    satsuma::vm::Agent failing_agent(
        make_config(shared_root, local_work_root),
        {},
        failing_options);
    const auto failure_started = std::chrono::steady_clock::now();
    expect(failing_agent.run_once() == 1, "Agent did not enter the failing renewal step");
    expect(
        std::chrono::steady_clock::now() - failure_started < 1s,
        "persistent renewal failure did not cancel the Job Object promptly");

    const std::filesystem::path run_directory = step_root(shared_root, run_id);
    const std::filesystem::path claim_path =
        run_directory / L"state" / L"client" / L"execute.claim.json";
    const satsuma::StepClaimLease first_claim =
        satsuma::load_step_claim_lease(claim_path);
    const std::filesystem::path result_directory =
        run_directory / L"results" / L"client" / L"execute";
    const std::filesystem::path stale_result =
        result_directory / L".jobs" / satsuma::path_from_utf8(first_claim.job_id) /
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

    while (satsuma::unix_time_ms() < first_claim.lease_expires_unix_ms) {
        std::this_thread::sleep_for(5ms);
    }
    satsuma::vm::AgentRuntimeOptions recovery_options;
    recovery_options.claim_lease_policy = test_policy();
    satsuma::vm::Agent recovery_agent(
        make_config(shared_root, local_work_root),
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

    std::this_thread::sleep_for(1100ms);
    expect(
        !std::filesystem::exists(
            local_work_root / satsuma::path_from_utf8(run_id) / L"child-survived.marker"),
        "renewal cancellation left a child process outside the Job Object kill boundary");
}

// 验证两个 Agent 同时扫描时只有一个 job 能领取和发布结果。
void test_concurrent_agents_execute_once(const std::filesystem::path& root) {
    const std::filesystem::path shared_root = root / L"share";
    const std::filesystem::path local_work_root = root / L"work";
    const std::string run_id = "run_concurrent_agents";
    write_echo_run(shared_root, run_id);

    satsuma::vm::AgentRuntimeOptions options;
    options.claim_lease_policy = test_policy();
    satsuma::vm::Agent first(
        make_config(shared_root, local_work_root / L"first"),
        {},
        options);
    satsuma::vm::Agent second(
        make_config(shared_root, local_work_root / L"second"),
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

    const std::filesystem::path run_directory = step_root(shared_root, run_id);
    const satsuma::StepClaimLease claim = satsuma::load_step_claim_lease(
        run_directory / L"state" / L"client" / L"echo.claim.json");
    const satsuma::ExecutionResult result = satsuma::load_json(
        run_directory / L"results" / L"client" / L"echo" / L"execution.json")
            .get<satsuma::ExecutionResult>();
    expect(
        result.job_id == claim.job_id && result.status == "exited",
        "concurrent Agent result does not belong to the elected claim owner");
}

// 验证损坏 claim 立即发布人工门禁，而不是执行任务或无限重连。
void test_invalid_claim_enters_manual_gate(const std::filesystem::path& root) {
    const std::filesystem::path shared_root = root / L"share";
    const std::filesystem::path local_work_root = root / L"work";
    const std::string run_id = "run_invalid_claim";
    write_echo_run(shared_root, run_id);
    const std::filesystem::path run_directory = step_root(shared_root, run_id);
    const std::filesystem::path claim_path =
        run_directory / L"state" / L"client" / L"echo.claim.json";
    satsuma::write_json_atomic(
        claim_path,
        {{"schema_version", 3}, {"job_id", "job_incomplete"}});

    satsuma::vm::AgentRuntimeOptions options;
    options.claim_lease_policy = test_policy();
    satsuma::vm::Agent agent(
        make_config(shared_root, local_work_root),
        {},
        options);
    expect(agent.run_once() == 0, "Agent executed a step with an invalid persisted claim");

    const std::filesystem::path recovery_path =
        run_directory / L"state" / L"client" / L"echo.claim-recovery.json";
    const nlohmann::json recovery = satsuma::load_json(recovery_path);
    expect(
        recovery.value("status", std::string{}) == "manual_intervention_required" &&
            recovery.value("reason", std::string{}) == "claim state failed validation" &&
            !std::filesystem::exists(
                run_directory / L"results" / L"client" / L"echo" / L"execution.json"),
        "invalid claim did not preserve the manual recovery gate");
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
        std::filesystem::remove_all(root);
        std::cout << "SatsumaVmAgentClaimTests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "SatsumaVmAgentClaimTests failed: " << error.what() << '\n';
        std::error_code cleanup_error;
        std::filesystem::remove_all(root, cleanup_error);
        return 1;
    }
}
