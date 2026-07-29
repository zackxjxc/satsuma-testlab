// VM Agent 任务领取、执行和结果落盘接口。
#pragma once

#include <filesystem>
#include <stop_token>
#include <string>

#include "satsuma/core/config.hpp"
#include "satsuma/core/task.hpp"
#include "claim_renewal.hpp"
#include "inventory.hpp"
#include "process_runner.hpp"

namespace satsuma::vm {

class InteractiveUserSession;

// Agent 进程内可注入的运行策略，不改变 agent.json 协议。
struct AgentRuntimeOptions {
    ClaimLeasePolicy claim_lease_policy; // 固定租约、续租和安全截止参数
    ClaimRenewOperation claim_renew_operation; // 测试可注入的续租事务
};

// 轮询共享目录并执行分配给当前 VM 的任务。
class Agent {
public:
    // 使用已验证的 VM 配置创建本次 Agent 会话。
    explicit Agent(
        AgentConfig config,
        std::filesystem::path helper_executable = {},
        AgentRuntimeOptions runtime_options = {});

    // 扫描一次共享目录并返回新执行的步骤数。
    [[nodiscard]] int run_once(std::stop_token stop_token = {});

    // 按配置的间隔持续扫描共享目录，直到收到停止请求。
    void run_watch(std::stop_token stop_token = {});

private:
    // 扫描并执行当前文件通道中等待当前 VM 的任务。
    [[nodiscard]] int execute_pending_runs(std::stop_token stop_token);

    // 执行已领取步骤，仅在仍持有有效 claim 时发布规范 execution.json。
    void execute_step(
        const std::filesystem::path& run_directory,
        const RunManifest& manifest,
        const TaskStep& step,
        const std::filesystem::path& claim_path,
        const StepClaimLease& claim,
        std::stop_token stop_token);

    // 部署并校验当前 VM 的全部 Artifact。
    void deploy_artifacts(
        const std::filesystem::path& run_directory,
        const std::filesystem::path& local_run_directory,
        const RunManifest& manifest,
        std::stop_token stop_token,
        const InteractiveUserSession* interactive_session) const;

    // 写入当前运行可见的 Agent 状态。
    void write_state(
        const std::filesystem::path& run_directory,
        const std::string& status,
        const std::string& job_id) const;

    // 原子发布跨运行可见的 Agent 就绪状态。
    void write_presence() const;

    AgentConfig config_;       // 当前 VM Agent 配置
    std::string session_id_;   // 当前进程会话 ID
    std::string boot_id_;      // 当前进程启动 ID
    std::filesystem::path helper_executable_; // 交互用户 helper 路径
    AgentRuntimeOptions runtime_options_; // 当前进程使用的可注入运行策略
    InventoryPublisher inventory_; // 当前会话的 Guest 环境快照
    ProcessRunner runner_;     // Windows Job Object 执行器
};

}  // namespace satsuma::vm
