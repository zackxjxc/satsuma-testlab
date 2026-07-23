// coro_rpc Host 监听器解析和生命周期测试。
#include <chrono>
#include <exception>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

#include "rpc_server.hpp"
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

// 验证 IPv4、IPv6 和错误端点解析。
void test_endpoint_parser() {
    const satsuma::host::ListenEndpoint ipv4 =
        satsuma::host::parse_listen_endpoint("127.0.0.1:37100");
    expect(ipv4.address == "127.0.0.1" && ipv4.port == 37'100, "IPv4 endpoint was parsed incorrectly");

    const satsuma::host::ListenEndpoint ipv6 =
        satsuma::host::parse_listen_endpoint("[::1]:37100");
    expect(ipv6.address == "::1" && ipv6.port == 37'100, "IPv6 endpoint was parsed incorrectly");

    try {
        static_cast<void>(satsuma::host::parse_listen_endpoint("127.0.0.1"));
        throw std::runtime_error("endpoint without a port was accepted");
    } catch (const satsuma::Error&) {
    }
}

// 验证本机临时端口的启动和跨线程停止。
void test_server_lifecycle(const std::filesystem::path& root) {
    satsuma::LabConfig config;
    config.lab_id = "rpc_server_test";
    config.host.listen = "127.0.0.1:0";
    config.shared_folder.host_root = root / L"share";
    satsuma::VmConfig vm;
    vm.id = "client";
    vm.agent_version = "0.1.0";
    config.vms.push_back(std::move(vm));

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
    server.stop();
    server_thread.join();
    if (server_error != nullptr) {
        std::rethrow_exception(server_error);
    }
}

}  // namespace

// 运行 RPC Server 测试并清理本次专用临时目录。
int main() {
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / satsuma::path_from_utf8(satsuma::make_id("rpc-server-test"));
    try {
        test_endpoint_parser();
        test_server_lifecycle(root);
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
