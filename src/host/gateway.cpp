// Host 常驻 VMCI 网关实现。
#include "gateway.hpp"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <limits>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <windows.h>

#include "satsuma/core/claim_store.hpp"
#include "satsuma/core/errors.hpp"
#include "satsuma/core/id.hpp"
#include "satsuma/core/json_io.hpp"
#include "satsuma/core/path.hpp"
#include "satsuma/core/sha256.hpp"
#include "satsuma/core/task.hpp"
#include "satsuma/core/update.hpp"

namespace satsuma::host {

// 每个 Host 状态根只允许一个 VMCI 网关写入，进程退出时由内核释放。
class GatewayStateLock {
public:
    explicit GatewayStateLock(const std::filesystem::path& state_root) {
        std::filesystem::create_directories(state_root);
        const std::filesystem::path path = state_root / L".gateway.lock";
        handle_ = CreateFileW(
            path.c_str(),
            GENERIC_READ | GENERIC_WRITE,
            0,
            nullptr,
            OPEN_ALWAYS,
            FILE_ATTRIBUTE_HIDDEN,
            nullptr);
        if (handle_ == INVALID_HANDLE_VALUE) {
            throw Error(
                "Cannot acquire the Host VMCI gateway state lock: Win32 error " +
                std::to_string(GetLastError()));
        }
    }

    ~GatewayStateLock() {
        if (handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(handle_);
        }
    }

