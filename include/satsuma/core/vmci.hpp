// 基于 libzmq 原生 vmci:// transport 的有界请求/响应通道。
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <stop_token>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace satsuma::transport {

inline constexpr std::size_t kVmciMaxMetadataBytes = 4U * 1024U * 1024U;
inline constexpr std::size_t kVmciChunkBytes = 1024U * 1024U;

struct VmciInfo {
    std::uint32_t version{0};
    int address_family{0};
    std::uint32_t local_cid{0};
};

struct Message {
    nlohmann::json metadata;
    std::vector<std::byte> payload;
};

// 查询当前系统的 VMware VMCI/vSockets 驱动信息。
[[nodiscard]] VmciInfo query_vmci_info();

// 生成 libzmq 原生 VMCI endpoint。
[[nodiscard]] std::string make_bind_endpoint(std::uint32_t port);
[[nodiscard]] std::string make_connect_endpoint(std::uint32_t cid, std::uint32_t port);

// 线程安全的单连接 REQ 客户端；通信失败后自动重建 socket。
class Client {
public:
    Client(
        std::string endpoint,
        std::chrono::milliseconds timeout = std::chrono::seconds(10));
    Client(std::uint32_t cid, std::uint32_t port,
           std::chrono::milliseconds timeout = std::chrono::seconds(10));
    ~Client();

    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;
    Client(Client&&) noexcept;
    Client& operator=(Client&&) noexcept;

    [[nodiscard]] Message request(
        const nlohmann::json& metadata,
        std::span<const std::byte> payload = {});

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// 单线程 REP 服务端；Handler 抛出的异常会转换为结构化错误响应。
class Server {
public:
    using Handler = std::function<Message(const Message&)>;

    Server(
        std::string endpoint,
        Handler handler,
        std::chrono::milliseconds poll_interval = std::chrono::milliseconds(250));
    Server(
        std::uint32_t port,
        Handler handler,
        std::chrono::milliseconds poll_interval = std::chrono::milliseconds(250));
    ~Server();

    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;
    Server(Server&&) noexcept;
    Server& operator=(Server&&) noexcept;

    void run(std::stop_token stop_token = {});

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace satsuma::transport
