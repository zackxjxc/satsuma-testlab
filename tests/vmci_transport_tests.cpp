#include <atomic>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

#include <zmq.h>

#include "gateway.hpp"
#include "satsuma/core/id.hpp"
#include "satsuma/core/json_io.hpp"
#include "satsuma/core/path.hpp"
#include "satsuma/core/sha256.hpp"
#include "satsuma/core/vmci.hpp"

namespace {

int failures = 0;

void expect(const bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

}  // namespace

int main() {
    using namespace std::chrono_literals;
    expect(zmq_has("vmci") == 1, "libzmq does not expose native VMCI support");
    expect(
        satsuma::transport::make_bind_endpoint(42510) == "vmci://@:42510",
        "VMCI bind endpoint changed");
    expect(
        satsuma::transport::make_connect_endpoint(2, 42510) == "vmci://2:42510",
        "VMCI connect endpoint changed");

    const std::string endpoint = "tcp://127.0.0.1:43875";
    satsuma::transport::Server server(
        endpoint,
        [](const satsuma::transport::Message& request) {
            satsuma::transport::Message response;
            response.metadata = {
                {"operation", request.metadata.at("operation")},
                {"payload_size", request.payload.size()},
            };
            response.payload = request.payload;
            return response;
        },
        50ms);
    std::jthread worker([&server](const std::stop_token stop_token) {
        server.run(stop_token);
    });

    satsuma::transport::Client client(endpoint, 2s);
    const std::vector<std::byte> payload = {
        std::byte{'v'}, std::byte{'m'}, std::byte{'c'}, std::byte{'i'},
    };
    const satsuma::transport::Message response = client.request(
        {{"operation", "echo"}}, payload);
    expect(response.metadata.at("operation") == "echo", "metadata was not preserved");
    expect(response.metadata.at("payload_size") == payload.size(), "payload size changed");
    expect(response.payload == payload, "binary payload changed");
    worker.request_stop();
    worker.join();

    const std::filesystem::path test_root =
        std::filesystem::temp_directory_path() /
        satsuma::path_from_utf8(satsuma::make_id("satsuma-vmci-gateway"));
    satsuma::LabConfig config;
    config.lab_id = "vmci_test_lab";
    config.transport.state_root = test_root;
    config.transport.vmci_port = 42510;
    satsuma::host::Gateway gateway(config);
    nlohmann::json base_request = {
        {"schema_version", 1},
        {"lab_id", config.lab_id},
        {"hardware_id", "hardware_01"},
        {"vm_id", "vm_01"},
        {"session_id", "session_01"},
    };

    nlohmann::json ping_request = base_request;
    ping_request["operation"] = "ping";
    const auto ping = gateway.handle({ping_request, {}});
    expect(ping.metadata.at("status") == "ready", "gateway ping failed");

    const std::filesystem::path binding_path =
        test_root / L"agents" / L"hardware_01.binding.json";
    satsuma::write_json_atomic(binding_path, {{"vm_id", "vm_01"}});
    nlohmann::json index_request = base_request;
    index_request["operation"] = "index";
    index_request["after"] = "";
    const auto index = gateway.handle({index_request, {}});
    expect(index.metadata.at("files").size() == 1, "gateway index omitted binding");

    nlohmann::json download_request = base_request;
    download_request["operation"] = "download";
    download_request["path"] = "agents/hardware_01.binding.json";
    download_request["offset"] = 0;
    const auto download = gateway.handle({download_request, {}});
    expect(download.metadata.at("eof") == true, "gateway download did not finish");
    expect(!download.payload.empty(), "gateway download payload is empty");

    const std::string presence_text = "{\"status\":\"idle\"}\n";
    const std::vector<std::byte> presence_payload(
        reinterpret_cast<const std::byte*>(presence_text.data()),
        reinterpret_cast<const std::byte*>(presence_text.data() + presence_text.size()));
    const std::filesystem::path presence_source = test_root / L"presence-source.json";
    {
        std::ofstream output(presence_source, std::ios::binary);
        output.write(presence_text.data(), static_cast<std::streamsize>(presence_text.size()));
    }
    nlohmann::json upload_request = base_request;
    upload_request.update({
        {"operation", "upload"},
        {"path", "agents/hardware_01.json"},
        {"transfer_id", "transfer_01"},
        {"sha256", satsuma::sha256_file(presence_source)},
        {"offset", 0},
        {"total_size", presence_payload.size()},
    });
    const auto upload = gateway.handle({upload_request, presence_payload});
    expect(upload.metadata.at("complete") == true, "gateway upload did not commit");
    expect(
        std::filesystem::is_regular_file(
            test_root / L"agents" / L"hardware_01.json"),
        "gateway upload target is missing");

    bool traversal_rejected = false;
    try {
        upload_request["path"] = "../outside.json";
        static_cast<void>(gateway.handle({upload_request, presence_payload}));
    } catch (const std::exception&) {
        traversal_rejected = true;
    }
    expect(traversal_rejected, "gateway accepted path traversal");
    std::filesystem::remove_all(test_root);

    if (failures != 0) {
        std::cerr << failures << " VMCI transport assertion(s) failed\n";
        return 1;
    }
    std::cout << "SatsumaVmciTransportTests passed\n";
    return 0;
}
