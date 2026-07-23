// Host RPC 会话和任务选择业务测试。
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

#include "rpc_service.hpp"
#include "satsuma/core/id.hpp"
#include "satsuma/core/json_io.hpp"
#include "satsuma/core/path.hpp"
#include "satsuma/core/task.hpp"

namespace {

// 在条件不满足时终止当前测试。
void expect(const bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

// 创建只包含一台 Client VM 的本地实验室配置。
[[nodiscard]] satsuma::LabConfig make_lab_config(const std::filesystem::path& root) {
    satsuma::LabConfig config;
    config.lab_id = "rpc_test_lab";
    config.shared_folder.host_root = root / L"share";
    satsuma::VmConfig vm;
    vm.id = "client";
    vm.agent_version = "0.1.0";
    config.vms.push_back(std::move(vm));
    return config;
}

// 创建可复用于注册、心跳和轮询请求的 Agent 身份。
[[nodiscard]] satsuma::AgentHello make_hello() {
    satsuma::AgentHello hello;
    hello.lab_id = "rpc_test_lab";
    hello.vm_id = "client";
    hello.session_id = "session_1";
    hello.boot_id = "boot_1";
    hello.request_id = "request_1";
    hello.agent_version = "0.1.0";
    return hello;
}

// 在共享目录写入一条 Client echo 任务。
void write_task(const std::filesystem::path& shared_root) {
    satsuma::RunManifest manifest;
    manifest.lab_id = "rpc_test_lab";
    manifest.run_id = "rpc_run";
    manifest.request_id = "request_task";
    manifest.name = "rpc task";
    manifest.created_at = satsuma::utc_timestamp();
    satsuma::TaskStep step;
    step.id = "echo_client";
    step.vm = "client";
    step.type = "echo";
    step.message = "hello";
    manifest.steps.push_back(std::move(step));
    satsuma::write_json_atomic(shared_root / L"runs" / L"rpc_run" / L"task.json", manifest);
}

// 验证注册、心跳、轮询、回报和错误会话。
void test_rpc_service(const std::filesystem::path& root) {
    satsuma::LabConfig config = make_lab_config(root);
    const std::filesystem::path shared_root = config.shared_folder.host_root;
    write_task(shared_root);
    satsuma::host::RpcService service(std::move(config));

    const satsuma::AgentHello hello = make_hello();
    const satsuma::SessionInfo session = service.register_agent(hello);
    expect(session.accepted, "valid Agent registration was rejected");
    expect(service.session_count() == 1, "Agent session was not stored");

    satsuma::AgentStatus heartbeat;
    heartbeat.lab_id = hello.lab_id;
    heartbeat.vm_id = hello.vm_id;
    heartbeat.session_id = hello.session_id;
    heartbeat.boot_id = hello.boot_id;
    heartbeat.request_id = "request_heartbeat";
    heartbeat.status = "idle";
    expect(service.heartbeat(heartbeat).action == "poll", "registered Agent did not receive poll directive");

    satsuma::PollRequest poll;
    poll.lab_id = hello.lab_id;
    poll.vm_id = hello.vm_id;
    poll.session_id = hello.session_id;
    poll.boot_id = hello.boot_id;
    poll.request_id = "request_poll";
    const satsuma::TaskReference task = service.poll_task(poll);
    expect(task.has_task && task.run_id == "rpc_run", "pending task was not returned to Agent");
    expect(task.manifest == "runs/rpc_run/task.json", "task manifest path is not portable");

    satsuma::JobStatus job;
    job.lab_id = hello.lab_id;
    job.vm_id = hello.vm_id;
    job.session_id = hello.session_id;
    job.boot_id = hello.boot_id;
    job.request_id = "request_report";
    job.run_id = "rpc_run";
    job.job_id = "job_1";
    job.step_id = "echo_client";
    job.status = "exited";
    job.has_exit_code = true;
    expect(service.report_job(job).accepted, "valid Job report was rejected");

    heartbeat.session_id = "stale_session";
    expect(service.heartbeat(heartbeat).action == "stop", "stale Agent session was not stopped");
}

}  // namespace

// 运行 Host RPC 业务测试并清理本次专用临时目录。
int main() {
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / satsuma::path_from_utf8(satsuma::make_id("host-rpc-test"));
    try {
        test_rpc_service(root);
        std::filesystem::remove_all(root);
        std::cout << "SatsumaHostRpcServiceTests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "SatsumaHostRpcServiceTests failed: " << error.what() << '\n';
        std::error_code cleanup_error;
        std::filesystem::remove_all(root, cleanup_error);
        return 1;
    }
}