    GatewayStateLock(const GatewayStateLock&) = delete;
    GatewayStateLock& operator=(const GatewayStateLock&) = delete;

private:
    HANDLE handle_{INVALID_HANDLE_VALUE};
};

namespace {

constexpr std::size_t kIndexPageSize = 512;

struct PeerIdentity {
    std::string hardware_id;
    std::string vm_id;
    std::string session_id;
};

[[nodiscard]] const nlohmann::json& require_object(
    const nlohmann::json& value,
    const char* field) {
    if (!value.contains(field) || !value.at(field).is_object()) {
        throw Error(std::string("VMCI request requires object field ") + field);
    }
    return value.at(field);
}

[[nodiscard]] std::string require_string(
    const nlohmann::json& value,
    const char* field) {
    if (!value.contains(field) || !value.at(field).is_string()) {
        throw Error(std::string("VMCI request requires string field ") + field);
    }
    return value.at(field).get<std::string>();
}

[[nodiscard]] PeerIdentity load_peer(
    const nlohmann::json& request,
    const LabConfig& config) {
    if (request.value("schema_version", 0) != 1 ||
        request.value("lab_id", std::string{}) != config.lab_id) {
        throw Error("VMCI request protocol or lab identity mismatch");
    }
    PeerIdentity peer{
        require_string(request, "hardware_id"),
        require_string(request, "vm_id"),
        require_string(request, "session_id"),
    };
    validate_identifier(peer.hardware_id, "VMCI hardware_id");
    validate_identifier(peer.vm_id, "VMCI vm_id");
    validate_identifier(peer.session_id, "VMCI session_id");
    return peer;
}

[[nodiscard]] std::filesystem::path protocol_path(
    const LabConfig& config,
    const std::string& relative) {
    const std::filesystem::path path = path_from_utf8(relative);
    validate_relative_path(path);
    return resolve_under_root(config.transport.state_root, path);
}

[[nodiscard]] std::string protocol_relative(
    const LabConfig& config,
    const std::filesystem::path& path) {
    std::string relative = path_to_utf8(
        std::filesystem::relative(path, config.transport.state_root));
    std::replace(relative.begin(), relative.end(), '\\', '/');
    return relative;
}

void add_file(
    const LabConfig& config,
    const std::filesystem::path& path,
    std::vector<nlohmann::json>& files,
    const std::string& known_sha256 = {}) {
    if (!std::filesystem::is_regular_file(path)) {
        return;
    }
    const std::uintmax_t size = std::filesystem::file_size(path);
    if (size > std::numeric_limits<std::uint64_t>::max()) {
        throw Error("VMCI protocol file size cannot be represented");
    }
    files.push_back({
        {"path", protocol_relative(config, path)},
        {"size", static_cast<std::uint64_t>(size)},
        {"sha256", known_sha256.empty() ? sha256_file(path) : known_sha256},
    });
}

struct PeerRunState {
    bool assigned{false};
    bool pending_work{false};
    bool pending_cleanup{false};
};

// 只有与 Host 请求身份一致且状态终结的回执，才算完成 Guest 清理。
[[nodiscard]] bool cleanup_result_is_terminal(
    const std::filesystem::path& request_path,
    const std::filesystem::path& result_path,
    const RunManifest& manifest,
    const PeerIdentity& peer) {
    if (!std::filesystem::is_regular_file(result_path)) {
        return false;
    }
    try {
        const nlohmann::json request = load_json(request_path);
        const nlohmann::json result = load_json(result_path);
        const std::string request_id = request.value("request_id", std::string{});
        const std::string status = result.value("status", std::string{});
        return !request_id.empty() &&
            request.value("schema_version", 0) == 1 &&
            request.value("lab_id", std::string{}) == manifest.lab_id &&
            request.value("run_id", std::string{}) == manifest.run_id &&
            request.value("vm_id", std::string{}) == peer.vm_id &&
            request.value("target", std::string{}) == "guest_work" &&
            result.value("schema_version", 0) == 1 &&
            result.value("lab_id", std::string{}) == manifest.lab_id &&
            result.value("run_id", std::string{}) == manifest.run_id &&
            result.value("vm_id", std::string{}) == peer.vm_id &&
            result.value("request_id", std::string{}) == request_id &&
            result.value("target", std::string{}) == "guest_work" &&
            (status == "deleted" || status == "failed");
    } catch (...) {
        return false;
    }
}

// 只向 Agent 暴露尚未完成的步骤，或等待该 Agent 回执的清理请求。
[[nodiscard]] PeerRunState inspect_peer_run(
    const std::filesystem::path& run,
    const RunManifest& manifest,
    const PeerIdentity& peer) {
    PeerRunState state;
    for (const TaskStep& step : manifest.steps) {
        if (step.vm != peer.vm_id) {
            continue;
        }
        state.assigned = true;
        if (!std::filesystem::is_regular_file(
                run / L"results" / path_from_utf8(peer.vm_id) /
                path_from_utf8(step.id) / L"execution.json")) {
            state.pending_work = true;
        }
    }
    const std::filesystem::path state_root = run / L"state";
    const std::filesystem::path cleanup_request =
        state_root / path_from_utf8(peer.vm_id + "-cleanup-request.json");
    const std::filesystem::path cleanup_result =
        state_root / path_from_utf8(peer.vm_id + "-cleanup.json");
    state.pending_cleanup = std::filesystem::is_regular_file(cleanup_request) &&
        !cleanup_result_is_terminal(
            cleanup_request,
            cleanup_result,
            manifest,
            peer);
    return state;
}

[[nodiscard]] std::vector<nlohmann::json> collect_inbound_files(
    const LabConfig& config,
    const PeerIdentity& peer) {
    std::vector<nlohmann::json> files;
    const std::filesystem::path agents = config.transport.state_root / L"agents";
    add_file(config, agents / path_from_utf8(peer.hardware_id + ".binding.json"), files);
    add_file(
        config,
        agents / path_from_utf8(peer.hardware_id + ".inventory-refresh.json"),
        files);

    const std::filesystem::path runs = config.transport.state_root / L"runs";
    if (std::filesystem::is_directory(runs)) {
        for (const auto& entry : std::filesystem::directory_iterator(runs)) {
            if (!entry.is_directory() || entry.path().filename().native().starts_with(L".")) {
                continue;
            }
            const std::filesystem::path manifest_path = entry.path() / L"task.json";
            if (!std::filesystem::is_regular_file(manifest_path)) {
                continue;
            }
            RunManifest manifest;
            try {
                manifest = load_run_manifest(manifest_path);
            } catch (...) {
                continue;
            }
            const PeerRunState state = inspect_peer_run(entry.path(), manifest, peer);
            if (!state.assigned || (!state.pending_work && !state.pending_cleanup)) {
                continue;
            }
            add_file(config, manifest_path, files);
            if (state.pending_work) {
                for (const ArtifactManifest& artifact : manifest.artifacts) {
                    if (artifact.vm == peer.vm_id) {
                        add_file(
                            config,
                            resolve_under_root(entry.path(), artifact.path),
                            files,
                            artifact.sha256);
                    }
                }
                add_file(config, entry.path() / L"cancel.json", files);
            }
            if (state.pending_cleanup) {
                add_file(
                    config,
                    entry.path() / L"state" /
                        path_from_utf8(peer.vm_id + "-cleanup-request.json"),
                    files);
            }
        }
    }

    const std::filesystem::path updates =
        config.transport.state_root / L"updates" / path_from_utf8(peer.vm_id);
    if (std::filesystem::is_directory(updates)) {
        for (const auto& entry : std::filesystem::directory_iterator(updates)) {
            if (!entry.is_directory() || entry.path().filename().native().starts_with(L".")) {
                continue;
            }
            const std::filesystem::path manifest_path = entry.path() / L"update.json";
            if (!std::filesystem::is_regular_file(manifest_path)) {
                continue;
            }
            try {
                const AgentUpdateManifest manifest = load_agent_update_manifest(manifest_path);
                if (manifest.lab_id == config.lab_id && manifest.vm_id == peer.vm_id) {
                    add_file(config, manifest_path, files);
                    add_file(
                        config,
                        resolve_under_root(entry.path(), manifest.binary),
                        files,
                        manifest.sha256);
                }
            } catch (...) {
            }
        }
    }
    std::sort(files.begin(), files.end(), [](const auto& left, const auto& right) {
        return left.at("path").template get<std::string>() <
            right.at("path").template get<std::string>();
    });
    return files;
}

[[nodiscard]] bool is_allowed_outbound_path(
    const std::filesystem::path& relative,
    const PeerIdentity& peer) {
    std::vector<std::string> components;
    for (const auto& component : relative) {
        components.push_back(path_to_utf8(component));
    }
    if (components.size() == 2 && components[0] == "agents") {
        return components[1] == peer.hardware_id + ".json" ||
            components[1] == peer.hardware_id + ".inventory.json" ||
            components[1] == peer.vm_id + ".json";
    }
    if (components.size() == 4 && components[0] == "agents" &&
        components[1] == "sessions") {
        return components[2] == peer.hardware_id &&
            components[3] == peer.session_id + ".json";
    }
    if (components.size() == 4 && components[0] == "updates") {
        return components[1] == peer.vm_id && components[3] == "result.json";
    }
    if (components.size() == 4 && components[0] == "runs" &&
        components[2] == "state") {
        return components[3] == peer.vm_id + "-agent.json" ||
            components[3] == peer.vm_id + "-agent-error.json" ||
            components[3] == peer.vm_id + "-cleanup.json";
    }
    if (components.size() >= 8 && components[0] == "runs" &&
        components[2] == "results" && components[3] == peer.vm_id &&
        components[5] == ".jobs") {
        return true;
    }
    return false;
}

// 读取 Host 权威清单，并确认当前 Agent 确实参与该运行。
[[nodiscard]] RunManifest load_authorized_run(
    const LabConfig& config,
    const PeerIdentity& peer,
    const std::string& run_id) {
    validate_identifier(run_id, "VMCI outbound run_id");
    const std::filesystem::path manifest_path =
        config.transport.state_root / L"runs" / path_from_utf8(run_id) / L"task.json";
    if (!std::filesystem::is_regular_file(manifest_path)) {
        throw Error("VMCI outbound run is no longer present on the Host: " + run_id);
    }
    const RunManifest manifest = load_run_manifest(manifest_path);
    if (manifest.lab_id != config.lab_id || manifest.run_id != run_id ||
        std::none_of(
            manifest.steps.begin(),
            manifest.steps.end(),
            [&peer](const TaskStep& step) { return step.vm == peer.vm_id; })) {
        throw Error("VMCI outbound run is outside the Agent scope: " + run_id);
    }
    return manifest;
}

// 确认 claim 对应 Host 清单中的同一 VM、步骤和重试策略。
void validate_claim_request(
    const LabConfig& config,
    const PeerIdentity& peer,
    const StepClaimLease& claim) {
    const RunManifest manifest = load_authorized_run(config, peer, claim.run_id);
    const auto step = std::find_if(
        manifest.steps.begin(),
        manifest.steps.end(),
        [&claim](const TaskStep& candidate) {
            return candidate.vm == claim.vm_id && candidate.id == claim.step_id;
        });
    if (step == manifest.steps.end() || step->retry_safe != claim.retry_safe) {
        throw Error("VMCI claim does not match the Host task manifest");
    }
}

[[nodiscard]] std::optional<nlohmann::json> describe_inbound_file(
    const LabConfig& config,
    const PeerIdentity& peer,
    const std::string& relative_text) {
    const std::filesystem::path relative = path_from_utf8(relative_text);
    validate_relative_path(relative);
    std::vector<std::string> components;
    for (const auto& component : relative) {
        components.push_back(path_to_utf8(component));
    }
    std::string known_hash;
    bool allowed = false;
    if (components.size() == 2 && components[0] == "agents") {
        allowed = components[1] == peer.hardware_id + ".binding.json" ||
            components[1] == peer.hardware_id + ".inventory-refresh.json";
    } else if (components.size() >= 3 && components[0] == "runs") {
        validate_identifier(components[1], "VMCI inbound run_id");
        const std::filesystem::path run =
            config.transport.state_root / L"runs" / path_from_utf8(components[1]);
        const std::filesystem::path manifest_path = run / L"task.json";
        if (!std::filesystem::is_regular_file(manifest_path)) {
            return std::nullopt;
        }
        const RunManifest manifest = load_run_manifest(manifest_path);
        const PeerRunState state = inspect_peer_run(run, manifest, peer);
        if (!state.assigned || (!state.pending_work && !state.pending_cleanup)) {
            return std::nullopt;
        }
        if (components.size() == 3 && components[2] == "task.json") {
            allowed = true;
        } else if (components.size() == 3 && components[2] == "cancel.json") {
            allowed = state.pending_work;
        } else if (components.size() == 4 && components[2] == "state" &&
                   components[3] == peer.vm_id + "-cleanup-request.json") {
            allowed = state.pending_cleanup;
        } else {
            const std::filesystem::path within_run = std::filesystem::relative(
                protocol_path(config, relative_text), run);
            const auto artifact = std::find_if(
                manifest.artifacts.begin(), manifest.artifacts.end(),
                [&peer, &within_run](const ArtifactManifest& item) {
                    return item.vm == peer.vm_id && item.path == within_run;
                });
            if (state.pending_work && artifact != manifest.artifacts.end()) {
                allowed = true;
                known_hash = artifact->sha256;
            }
        }
    } else if (components.size() == 4 && components[0] == "updates" &&
               components[1] == peer.vm_id) {
        validate_identifier(components[2], "VMCI inbound update_id");
        const std::filesystem::path directory =
            config.transport.state_root / L"updates" / path_from_utf8(peer.vm_id) /
            path_from_utf8(components[2]);
        const std::filesystem::path manifest_path = directory / L"update.json";
        if (!std::filesystem::is_regular_file(manifest_path)) {
            return std::nullopt;
        }
        const AgentUpdateManifest manifest = load_agent_update_manifest(manifest_path);
        if (manifest.lab_id == config.lab_id && manifest.vm_id == peer.vm_id) {
            if (components[3] == "update.json") {
                allowed = true;
            } else if (path_from_utf8(components[3]) == manifest.binary) {
                allowed = true;
                known_hash = manifest.sha256;
            }
        }
    }
    const std::filesystem::path path = protocol_path(config, relative_text);
    if (!allowed || !std::filesystem::is_regular_file(path)) {
        return std::nullopt;
    }
    const std::uintmax_t size = std::filesystem::file_size(path);
    return nlohmann::json({
        {"path", relative_text},
        {"size", static_cast<std::uint64_t>(size)},
        {"sha256", known_hash.empty() ? sha256_file(path) : known_hash},
    });
}

[[nodiscard]] std::filesystem::path checked_outbound_path(
    const LabConfig& config,
    const PeerIdentity& peer,
    const std::string& relative_text) {
    const std::filesystem::path relative = path_from_utf8(relative_text);
    validate_relative_path(relative);
    if (!is_allowed_outbound_path(relative, peer)) {
        throw Error("VMCI upload path is outside the Agent write scope: " + relative_text);
    }
    std::vector<std::string> components;
    for (const auto& component : relative) {
        components.push_back(path_to_utf8(component));
    }
    if (!components.empty() && components[0] == "runs") {
        const RunManifest manifest = load_authorized_run(config, peer, components[1]);
        if (components[2] == "results") {
            validate_identifier(components[4], "VMCI outbound step_id");
            validate_identifier(components[6], "VMCI outbound job_id");
            const bool owns_step = std::any_of(
                manifest.steps.begin(),
                manifest.steps.end(),
                [&peer, &components](const TaskStep& step) {
                    return step.vm == peer.vm_id && step.id == components[4];
                });
            if (!owns_step) {
                throw Error("VMCI result staging path is outside the task step scope");
            }
        }
    } else if (!components.empty() && components[0] == "updates") {
        validate_identifier(components[2], "VMCI outbound update_id");
        const std::filesystem::path manifest_path =
            config.transport.state_root / L"updates" / path_from_utf8(peer.vm_id) /
            path_from_utf8(components[2]) / L"update.json";
        if (!std::filesystem::is_regular_file(manifest_path)) {
            throw Error("VMCI outbound update is no longer present on the Host");
        }
        const AgentUpdateManifest manifest = load_agent_update_manifest(manifest_path);
        if (manifest.lab_id != config.lab_id || manifest.vm_id != peer.vm_id ||
            manifest.update_id != components[2]) {
            throw Error("VMCI outbound update is outside the Agent scope");
        }
    }
    return resolve_under_root(config.transport.state_root, relative);
}

[[nodiscard]] transport::Message handle_index(
    const LabConfig& config,
    const PeerIdentity& peer,
    const nlohmann::json& request) {
    const std::string after = request.value("after", std::string{});
    const std::vector<nlohmann::json> all_files = collect_inbound_files(config, peer);
    nlohmann::json page = nlohmann::json::array();
    bool complete = true;
    std::string next;
    for (const nlohmann::json& file : all_files) {
        const std::string path = file.at("path").get<std::string>();
        if (!after.empty() && path <= after) {
            continue;
        }
        if (page.size() == kIndexPageSize) {
            complete = false;
            break;
        }
        page.push_back(file);
        next = path;
    }
    return {{{"files", std::move(page)}, {"complete", complete}, {"next", next}}, {}};
}

[[nodiscard]] transport::Message handle_download(
    const LabConfig& config,
    const PeerIdentity& peer,
    const nlohmann::json& request) {
    const std::string relative = require_string(request, "path");
    const std::optional<nlohmann::json> descriptor =
        describe_inbound_file(config, peer, relative);
    if (!descriptor.has_value()) {
        throw Error("VMCI download path is outside the Agent read scope: " + relative);
    }
    const std::filesystem::path path = protocol_path(config, relative);
    const std::uint64_t total = descriptor->at("size").get<std::uint64_t>();
    const std::uint64_t offset = request.value("offset", std::uint64_t{0});
    if (offset > total) {
        throw Error("VMCI download offset is beyond the file size");
    }
    const std::uint64_t remaining = total - offset;
    const std::size_t count = static_cast<std::size_t>(
        std::min<std::uint64_t>(remaining, transport::kVmciChunkBytes));
    std::vector<std::byte> payload(count);
    std::ifstream input(path, std::ios::binary);
    input.seekg(static_cast<std::streamoff>(offset));
    if (count != 0) {
        input.read(reinterpret_cast<char*>(payload.data()), static_cast<std::streamsize>(count));
    }
    if (!input && count != 0) {
        throw Error("Cannot read VMCI protocol file: " + relative);
    }
    return {{{"path", relative},
             {"offset", offset},
             {"total_size", total},
             {"sha256", descriptor->at("sha256")},
             {"eof", offset + count == total}},
            std::move(payload)};
}

[[nodiscard]] transport::Message handle_upload(
    const LabConfig& config,
    const PeerIdentity& peer,
    const transport::Message& message) {
    const nlohmann::json& request = message.metadata;
    const std::string relative = require_string(request, "path");
    const std::string transfer_id = require_string(request, "transfer_id");
    const std::string expected_hash = require_string(request, "sha256");
    validate_identifier(transfer_id, "VMCI transfer_id");
    if (expected_hash.size() != 64) {
        throw Error("VMCI upload requires a SHA-256 digest");
    }
    const std::uint64_t offset = request.value("offset", std::uint64_t{0});
    const std::uint64_t total = request.at("total_size").get<std::uint64_t>();
    if (offset > total || message.payload.size() > total - offset) {
        throw Error("VMCI upload chunk is outside the declared file size");
    }
    const std::filesystem::path target = checked_outbound_path(config, peer, relative);
    const std::filesystem::path transfer = resolve_under_root(
        config.transport.state_root,
        std::filesystem::path(L".transfers") /
            path_from_utf8(peer.session_id) /
            path_from_utf8(transfer_id + ".part"));
    std::filesystem::create_directories(transfer.parent_path());
    if (offset == 0) {
        std::error_code remove_error;
        std::filesystem::remove(transfer, remove_error);
    }
    const std::uintmax_t actual_size = std::filesystem::is_regular_file(transfer)
        ? std::filesystem::file_size(transfer)
        : 0;
    if (actual_size != offset) {
        throw Error("VMCI upload offset does not match the staged transfer size");
    }
    {
        std::ofstream output(transfer, std::ios::binary | std::ios::app);
        if (!output) {
            throw Error("Cannot open VMCI staged upload");
        }
        if (!message.payload.empty()) {
            output.write(
                reinterpret_cast<const char*>(message.payload.data()),
                static_cast<std::streamsize>(message.payload.size()));
        }
        output.flush();
        if (!output) {
            throw Error("Cannot write VMCI staged upload");
        }
    }
    const std::uint64_t next = offset + message.payload.size();
    if (next == total) {
        if (sha256_file(transfer) != expected_hash) {
            throw Error("VMCI uploaded file SHA-256 mismatch");
        }
        std::filesystem::create_directories(target.parent_path());
        if (!MoveFileExW(
                transfer.c_str(),
                target.c_str(),
                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            throw Error(
                "MoveFileExW(VMCI upload publish) failed with Win32 error " +
                std::to_string(GetLastError()));
        }
    }
    return {{{"path", relative}, {"next_offset", next}, {"complete", next == total}}, {}};
}

[[nodiscard]] std::filesystem::path claim_path(
    const LabConfig& config,
    const StepClaimLease& claim) {
    return resolve_under_root(
        config.transport.state_root,
        std::filesystem::path(L"runs") / path_from_utf8(claim.run_id) / L"state" /
            path_from_utf8(claim.vm_id) / path_from_utf8(claim.step_id + ".claim.json"));
}

[[nodiscard]] std::filesystem::path result_path(
    const LabConfig& config,
    const StepClaimLease& claim) {
    return resolve_under_root(
        config.transport.state_root,
        std::filesystem::path(L"runs") / path_from_utf8(claim.run_id) / L"results" /
            path_from_utf8(claim.vm_id) / path_from_utf8(claim.step_id) / L"execution.json");
}

[[nodiscard]] std::filesystem::path recovery_path(
    const LabConfig& config,
    const StepClaimLease& claim) {
    return resolve_under_root(
        config.transport.state_root,
        std::filesystem::path(L"runs") / path_from_utf8(claim.run_id) / L"state" /
            path_from_utf8(claim.vm_id) /
            path_from_utf8(claim.step_id + ".claim-recovery.json"));
}

[[nodiscard]] transport::Message handle_claim_acquire(
    const LabConfig& config,
    const PeerIdentity& peer,
    const nlohmann::json& request) {
    const StepClaimLease proposed = require_object(request, "claim").get<StepClaimLease>();
    if (proposed.vm_id != peer.vm_id || proposed.session_id != peer.session_id) {
        throw Error("VMCI claim owner does not match the connected Agent");
    }
    validate_claim_request(config, peer, proposed);
    vm::StepClaimAcquireResult result;
    try {
        result = vm::acquire_step_claim_transaction(
            claim_path(config, proposed),
            result_path(config, proposed),
            proposed);
    } catch (const vm::StepClaimStateError& error) {
        write_json_atomic(recovery_path(config, proposed), {
            {"schema_version", 1},
            {"status", "manual_intervention_required"},
            {"reason", "claim state failed validation"},
            {"error", error.what()},
            {"current_boot_id", proposed.boot_id},
            {"observed_at", utc_timestamp()},
        });
        return {{{"status", "state_error"}, {"error", error.what()}, {"claim", nullptr}}, {}};
    }
    std::string status;
    switch (result.status) {
        case vm::StepClaimAcquireStatus::Acquired: status = "acquired"; break;
        case vm::StepClaimAcquireStatus::Completed: status = "completed"; break;
        case vm::StepClaimAcquireStatus::Wait: status = "wait"; break;
        case vm::StepClaimAcquireStatus::ManualInterventionRequired:
            status = "manual_intervention_required";
            break;
    }
    nlohmann::json response = {{"status", status}};
    response["claim"] = result.claim.has_value()
        ? nlohmann::json(*result.claim)
        : nlohmann::json(nullptr);
    if (result.status == vm::StepClaimAcquireStatus::ManualInterventionRequired &&
        result.claim.has_value()) {
        write_json_atomic(recovery_path(config, proposed), {
            {"schema_version", 1},
            {"status", "manual_intervention_required"},
            {"reason", "expired claim belongs to an unsafe step"},
            {"claim", *result.claim},
            {"current_boot_id", proposed.boot_id},
            {"observed_at", utc_timestamp()},
        });
    }
    return {std::move(response), {}};
}

[[nodiscard]] transport::Message handle_claim_renew(
    const LabConfig& config,
    const PeerIdentity& peer,
    const nlohmann::json& request) {
    const StepClaimLease owner = require_object(request, "claim").get<StepClaimLease>();
    if (owner.vm_id != peer.vm_id || owner.session_id != peer.session_id) {
        throw Error("VMCI renewal owner does not match the connected Agent");
    }
    validate_claim_request(config, peer, owner);
    const std::int64_t duration = request.at("lease_duration_ms").get<std::int64_t>();
    const vm::StepClaimRenewResult result = vm::renew_step_claim_transaction(
        claim_path(config, owner), owner, duration);
    const char* status = result.status == vm::StepClaimRenewStatus::Renewed
        ? "renewed"
        : (result.status == vm::StepClaimRenewStatus::LeaseExpired
            ? "lease_expired"
            : "ownership_lost");
    nlohmann::json response = {{"status", status}};
    response["claim"] = result.claim.has_value()
        ? nlohmann::json(*result.claim)
        : nlohmann::json(nullptr);
    const std::filesystem::path cancellation =
        config.transport.state_root / L"runs" / path_from_utf8(owner.run_id) / L"cancel.json";
    response["cancelled"] = std::filesystem::is_regular_file(cancellation);
    return {std::move(response), {}};
}

[[nodiscard]] transport::Message handle_cancelled(
    const LabConfig& config,
    const PeerIdentity& peer,
    const nlohmann::json& request) {
    static_cast<void>(peer);
    const std::string run_id = require_string(request, "run_id");
    validate_identifier(run_id, "VMCI cancellation run_id");
    const std::filesystem::path path =
        config.transport.state_root / L"runs" / path_from_utf8(run_id) / L"cancel.json";
    return {{{"cancelled", std::filesystem::is_regular_file(path)}}, {}};
}

[[nodiscard]] transport::Message handle_result_publish(
    const LabConfig& config,
    const PeerIdentity& peer,
    const nlohmann::json& request) {
    const StepClaimLease owner = require_object(request, "claim").get<StepClaimLease>();
    if (owner.vm_id != peer.vm_id || owner.session_id != peer.session_id) {
        throw Error("VMCI result owner does not match the connected Agent");
    }
    validate_claim_request(config, peer, owner);
    const nlohmann::json& result = require_object(request, "result");
    if (!request.contains("evidence") || !request.at("evidence").is_array()) {
        throw Error("VMCI result publish requires an evidence array");
    }
    std::vector<vm::StepResultEvidenceFile> evidence;
    const std::filesystem::path canonical_root = result_path(config, owner).parent_path();
    for (const nlohmann::json& item : request.at("evidence")) {
        const std::filesystem::path staged = checked_outbound_path(
            config, peer, require_string(item, "staged_path"));
        const std::filesystem::path canonical_protocol =
            path_from_utf8(require_string(item, "canonical_path"));
        validate_relative_path(canonical_protocol);
        const std::filesystem::path canonical =
            resolve_under_root(config.transport.state_root, canonical_protocol);
        const std::filesystem::path within_result =
            std::filesystem::relative(canonical, canonical_root);
        validate_relative_path(within_result);
        const auto first = within_result.begin();
        if (first == within_result.end() ||
            (first->native() != L"stdout.log" && first->native() != L"stderr.log" &&
             first->native() != L"files")) {
            throw Error("VMCI canonical result evidence is outside the step result scope");
        }
        static_cast<void>(resolve_under_root(canonical_root, within_result));
        evidence.push_back({staged, canonical});
    }
    const vm::StepResultPublishStatus status = vm::publish_step_result_if_owned(
        claim_path(config, owner), owner, result_path(config, owner), result, evidence);
    const char* text = status == vm::StepResultPublishStatus::Published
        ? "published"
        : (status == vm::StepResultPublishStatus::LeaseExpired
            ? "lease_expired"
            : "ownership_lost");
    return {{{"status", text}}, {}};
}

}  // namespace

Gateway::Gateway(LabConfig config)
    : config_(std::move(config)),
      state_lock_(std::make_unique<GatewayStateLock>(config_.transport.state_root)) {
    std::filesystem::create_directories(config_.transport.state_root / L"agents");
    std::filesystem::create_directories(config_.transport.state_root / L"runs");
    std::filesystem::create_directories(config_.transport.state_root / L"updates");
}

Gateway::~Gateway() = default;

void Gateway::run(const std::stop_token stop_token) {
    transport::Server server(
        config_.transport.vmci_port,
        [this](const transport::Message& request) { return handle(request); });
    server.run(stop_token);
}

transport::Message Gateway::handle(const transport::Message& message) {
    const nlohmann::json& request = message.metadata;
    const PeerIdentity peer = load_peer(request, config_);
    const std::string operation = require_string(request, "operation");
    if (operation == "ping") {
        return {{{"status", "ready"}, {"transport", "vmci"}}, {}};
    }
    if (operation == "index") {
        return handle_index(config_, peer, request);
    }
    if (operation == "download") {
        return handle_download(config_, peer, request);
    }
    if (operation == "upload") {
        return handle_upload(config_, peer, message);
    }
    if (operation == "claim_acquire") {
        return handle_claim_acquire(config_, peer, request);
    }
    if (operation == "claim_renew") {
        return handle_claim_renew(config_, peer, request);
    }
    if (operation == "cancelled") {
        return handle_cancelled(config_, peer, request);
    }
    if (operation == "result_publish") {
        return handle_result_publish(config_, peer, request);
    }
    throw Error("Unknown VMCI gateway operation: " + operation);
}

}  // namespace satsuma::host
