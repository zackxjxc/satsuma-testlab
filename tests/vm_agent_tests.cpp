// VM Agent 文件通道和进程取消测试。
#include <chrono>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stop_token>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

#include <nlohmann/json.hpp>

#include "agent.hpp"
#include "hardware_identity.hpp"
#include "inventory.hpp"
#include "interactive_process.hpp"
#include "process_runner.hpp"
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

// 读取测试结果文本。
[[nodiscard]] std::string read_text(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>(),
    };
}

// 在有限时间内等待文件通道证据出现。
[[nodiscard]] bool wait_for_file(
    const std::filesystem::path& path,
    const std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (std::filesystem::is_regular_file(path)) {
            return true;
        }
        std::this_thread::sleep_for(20ms);
    }
    return std::filesystem::is_regular_file(path);
}

// 创建一个只包含 echo 步骤的文件任务。
void write_echo_run(const std::filesystem::path& shared_root) {
    satsuma::RunManifest manifest;
    manifest.lab_id = "vm_agent_test";
    manifest.run_id = "run_file_success";
    manifest.request_id = "request_file_success";
    manifest.name = "file-channel-success";
    manifest.created_at = satsuma::utc_timestamp();
    satsuma::TaskStep step;
    step.id = "echo_success";
    step.vm = "vm_01";
    step.type = "echo";
    step.message = "file channel completed without Host";
    step.retry_safe = true;
    manifest.steps.push_back(std::move(step));

    const std::filesystem::path run_directory = shared_root / L"runs" / L"run_file_success";
    satsuma::write_json_atomic(run_directory / L"task.json", nlohmann::json(manifest));
}

// 创建一个长时间运行的 execute 步骤，供 Agent 停止测试取消。
void write_cancellable_run(
    const std::filesystem::path& shared_root,
    const std::filesystem::path& fixture) {
    const std::filesystem::path run_directory = shared_root / L"runs" / L"run_stop_execution";
    const std::filesystem::path artifact = run_directory / L"artifacts" / L"vm_01" / L"fixture.exe";
    std::filesystem::create_directories(artifact.parent_path());
    std::filesystem::copy_file(fixture, artifact, std::filesystem::copy_options::overwrite_existing);

    satsuma::RunManifest manifest;
    manifest.lab_id = "vm_agent_test";
    manifest.run_id = "run_stop_execution";
    manifest.request_id = "request_stop_execution";
    manifest.name = "agent-stop-execution";
    manifest.created_at = satsuma::utc_timestamp();
    manifest.artifacts.push_back({
        "vm_01",
        satsuma::path_from_utf8("artifacts/vm_01/fixture.exe"),
        satsuma::sha256_file(artifact),
    });
    satsuma::TaskStep step;
    step.id = "execute_until_stopped";
    step.vm = "vm_01";
    step.type = "execute";
    step.program = satsuma::path_from_utf8("artifacts/vm_01/fixture.exe");
    step.arguments = {
        "--ready-file", "ready.marker",
        "--sleep-ms", "30000",
        "--message", "unexpected completion",
    };
    step.timeout_seconds = 60;
    manifest.steps.push_back(std::move(step));
    satsuma::write_json_atomic(run_directory / L"task.json", nlohmann::json(manifest));
}

// 创建一个交互用户 execute 步骤，可附带后续 echo 证明 Agent 继续运行。
void write_interactive_run(
    const std::filesystem::path& shared_root,
    const std::filesystem::path& fixture,
    const std::string& run_id,
    const bool append_echo) {
    const std::filesystem::path run_directory =
        shared_root / L"runs" / satsuma::path_from_utf8(run_id);
    const std::filesystem::path artifact =
        run_directory / L"artifacts" / L"vm_01" / L"fixture.exe";
    std::filesystem::create_directories(artifact.parent_path());
    std::filesystem::copy_file(
        fixture,
        artifact,
        std::filesystem::copy_options::overwrite_existing);

    satsuma::RunManifest manifest;
    manifest.lab_id = "vm_agent_test";
    manifest.run_id = run_id;
    manifest.request_id = satsuma::make_id("request");
    manifest.name = "interactive-user-execution";
    manifest.created_at = satsuma::utc_timestamp();
    manifest.artifacts.push_back({
        "vm_01",
        satsuma::path_from_utf8("artifacts/vm_01/fixture.exe"),
        satsuma::sha256_file(artifact),
    });
    satsuma::TaskStep execute;
    execute.id = "interactive_execute";
    execute.vm = "vm_01";
    execute.type = "execute";
    execute.program = satsuma::path_from_utf8("artifacts/vm_01/fixture.exe");
    execute.arguments = {
        "--message", "interactive Agent argument with spaces",
        "--output", "collected/result.json",
        "--session-file", "collected/session.txt",
        "--identity-file", "collected/identity.txt",
    };
    execute.run_as = satsuma::TaskRunAs::InteractiveUser;
    execute.collect_files = {
        satsuma::path_from_utf8("collected/result.json"),
        satsuma::path_from_utf8("collected/session.txt"),
        satsuma::path_from_utf8("collected/identity.txt"),
    };
    manifest.steps.push_back(std::move(execute));
    if (append_echo) {
        satsuma::TaskStep echo;
        echo.id = "after_interactive_failure";
        echo.vm = "vm_01";
        echo.type = "echo";
        echo.message = "Agent continued";
        manifest.steps.push_back(std::move(echo));
    }
    satsuma::write_json_atomic(
        run_directory / L"task.json",
        nlohmann::json(manifest));
}

