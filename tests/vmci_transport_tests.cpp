#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

#include <zmq.h>
#include <zmq.hpp>

#include "gateway.hpp"
#include "satsuma/core/claim.hpp"
#include "satsuma/core/id.hpp"
#include "satsuma/core/json_io.hpp"
#include "satsuma/core/path.hpp"
#include "satsuma/core/sha256.hpp"
#include "satsuma/core/task.hpp"
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

    {
        zmq::context_t malformed_context(1);
        zmq::socket_t malformed_client(malformed_context, zmq::socket_type::req);
        malformed_client.set(zmq::sockopt::linger, 0);
        malformed_client.set(zmq::sockopt::rcvtimeo, 250);
        malformed_client.connect(endpoint);
        malformed_client.send(zmq::str_buffer("not-json"), zmq::send_flags::none);
        zmq::message_t ignored_reply;
        expect(
            !malformed_client.recv(ignored_reply, zmq::recv_flags::none),
            "malformed request unexpectedly received a reply");
    }
    std::this_thread::sleep_for(100ms);
    const satsuma::transport::Message recovered = client.request(
        {{"operation", "recovered"}});
    expect(
        recovered.metadata.at("operation") == "recovered",
        "server did not recover after malformed metadata");
    worker.request_stop();
    worker.join();

    const std::filesystem::path test_root =
        std::filesystem::temp_directory_path() /
        satsuma::path_from_utf8(satsuma::make_id("satsuma-vmci-gateway"));
    satsuma::LabConfig config;
    config.lab_id = "vmci_test_lab";
    config.transport.state_root = test_root;
    config.transport.vmci_port = 42510;
    auto gateway = std::make_unique<satsuma::host::Gateway>(config);
    bool duplicate_gateway_rejected = false;
    try {
        satsuma::host::Gateway duplicate(config);
    } catch (const std::exception&) {
        duplicate_gateway_rejected = true;
    }
    expect(
        duplicate_gateway_rejected,
        "two VMCI gateways acquired the same Host state root");
    nlohmann::json base_request = {
        {"schema_version", 1},
        {"lab_id", config.lab_id},
        {"hardware_id", "hardware_01"},
        {"vm_id", "vm_01"},
        {"session_id", "session_01"},
    };

    nlohmann::json ping_request = base_request;
    ping_request["operation"] = "ping";
    const auto ping = gateway->handle({ping_request, {}});
    expect(ping.metadata.at("status") == "ready", "gateway ping failed");

    const std::filesystem::path binding_path =
        test_root / L"agents" / L"hardware_01.binding.json";
    satsuma::write_json_atomic(binding_path, {{"vm_id", "vm_01"}});
    nlohmann::json index_request = base_request;
    index_request["operation"] = "index";
    index_request["after"] = "";
    const auto index = gateway->handle({index_request, {}});
    expect(index.metadata.at("files").size() == 1, "gateway index omitted binding");

    nlohmann::json download_request = base_request;
    download_request["operation"] = "download";
    download_request["path"] = "agents/hardware_01.binding.json";
    download_request["offset"] = 0;
    const auto download = gateway->handle({download_request, {}});
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
    const auto upload = gateway->handle({upload_request, presence_payload});
    expect(upload.metadata.at("complete") == true, "gateway upload did not commit");
    expect(
        std::filesystem::is_regular_file(
            test_root / L"agents" / L"hardware_01.json"),
        "gateway upload target is missing");

    bool traversal_rejected = false;
    try {
        upload_request["path"] = "../outside.json";
        static_cast<void>(gateway->handle({upload_request, presence_payload}));
    } catch (const std::exception&) {
        traversal_rejected = true;
    }
    expect(traversal_rejected, "gateway accepted path traversal");

    satsuma::RunManifest manifest;
    manifest.lab_id = config.lab_id;
    manifest.run_id = "run_stale_cache";
    manifest.name = "stale cache boundary";
    manifest.created_at = satsuma::utc_timestamp();
    satsuma::TaskStep step;
    step.id = "echo";
    step.vm = "vm_01";
    step.type = "echo";
    step.message = "stale cache";
    step.run_as = satsuma::TaskRunAs::System;
    step.retry_safe = true;
    manifest.steps.push_back(step);
    const std::filesystem::path stale_run =
        test_root / L"runs" / L"run_stale_cache";
    satsuma::write_json_atomic(stale_run / L"task.json", manifest);
    const auto index_contains = [&gateway, &index_request](const std::string& path) {
        const auto current = gateway->handle({index_request, {}});
        return std::any_of(
            current.metadata.at("files").begin(),
            current.metadata.at("files").end(),
            [&path](const nlohmann::json& file) {
                return file.at("path").get<std::string>() == path;
            });
    };
    expect(
        index_contains("runs/run_stale_cache/task.json"),
        "gateway index omitted a pending Host task");

    satsuma::write_json_atomic(
        stale_run / L"results" / L"vm_01" / L"echo" / L"execution.json",
        {{"status", "exited"}});
    expect(
        !index_contains("runs/run_stale_cache/task.json"),
        "gateway retained a completed run in the Guest mirror index");
    satsuma::write_json_atomic(
        stale_run / L"state" / L"vm_01-cleanup-request.json",
        {
            {"schema_version", 1},
            {"lab_id", config.lab_id},
            {"run_id", manifest.run_id},
            {"vm_id", "vm_01"},
            {"request_id", "cleanup_01"},
            {"target", "guest_work"},
        });
    expect(
        index_contains("runs/run_stale_cache/task.json") &&
            index_contains("runs/run_stale_cache/state/vm_01-cleanup-request.json"),
        "gateway omitted a pending Guest cleanup request");
    satsuma::write_json_atomic(
        stale_run / L"state" / L"vm_01-cleanup.json",
        {{"request_id", "wrong_request"}, {"status", "deleted"}});
    expect(
        index_contains("runs/run_stale_cache/state/vm_01-cleanup-request.json"),
        "gateway accepted an invalid Guest cleanup result as terminal");
    satsuma::write_json_atomic(
        stale_run / L"state" / L"vm_01-cleanup.json",
        {
            {"schema_version", 1},
            {"lab_id", config.lab_id},
            {"run_id", manifest.run_id},
            {"vm_id", "vm_01"},
            {"request_id", "cleanup_01"},
            {"target", "guest_work"},
            {"status", "deleted"},
        });
    expect(
        !index_contains("runs/run_stale_cache/task.json"),
        "gateway retained a completed cleanup in the Guest mirror index");

    upload_request.update({
        {"path", "runs/run_stale_cache/state/vm_01-agent.json"},
        {"transfer_id", "transfer_run_state"},
    });
    const auto run_upload = gateway->handle({upload_request, presence_payload});
    expect(run_upload.metadata.at("complete") == true, "authorized run state upload failed");
    std::filesystem::remove_all(stale_run);

    bool stale_upload_rejected = false;
    try {
        upload_request["transfer_id"] = "transfer_stale_state";
        static_cast<void>(gateway->handle({upload_request, presence_payload}));
    } catch (const std::exception&) {
        stale_upload_rejected = true;
    }
    expect(
        stale_upload_rejected && !std::filesystem::exists(stale_run),
        "stale Guest mirror recreated a deleted Host run through upload");

    nlohmann::json claim_request = base_request;
    claim_request["operation"] = "claim_acquire";
    claim_request["claim"] = satsuma::make_step_claim_lease(
        "run_stale_cache",
        "vm_01",
        "echo",
        "job_stale_cache",
        "session_01",
        "boot_01",
        satsuma::unix_time_ms(),
        5'000,
        true);
    bool stale_claim_rejected = false;
    try {
        static_cast<void>(gateway->handle({claim_request, {}}));
    } catch (const std::exception&) {
        stale_claim_rejected = true;
    }
    expect(
        stale_claim_rejected && !std::filesystem::exists(stale_run),
        "stale Guest mirror recreated a deleted Host run through claim acquisition");
    gateway.reset();
    std::filesystem::remove_all(test_root);

    if (failures != 0) {
        std::cerr << failures << " VMCI transport assertion(s) failed\n";
        return 1;
    }
    std::cout << "SatsumaVmciTransportTests passed\n";
    return 0;
}
