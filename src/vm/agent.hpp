// VM Agent 任务领取、执行和结果落盘接口。
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

#include "satsuma/core/config.hpp"
#include "satsuma/core/task.hpp"
#include "process_runner.hpp"
#include "rpc_client.hpp"

namespace satsuma::vm {

// 轮询共享目录并执行分配给当前 VM 的任务。
class Agent {
public:
    // 使用已验证的 VM 配置创建本次 Agent 会话。
    explicit Agent(AgentConfig config);

    // 扫描一次共享目录并返回新执行的步骤数。
    [[nodiscard]] int run_once();

    // 按配置的间隔持续扫描共享目录。
    [[noreturn]] void run_watch();

    // 完成一次连接、心跳和任务通知同步。
    [[nodiscard]] bool synchronize_rpc();

private:
    // 执行一个已领取步骤并始终生成 execution.json。
    void execute_step(
        const std::filesystem::path& run_directory,
        const RunManifest& manifest,
        const TaskStep& step,
        const std::string& job_id);

    // 部署并校验当前 VM 的全部 Artifact。
    void deploy_artifacts(
        const std::filesystem::path& run_directory,
        const std::filesystem::path& local_run_directory,
        const RunManifest& manifest) const;

    // 写入当前运行可见的 Agent 状态。
    void write_state(
        const std::filesystem::path& run_directory,
        const std::string& status,
        const std::string& job_id) const;

    // 尽力向 Host 上报 Job 状态，不影响文件通道执行结果。
    void report_job_state(
        const RunManifest& manifest,
        const TaskStep& step,
        const std::string& job_id,
        const std::string& status,
        const std::optional<std::uint32_t>& exit_code);

    AgentConfig config_;       // 当前 VM Agent 配置
    std::string session_id_;   // 当前进程会话 ID
    std::string boot_id_;      // 当前进程启动 ID
    ProcessRunner runner_;     // Windows Job Object 执行器
    RpcClient rpc_client_;     // Host RPC 客户端
};

}  // namespace satsuma::vm
