// Host RPC 会话状态和任务选择接口。
#pragma once

#include <cstddef>
#include <mutex>
#include <string>
#include <unordered_map>

#include "satsuma/core/config.hpp"
#include "satsuma/core/rpc_protocol.hpp"

namespace satsuma::host {

// 实现 Agent 注册、心跳、任务轮询和状态上报业务逻辑。
class RpcService {
public:
    // 使用实验室配置创建隔离的 RPC 会话表。
    explicit RpcService(LabConfig config);

    // 注册或替换指定 VM 的 Agent 会话。
    [[nodiscard]] SessionInfo register_agent(AgentHello request);

    // 更新 Agent 状态并返回下一步控制提示。
    [[nodiscard]] HostDirective heartbeat(AgentStatus request);

    // 返回当前 VM 可领取的第一条共享目录任务。
    [[nodiscard]] TaskReference poll_task(PollRequest request);

    // 接受 Agent 的 Job 状态摘要。
    [[nodiscard]] RpcAck report_job(JobStatus request);

    // 返回当前已登记的 VM 会话数量，供诊断和测试使用。
    [[nodiscard]] std::size_t session_count() const;

private:
    // Host 内存中的单个 Agent 会话状态。
    struct SessionEntry {
        std::string session_id;    // 当前会话 ID
        std::string boot_id;       // 当前启动 ID
        std::string agent_version; // Agent 版本
        std::string status;        // 最近状态
        std::string job_id;        // 最近 Job ID
        std::string updated_at;    // 最近心跳 UTC 时间
    };

    // 检查请求是否属于当前已登记会话。
    [[nodiscard]] bool matches_session(
        const std::string& vm_id,
        const std::string& session_id,
        const std::string& boot_id) const;

    LabConfig config_;                                  // 当前实验室配置
    mutable std::mutex mutex_;                          // 会话表并发保护
    std::unordered_map<std::string, SessionEntry> sessions_; // 按 VM ID 保存会话
};

}  // namespace satsuma::host
