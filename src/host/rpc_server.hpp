// coro_rpc Host 监听器生命周期接口。
#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>

#include "satsuma/core/config.hpp"

namespace satsuma::host {

// 解析后的 TCP 监听端点。
struct ListenEndpoint {
    std::string address;       // IPv4、IPv6 或主机名
    unsigned short port{0};    // TCP 端口，0 仅用于本机测试
};

// 解析 address:port 或 [IPv6]:port 配置。
[[nodiscard]] ListenEndpoint parse_listen_endpoint(std::string_view value);

// 管理 coro_rpc Server、处理器注册和安全停止。
class RpcServer {
public:
    // 使用指定线程数创建尚未启动的 RPC Server。
    explicit RpcServer(LabConfig config, std::size_t thread_count = 1);

    RpcServer(const RpcServer&) = delete;
    RpcServer& operator=(const RpcServer&) = delete;

    // 停止监听并释放内部线程池。
    ~RpcServer();

    // 阻塞运行，直到 stop 被调用或监听失败。
    void start();

    // 可从其他线程安全请求停止。
    void stop();

private:
    struct Impl;                 // 隔离第三方 RPC 头文件
    std::unique_ptr<Impl> impl_; // RPC Server 实现
};

}  // namespace satsuma::host