// 创建一个 Windows PowerShell Artifact 脚本任务。
void write_powershell_run(const std::filesystem::path& shared_root) {
    const std::filesystem::path run_directory =
        shared_root / L"runs" / L"run_powershell_script";
    const std::filesystem::path artifact =
        run_directory / L"artifacts" / L"vm_01" / L"script.ps1";
    std::filesystem::create_directories(artifact.parent_path());
    std::ofstream script(artifact, std::ios::binary);
    script
        << "param([Parameter(ValueFromRemainingArguments=$true)][string[]]$Values)\r\n"
        << "$ErrorActionPreference = 'Stop'\r\n"
        << "New-Item -ItemType Directory -Force -Path results | Out-Null\r\n"
        << "$encoding = New-Object System.Text.UTF8Encoding($false)\r\n"
        << "[IO.File]::WriteAllLines((Join-Path $PWD 'results\\script.txt'), $Values, $encoding)\r\n"
        << "$Values | ForEach-Object { Write-Output $_ }\r\n"
        << "[Console]::Error.WriteLine('script-stderr')\r\n"
        << "exit 7\r\n";
    script.close();

    satsuma::RunManifest manifest;
    manifest.lab_id = "vm_agent_test";
    manifest.run_id = "run_powershell_script";
    manifest.request_id = "request_powershell_script";
    manifest.name = "powershell-script";
    manifest.created_at = satsuma::utc_timestamp();
    manifest.artifacts.push_back({
        "vm_01",
        satsuma::path_from_utf8("artifacts/vm_01/script.ps1"),
        satsuma::sha256_file(artifact),
    });
    satsuma::TaskStep step;
    step.id = "powershell";
    step.vm = "vm_01";
    step.type = "script";
    step.engine = satsuma::ScriptEngine::WindowsPowerShell;
    step.script = satsuma::path_from_utf8("artifacts/vm_01/script.ps1");
    step.arguments = {"", "argument with spaces", "中文", "quote\"value", "C:\\tail\\"};
    step.collect_files = {satsuma::path_from_utf8("results/script.txt")};
    manifest.steps.push_back(std::move(step));
    satsuma::write_json_atomic(run_directory / L"task.json", nlohmann::json(manifest));
}

// 创建一个 CMD Artifact 脚本任务，覆盖 CMD 元字符参数。
void write_cmd_run(const std::filesystem::path& shared_root) {
    const std::filesystem::path run_directory = shared_root / L"runs" / L"run_cmd_script";
    const std::filesystem::path artifact =
        run_directory / L"artifacts" / L"vm_01" / L"script.cmd";
    std::filesystem::create_directories(artifact.parent_path());
    std::ofstream script(artifact, std::ios::binary);
    script
        << "@echo off\r\n"
        << "setlocal DisableDelayedExpansion\r\n"
        << "if not exist results mkdir results\r\n"
        << ">results\\cmd.txt <nul set /p \"=%~1\"\r\n"
        << "exit /b 9\r\n";
    script.close();

    satsuma::RunManifest manifest;
    manifest.lab_id = "vm_agent_test";
    manifest.run_id = "run_cmd_script";
    manifest.request_id = "request_cmd_script";
    manifest.name = "cmd-script";
    manifest.created_at = satsuma::utc_timestamp();
    manifest.artifacts.push_back({
        "vm_01",
        satsuma::path_from_utf8("artifacts/vm_01/script.cmd"),
        satsuma::sha256_file(artifact),
    });
    satsuma::TaskStep step;
    step.id = "cmd";
    step.vm = "vm_01";
    step.type = "script";
    step.engine = satsuma::ScriptEngine::Cmd;
    step.script = satsuma::path_from_utf8("artifacts/vm_01/script.cmd");
    step.arguments = {"percent%PATH% bang! amp& pipe| caret^"};
    step.collect_files = {satsuma::path_from_utf8("results/cmd.txt")};
    manifest.steps.push_back(std::move(step));
    satsuma::write_json_atomic(run_directory / L"task.json", nlohmann::json(manifest));
}

