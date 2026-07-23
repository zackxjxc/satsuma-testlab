// VM Agent 的 coro_rpc 同步包装接口。
#pragma once

#include <memory>
#include <string>

#include "satsuma/core/config.hpp"
#include "satsuma/core/rpc_protocol.hpp"

namespace satsuma::vm {

// 管理单个 VM Agent 的 RPC 连接和版本化请求身份。
class RpcClient {
public:
    // 绑定 Agent 配置和当前进程会话身份。
    RpcClient(AgentConfig config, std::string session_id, std::string boot_id);

    RpcClient(const RpcClient&) = delete;
    RpcClient& operator=(const RpcClient&) = delete;

    // 关闭仍存活的网络连接。
    ~RpcClient();

    // 连接 Host、注册 Agent，并返回 Host 会话响应。
    [[nodiscard]] SessionInfo connect();

    // 发送一次 Agent 心跳。
    [[nodiscard]] HostDirective heartbeat(const std::string& status, const std::string& job_id);

    // 请求下一条共享目录任务引用。
    [[nodiscard]] TaskReference poll_task();

    // 上报一个 Job 状态摘要。
    [[nodiscard]] RpcAck report_job(JobStatus status);

    // 主动断开；对象之后仍可重新 connect。
    void disconnect() noexcept;

    // 返回当前包装器是否持有已注册连接。
    [[nodiscard]] bool connected() const noexcept;

private:
    struct Impl;                 // 隔离 yalantinglibs 客户端头文件
    std::unique_ptr<Impl> impl_; // 固定地址的客户端实现
};

}  // namespace satsuma::vm
