// VM Agent 的 coro_rpc 同步包装实现。
#ifdef _MSC_VER
// async_simple 的模板在 MSVC 下会产生已知的不可达代码警告。
#pragma warning(disable : 4702)
#endif

#include "rpc_client.hpp"

#include <winsock2.h>
#include <ws2tcpip.h>

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
namespace {

// 在进入 coro_rpc 前确认端点可建立 TCP 连接，规避其 Windows 连接超时崩溃。
[[nodiscard]] bool tcp_endpoint_reachable(
    const TcpEndpoint& endpoint,
    const std::chrono::milliseconds timeout) {
    static const bool winsock_ready = [] {
        WSADATA data{};
        return WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    if (!winsock_ready) {
        return false;
    }

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    addrinfo* addresses = nullptr;
    const std::string port = std::to_string(endpoint.port);
    if (getaddrinfo(endpoint.address.c_str(), port.c_str(), &hints, &addresses) != 0) {
        return false;
    }

    bool reachable = false;
    for (const addrinfo* address = addresses; address != nullptr; address = address->ai_next) {
        const SOCKET socket_handle = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
        if (socket_handle == INVALID_SOCKET) {
            continue;
        }

        u_long nonblocking = 1;
        if (ioctlsocket(socket_handle, FIONBIO, &nonblocking) == 0) {
            const int connect_result = connect(
                socket_handle,
                address->ai_addr,
                static_cast<int>(address->ai_addrlen));
            const int connect_error = connect_result == 0 ? 0 : WSAGetLastError();
            if (connect_result == 0) {
                reachable = true;
            } else if (
                connect_error == WSAEWOULDBLOCK ||
                connect_error == WSAEINPROGRESS ||
                connect_error == WSAEALREADY) {
                fd_set write_set;
                fd_set error_set;
                FD_ZERO(&write_set);
                FD_ZERO(&error_set);
                FD_SET(socket_handle, &write_set);
                FD_SET(socket_handle, &error_set);
                const auto timeout_count = timeout.count();
                timeval wait_time{
                    static_cast<long>(timeout_count / 1000),
                    static_cast<long>((timeout_count % 1000) * 1000),
                };
                if (select(0, nullptr, &write_set, &error_set, &wait_time) > 0) {
                    int socket_error = 0;
                    int error_size = sizeof(socket_error);
                    if (getsockopt(
                            socket_handle,
                            SOL_SOCKET,
                            SO_ERROR,
                            reinterpret_cast<char*>(&socket_error),
                            &error_size) == 0 &&
                        socket_error == 0) {
                        reachable = true;
                    }
                }
            }
        }
        closesocket(socket_handle);
        if (reachable) {
            break;
        }
    }
    freeaddrinfo(addresses);
    return reachable;
}

}  // namespace

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
    if (!tcp_endpoint_reachable(
            endpoint,
            std::chrono::milliseconds(impl_->config.rpc_timeout_ms))) {
        disconnect();
        throw Error("RPC endpoint is unavailable: " + impl_->config.host);
    }
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
        try {
            async_simple::coro::syncAwait(coro_io::post(
                [] {},
                &impl_->client->get_executor()));
        } catch (...) {
            // disconnect 不向析构路径传播关闭错误
        }
        impl_->client.reset();
    }
    impl_->registered = false;
}

bool RpcClient::connected() const noexcept {
    return impl_->registered;
}

}  // namespace satsuma::vm
