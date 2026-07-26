// VM Agent 文件通道和进程取消测试。
#include <chrono>
#include <exception>
#include <filesystem>
#include <iostream>
#include <stop_token>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

#include <nlohmann/json.hpp>

#include "agent.hpp"
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
    step.vm = "client";
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
    const std::filesystem::path artifact = run_directory / L"artifacts" / L"client" / L"fixture.exe";
    std::filesystem::create_directories(artifact.parent_path());
    std::filesystem::copy_file(fixture, artifact, std::filesystem::copy_options::overwrite_existing);

    satsuma::RunManifest manifest;
    manifest.lab_id = "vm_agent_test";
    manifest.run_id = "run_stop_execution";
    manifest.request_id = "request_stop_execution";
    manifest.name = "agent-stop-execution";
    manifest.created_at = satsuma::utc_timestamp();
    manifest.artifacts.push_back({
        "client",
        satsuma::path_from_utf8("artifacts/client/fixture.exe"),
        satsuma::sha256_file(artifact),
    });
    satsuma::TaskStep step;
    step.id = "execute_until_stopped";
    step.vm = "client";
    step.type = "execute";
    step.program = satsuma::path_from_utf8("artifacts/client/fixture.exe");
    step.arguments = {
        "--ready-file", "ready.marker",
        "--sleep-ms", "30000",
        "--message", "unexpected completion",
    };
    step.timeout_seconds = 60;
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

// 验证 watch 模式不依赖 Host，并在停止时生成失败执行证据。
void test_file_watch_and_agent_stop(
    const std::filesystem::path& root,
    const std::filesystem::path& fixture) {
    const std::filesystem::path shared_root = root / L"share";
    write_echo_run(shared_root);
    write_cancellable_run(shared_root, fixture);

    satsuma::AgentConfig config;
    config.lab_id = "vm_agent_test";
    config.vm_id = "client";
    config.agent_version = "0.1.0";
    config.host = "192.0.2.1:9";
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
        shared_root / L"runs" / L"run_file_success" / L"results" / L"client" /
        L"echo_success" / L"execution.json";
    const std::filesystem::path execute_ready =
        root / L"work" / L"run_stop_execution" / L"ready.marker";
    const bool echo_completed = wait_for_file(echo_result, 5s);
    const bool execute_started = wait_for_file(execute_ready, 5s);
    const auto stop_started = std::chrono::steady_clock::now();
    stop_source.request_stop();
    worker.join();
    const auto stop_duration = std::chrono::steady_clock::now() - stop_started;
    if (worker_error != nullptr) {
        std::rethrow_exception(worker_error);
    }

    expect(echo_completed, "Agent did not complete a file task without Host RPC");
    expect(execute_started, "Agent did not start the cancellable file task");
    expect(stop_duration < 5s, "Agent stop exceeded five seconds");
    expect(
        std::filesystem::is_regular_file(shared_root / L"agents" / L"client.json"),
        "Agent did not publish presence through the file channel");

    const satsuma::ExecutionResult echo =
        satsuma::load_json(echo_result).get<satsuma::ExecutionResult>();
    expect(echo.status == "exited" && echo.exit_code == 0, "file-only echo task did not succeed");

    const std::filesystem::path stopped_result =
        shared_root / L"runs" / L"run_stop_execution" / L"results" / L"client" /
        L"execute_until_stopped" / L"execution.json";
    expect(std::filesystem::is_regular_file(stopped_result), "Agent stop did not publish execution.json");
    const satsuma::ExecutionResult stopped =
        satsuma::load_json(stopped_result).get<satsuma::ExecutionResult>();
    expect(stopped.status == "failed", "Agent stop did not mark the step as failed");
    expect(!stopped.timed_out, "Agent stop was incorrectly recorded as a timeout");
    expect(stopped.error == "Agent stop requested", "Agent stop did not preserve the stable error text");
}

}  // namespace

// 运行文件通道和取消测试并清理专用临时目录。
int main(const int argc, char* argv[]) {
    if (argc != 2) {
        std::cerr << "Usage: SatsumaVmAgentTests <fixture.exe>\n";
        return 2;
    }
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        satsuma::path_from_utf8(satsuma::make_id("vm-agent-test"));
    try {
        const std::filesystem::path fixture = std::filesystem::absolute(argv[1]);
        test_process_runner_cancellation(root / L"process-runner", fixture);
        test_file_watch_and_agent_stop(root / L"agent-watch", fixture);
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
