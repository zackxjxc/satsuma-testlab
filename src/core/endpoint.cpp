// Host 与 VM 共用的 TCP 端点解析实现。
#include "satsuma/core/endpoint.hpp"

#include <charconv>
#include <string>
#include <system_error>

#include "satsuma/core/errors.hpp"

namespace satsuma {

TcpEndpoint parse_tcp_endpoint(const std::string_view value) {
    if (value.empty()) {
        throw Error("TCP endpoint must not be empty");
    }

    TcpEndpoint endpoint;
    std::string_view port_text;
    if (value.front() == '[') {
        const std::size_t bracket = value.find(']');
        if (bracket == std::string_view::npos || bracket + 1 >= value.size() || value[bracket + 1] != ':') {
            throw Error("Invalid bracketed IPv6 endpoint");
        }
        endpoint.address = std::string(value.substr(1, bracket - 1));
        port_text = value.substr(bracket + 2);
    } else {
        const std::size_t separator = value.rfind(':');
        if (separator == std::string_view::npos) {
            throw Error("TCP endpoint must include a port");
        }
        endpoint.address = std::string(value.substr(0, separator));
        port_text = value.substr(separator + 1);
    }
    if (endpoint.address.empty() || port_text.empty()) {
        throw Error("TCP address and port must not be empty");
    }

    unsigned int parsed_port = 0;
    const auto [end, error] = std::from_chars(
        port_text.data(),
        port_text.data() + port_text.size(),
        parsed_port);
    if (error != std::errc{} || end != port_text.data() + port_text.size() || parsed_port > 65'535) {
        throw Error("TCP port must be between 0 and 65535");
    }
    endpoint.port = static_cast<unsigned short>(parsed_port);
    return endpoint;
}

}  // namespace satsuma
