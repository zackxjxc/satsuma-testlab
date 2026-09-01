// VM Agent 任务领取、执行和结果落盘接口。
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <stop_token>
#include <string>

#include "satsuma/core/config.hpp"
#include "satsuma/core/task.hpp"
#include "claim_renewal.hpp"
#include "inventory.hpp"
#include "process_runner.hpp"

namespace satsuma::vm {

class InteractiveUserSession;
class VmciChannel;

using ClaimAcquireOperation = std::function<StepClaimAcquireResult(
    const std::filesystem::path&,
    const std::filesystem::path&,
    const StepClaimLease&)>;

using ResultPublishOperation = std::function<StepResultPublishStatus(
    const std::filesystem::path&,
    const StepClaimLease&,
    const std::filesystem::path&,
    const nlohmann::json&,
    const std::vector<StepResultEvidenceFile>&)>;

using CancellationCheck = std::function<bool(
    const std::filesystem::path&,
    const std::string&)>;

// Agent 进程内可注入的运行策略，不改变 agent.json 协议。
struct AgentRuntimeOptions {
    ClaimLeasePolicy claim_lease_policy; // 固定租约、续租和安全截止参数
    ClaimRenewOperation claim_renew_operation; // 测试可注入的续租事务
    ClaimAcquireOperation claim_acquire_operation; // 生产由 Host VMCI 网关执行
    ResultPublishOperation result_publish_operation; // 生产由 Host VMCI 网关 fencing
    CancellationCheck cancellation_check; // 执行中远程取消探测
};

// 同步 VMCI 本地镜像并执行分配给当前 VM 的任务。
class Agent {
public:
    // 使用已验证的 VM 配置创建本次 Agent 会话。
    explicit Agent(
        AgentConfig config,
        std::filesystem::path helper_executable = {},
        AgentRuntimeOptions runtime_options = {});
    ~Agent();

    // 同步并扫描一次本地镜像，返回新执行的步骤数。
    [[nodiscard]] int run_once(std::stop_token stop_token = {});

    // 按配置的间隔持续同步和扫描，直到收到停止请求。
    void run_watch(std::stop_token stop_token = {});

private:
    struct ExecutionWorkspace;

    // 扫描并执行当前传输镜像中等待当前 VM 的任务。
    [[nodiscard]] int execute_pending_runs(std::stop_token stop_token);

    // 执行已领取步骤，仅在仍持有有效 claim 时发布规范 execution.json。
    void execute_step(
        const std::filesystem::path& run_directory,
        const RunManifest& manifest,
        const TaskStep& step,
        const std::filesystem::path& claim_path,
        const StepClaimLease& claim,
        std::stop_token stop_token);

    // 执行 echo、进程或脚本负载，并收集声明的证据文件。
    void execute_step_payload(
        const std::filesystem::path& run_directory,
        const RunManifest& manifest,
        const TaskStep& step,
        const StepClaimLease& claim,
        std::stop_token stop_token,
        ExecutionWorkspace& workspace);

    // 在 claim fencing 下发布规范结果，失权时只保留 job 取证文件。
    void publish_step_execution(
        const std::filesystem::path& run_directory,
        const std::filesystem::path& claim_path,
        const StepClaimLease& claim,
        std::stop_token lease_loss_token,
        ClaimRenewalSession& renewal_session,
        ExecutionWorkspace& workspace);

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
    std::string session_started_at_; // 当前进程启动时间
    std::string binary_sha256_; // 当前运行 Agent 二进制哈希
    std::filesystem::path helper_executable_; // 交互用户 helper 路径
    AgentRuntimeOptions runtime_options_; // 当前进程使用的可注入运行策略
    InventoryPublisher inventory_; // 当前会话的 Guest 环境快照
    ProcessRunner runner_;     // Windows Job Object 执行器
    std::unique_ptr<VmciChannel> vmci_channel_; // 生产 VMCI 通道；测试可为空
    std::uint64_t vmci_channel_failure_count_{0}; // 传输通道累计失败次数
    std::uint64_t consecutive_vmci_channel_failures_{0}; // 当前连续失败次数
    std::string last_vmci_channel_error_; // 最近一次传输失败
    std::string last_vmci_channel_error_at_; // 最近一次传输失败时间
    std::string last_vmci_channel_recovered_at_; // 最近一次传输恢复时间
};

}  // namespace satsuma::vm
