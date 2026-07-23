// coro_rpc Host 监听器生命周期实现。
#include "rpc_server.hpp"

#include <atomic>
#include <charconv>
#include <string>
#include <system_error>
#include <utility>

#include <ylt/coro_rpc/coro_rpc_server.hpp>

#include "rpc_service.hpp"
#include "satsuma/core/errors.hpp"

namespace satsuma::host {

// 保存第三方 Server 和本项目处理器对象。
struct RpcServer::Impl {
    // 构造 Server 并注册全部版本化 RPC 接口。
    Impl(LabConfig config, const std::size_t thread_count)
        : endpoint(parse_listen_endpoint(config.host.listen)),
          service(std::move(config)),
          server(thread_count, endpoint.port, endpoint.address) {
        server.register_handler<
            &RpcService::register_agent,
            &RpcService::heartbeat,
            &RpcService::poll_task,
            &RpcService::report_job>(&service);
    }

    ListenEndpoint endpoint;                 // 解析后的监听地址
    RpcService service;                      // Host RPC 业务处理器
    coro_rpc::coro_rpc_server server;         // yalantinglibs RPC Server
    std::atomic_bool stop_requested{false};  // 主动停止标记
};

ListenEndpoint parse_listen_endpoint(const std::string_view value) {
    if (value.empty()) {
        throw Error("Host listen endpoint must not be empty");
    }

    ListenEndpoint endpoint;
    std::string_view port_text;
    if (value.front() == '[') {
        const std::size_t bracket = value.find(']');
        if (bracket == std::string_view::npos || bracket + 1 >= value.size() || value[bracket + 1] != ':') {
            throw Error("Invalid bracketed IPv6 listen endpoint");
        }
        endpoint.address = std::string(value.substr(1, bracket - 1));
        port_text = value.substr(bracket + 2);
    } else {
        const std::size_t separator = value.rfind(':');
        if (separator == std::string_view::npos) {
            throw Error("Host listen endpoint must include a TCP port");
        }
        endpoint.address = std::string(value.substr(0, separator));
        port_text = value.substr(separator + 1);
    }
    if (endpoint.address.empty() || port_text.empty()) {
        throw Error("Host listen address and port must not be empty");
    }

    unsigned int parsed_port = 0;
    const auto [end, error] = std::from_chars(
        port_text.data(),
        port_text.data() + port_text.size(),
        parsed_port);
    if (error != std::errc{} || end != port_text.data() + port_text.size() || parsed_port > 65'535) {
        throw Error("Host listen port must be between 0 and 65535");
    }
    endpoint.port = static_cast<unsigned short>(parsed_port);
    return endpoint;
}

RpcServer::RpcServer(LabConfig config, const std::size_t thread_count)
    : impl_(nullptr) {
    if (thread_count == 0) {
        throw Error("RPC Server thread count must be greater than zero");
    }
    impl_ = std::make_unique<Impl>(std::move(config), thread_count);
}

RpcServer::~RpcServer() {
    stop();
}

void RpcServer::start() {
    const coro_rpc::err_code error = impl_->server.start();
    if (error && !impl_->stop_requested.load()) {
        throw Error("RPC Server stopped with error: " + std::string(error.message()));
    }
}

void RpcServer::stop() {
    if (impl_ != nullptr) {
        impl_->stop_requested.store(true);
        impl_->server.stop();
    }
}

}  // namespace satsuma::host
