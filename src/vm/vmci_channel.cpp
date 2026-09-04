// Guest Agent 的 VMCI 文件镜像与权威 claim 客户端实现。
#include "vmci_channel.hpp"

#include <algorithm>
#include <fstream>
#include <set>
#include <string>
#include <utility>

#include <windows.h>

#include "hardware_identity.hpp"
#include "satsuma/core/errors.hpp"
#include "satsuma/core/id.hpp"
#include "satsuma/core/path.hpp"
#include "satsuma/core/sha256.hpp"

namespace satsuma::vm {
namespace {

[[nodiscard]] std::filesystem::path staged_download_path(
    const std::filesystem::path& target) {
    std::filesystem::path staged = target;
    staged += path_from_utf8(".vmci-" + make_id("download") + ".part");
    return staged;
}

void replace_file(
    const std::filesystem::path& source,
    const std::filesystem::path& destination) {
    if (!MoveFileExW(
            windows_file_path(source).c_str(),
            windows_file_path(destination).c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        throw Error(
            "MoveFileExW(VMCI file publish) failed with Win32 error " +
            std::to_string(GetLastError()));
    }
}

[[nodiscard]] std::filesystem::path protocol_relative(
    const AgentConfig& config,
    const std::filesystem::path& path) {
    return std::filesystem::relative(path, config.mirror_root);
}

// VMCI 元数据固定使用正斜杠，不暴露 Windows 本机分隔符。
[[nodiscard]] std::string protocol_text(const std::filesystem::path& relative) {
    std::string text = path_to_utf8(relative);
    std::replace(text.begin(), text.end(), '\\', '/');
    return text;
}

}  // namespace

VmciChannel::VmciChannel(const AgentConfig& config, std::string session_id)
    : config_(config),
      session_id_(std::move(session_id)),
      client_(
          config.transport.host_cid,
          config.transport.vmci_port,
          std::chrono::milliseconds(config.transport.request_timeout_ms)),
      cancellation_client_(
          config.transport.host_cid,
          config.transport.vmci_port,
          std::chrono::milliseconds(
              std::min(config.transport.request_timeout_ms, 1'000))) {
    std::filesystem::create_directories(windows_file_path(config_.mirror_root));
}

void VmciChannel::update_config(const AgentConfig& config) {
    config_ = config;
}

#ifdef SATSUMA_TEST_LOCAL_MIRROR
VmciChannel::VmciChannel(
    const AgentConfig& config, std::string session_id, const std::string& test_endpoint)
    : config_(config), session_id_(std::move(session_id)),
      client_(test_endpoint, std::chrono::milliseconds(config.transport.request_timeout_ms)),
      cancellation_client_(test_endpoint, std::chrono::milliseconds(1000)) {}
#endif

nlohmann::json VmciChannel::request_base(const char* operation) const {
    nlohmann::json request = {
        {"schema_version", 1},
        {"operation", operation},
        {"lab_id", config_.lab_id},
        {"hardware_id", config_.hardware_id},
        {"vm_id", config_.vm_id},
        {"session_id", session_id_},
    };
    if (config_.auto_enroll) {
        request["enrollment_id"] = config_.enrollment_id;
    }
    return request;
}

bool VmciChannel::enroll(AgentConfig& config, const std::string& binary_sha256) {
    if (!config.auto_enroll) {
        return false;
    }
    const auto response = client_.request({
        {"schema_version", 1}, {"operation", "enroll"}, {"enrollment_version", 1},
        {"protocol_version", config.protocol_version}, {"hardware_id", config.hardware_id},
        {"session_id", session_id_}, {"agent_version", config.agent_version},
        {"binary_sha256", binary_sha256},
    });
    if (response.metadata.value("session_id", std::string{}) != session_id_) {
        throw Error("Host enrollment response belongs to another Agent session");
    }
    const bool changed = apply_agent_enrollment(config, response.metadata);
    update_config(config);
    return changed;
}

void VmciChannel::synchronize_inbound() {
    std::vector<nlohmann::json> descriptors;
    std::string after;
    for (;;) {
        nlohmann::json request = request_base("index");
        request["after"] = after;
        transport::Message response = client_.request(request);
        if (!response.metadata.contains("files") ||
            !response.metadata.at("files").is_array()) {
            throw Error("VMCI index response omitted files");
        }
        for (const nlohmann::json& descriptor : response.metadata.at("files")) {
            descriptors.push_back(descriptor);
            download_file(descriptor);
        }
        if (response.metadata.value("complete", false)) {
            break;
        }
        const std::string next = response.metadata.value("next", std::string{});
        if (next.empty() || next <= after) {
            throw Error("VMCI index pagination did not advance");
        }
        after = next;
    }
    remove_stale_inbound(descriptors);
}

void VmciChannel::download_file(const nlohmann::json& descriptor) {
    const std::string relative_text = descriptor.at("path").get<std::string>();
    const std::uint64_t expected_size = descriptor.at("size").get<std::uint64_t>();
    const std::string expected_hash = descriptor.at("sha256").get<std::string>();
    const std::filesystem::path relative = path_from_utf8(relative_text);
    validate_relative_path(relative);
    const std::filesystem::path target = resolve_under_root(config_.mirror_root, relative);
    if (std::filesystem::is_regular_file(windows_file_path(target)) &&
        std::filesystem::file_size(windows_file_path(target)) == expected_size &&
        sha256_file(target) == expected_hash) {
        return;
    }

    std::filesystem::create_directories(windows_file_path(target.parent_path()));
    const std::filesystem::path staged = staged_download_path(target);
    try {
        std::ofstream output(staged, std::ios::binary | std::ios::trunc);
        if (!output) {
            throw Error("Cannot create VMCI staged download: " + relative_text);
        }
        std::uint64_t offset = 0;
        do {
            nlohmann::json request = request_base("download");
            request["path"] = relative_text;
            request["offset"] = offset;
            const transport::Message response = client_.request(request);
            if (response.metadata.at("offset").get<std::uint64_t>() != offset ||
                response.metadata.at("total_size").get<std::uint64_t>() != expected_size ||
                response.metadata.at("sha256").get<std::string>() != expected_hash) {
                throw Error("VMCI download metadata changed during transfer");
            }
            if (!response.payload.empty()) {
                output.write(
                    reinterpret_cast<const char*>(response.payload.data()),
                    static_cast<std::streamsize>(response.payload.size()));
            }
            offset += response.payload.size();
            if (response.metadata.value("eof", false)) {
                if (offset != expected_size) {
                    throw Error("VMCI download ended before the declared file size");
                }
                break;
            }
            if (response.payload.empty()) {
                throw Error("VMCI download made no progress");
            }
        } while (offset < expected_size);
        output.flush();
        if (!output) {
            throw Error("Cannot finish VMCI staged download");
        }
        output.close();
        if (sha256_file(staged) != expected_hash) {
            throw Error("VMCI downloaded file SHA-256 mismatch");
        }
        replace_file(staged, target);
    } catch (...) {
        std::error_code cleanup_error;
        std::filesystem::remove(windows_file_path(staged), cleanup_error);
        throw;
    }
}

void VmciChannel::remove_stale_inbound(
    const std::vector<nlohmann::json>& descriptors) {
    std::set<std::wstring> active_runs;
    std::set<std::wstring> active_updates;
    for (const nlohmann::json& descriptor : descriptors) {
        const std::filesystem::path relative =
            path_from_utf8(descriptor.at("path").get<std::string>());
        auto component = relative.begin();
        if (component == relative.end()) {
            continue;
        }
        const std::wstring top = component->native();
        ++component;
        if (component == relative.end()) {
            continue;
        }
        if (_wcsicmp(top.c_str(), L"runs") == 0) {
            active_runs.insert(component->native());
        } else if (_wcsicmp(top.c_str(), L"updates") == 0) {
            ++component;
            if (component != relative.end()) {
                active_updates.insert(component->native());
            }
        }
    }

    const std::filesystem::path runs = config_.mirror_root / L"runs";
    if (std::filesystem::is_directory(runs)) {
        for (const auto& entry : std::filesystem::directory_iterator(runs)) {
            if (entry.is_directory() && !active_runs.contains(entry.path().filename().native())) {
                std::error_code cleanup_error;
                std::filesystem::remove_all(windows_file_path(entry.path()), cleanup_error);
            }
        }
    }
    const std::filesystem::path updates =
        config_.mirror_root / L"updates" / path_from_utf8(config_.vm_id);
    if (std::filesystem::is_directory(updates)) {
        for (const auto& entry : std::filesystem::directory_iterator(updates)) {
            if (entry.is_directory() && !active_updates.contains(entry.path().filename().native())) {
                std::error_code cleanup_error;
                std::filesystem::remove_all(windows_file_path(entry.path()), cleanup_error);
            }
        }
    }
}

void VmciChannel::upload_file(
    const std::filesystem::path& local_path,
    const std::filesystem::path& relative) {
    if (!std::filesystem::is_regular_file(windows_file_path(local_path))) {
        return;
    }
    validate_relative_path(relative);
    const std::uint64_t total = std::filesystem::file_size(windows_file_path(local_path));
    const std::string digest = sha256_file(local_path);
    const std::string transfer_id = make_id("transfer");
    std::ifstream input(local_path, std::ios::binary);
    if (!input) {
        throw Error("Cannot open VMCI upload file: " + path_to_utf8(local_path));
    }
    std::uint64_t offset = 0;
    bool first_chunk = true;
    do {
        const std::size_t count = static_cast<std::size_t>(
            std::min<std::uint64_t>(total - offset, transport::kVmciChunkBytes));
        std::vector<std::byte> payload(count);
        if (count != 0) {
            input.read(reinterpret_cast<char*>(payload.data()), static_cast<std::streamsize>(count));
            if (!input) {
                throw Error("Cannot read VMCI upload file");
            }
        }
        nlohmann::json request = request_base("upload");
        request["path"] = protocol_text(relative);
        request["transfer_id"] = transfer_id;
        request["offset"] = offset;
        request["total_size"] = total;
        request["sha256"] = digest;
        const transport::Message response = client_.request(request, payload);
        const std::uint64_t next = response.metadata.at("next_offset").get<std::uint64_t>();
        if (next != offset + count) {
            throw Error("VMCI upload response returned an invalid offset");
        }
        offset = next;
        first_chunk = false;
    } while (offset < total || first_chunk);
}

void VmciChannel::synchronize_outbound() {
    std::vector<std::filesystem::path> files;
    const std::filesystem::path agents = config_.mirror_root / L"agents";
    files.push_back(agents / path_from_utf8(config_.hardware_id + ".json"));
    files.push_back(agents / path_from_utf8(config_.hardware_id + ".inventory.json"));
    files.push_back(
        agents / L"sessions" / path_from_utf8(config_.hardware_id) /
            path_from_utf8(session_id_ + ".json"));
    if (config_.vm_id != config_.hardware_id) {
        files.push_back(agents / path_from_utf8(config_.vm_id + ".json"));
    }

    const std::filesystem::path runs = config_.mirror_root / L"runs";
    if (std::filesystem::is_directory(runs)) {
        for (const auto& run : std::filesystem::directory_iterator(runs)) {
            const std::filesystem::path state = run.path() / L"state";
            files.push_back(state / path_from_utf8(config_.vm_id + "-agent.json"));
            files.push_back(state / path_from_utf8(config_.vm_id + "-agent-error.json"));
            files.push_back(state / path_from_utf8(config_.vm_id + "-cleanup.json"));
        }
    }
    const std::filesystem::path updates =
        config_.mirror_root / L"updates" / path_from_utf8(config_.vm_id);
    if (std::filesystem::is_directory(updates)) {
        for (const auto& update : std::filesystem::directory_iterator(updates)) {
            files.push_back(update.path() / L"result.json");
        }
    }
    for (const std::filesystem::path& file : files) {
        if (std::filesystem::is_regular_file(windows_file_path(file))) {
            upload_file(file, protocol_relative(config_, file));
        }
    }
}

StepClaimAcquireResult VmciChannel::acquire_claim(const StepClaimLease& proposed) {
    nlohmann::json request = request_base("claim_acquire");
    request["claim"] = proposed;
    const transport::Message response = client_.request(request);
    const std::string status = response.metadata.at("status").get<std::string>();
    StepClaimAcquireResult result;
    if (status == "acquired") {
        result.status = StepClaimAcquireStatus::Acquired;
    } else if (status == "completed") {
        result.status = StepClaimAcquireStatus::Completed;
    } else if (status == "wait") {
        result.status = StepClaimAcquireStatus::Wait;
    } else if (status == "manual_intervention_required") {
        result.status = StepClaimAcquireStatus::ManualInterventionRequired;
    } else if (status == "state_error") {
        throw StepClaimStateError(
            response.metadata.value("error", std::string{"remote claim state error"}));
    } else {
        throw Error("VMCI claim acquisition returned an unknown status");
    }
    if (!response.metadata.at("claim").is_null()) {
        result.claim = response.metadata.at("claim").get<StepClaimLease>();
    }
    return result;
}

StepClaimRenewResult VmciChannel::renew_claim(
    const StepClaimLease& owner,
    const std::int64_t lease_duration_ms) {
    nlohmann::json request = request_base("claim_renew");
    request["claim"] = owner;
    request["lease_duration_ms"] = lease_duration_ms;
    const transport::Message response = client_.request(request);
    const std::string status = response.metadata.at("status").get<std::string>();
    StepClaimRenewResult result;
    if (status == "renewed") {
        result.status = StepClaimRenewStatus::Renewed;
    } else if (status == "lease_expired") {
        result.status = StepClaimRenewStatus::LeaseExpired;
    } else if (status == "ownership_lost") {
        result.status = StepClaimRenewStatus::OwnershipLost;
    } else {
        throw Error("VMCI claim renewal returned an unknown status");
    }
    if (!response.metadata.at("claim").is_null()) {
        result.claim = response.metadata.at("claim").get<StepClaimLease>();
    }
    return result;
}

StepResultPublishStatus VmciChannel::publish_result(
    const StepClaimLease& owner,
    const nlohmann::json& result,
    const std::vector<StepResultEvidenceFile>& evidence) {
    nlohmann::json mappings = nlohmann::json::array();
    for (const StepResultEvidenceFile& file : evidence) {
        const std::filesystem::path staged = protocol_relative(config_, file.staged_path);
        const std::filesystem::path canonical = protocol_relative(config_, file.canonical_path);
        upload_file(file.staged_path, staged);
        mappings.push_back({
            {"staged_path", protocol_text(staged)},
            {"canonical_path", protocol_text(canonical)},
        });
    }
    nlohmann::json request = request_base("result_publish");
    request["claim"] = owner;
    request["result"] = result;
    request["evidence"] = std::move(mappings);
    const transport::Message response = client_.request(request);
    const std::string status = response.metadata.at("status").get<std::string>();
    if (status == "published") {
        return StepResultPublishStatus::Published;
    }
    if (status == "lease_expired") {
        return StepResultPublishStatus::LeaseExpired;
    }
    if (status == "ownership_lost") {
        return StepResultPublishStatus::OwnershipLost;
    }
    throw Error("VMCI result publish returned an unknown status");
}

bool VmciChannel::cancelled(const std::string& run_id) {
    nlohmann::json request = request_base("cancelled");
    request["run_id"] = run_id;
    return cancellation_client_.request(request).metadata.at("cancelled").get<bool>();
}

}  // namespace satsuma::vm