// 验证停止信号通过 Win32 事件立即终止 Job Object。
void test_process_runner_cancellation(
    const std::filesystem::path& root,
    const std::filesystem::path& fixture) {
    std::filesystem::create_directories(root);
    satsuma::vm::ProcessRequest request;
    request.program = fixture;
    const std::filesystem::path ready_path = root / L"ready.marker";
    request.arguments = {
        "--ready-file", satsuma::path_to_utf8(ready_path),
        "--sleep-ms", "30000",
        "--message", "unexpected completion",
    };
    request.working_directory = root;
    request.stdout_path = root / L"stdout.log";
    request.stderr_path = root / L"stderr.log";
    request.timeout = 60s;

    std::stop_source stop_source;
    request.stop_token = stop_source.get_token();
    bool cancelled = false;  // 工作线程是否观察到稳定取消错误
    std::exception_ptr worker_error;
    std::thread worker([&] {
        try {
            static_cast<void>(satsuma::vm::ProcessRunner().run(request));
        } catch (const satsuma::Error& error) {
            cancelled = std::string(error.what()) == "Agent stop requested";
            if (!cancelled) {
                worker_error = std::current_exception();
            }
        } catch (...) {
            worker_error = std::current_exception();
        }
    });

    const bool process_started = wait_for_file(ready_path, 5s);
    const auto stop_started = std::chrono::steady_clock::now();
    stop_source.request_stop();
    worker.join();
    const auto stop_duration = std::chrono::steady_clock::now() - stop_started;
    if (worker_error != nullptr) {
        std::rethrow_exception(worker_error);
    }
    expect(process_started, "ProcessRunner did not start the fixture process");
    expect(cancelled, "ProcessRunner did not return the stable cancellation error");
    expect(stop_duration < 5s, "ProcessRunner cancellation exceeded five seconds");
}

// 验证失控日志会终止完整进程树，而不是持续耗尽磁盘。
void test_process_runner_output_limit(
    const std::filesystem::path& root,
    const std::filesystem::path& fixture) {
    std::filesystem::create_directories(root);
    satsuma::vm::ProcessRequest request;
    request.program = fixture;
    request.arguments = {"--stdout-bytes", "1048576"};
    request.working_directory = root;
    request.stdout_path = root / L"stdout.log";
    request.stderr_path = root / L"stderr.log";
    request.timeout = 5s;
    request.max_output_bytes = 64 * 1024;

    const satsuma::vm::ProcessResult result = satsuma::vm::ProcessRunner{}.run(request);
    expect(result.output_limit_exceeded, "ProcessRunner accepted output above its limit");
    expect(!result.timed_out, "output limit was reported as a timeout");
}

// 验证 watch 模式不依赖 Host，并在停止时生成失败执行证据。
void test_file_watch_and_agent_stop(
    const std::filesystem::path& root,
    const std::filesystem::path& fixture) {
    const std::filesystem::path shared_root = root / L"share";
    write_echo_run(shared_root);
    write_cancellable_run(shared_root, fixture);

    satsuma::AgentConfig config;
    config.lab_id = "vm_agent_test";
    config.vm_id = "vm_01";
    config.agent_version = "0.1.0";
    config.shared_root = shared_root;
    config.local_work_root = root / L"work";
    config.poll_interval_ms = 30'000;
    config.reconnect_interval_ms = 30'000;

    satsuma::vm::Agent agent(std::move(config));
    std::stop_source stop_source;
    std::exception_ptr worker_error;
    std::thread worker([&] {
        try {
            agent.run_watch(stop_source.get_token());
        } catch (...) {
            worker_error = std::current_exception();
        }
    });

    const std::filesystem::path echo_result =
        shared_root / L"runs" / L"run_file_success" / L"results" / L"vm_01" /
        L"echo_success" / L"execution.json";
    const std::filesystem::path execute_ready =
        root / L"work" / L"vm_agent_test" / L"run_stop_execution" / L"vm_01" /
        L"ready.marker";
    const bool echo_completed = wait_for_file(echo_result, 5s);
    const bool execute_started = wait_for_file(execute_ready, 5s);
    const auto stop_started = std::chrono::steady_clock::now();
    stop_source.request_stop();
    worker.join();
    const auto stop_duration = std::chrono::steady_clock::now() - stop_started;
    if (worker_error != nullptr) {
        std::rethrow_exception(worker_error);
    }

    expect(echo_completed, "Agent did not complete a file task without a Host control channel");
    expect(execute_started, "Agent did not start the cancellable file task");
    expect(stop_duration < 5s, "Agent stop exceeded five seconds");
    expect(
        std::filesystem::is_regular_file(shared_root / L"agents" / L"vm_01.json"),
        "Agent did not publish presence through the file channel");
    const nlohmann::json presence = satsuma::load_json(
        shared_root / L"agents" / L"vm_01.json");
    expect(
        presence.value("agent_version", std::string{}) == "0.1.0" &&
            presence.value("update_id", std::string{}).empty() &&
            presence.value("binary_sha256", std::string{}).size() == 64 &&
            presence.at("runtime").value("started_at", std::string{}).size() > 10 &&
            presence.at("runtime").value("file_channel_failure_count", 1) == 0 &&
            !presence.at("runtime").contains("last_file_channel_error_at") &&
            !presence.at("runtime").contains("last_file_channel_recovered_at") &&
            presence.at("inventory").value("sha256", std::string{}).size() == 64,
        "Agent presence did not publish its build and runtime identity");
    expect(
        std::filesystem::is_regular_file(shared_root / L"agents" / L"vm_01.inventory.json"),
        "Agent did not publish its environment inventory");

    const satsuma::ExecutionResult echo =
        satsuma::load_json(echo_result).get<satsuma::ExecutionResult>();
    expect(echo.status == "exited" && echo.exit_code == 0, "file-only echo task did not succeed");

    const std::filesystem::path stopped_result =
        shared_root / L"runs" / L"run_stop_execution" / L"results" / L"vm_01" /
        L"execute_until_stopped" / L"execution.json";
    expect(std::filesystem::is_regular_file(stopped_result), "Agent stop did not publish execution.json");
    const satsuma::ExecutionResult stopped =
        satsuma::load_json(stopped_result).get<satsuma::ExecutionResult>();
    expect(stopped.status == "failed", "Agent stop did not mark the step as failed");
    expect(stopped.run_as == satsuma::TaskRunAs::System,
        "default execute step did not retain the SYSTEM identity");
    expect(!stopped.timed_out, "Agent stop was incorrectly recorded as a timeout");
    expect(stopped.error == "Agent stop requested", "Agent stop did not preserve the stable error text");
}

