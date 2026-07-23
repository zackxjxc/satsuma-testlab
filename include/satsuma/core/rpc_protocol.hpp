// Host 与 VM 之间的版本化 RPC 数据契约。
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace satsuma {

inline constexpr int kRpcProtocolVersion = 1;  // 当前 RPC 协议版本

// VM 首次注册时发送的身份和版本信息。
struct AgentHello {
    int protocol_version{kRpcProtocolVersion}; // RPC 协议版本
    std::string lab_id;                        // 实验室稳定 ID
    std::string vm_id;                         // 虚拟机稳定 ID
    std::string session_id;                    // 快照恢复后重新生成的会话 ID
    std::string boot_id;                       // Agent 进程启动 ID
    std::string request_id;                    // 本次请求唯一 ID
    std::string agent_version;                 // Agent 语义版本
};

// Host 对 Agent 注册请求的响应。
struct SessionInfo {
    int protocol_version{kRpcProtocolVersion}; // Host RPC 协议版本
    bool accepted{false};                      // 是否接受当前 Agent
    std::string session_id;                    // 已登记的 Agent 会话 ID
    std::string host_time;                     // Host 当前 UTC 时间
    std::string message;                       // 拒绝原因或提示
};

// VM 周期性发送的实时状态。
struct AgentStatus {
    int protocol_version{kRpcProtocolVersion}; // RPC 协议版本
    std::string lab_id;                        // 实验室稳定 ID
    std::string vm_id;                         // 虚拟机稳定 ID
    std::string session_id;                    // 当前会话 ID
    std::string boot_id;                       // 当前启动 ID
    std::string request_id;                    // 本次请求唯一 ID
    std::string status;                        // idle、running 或 error
    std::string job_id;                        // 当前 Job ID，可为空
};

// Host 在心跳响应中返回的控制提示。
struct HostDirective {
    int protocol_version{kRpcProtocolVersion}; // Host RPC 协议版本
    std::string action{"none"};                // none、poll 或 stop
    std::string message;                       // 面向 Agent 的提示
};

// VM 请求下一条任务引用。
struct PollRequest {
    int protocol_version{kRpcProtocolVersion}; // RPC 协议版本
    std::string lab_id;                        // 实验室稳定 ID
    std::string vm_id;                         // 虚拟机稳定 ID
    std::string session_id;                    // 当前会话 ID
    std::string boot_id;                       // 当前启动 ID
    std::string request_id;                    // 本次请求唯一 ID
};

// Host 返回的共享目录任务清单引用。
struct TaskReference {
    int protocol_version{kRpcProtocolVersion}; // Host RPC 协议版本
    bool has_task{false};                      // 是否包含新任务
    std::string type;                          // 有任务时固定为 run_manifest
    std::string run_id;                        // 运行唯一 ID
    std::string manifest;                      // 共享根目录内相对路径
};

// VM 上报的 Job 状态摘要。
struct JobStatus {
    int protocol_version{kRpcProtocolVersion}; // RPC 协议版本
    std::string lab_id;                        // 实验室稳定 ID
    std::string vm_id;                         // 虚拟机稳定 ID
    std::string session_id;                    // 当前会话 ID
    std::string boot_id;                       // 当前启动 ID
    std::string request_id;                    // 本次请求唯一 ID
    std::string run_id;                        // 运行唯一 ID
    std::string job_id;                        // Job 唯一 ID
    std::string step_id;                       // 步骤唯一 ID
    std::string status;                        // running、exited、timed_out 或 failed
    bool has_exit_code{false};                 // 是否包含进程退出码
    std::uint32_t exit_code{0};                // 进程退出码
};

// Host 对状态上报的确认。
struct RpcAck {
    int protocol_version{kRpcProtocolVersion}; // Host RPC 协议版本
    bool accepted{false};                      // 是否接受本次上报
    std::string message;                       // 拒绝原因或提示
};

// 验证 Agent 注册请求和实验室边界。
void validate_rpc_request(const AgentHello& request, std::string_view expected_lab_id);

// 验证 Agent 心跳请求和会话字段。
void validate_rpc_request(const AgentStatus& request, std::string_view expected_lab_id);

// 验证 Agent 任务轮询请求和会话字段。
void validate_rpc_request(const PollRequest& request, std::string_view expected_lab_id);

// 验证 Agent Job 状态上报和运行字段。
void validate_rpc_request(const JobStatus& request, std::string_view expected_lab_id);

}  // namespace satsuma
