// coro_rpc Host 监听器生命周期实现。
#include "rpc_server.hpp"

#include <atomic>
#include <string>
#include <utility>

#include <ylt/coro_rpc/coro_rpc_server.hpp>

#include "satsuma/core/errors.hpp"
#include "satsuma/rpc/service.hpp"

namespace satsuma::host {

// 保存第三方 Server 和本项目处理器对象。
struct RpcServer::Impl {
    // 构造 Server 并注册全部版本化 RPC 接口。
    Impl(LabConfig config, const std::size_t thread_count)
        : endpoint(parse_tcp_endpoint(config.host.listen)),
          service(std::move(config)),
          server(thread_count, endpoint.port, endpoint.address) {
        server.register_handler<
            &RpcService::register_agent,
            &RpcService::heartbeat,
            &RpcService::poll_task,
            &RpcService::report_job>(&service);
    }

    TcpEndpoint endpoint;                    // 解析后的监听地址
    RpcService service;                      // Host RPC 业务处理器
    coro_rpc::coro_rpc_server server;         // yalantinglibs RPC Server
    std::atomic_bool stop_requested{false};  // 主动停止标记
};

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