// 验证共享目录恢复后，Agent 保留累计故障并清零连续故障状态。
void test_file_channel_runtime_recovery(const std::filesystem::path& root) {
    std::filesystem::create_directories(root);
    const std::filesystem::path shared_root = root / L"share";
    std::ofstream blocker(shared_root, std::ios::binary);
    blocker << "shared folder unavailable";
    blocker.close();

    satsuma::AgentConfig config;
    config.lab_id = "vm_agent_test";
    config.vm_id = "vm_01";
    config.agent_version = "0.1.0";
    config.shared_root = shared_root;
    config.local_work_root = root / L"work";
    config.poll_interval_ms = 20;
    config.reconnect_interval_ms = 20;

    satsuma::vm::Agent agent(std::move(config));
    std::stop_source stop_source;
    std::exception_ptr worker_error;
    std::thread worker([&] {
        try {
            agent.run_watch(stop_source.get_token());
        } catch (...) {
            worker_error = std::current_exception();
        }
    });

    std::this_thread::sleep_for(250ms);
    std::error_code recovery_error;
    const bool blocker_removed = std::filesystem::remove(shared_root, recovery_error);
    if (blocker_removed) {
        std::filesystem::create_directories(shared_root, recovery_error);
    }

    bool recovered = false;
    nlohmann::json presence;
    const std::filesystem::path presence_path = shared_root / L"agents" / L"vm_01.json";
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!recovery_error && std::chrono::steady_clock::now() < deadline) {
        try {
            if (std::filesystem::is_regular_file(presence_path)) {
                presence = satsuma::load_json(presence_path);
                const nlohmann::json& runtime = presence.at("runtime");
                recovered = runtime.value("file_channel_failure_count", 0) > 0 &&
                    runtime.value("consecutive_file_channel_failures", 1) == 0 &&
                    !runtime.value("last_file_channel_recovered_at", std::string{}).empty();
                if (recovered) {
                    break;
                }
            }
        } catch (const std::exception&) {
        }
        std::this_thread::sleep_for(20ms);
    }

    stop_source.request_stop();
    worker.join();
    if (worker_error != nullptr) {
        std::rethrow_exception(worker_error);
    }
    expect(blocker_removed && !recovery_error, "test could not restore the shared folder");
    expect(recovered, "Agent did not publish recovered file-channel runtime state");
    const nlohmann::json& runtime = presence.at("runtime");
    expect(
        !runtime.value("last_file_channel_error", std::string{}).empty() &&
            !runtime.value("last_file_channel_error_at", std::string{}).empty(),
        "Agent recovery presence omitted the previous file-channel failure");
}

// 验证清单在会话内缓存、自愈，并只响应显式刷新重新采集。
void test_inventory_cache_and_refresh(const std::filesystem::path& root) {
    satsuma::AgentConfig config;
    config.lab_id = "vm_agent_test";
    config.vm_id = "vm_01";
    config.shared_root = root / L"inventory-share";
    satsuma::vm::InventoryPublisher publisher(config, "boot_inventory_test");
    publisher.synchronize();

    const std::filesystem::path inventory_path =
        config.shared_root / L"agents" / L"vm_01.inventory.json";
    const nlohmann::json original = satsuma::load_json(inventory_path);
    const std::string original_digest = publisher.digest();
    satsuma::vm::InventoryPublisher restarted(config, "boot_inventory_restart");
    restarted.synchronize();
    expect(
        satsuma::load_json(inventory_path) == original &&
            restarted.digest() == original_digest,
        "Agent rewrote an unchanged inventory during Service restart");

    satsuma::write_json_atomic(inventory_path, {{"tampered", true}});
    publisher.synchronize();
    expect(
        satsuma::load_json(inventory_path) == original && publisher.digest() == original_digest,
        "Agent inventory cache did not restore the original snapshot");

    satsuma::write_json_atomic(
        config.shared_root / L"agents" / L"vm_01.inventory-refresh.json",
        {
            {"schema_version", 1},
            {"lab_id", config.lab_id},
            {"vm_id", config.vm_id},
            {"hardware_id", config.vm_id},
            {"request_id", "inventory_refresh_test"},
        });
    publisher.synchronize();
    const nlohmann::json refreshed = satsuma::load_json(inventory_path);
    expect(
        refreshed.value("refresh_request_id", std::string{}) == "inventory_refresh_test",
        "Agent ignored an explicit inventory refresh request");
}

