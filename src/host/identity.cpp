// Host 侧 Agent 硬件发现、绑定和 presence 校验实现。
#include "identity.hpp"

#include <algorithm>
#include <chrono>
#include <chrono>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>
#include <windows.h>

#include "satsuma/core/errors.hpp"
#include "satsuma/core/hardware_identity.hpp"
#include "satsuma/core/id.hpp"
#include "satsuma/core/json_io.hpp"
#include "satsuma/core/lifecycle.hpp"
#include "satsuma/core/path.hpp"
#include "satsuma/core/sha256.hpp"

namespace satsuma::host {
namespace {

constexpr std::chrono::minutes kActivePresenceWindow{2}; // 覆盖最大 60 秒 Agent 轮询间隔

// 只把近期更新的 presence 或 Session heartbeat 视为在线身份。
[[nodiscard]] bool is_active_presence(const std::filesystem::directory_entry& entry) {
    return entry.last_write_time() >=
        std::filesystem::file_time_type::clock::now() - kActivePresenceWindow;
}

// 查找绑定到指定硬件 UUID 的 Host VM 配置。
[[nodiscard]] const VmConfig* find_vm_by_hardware(
    const LabConfig& config,
    const std::string& hardware_id) {
    const auto match = std::find_if(
        config.vms.begin(),
        config.vms.end(),
        [&hardware_id](const VmConfig& vm) { return vm.hardware_id == hardware_id; });
    return match == config.vms.end() ? nullptr : &*match;
}

// 校验硬件 presence 的固定公共字段。
void validate_presence_common(
    const nlohmann::json& presence,
    const LabConfig& config,
    const std::string& hardware_id) {
    const int protocol_version = presence.value("protocol_version", 0);
    if (presence.value("schema_version", 0) != 2 ||
        protocol_version != kRunManifestProtocolVersion ||
        presence.value("lab_id", std::string{}) != config.lab_id ||
        normalize_hardware_id(presence.value("hardware_id", std::string{})) != hardware_id) {
        throw Error("Agent presence identity mismatch for hardware_id " + hardware_id);
    }
}

// 汇总所有 presence 别名中的硬件到 VM 标识映射，用于发现克隆冲突。
[[nodiscard]] std::map<std::string, std::set<std::string>> collect_presence_identities(
    const LabConfig& config) {
    std::map<std::string, std::set<std::string>> identities;
    const std::filesystem::path agents_root = config.transport.state_root / L"agents";
    if (!std::filesystem::is_directory(agents_root)) {
        return identities;
    }
    for (const auto& entry : std::filesystem::directory_iterator(agents_root)) {
        if (!entry.is_regular_file() || entry.path().extension() != L".json" ||
            !is_active_presence(entry)) {
            continue;
        }
        try {
            const nlohmann::json presence = load_json(entry.path());
            const int protocol_version = presence.value("protocol_version", 0);
            if (presence.value("schema_version", 0) != 2 ||
                protocol_version != kRunManifestProtocolVersion ||
                presence.value("lab_id", std::string{}) != config.lab_id) {
                continue;
            }
            const std::string hardware_id = normalize_hardware_id(
                presence.value("hardware_id", std::string{}));
            const std::string vm_id = presence.value("vm_id", std::string{});
            validate_identifier(vm_id, "presence vm_id");
            identities[hardware_id].insert(vm_id);
        } catch (...) {
        }
    }
    return identities;
}

// 会话 heartbeat 使没有 VM 标识别名的未绑定 Agent 也能暴露同时运行的重复硬件 UUID。
[[nodiscard]] std::map<std::string, std::set<std::string>> collect_active_sessions(
    const LabConfig& config) {
    struct SessionWindow {
        std::string session_id;
        std::string started_at;
        std::string updated_at;
    };
    std::map<std::string, std::vector<SessionWindow>> windows;
    const std::filesystem::path sessions_root =
        config.transport.state_root / L"agents" / L"sessions";
    if (!std::filesystem::is_directory(sessions_root)) {
        return {};
    }
    for (const auto& hardware_entry : std::filesystem::directory_iterator(sessions_root)) {
        if (!hardware_entry.is_directory()) {
            continue;
        }
        std::string hardware_id;
        try {
            hardware_id = normalize_hardware_id(
                path_to_utf8(hardware_entry.path().filename()));
        } catch (...) {
            continue;
        }
        for (const auto& session_entry :
             std::filesystem::directory_iterator(hardware_entry.path())) {
            try {
                if (!session_entry.is_regular_file() ||
                    session_entry.path().extension() != L".json" ||
                    !is_active_presence(session_entry)) {
                    continue;
                }
                const nlohmann::json presence = load_json(session_entry.path());
                validate_presence_common(presence, config, hardware_id);
                const std::string session_id = presence.value("session_id", std::string{});
                validate_identifier(session_id, "presence session_id");
                windows[hardware_id].push_back({
                    session_id,
                    presence.value("runtime", nlohmann::json::object())
                        .value("started_at", std::string{}),
                    presence.value("updated_at", std::string{}),
                });
            } catch (...) {
            }
        }
    }

    std::map<std::string, std::set<std::string>> sessions;
    for (const auto& [hardware_id, candidates] : windows) {
        std::set<std::string>& active = sessions[hardware_id];
        for (std::size_t left = 0; left < candidates.size(); ++left) {
            for (std::size_t right = left + 1; right < candidates.size(); ++right) {
                const SessionWindow& first = candidates[left];
                const SessionWindow& second = candidates[right];
                const bool missing_timestamps =
                    first.started_at.empty() || first.updated_at.empty() ||
                    second.started_at.empty() || second.updated_at.empty();
                const bool overlaps = missing_timestamps ||
                    (first.started_at <= second.updated_at &&
                     second.started_at <= first.updated_at);
                if (overlaps) {
                    active.insert(first.session_id);
                    active.insert(second.session_id);
                }
            }
        }
        if (active.empty() && !candidates.empty()) {
            const auto latest = std::max_element(
                candidates.begin(),
                candidates.end(),
                [](const SessionWindow& left, const SessionWindow& right) {
                    return left.updated_at < right.updated_at;
                });
            active.insert(latest->session_id);
        }
    }
    return sessions;
}

// 同一 UUID 同时声明多个 VM 标识时拒绝继续绑定或诊断。
void reject_hardware_identity_conflict(
    const LabConfig& config,
    const std::string& hardware_id) {
    const auto identities = collect_presence_identities(config);
    const auto match = identities.find(hardware_id);
    const auto sessions = collect_active_sessions(config);
    const auto session_match = sessions.find(hardware_id);
    if ((match != identities.end() && match->second.size() > 1) ||
        (session_match != sessions.end() && session_match->second.size() > 1)) {
        throw Error(
            "Agent hardware identity conflict: multiple VMs report hardware_id " +
            hardware_id);
    }
}

}  // namespace

std::filesystem::path vm_presence_path(const LabConfig& config, const VmConfig& vm) {
    const std::string key = vm.hardware_id.empty() ? vm.id : vm.hardware_id;
    return resolve_under_root(
        config.transport.state_root,
        std::filesystem::path(L"agents") / path_from_utf8(key + ".json"));
}

// 返回指定 presence 所声明硬件身份的环境清单路径。
[[nodiscard]] std::filesystem::path presence_inventory_path(
    const LabConfig& config,
    const nlohmann::json& presence) {
    const std::string hardware_id = presence.value("hardware_id", std::string{});
    if (hardware_id.empty()) {
        throw Error("Agent presence omitted hardware_id");
    }
    return resolve_under_root(
        config.transport.state_root,
        std::filesystem::path(L"agents") / path_from_utf8(hardware_id + ".inventory.json"));
}

nlohmann::json load_vm_presence(const LabConfig& config, const VmConfig& vm) {
    const std::filesystem::path path = vm_presence_path(config, vm);
    const nlohmann::json presence = load_json(path);
    if (!vm.hardware_id.empty()) {
        reject_hardware_identity_conflict(config, vm.hardware_id);
    }
    const std::string status = presence.value("status", std::string{});
    if (status == "unbound") {
        throw Error(
            "Agent " + presence.value("hardware_id", std::string{"unknown"}) +
            " is unbound; run SatsumaHost agent rebind --vm " + vm.id +
            " --hardware-id <uuid> --config <path>");
    }
    if (presence.value("lab_id", std::string{}) != config.lab_id ||
        presence.value("vm_id", std::string{}) != vm.id) {
        throw Error("Agent identity mismatch for VM " + vm.id);
    }
    if (!vm.hardware_id.empty()) {
        validate_presence_common(presence, config, vm.hardware_id);
    }
    if (!vm.agent_version.empty() &&
        presence.value("agent_version", std::string{}) != vm.agent_version) {
        throw Error("Agent version mismatch for VM " + vm.id);
    }
    if (!vm.agent_sha256.empty() &&
        presence.value("binary_sha256", std::string{}) != vm.agent_sha256) {
        throw Error("Agent binary SHA-256 mismatch for VM " + vm.id);
    }
    return presence;
}

nlohmann::json load_vm_inventory(const LabConfig& config, const VmConfig& vm) {
    const nlohmann::json presence = load_vm_presence(config, vm);
    if (!presence.contains("inventory") || !presence.at("inventory").is_object()) {
        throw Error("Agent presence omitted inventory reference for VM " + vm.id);
    }
    const nlohmann::json& reference = presence.at("inventory");
    if (reference.value("observed_at", std::string{}).empty()) {
        throw Error("Agent inventory reference is invalid for VM " + vm.id);
    }
    const std::string expected_digest = reference.value("sha256", std::string{});
    const std::filesystem::path path = presence_inventory_path(config, presence);
    if (expected_digest.size() != 64 || sha256_file(path) != expected_digest) {
        throw Error("Agent inventory digest mismatch for VM " + vm.id);
    }
    const nlohmann::json inventory = load_json(path);
    if (inventory.value("schema_version", 0) != 2 ||
        inventory.value("lab_id", std::string{}) != config.lab_id ||
        inventory.value("vm_id", std::string{}) != vm.id ||
        inventory.value("hardware_id", std::string{}) !=
            presence.value("hardware_id", std::string{}) ||
        inventory.value("observed_at", std::string{}) !=
            reference.value("observed_at", std::string{})) {
        throw Error("Agent inventory identity mismatch for VM " + vm.id);
    }
    if (!inventory.contains("script_engines") || !inventory.at("script_engines").is_array() ||
        !inventory.contains("drives") || !inventory.at("drives").is_array()) {
        throw Error("Agent inventory capability fields are invalid for VM " + vm.id);
    }
    return inventory;
}

nlohmann::json refresh_vm_inventory(
    const LabConfig& config,
    const VmConfig& vm,
    const std::chrono::seconds timeout) {
    if (timeout.count() < 1 || timeout.count() > 300) {
        throw Error("Inventory refresh timeout must be between 1 and 300 seconds");
    }
    const nlohmann::json presence = load_vm_presence(config, vm);
    const std::string hardware_id = presence.value("hardware_id", std::string{});
    const std::string request_id = make_id("inventory");
    const std::filesystem::path request_path = resolve_under_root(
        config.transport.state_root,
        std::filesystem::path(L"agents") /
            path_from_utf8(hardware_id + ".inventory-refresh.json"));
    write_json_atomic(request_path, {
        {"schema_version", 1},
        {"lab_id", config.lab_id},
        {"vm_id", vm.id},
        {"hardware_id", hardware_id},
        {"request_id", request_id},
        {"requested_at", utc_timestamp()},
    });

    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::string last_error;
    do {
        try {
            nlohmann::json inventory = load_vm_inventory(config, vm);
            if (inventory.value("refresh_request_id", std::string{}) == request_id) {
                std::error_code cleanup_error;
                std::filesystem::remove(request_path, cleanup_error);
                return inventory;
            }
        } catch (const std::exception& error) {
            last_error = error.what();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    } while (std::chrono::steady_clock::now() < deadline);
    throw Error(
        "Timed out while waiting for Agent inventory refresh for VM " + vm.id +
        (last_error.empty() ? std::string{} : ": " + last_error));
}

nlohmann::json discover_agents(const LabConfig& config) {
    nlohmann::json agents = nlohmann::json::array();
    const std::filesystem::path agents_root =
        config.transport.state_root / L"agents";
    if (!std::filesystem::is_directory(agents_root)) {
        return {
            {"status", "discovered"},
            {"count", 0},
            {"collisions", nlohmann::json::array()},
            {"agents", std::move(agents)},
        };
    }

    for (const auto& entry : std::filesystem::directory_iterator(agents_root)) {
        if (!entry.is_regular_file() || entry.path().extension() != L".json" ||
            !is_active_presence(entry)) {
            continue;
        }
        std::string hardware_id;
        try {
            hardware_id = normalize_hardware_id(path_to_utf8(entry.path().stem()));
        } catch (...) {
            continue;
        }
        try {
            const nlohmann::json presence = load_json(entry.path());
            validate_presence_common(presence, config, hardware_id);
            nlohmann::json discovered = {
                {"hardware_id", hardware_id},
                {"vm_id", presence.value("vm_id", std::string{})},
                {"status", presence.value("status", std::string{})},
                {"agent_version", presence.value("agent_version", std::string{})},
                {"build_number", presence.value("build_number", std::string{})},
                {"build_attempt", presence.value("build_attempt", std::string{})},
                {"git_commit", presence.value("git_commit", std::string{})},
                {"binary_sha256", presence.value("binary_sha256", std::string{})},
                {"inventory", presence.value("inventory", nlohmann::json(nullptr))},
                {"runtime", presence.value("runtime", nlohmann::json(nullptr))},
                {"updated_at", presence.value("updated_at", std::string{})},
            };
            if (const VmConfig* vm = find_vm_by_hardware(config, hardware_id); vm != nullptr) {
                discovered["configured_vm_id"] = vm->id;
                discovered["vmx"] = path_to_utf8(vm->vmx);
            } else {
                discovered["configured_vm_id"] = nullptr;
                discovered["vmx"] = nullptr;
            }
            agents.push_back(std::move(discovered));
        } catch (...) {
            continue;
        }
    }
    std::sort(agents.begin(), agents.end(), [](const auto& left, const auto& right) {
        return left.at("hardware_id").template get<std::string>() <
            right.at("hardware_id").template get<std::string>();
    });
    const auto identities = collect_presence_identities(config);
    const auto sessions = collect_active_sessions(config);
    std::set<std::string> observed_hardware_ids;
    for (const auto& [hardware_id, vm_ids] : identities) {
        static_cast<void>(vm_ids);
        observed_hardware_ids.insert(hardware_id);
    }
    for (const auto& [hardware_id, session_ids] : sessions) {
        static_cast<void>(session_ids);
        observed_hardware_ids.insert(hardware_id);
    }
    nlohmann::json collisions = nlohmann::json::array();
    for (const std::string& hardware_id : observed_hardware_ids) {
        const auto identity_match = identities.find(hardware_id);
        const auto session_match = sessions.find(hardware_id);
        const std::set<std::string> vm_ids = identity_match == identities.end()
            ? std::set<std::string>{}
            : identity_match->second;
        const std::set<std::string> session_ids = session_match == sessions.end()
            ? std::set<std::string>{}
            : session_match->second;
        if (vm_ids.size() > 1 || session_ids.size() > 1) {
            collisions.push_back({
                {"hardware_id", hardware_id},
                {"vm_ids", vm_ids},
                {"active_session_ids", session_ids},
                {"message", "Multiple VMs report the same SMBIOS UUID"},
            });
        }
    }
    return {
        {"status", collisions.empty() ? "discovered" : "identity_conflict"},
        {"count", agents.size()},
        {"collisions", std::move(collisions)},
        {"agents", std::move(agents)},
    };
}

void publish_initial_agent_binding(
    const LabConfig& config,
    const std::string& hardware_id) {
    const VmConfig* vm = find_vm_by_hardware(config, hardware_id);
    if (vm == nullptr) {
        return;
    }
    const auto path = resolve_under_root(
        config.transport.state_root,
        std::filesystem::path(L"agents") / path_from_utf8(hardware_id + ".binding.json"));
    if (std::filesystem::exists(path)) {
        return;
    }
    reject_hardware_identity_conflict(config, hardware_id);
    const auto temporary = path.parent_path() / path_from_utf8(".tmp-" + make_id("binding"));
    try {
        write_json_atomic(temporary, {
            {"schema_version", 1}, {"lab_id", config.lab_id},
            {"hardware_id", hardware_id}, {"vm_id", vm->id}, {"bound_at", utc_timestamp()},
        });
        if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_WRITE_THROUGH)) {
            const DWORD error = GetLastError();
            if (error != ERROR_ALREADY_EXISTS && error != ERROR_FILE_EXISTS) {
                throw Error("Cannot publish initial Agent binding; Win32 error " +
                            std::to_string(error));
            }
        }
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        throw;
    }
    std::error_code ignored;
    std::filesystem::remove(temporary, ignored);
}

