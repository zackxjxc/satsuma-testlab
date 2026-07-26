// coro_rpc Host 监听器解析和生命周期测试。
#include <chrono>
#include <exception>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

#include <winsock2.h>

#include "rpc_server.hpp"
#include "agent.hpp"
#include "rpc_client.hpp"
#include "satsuma/core/errors.hpp"
#include "satsuma/core/id.hpp"
#include "satsuma/core/path.hpp"

namespace {

// 在条件不满足时终止当前测试。
void expect(const bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

// 临时绑定回环端口并返回系统分配的可用端口。
[[nodiscard]] unsigned short find_available_port() {
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
        throw std::runtime_error("WSAStartup failed");
    }
    const SOCKET socket_handle = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socket_handle == INVALID_SOCKET) {
        WSACleanup();
        throw std::runtime_error("socket creation failed");
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    if (bind(socket_handle, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR) {
        closesocket(socket_handle);
        WSACleanup();
        throw std::runtime_error("temporary socket bind failed");
    }
    int address_size = sizeof(address);
    if (getsockname(socket_handle, reinterpret_cast<sockaddr*>(&address), &address_size) == SOCKET_ERROR) {
        closesocket(socket_handle);
        WSACleanup();
        throw std::runtime_error("getsockname failed");
    }
    const unsigned short port = ntohs(address.sin_port);
    closesocket(socket_handle);
    WSACleanup();
    return port;
}

// 验证 IPv4、IPv6 和错误端点解析。
void test_endpoint_parser() {
    const satsuma::TcpEndpoint ipv4 = satsuma::parse_tcp_endpoint("127.0.0.1:37100");
    expect(ipv4.address == "127.0.0.1" && ipv4.port == 37'100, "IPv4 endpoint was parsed incorrectly");

    const satsuma::TcpEndpoint ipv6 = satsuma::parse_tcp_endpoint("[::1]:37100");
    expect(ipv6.address == "::1" && ipv6.port == 37'100, "IPv6 endpoint was parsed incorrectly");

    try {
        static_cast<void>(satsuma::parse_tcp_endpoint("127.0.0.1"));
        throw std::runtime_error("endpoint without a port was accepted");
    } catch (const satsuma::Error&) {
    }
}

// 验证 RPC 服务不可用时，Client 可以安全清理并再次连接。
void test_unavailable_server_retry(const std::filesystem::path& root) {
    const unsigned short port = find_available_port();
    satsuma::AgentConfig config;
    config.lab_id = "rpc_unavailable_test";
    config.vm_id = "client";
    config.agent_version = "0.1.0";
    config.host = "127.0.0.1:" + std::to_string(port);
    config.shared_root = root / L"share";
    config.local_work_root = root / L"work";
    config.rpc_timeout_ms = 200;

    satsuma::vm::RpcClient client(config, "session_1", "boot_1");
    for (int attempt = 0; attempt < 2; ++attempt) {
        try {
            static_cast<void>(client.connect());
            throw std::runtime_error("RPC Client connected to an unavailable server");
        } catch (const satsuma::Error&) {
        }
        expect(!client.connected(), "failed RPC Client remained connected");
        client.disconnect();
    }

    config.host = "192.0.2.1:9";
    config.rpc_timeout_ms = 50;
    satsuma::vm::RpcClient timeout_client(config, "session_2", "boot_2");
    try {
        static_cast<void>(timeout_client.connect());
        throw std::runtime_error("RPC Client connected to a TEST-NET endpoint");
    } catch (const satsuma::Error&) {
    }
    expect(!timeout_client.connected(), "timed out RPC Client remained connected");
}

// 验证本机临时端口的启动和跨线程停止。
void test_server_lifecycle(const std::filesystem::path& root) {
    const unsigned short port = find_available_port();
    satsuma::LabConfig config;
    config.lab_id = "rpc_server_test";
    config.host.listen = "127.0.0.1:" + std::to_string(port);
    config.shared_folder.host_root = root / L"share";
    satsuma::VmConfig vm;
    vm.id = "client";
    vm.agent_version = "0.1.0";
    config.vms.push_back(std::move(vm));

    satsuma::AgentConfig agent_config;
    agent_config.lab_id = config.lab_id;
    agent_config.vm_id = "client";
    agent_config.agent_version = "0.1.0";
    agent_config.host = config.host.listen;
    agent_config.shared_root = config.shared_folder.host_root;
    agent_config.local_work_root = root / L"work";
    agent_config.rpc_timeout_ms = 1000;

    satsuma::host::RpcServer server(std::move(config));
    std::exception_ptr server_error;
    std::thread server_thread([&server, &server_error] {
        try {
            server.start();
        } catch (...) {
            server_error = std::current_exception();
        }
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    std::exception_ptr client_error;
    try {
        satsuma::vm::RpcClient client(agent_config, "session_1", "boot_1");
        const satsuma::SessionInfo session = client.connect();
        expect(session.accepted && client.connected(), "RPC Agent registration failed");
        const satsuma::HostDirective directive = client.heartbeat("idle", "");
        expect(directive.action == "poll", "RPC heartbeat did not return poll directive");

        // 跨线程误用必须在进入第三方 Client 前失败，且不能破坏 owner 线程上的连接。
        bool foreign_thread_rejected = false;
        std::exception_ptr foreign_thread_error;
        std::thread foreign_thread([&client, &foreign_thread_rejected, &foreign_thread_error] {
            try {
                static_cast<void>(client.heartbeat("idle", ""));
            } catch (const satsuma::Error& error) {
                foreign_thread_rejected =
                    std::string(error.what()) == "RPC Client cannot be used from a different thread";
            } catch (...) {
                foreign_thread_error = std::current_exception();
            }
        });
        foreign_thread.join();
        if (foreign_thread_error != nullptr) {
            std::rethrow_exception(foreign_thread_error);
        }
        expect(foreign_thread_rejected, "RPC Client accepted a call from a foreign thread");

        const satsuma::TaskReference task = client.poll_task();
        expect(!task.has_task, "RPC poll returned an unexpected task");

        satsuma::JobStatus job;
        job.run_id = "rpc_run";
        job.job_id = "job_1";
        job.step_id = "step_1";
        job.status = "running";
        expect(client.report_job(std::move(job)).accepted, "RPC Job report was rejected");
        client.disconnect();

        satsuma::vm::Agent agent(agent_config);
        expect(!agent.synchronize_rpc(), "Agent RPC synchronization returned an unexpected task");
    } catch (...) {
        client_error = std::current_exception();
    }

    server.stop();
    server_thread.join();
    if (server_error != nullptr) {
        std::rethrow_exception(server_error);
    }
    if (client_error != nullptr) {
        std::rethrow_exception(client_error);
    }
}

// 验证 Server 停止后 Client 有限清理，并能连接同端口上的新 Server。
void test_server_restart_recovery(const std::filesystem::path& root) {
    const unsigned short port = find_available_port();
    satsuma::LabConfig config;
    config.lab_id = "rpc_restart_test";
    config.host.listen = "127.0.0.1:" + std::to_string(port);
    config.shared_folder.host_root = root / L"restart-share";
    satsuma::VmConfig vm;
    vm.id = "client";
    vm.agent_version = "0.1.0";
    config.vms.push_back(std::move(vm));

    satsuma::AgentConfig agent_config;
    agent_config.lab_id = config.lab_id;
    agent_config.vm_id = "client";
    agent_config.agent_version = "0.1.0";
    agent_config.host = config.host.listen;
    agent_config.shared_root = config.shared_folder.host_root;
    agent_config.local_work_root = root / L"restart-work";
    agent_config.rpc_timeout_ms = 200;
    satsuma::vm::RpcClient client(agent_config, "session_restart", "boot_restart");

    {
        satsuma::host::RpcServer server(config);
        std::exception_ptr server_error;
        std::thread server_thread([&server, &server_error] {
            try {
                server.start();
            } catch (...) {
                server_error = std::current_exception();
            }
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        expect(client.connect().accepted, "RPC Client did not connect before Server restart");
        server.stop();
        server_thread.join();
        if (server_error != nullptr) {
            std::rethrow_exception(server_error);
        }
    }

    const auto disconnect_started = std::chrono::steady_clock::now();
    try {
        static_cast<void>(client.heartbeat("idle", ""));
        throw std::runtime_error("RPC heartbeat succeeded after Server stopped");
    } catch (const satsuma::Error&) {
    }
    client.disconnect();
    const auto disconnect_duration = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - disconnect_started);
    expect(disconnect_duration < std::chrono::seconds(2), "RPC disconnect exceeded its bounded wait");

    {
        satsuma::host::RpcServer server(config);
        std::exception_ptr server_error;
        std::thread server_thread([&server, &server_error] {
            try {
                server.start();
            } catch (...) {
                server_error = std::current_exception();
            }
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        expect(client.connect().accepted, "RPC Client did not reconnect after Server restart");
        client.disconnect();
        server.stop();
        server_thread.join();
        if (server_error != nullptr) {
            std::rethrow_exception(server_error);
        }
    }
}

}  // namespace

// 运行 RPC Server 测试并清理本次专用临时目录。
int main() {
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / satsuma::path_from_utf8(satsuma::make_id("rpc-server-test"));
    try {
        test_endpoint_parser();
        test_unavailable_server_retry(root);
        test_server_lifecycle(root);
        test_server_restart_recovery(root);
        std::filesystem::remove_all(root);
        std::cout << "SatsumaRpcServerTests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "SatsumaRpcServerTests failed: " << error.what() << '\n';
        std::error_code cleanup_error;
        std::filesystem::remove_all(root, cleanup_error);
        return 1;
    }
}