// 验证硬件绑定后 Agent 在同一进程内立即重建新 VM 身份的清单。
void test_inventory_rebind_without_restart(const std::filesystem::path& root) {
    satsuma::AgentConfig config;
    config.lab_id = "vm_agent_test";
    config.vm_id = "564d1234-abcd-4321-9876-001122334455";
    config.hardware_id = config.vm_id;
    config.identity_unbound = true;
    config.shared_root = root / L"share";
    config.local_work_root = root / L"work";
    std::filesystem::create_directories(config.shared_root / L"agents");
    satsuma::write_json_atomic(
        config.shared_root / L"agents" /
            L"564d1234-abcd-4321-9876-001122334455.binding.json",
        {
            {"schema_version", 1},
            {"lab_id", config.lab_id},
            {"hardware_id", config.hardware_id},
            {"vm_id", "vm_01"},
        });

    satsuma::vm::Agent agent(std::move(config));
    static_cast<void>(agent.run_once());

    const nlohmann::json inventory = satsuma::load_json(
        root / L"share" / L"agents" /
            L"564d1234-abcd-4321-9876-001122334455.inventory.json");
    const nlohmann::json presence = satsuma::load_json(
        root / L"share" / L"agents" /
            L"564d1234-abcd-4321-9876-001122334455.json");
    expect(
        inventory.value("vm_id", std::string{}) == "vm_01" &&
            inventory.value("hardware_id", std::string{}) ==
                "564d1234-abcd-4321-9876-001122334455" &&
            presence.value("vm_id", std::string{}) == "vm_01",
        "Agent binding did not update inventory and presence in the same process");
}

// 验证 script 复用进程树、日志、退出码和精确文件收集。
void test_powershell_script_execution(const std::filesystem::path& root) {
    const std::filesystem::path shared_root = root / L"share";
    write_powershell_run(shared_root);
    satsuma::AgentConfig config;
    config.lab_id = "vm_agent_test";
    config.vm_id = "vm_01";
    config.shared_root = shared_root;
    config.local_work_root = root / L"work";
    satsuma::vm::Agent agent(std::move(config));
    expect(agent.run_once() == 1, "Agent did not execute the PowerShell script step");

    const std::filesystem::path result_root = shared_root / L"runs" /
        L"run_powershell_script" / L"results" / L"vm_01" / L"powershell";
    const satsuma::ExecutionResult result =
        satsuma::load_json(result_root / L"execution.json").get<satsuma::ExecutionResult>();
    expect(
        result.status == "exited" && result.exit_code == 7 && result.files.size() == 1,
        "PowerShell script did not preserve its exit code or collected file");
    expect(
        read_text(result_root / L"files" / L"results" / L"script.txt") ==
            "\r\nargument with spaces\r\n中文\r\nquote\"value\r\nC:\\tail\\\r\n",
        "PowerShell script argument changed during execution");

    const std::filesystem::path local_run_directory =
        root / L"work" / L"vm_agent_test" / L"run_powershell_script" / L"vm_01";
    expect(std::filesystem::is_directory(local_run_directory),
        "Agent deleted Guest work before an explicit cleanup request");
    const std::filesystem::path state_directory =
        shared_root / L"runs" / L"run_powershell_script" / L"state";
    satsuma::write_json_atomic(state_directory / L"vm_01-cleanup-request.json", {
        {"schema_version", 1},
        {"lab_id", "vm_agent_test"},
        {"run_id", "run_powershell_script"},
        {"vm_id", "vm_01"},
        {"request_id", "cleanup_test"},
        {"target", "guest_work"},
        {"requested_at", "2026-07-29T00:00:00.000Z"},
    });
    expect(agent.run_once() == 0, "Agent re-executed a completed step during Guest cleanup");
    expect(!std::filesystem::exists(local_run_directory),
        "Agent did not delete Guest work after the cleanup request");
    const nlohmann::json cleanup = satsuma::load_json(state_directory / L"vm_01-cleanup.json");
    expect(
        cleanup.at("status") == "deleted" && cleanup.at("request_id") == "cleanup_test" &&
            cleanup.at("failed_path_count") == 0,
        "Agent Guest cleanup result is incomplete");
}

