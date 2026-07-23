// Host 与 VM 共用的 TCP 端点解析接口。
#pragma once

#include <string>
#include <string_view>

namespace satsuma {

// 解析后的 TCP 端点。
struct TcpEndpoint {
    std::string address;       // IPv4、IPv6 或主机名
    unsigned short port{0};    // TCP 端口，0 仅用于本机测试
};

// 解析 address:port 或 [IPv6]:port 配置。
[[nodiscard]] TcpEndpoint parse_tcp_endpoint(std::string_view value);

}  // namespace satsuma
