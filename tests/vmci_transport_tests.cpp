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
#include "controller.hpp"
#include "artifact_store.hpp"
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

// 用实际 Gateway 请求验证物理文件隔离、原协议路径和旧运行兼容性。
void test_artifact_download_isolation(const std::filesystem::path& root) {
    satsuma::LabConfig config;
    config.lab_id = "artifact_download_test";
    config.transport.state_root = root / L"state";
    for (const std::string vm : {"vm_01", "vm_02"}) {
        satsuma::VmConfig item;
        item.id = vm;
        config.vms.push_back(item);
    }
    satsuma::host::Controller controller(config);
    satsuma::host::Gateway gateway(config);
    const auto first = root / L"first.json";
    const auto second = root / L"second.json";
    satsuma::write_json_atomic(first, {{"payload", "first"}});
    satsuma::write_json_atomic(second, {{"payload", "second"}});
    auto request_for = [&config](const std::string& vm, const std::string& operation) {
        return nlohmann::json{
            {"schema_version", 1}, {"lab_id", config.lab_id},
            {"hardware_id", "hardware_" + vm}, {"vm_id", vm},
            {"session_id", "session_" + vm}, {"operation", operation},
        };
    };
    auto rejected_download = [&](nlohmann::json request) {
        try {
            static_cast<void>(gateway.handle({std::move(request), {}}));
        } catch (const std::exception&) {
            return true;
        }
        return false;
    };
    for (const bool same_content : {true, false}) {
        satsuma::TaskPlan plan;
        plan.run_id = same_content ? "same_content" : "different_content";
        plan.name = "cross VM download";
        for (const std::string vm : {"vm_01", "vm_02"}) {
            satsuma::TaskStep step;
            step.id = "echo_" + vm;
            step.vm = vm;
            step.type = "echo";
            step.run_as = satsuma::TaskRunAs::System;
            step.message = "download only";
            plan.steps.push_back(step);
        }
        plan.artifacts = {
            {first, "vm_01", L"artifacts/preflight.json", std::nullopt},
            {same_content ? first : second, "vm_02", L"artifacts/preflight.json", std::nullopt},
        };
        const auto manifest = controller.create_run(plan);
        const auto run = config.transport.state_root / L"runs" /
            satsuma::path_from_utf8(manifest.run_id);
        const std::string logical = "runs/" + manifest.run_id + "/artifacts/preflight.json";
        for (std::size_t index = 0; index < 2; ++index) {
            const auto& artifact = manifest.artifacts[index];
            const auto response = gateway.handle({request_for(artifact.vm, "index"), {}});
            bool found = false;
            for (const auto& file : response.metadata.at("files")) {
                const std::string path = file.at("path");
                expect(path.find(".artifacts") == std::string::npos,
                    "gateway exposed a Host physical storage path to the Guest");
                if (path == logical) {
                    found = true;
                    expect(file.at("sha256") == artifact.sha256,
                        "gateway index used the other VM's Artifact hash");
                }
            }
            expect(found, "gateway index omitted a per-VM logical Artifact path");
            auto download = request_for(artifact.vm, "download");
            download["path"] = logical;
            const auto file = gateway.handle({download, {}});
            const std::string text(reinterpret_cast<const char*>(file.payload.data()), file.payload.size());
            expect(file.metadata.at("sha256") == artifact.sha256 &&
                    nlohmann::json::parse(text) == satsuma::load_json(plan.artifacts[index].source),
                "gateway returned bytes belonging to another VM's same-name Artifact");
            download["path"] = "runs/" + manifest.run_id + "/.artifacts/0.bin";
            expect(rejected_download(download), "Guest read the Host physical Artifact store directly");
        }
        auto outsider = request_for("vm_03", "download");
        outsider["path"] = logical;
        expect(rejected_download(outsider), "unassigned VM downloaded a private Artifact");

        // 新布局缺件时不能从同名逻辑路径取回其他内容。
        std::filesystem::remove(satsuma::host::artifact_storage_path(run, manifest, 1));
        satsuma::write_json_atomic(run / L"artifacts" / L"preflight.json", {{"payload", "decoy"}});
        auto missing = request_for("vm_02", "download");
        missing["path"] = logical;
        expect(rejected_download(missing), "missing isolated Artifact fell back to a shared logical path");
        std::filesystem::remove_all(run);
    }

    satsuma::RunManifest legacy;
    legacy.lab_id = config.lab_id;
    legacy.run_id = "legacy_layout";
    legacy.name = "legacy Artifact layout";
    legacy.created_at = satsuma::utc_timestamp();
    satsuma::TaskStep step;
    step.id = "echo";
    step.vm = "vm_01";
    step.type = "echo";
    step.message = "legacy";
    step.run_as = satsuma::TaskRunAs::System;
    legacy.steps.push_back(step);
    legacy.artifacts.push_back({"vm_01", L"artifacts/preflight.json", satsuma::sha256_file(first)});
    const auto old_run = config.transport.state_root / L"runs" / L"legacy_layout";
    std::filesystem::create_directories(old_run / L"artifacts");
    std::filesystem::copy_file(first, old_run / L"artifacts" / L"preflight.json");
    satsuma::write_json_atomic(old_run / L"task.json", legacy);
    auto legacy_request = request_for("vm_01", "download");
    legacy_request["path"] = "runs/legacy_layout/artifacts/preflight.json";
    const auto legacy_file = gateway.handle({legacy_request, {}});
    expect(legacy_file.metadata.at("sha256") == legacy.artifacts[0].sha256 &&
            !legacy_file.payload.empty(), "gateway no longer reads existing legacy runs");
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
    test_artifact_download_isolation(test_root / L"artifact-isolation");
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