// 验证 CMD 固定启动模板不会解释任务参数中的元字符。
void test_cmd_script_execution(const std::filesystem::path& root) {
    const std::filesystem::path shared_root = root / L"share";
    write_cmd_run(shared_root);
    satsuma::AgentConfig config;
    config.lab_id = "vm_agent_test";
    config.vm_id = "vm_01";
    config.shared_root = shared_root;
    config.local_work_root = root / L"work";
    satsuma::vm::Agent agent(std::move(config));
    expect(agent.run_once() == 1, "Agent did not execute the CMD script step");

    const std::filesystem::path result_root = shared_root / L"runs" / L"run_cmd_script" /
        L"results" / L"vm_01" / L"cmd";
    const satsuma::ExecutionResult result =
        satsuma::load_json(result_root / L"execution.json").get<satsuma::ExecutionResult>();
    expect(
        result.status == "exited" && result.exit_code == 9 && result.files.size() == 1,
        "CMD script did not preserve its exit code or collected file: status=" + result.status +
            ", error=" + result.error + ", stdout=" + read_text(result_root / L"stdout.log") +
            ", stderr=" + read_text(result_root / L"stderr.log"));
    const std::string actual = read_text(
        result_root / L"files" / L"results" / L"cmd.txt");
    expect(
        actual == "percent%PATH% bang! amp& pipe| caret^",
        "CMD script argument metacharacters were interpreted: " + actual);
}

// 验证 Host 文件取消请求会终止当前 Job Object 并生成稳定结果。
void test_file_cancellation(
    const std::filesystem::path& root,
    const std::filesystem::path& fixture) {
    const std::filesystem::path shared_root = root / L"share";
    write_cancellable_run(shared_root, fixture);
    satsuma::AgentConfig config;
    config.lab_id = "vm_agent_test";
    config.vm_id = "vm_01";
    config.agent_version = "0.1.0";
    config.shared_root = shared_root;
    config.local_work_root = root / L"work";
    config.poll_interval_ms = 30'000;
    config.reconnect_interval_ms = 30'000;

    satsuma::vm::Agent agent(std::move(config));
    std::stop_source stop_source;
    std::thread worker([&] { agent.run_watch(stop_source.get_token()); });
    const std::filesystem::path ready =
        root / L"work" / L"vm_agent_test" / L"run_stop_execution" / L"vm_01" /
        L"ready.marker";
    expect(wait_for_file(ready, 5s), "cancellable task did not start");
    const std::filesystem::path run_directory =
        shared_root / L"runs" / L"run_stop_execution";
    satsuma::write_json_atomic(run_directory / L"cancel.json", {
        {"schema_version", 1},
        {"run_id", "run_stop_execution"},
        {"reason", "test cancellation"},
    });
    const std::filesystem::path result_path =
        run_directory / L"results" / L"vm_01" / L"execute_until_stopped" / L"execution.json";
    const bool completed = wait_for_file(result_path, 5s);
    stop_source.request_stop();
    worker.join();
    expect(completed, "file cancellation did not publish execution.json");
    const satsuma::ExecutionResult result =
        satsuma::load_json(result_path).get<satsuma::ExecutionResult>();
    expect(
        result.status == "failed" && result.error == "Run cancellation requested",
        "file cancellation did not preserve its stable result");
}

// 验证 Agent 通过用户 helper 执行、收集文件并记录真实 Session。
void test_agent_interactive_execution(
    const std::filesystem::path& root,
    const std::filesystem::path& fixture,
    const std::filesystem::path& helper) {
    const std::filesystem::path shared_root = root / L"share";
    const std::string run_id = "run_interactive_success";
    write_interactive_run(shared_root, fixture, run_id, false);

    satsuma::AgentConfig config;
    config.lab_id = "vm_agent_test";
    config.vm_id = "vm_01";
    config.agent_version = "0.1.0";
    config.shared_root = shared_root;
    config.local_work_root = root / L"system-work";
    satsuma::vm::Agent agent(config, helper);
    expect(agent.run_once() == 1, "Agent did not claim the interactive step");

    const std::filesystem::path result_path =
        shared_root / L"runs" / satsuma::path_from_utf8(run_id) /
        L"results" / L"vm_01" / L"interactive_execute" / L"execution.json";
    const satsuma::ExecutionResult result =
        satsuma::load_json(result_path).get<satsuma::ExecutionResult>();
    if (result.status == "failed" &&
        result.error == satsuma::vm::kNoInteractiveUserSessionError) {
        std::cout << "Agent interactive execution skipped: " << result.error << '\n';
        return;
    }
    expect(result.status == "exited" && result.exit_code == 0,
        "Agent interactive step did not exit successfully");
    expect(result.run_as == satsuma::TaskRunAs::InteractiveUser &&
           result.interactive_session_id.has_value(),
        "Agent execution result did not record the interactive identity");
    expect(result.files.size() == 3,
        "Agent did not collect all interactive result files");

    const std::filesystem::path result_directory = result_path.parent_path();
    const std::string stdout_text = read_text(result_directory / L"stdout.log");
    expect(stdout_text.find("interactive Agent argument with spaces") != std::string::npos,
        "Agent interactive stdout or argument boundaries changed");
    const std::filesystem::path collected_session =
        result_directory / L"files" / L"collected" / L"session.txt";
    expect(std::stoul(read_text(collected_session)) == *result.interactive_session_id,
        "Agent collected a Session ID different from execution.json");
    const std::filesystem::path collected_identity =
        result_directory / L"files" / L"collected" / L"identity.txt";
    const std::string identity_text = read_text(collected_identity);
    expect(identity_text == "user\r\n" || identity_text == "user\n",
        "Agent interactive target unexpectedly ran as LocalSystem");

    try {
        satsuma::vm::InteractiveUserSession cleanup =
            satsuma::vm::InteractiveUserSession::acquire(config.lab_id, run_id);
        std::error_code cleanup_error;
        std::filesystem::remove_all(cleanup.working_directory(), cleanup_error);
    } catch (...) {
    }
}

