// VM Agent 的 coro_rpc 同步包装实现。
#ifdef _MSC_VER
// async_simple 的模板在 MSVC 下会产生已知的不可达代码警告。
#pragma warning(disable : 4702)
#endif

#include "rpc_client.hpp"

#include <chrono>
#include <memory>
#include <string>
#include <utility>

#include <async_simple/coro/SyncAwait.h>
#include <ylt/coro_rpc/coro_rpc_client.hpp>

#include "satsuma/core/endpoint.hpp"
#include "satsuma/core/errors.hpp"
#include "satsuma/core/id.hpp"
#include "satsuma/rpc/service.hpp"

namespace satsuma::vm {

// 保存第三方 Client 和当前 Agent 身份。
struct RpcClient::Impl {
    // 复制配置并验证会话标识符。
    Impl(AgentConfig value, std::string session, std::string boot)
        : config(std::move(value)), session_id(std::move(session)), boot_id(std::move(boot)) {
        validate_identifier(session_id, "session_id");
        validate_identifier(boot_id, "boot_id");
    }

    // 调用一个版本化 RPC，并把传输错误转换为 Satsuma Error。
    template <auto Function, typename Request>
    [[nodiscard]] auto call(Request request) {
        if (client == nullptr) {
            throw Error("RPC Client is not connected");
        }
        auto result = async_simple::coro::syncAwait(
            client->call_for<Function>(
                std::chrono::milliseconds(config.rpc_timeout_ms),
                std::move(request)));
        if (!result) {
            registered = false;
            client->close();
            throw Error("RPC call failed: " + result.error().msg);
        }
        return std::move(result).value();
    }

    AgentConfig config;                                  // Agent RPC 配置
    std::string session_id;                              // 当前会话 ID
    std::string boot_id;                                 // 当前启动 ID
    std::unique_ptr<coro_rpc::coro_rpc_client> client;   // yalantinglibs Client
    bool registered{false};                              // Host 是否接受会话
};

RpcClient::RpcClient(AgentConfig config, std::string session_id, std::string boot_id)
    : impl_(std::make_unique<Impl>(
          std::move(config),
          std::move(session_id),
          std::move(boot_id))) {}

RpcClient::~RpcClient() {
    disconnect();
}

SessionInfo RpcClient::connect() {
    disconnect();
    impl_->client = std::make_unique<coro_rpc::coro_rpc_client>();
    const TcpEndpoint endpoint = parse_tcp_endpoint(impl_->config.host);
    const coro_rpc::err_code error = async_simple::coro::syncAwait(
        impl_->client->connect(
            endpoint.address,
            std::to_string(endpoint.port),
            std::chrono::milliseconds(impl_->config.rpc_timeout_ms)));
    if (error) {
        disconnect();
        throw Error("RPC connect failed: " + std::string(error.message()));
    }

    AgentHello request;
    request.lab_id = impl_->config.lab_id;
    request.vm_id = impl_->config.vm_id;
    request.session_id = impl_->session_id;
    request.boot_id = impl_->boot_id;
    request.request_id = make_id("request");
    request.agent_version = impl_->config.agent_version;
    SessionInfo response = impl_->call<&host::RpcService::register_agent>(std::move(request));
    if (!response.accepted) {
        disconnect();
        throw Error("RPC registration rejected: " + response.message);
    }
    impl_->registered = true;
    return response;
}

HostDirective RpcClient::heartbeat(const std::string& status, const std::string& job_id) {
    AgentStatus request;
    request.lab_id = impl_->config.lab_id;
    request.vm_id = impl_->config.vm_id;
    request.session_id = impl_->session_id;
    request.boot_id = impl_->boot_id;
    request.request_id = make_id("request");
    request.status = status;
    request.job_id = job_id;
    return impl_->call<&host::RpcService::heartbeat>(std::move(request));
}

TaskReference RpcClient::poll_task() {
    PollRequest request;
    request.lab_id = impl_->config.lab_id;
    request.vm_id = impl_->config.vm_id;
    request.session_id = impl_->session_id;
    request.boot_id = impl_->boot_id;
    request.request_id = make_id("request");
    return impl_->call<&host::RpcService::poll_task>(std::move(request));
}

RpcAck RpcClient::report_job(JobStatus status) {
    status.lab_id = impl_->config.lab_id;
    status.vm_id = impl_->config.vm_id;
    status.session_id = impl_->session_id;
    status.boot_id = impl_->boot_id;
    status.request_id = make_id("request");
    return impl_->call<&host::RpcService::report_job>(std::move(status));
}

void RpcClient::disconnect() noexcept {
    if (impl_->client != nullptr) {
        impl_->client->close();
        impl_->client.reset();
    }
    impl_->registered = false;
}

bool RpcClient::connected() const noexcept {
    return impl_->registered;
}

}  // namespace satsuma::vm