nlohmann::json bind_agent_hardware(
    const std::filesystem::path& config_path,
    const LabConfig& config,
    const std::string& vm_id,
    const std::string& hardware_id) {
    validate_identifier(vm_id, "hardware binding VM id");
    const std::string normalized = normalize_hardware_id(hardware_id);
    reject_hardware_identity_conflict(config, normalized);
    const VmConfig* vm = find_vm(config, vm_id);
    if (vm == nullptr) {
        throw Error("Agent hardware binding references an unknown VM: " + vm_id);
    }
    const VmConfig* existing = find_vm_by_hardware(config, normalized);
    if (existing != nullptr && existing->id != vm_id) {
        throw Error(
            "hardware_id is already bound to VM " + existing->id + ": " + normalized);
    }

    const std::filesystem::path presence_path = resolve_under_root(
        config.transport.state_root,
        std::filesystem::path(L"agents") / path_from_utf8(normalized + ".json"));
    if (!std::filesystem::is_regular_file(presence_path)) {
        throw Error("No online Agent presence for hardware_id " + normalized);
    }
    const nlohmann::json presence = load_json(presence_path);
    validate_presence_common(presence, config, normalized);

    nlohmann::json config_json = load_json(config_path);
    bool updated = false;
    for (auto& vm_json : config_json.at("vms")) {
        if (vm_json.value("id", std::string{}) == vm_id) {
            vm_json["hardware_id"] = normalized;
            updated = true;
            break;
        }
    }
    if (!updated) {
        throw Error("Cannot locate VM in lab configuration: " + vm_id);
    }

    const nlohmann::json binding = {
        {"schema_version", 1},
        {"lab_id", config.lab_id},
        {"hardware_id", normalized},
        {"vm_id", vm_id},
        {"bound_at", utc_timestamp()},
    };
    const std::filesystem::path binding_path = resolve_under_root(
        config.transport.state_root,
        std::filesystem::path(L"agents") /
            path_from_utf8(normalized + ".binding.json"));

    const nlohmann::json original_config = load_json(config_path);
    write_json_atomic(config_path, config_json);
    try {
        write_json_atomic(binding_path, binding);
    } catch (...) {
        write_json_atomic(config_path, original_config);
        throw;
    }
    return {
        {"status", "bound"},
        {"vm_id", vm_id},
        {"hardware_id", normalized},
        {"config", path_to_utf8(config_path)},
    };
}

}  // namespace satsuma::host