// 验证无活动用户生成失败终态且不阻塞后续 SYSTEM 步骤。
void test_agent_no_interactive_session(
    const std::filesystem::path& root,
    const std::filesystem::path& fixture,
    const std::filesystem::path& helper) {
    const std::filesystem::path shared_root = root / L"share";
    const std::string run_id = "run_no_interactive_session";
    write_interactive_run(shared_root, fixture, run_id, true);

    satsuma::AgentConfig config;
    config.lab_id = "vm_agent_test";
    config.vm_id = "vm_01";
    config.agent_version = "0.1.0";
    config.shared_root = shared_root;
    config.local_work_root = root / L"system-work";
    satsuma::vm::Agent agent(std::move(config), helper);

    satsuma::vm::set_interactive_session_unavailable_for_test(true);
    int executed = 0;
    try {
        executed = agent.run_once();
        satsuma::vm::set_interactive_session_unavailable_for_test(false);
    } catch (...) {
        satsuma::vm::set_interactive_session_unavailable_for_test(false);
        throw;
    }
    expect(executed == 2, "Agent did not continue after the interactive identity failure");

    const std::filesystem::path results =
        shared_root / L"runs" / satsuma::path_from_utf8(run_id) /
        L"results" / L"vm_01";
    const satsuma::ExecutionResult failed = satsuma::load_json(
        results / L"interactive_execute" / L"execution.json")
            .get<satsuma::ExecutionResult>();
    expect(failed.status == "failed" &&
           failed.error == satsuma::vm::kNoInteractiveUserSessionError,
        "no-session step did not preserve the stable failure");
    expect(failed.run_as == satsuma::TaskRunAs::InteractiveUser &&
           !failed.interactive_session_id.has_value(),
        "no-session result reported an identity that was not acquired");
    expect(std::filesystem::is_regular_file(
        results / L"interactive_execute" / L"stdout.log"),
        "no-session failure did not publish an empty stdout log");
    expect(read_text(results / L"interactive_execute" / L"stdout.log").empty() &&
           read_text(results / L"interactive_execute" / L"stderr.log").empty(),
        "no-session failure published unexpected process logs");
    const satsuma::ExecutionResult continued = satsuma::load_json(
        results / L"after_interactive_failure" / L"execution.json")
            .get<satsuma::ExecutionResult>();
    expect(continued.status == "exited" && continued.exit_code == 0,
        "Agent did not execute the SYSTEM step after a no-session failure");
}

// 验证一个损坏运行不会阻塞按名称排在它之后的合法任务。
void test_invalid_run_is_isolated(const std::filesystem::path& root) {
    const std::filesystem::path shared_root = root / L"share";
    const std::filesystem::path invalid_run = shared_root / L"runs" / L"a-invalid-run";
    std::filesystem::create_directories(invalid_run);
    std::ofstream(invalid_run / L"task.json", std::ios::binary) << "{invalid json";
    write_echo_run(shared_root);

    satsuma::AgentConfig config;
    config.lab_id = "vm_agent_test";
    config.vm_id = "vm_01";
    config.agent_version = "0.1.0";
    config.shared_root = shared_root;
    config.local_work_root = root / L"work";
    satsuma::vm::Agent agent(std::move(config));

    expect(agent.run_once() == 1, "invalid run blocked a later valid run");
    expect(
        std::filesystem::is_regular_file(
            invalid_run / L"state" / L"vm_01-agent-error.json"),
        "invalid run did not receive a stable Agent error record");
    expect(
        std::filesystem::is_regular_file(
            shared_root / L"runs" / L"run_file_success" / L"results" /
            L"vm_01" / L"echo_success" / L"execution.json"),
        "valid run after an invalid run did not complete");
}

// 验证兼容读取的 v1 配置不能构造生产 Agent。
void test_agent_rejects_legacy_file_protocol(const std::filesystem::path& root) {
    satsuma::AgentConfig config;
    config.protocol_version = satsuma::kLegacyRunManifestProtocolVersion;
    config.lab_id = "vm_agent_test";
    config.vm_id = "vm_01";
    config.agent_version = "0.1.0";
    config.shared_root = root / L"share";
    config.local_work_root = root / L"work";
    bool rejected = false;
    try {
        satsuma::vm::Agent agent(std::move(config));
    } catch (const satsuma::Error& error) {
        rejected = std::string(error.what()) ==
            "Agent execution requires the current file protocol version";
    }
    expect(rejected, "production Agent accepted a legacy v1 file protocol config");
}

// 验证首次发现、Host 绑定、本地回退和硬件变化迁移记录。
void test_agent_hardware_identity(const std::filesystem::path& root) {
    constexpr char first_hardware[] = "564d1234-abcd-4321-9876-001122334455";
    constexpr char second_hardware[] = "564d5678-abcd-4321-9876-001122334455";
    satsuma::AgentConfig config;
    config.lab_id = "vm_agent_test";
    config.agent_version = "0.1.0";
    config.shared_root = root / L"share";
    config.local_work_root = root / L"work";
    satsuma::vm::prepare_agent_hardware_identity(config, first_hardware);
    expect(
        config.identity_unbound && config.vm_id == first_hardware &&
            config.hardware_id == first_hardware,
        "new hardware did not enter unbound discovery mode");

    satsuma::write_json_atomic(
        config.shared_root / L"agents" / L"564d1234-abcd-4321-9876-001122334455.binding.json",
        {
            {"schema_version", 1},
            {"lab_id", config.lab_id},
            {"hardware_id", first_hardware},
            {"vm_id", "vm_01"},
        });
    expect(
        satsuma::vm::refresh_agent_binding(config) &&
            !config.identity_unbound && config.vm_id == "vm_01",
        "Host hardware binding was not applied without rewriting agent.json");

    const std::filesystem::path binding_path =
        config.shared_root / L"agents" /
        L"564d1234-abcd-4321-9876-001122334455.binding.json";
    std::filesystem::remove(binding_path);
    satsuma::AgentConfig restarted;
    restarted.lab_id = config.lab_id;
    restarted.agent_version = config.agent_version;
    restarted.shared_root = config.shared_root;
    restarted.local_work_root = config.local_work_root;
    satsuma::vm::prepare_agent_hardware_identity(restarted, first_hardware);
    expect(
        !restarted.identity_unbound && restarted.vm_id == "vm_01" &&
            !restarted.vm_id_configured,
        "Agent restart did not recover the Host-confirmed VM identity from local cache");

    satsuma::write_json_atomic(binding_path, {
        {"schema_version", 1},
        {"lab_id", restarted.lab_id},
        {"hardware_id", first_hardware},
        {"vm_id", "vm_03"},
    });
    expect(
        satsuma::vm::refresh_agent_binding(restarted) && restarted.vm_id == "vm_03",
        "Restored Host binding did not replace the locally cached VM identity");

    satsuma::AgentConfig migrated;
    migrated.lab_id = config.lab_id;
    migrated.agent_version = config.agent_version;
    migrated.shared_root = config.shared_root;
    migrated.local_work_root = config.local_work_root;
    satsuma::vm::prepare_agent_hardware_identity(migrated, second_hardware);
    expect(
        migrated.identity_unbound && migrated.previous_hardware_id == first_hardware &&
            migrated.previous_vm_id == "vm_03",
        "hardware change did not preserve the previous identity evidence");
    satsuma::vm::write_hardware_migration_marker(migrated);
    expect(
        std::filesystem::is_regular_file(
            migrated.shared_root / L"agents" /
                L"564d1234-abcd-4321-9876-001122334455.migrated.json"),
        "hardware change did not publish its migration marker");

    satsuma::AgentConfig legacy = migrated;
    legacy.vm_id = "vm_02";
    legacy.vm_id_configured = true;
    satsuma::vm::prepare_agent_hardware_identity(
        legacy,
        "564d9999-abcd-4321-9876-001122334455");
    expect(
        !legacy.identity_unbound && legacy.vm_id == "vm_02",
        "legacy configured vm_id did not retain priority");
}

}  // namespace

// 运行文件通道和取消测试并清理专用临时目录。
int main(const int argc, char* argv[]) {
    if (argc != 3) {
        std::cerr << "Usage: SatsumaVmAgentTests <fixture.exe> <SatsumaVM.exe>\n";
        return 2;
    }
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        satsuma::path_from_utf8(satsuma::make_id("vm-agent-test"));
    try {
        const std::filesystem::path fixture = std::filesystem::absolute(argv[1]);
        const std::filesystem::path helper = std::filesystem::absolute(argv[2]);
        test_process_runner_cancellation(root / L"process-runner", fixture);
        test_process_runner_output_limit(root / L"process-output-limit", fixture);
        test_file_watch_and_agent_stop(root / L"agent-watch", fixture);
        test_file_channel_runtime_recovery(root / L"file-channel-recovery");
        test_inventory_cache_and_refresh(root / L"inventory");
        test_inventory_rebind_without_restart(root / L"inventory-rebind");
        test_powershell_script_execution(root / L"powershell-script");
        test_cmd_script_execution(root / L"cmd-script");
        test_file_cancellation(root / L"file-cancellation", fixture);
        test_agent_interactive_execution(
            root / L"interactive-agent",
            fixture,
            helper);
        test_agent_no_interactive_session(
            root / L"no-interactive-session",
            fixture,
            helper);
        test_invalid_run_is_isolated(root / L"invalid-run-isolation");
        test_agent_rejects_legacy_file_protocol(root / L"legacy-protocol");
        test_agent_hardware_identity(root / L"hardware-identity");
        std::filesystem::remove_all(root);
        std::cout << "SatsumaVmAgentTests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "SatsumaVmAgentTests failed: " << error.what() << '\n';
        std::error_code cleanup_error;
        std::filesystem::remove_all(root, cleanup_error);
        return 1;
    }
}
