// VM Agent 硬件身份初始化、绑定和迁移记录实现。
#include "hardware_identity.hpp"

#include <filesystem>
#include <string>

#include <nlohmann/json.hpp>

#include "satsuma/core/errors.hpp"
#include "satsuma/core/hardware_identity.hpp"
#include "satsuma/core/id.hpp"
#include "satsuma/core/json_io.hpp"
#include "satsuma/core/lifecycle.hpp"
#include "satsuma/core/path.hpp"

namespace satsuma::vm {
namespace {

// 缓存放在安装根目录，快照恢复时与 Agent 安装状态一起回退。
[[nodiscard]] std::filesystem::path hardware_cache_path(const AgentConfig& config) {
    const std::filesystem::path storage_root = config.storage_root.empty()
        ? config.local_work_root.parent_path()
        : config.storage_root;
    const std::filesystem::path agent_root =
        config.legacy_storage_layout || config.storage_root.empty()
        ? storage_root
        : storage_root / L"agent";
    return agent_root / L"hardware-id-cache.json";
}

// Host 以硬件 UUID 为键发布 VM 标识绑定。
[[nodiscard]] std::filesystem::path binding_path(const AgentConfig& config) {
    return config.channel_root / L"agents" /
        path_from_utf8(config.hardware_id + ".binding.json");
}

// 读取并严格验证当前硬件的绑定文件。
[[nodiscard]] std::string load_binding(const AgentConfig& config) {
    const nlohmann::json binding = load_json(binding_path(config));
    if (binding.value("schema_version", 0) != 1 ||
        binding.value("lab_id", std::string{}) != config.lab_id ||
        normalize_hardware_id(binding.value("hardware_id", std::string{})) !=
            config.hardware_id) {
        throw Error("Agent hardware binding does not match this machine");
    }
    const std::string vm_id = binding.value("vm_id", std::string{});
    validate_identifier(vm_id, "Agent hardware binding vm_id");
    return vm_id;
}

// 原子保存当前硬件及已确认的 VM 标识。
void write_hardware_cache(const AgentConfig& config) {
    nlohmann::json cache = {
        {"schema_version", 1},
        {"hardware_id", config.hardware_id},
        {"updated_at", utc_timestamp()},
    };
    if (!config.identity_unbound) {
        cache["vm_id"] = config.vm_id;
    }
    write_json_atomic(hardware_cache_path(config), cache);
}

}  // namespace

AgentConfig load_runtime_agent_config(const std::filesystem::path& config_path) {
    AgentConfig config = load_agent_config(config_path);
    prepare_agent_hardware_identity(config);
    return config;
}

void prepare_agent_hardware_identity(
    AgentConfig& config,
    const std::string& hardware_id) {
    config.hardware_id = normalize_hardware_id(
        hardware_id.empty() ? read_smbios_hardware_id() : hardware_id);
    config.previous_hardware_id.clear();
    config.previous_vm_id.clear();

    const std::filesystem::path cache_path = hardware_cache_path(config);
    std::string cached_vm_id; // 同一硬件上次经 Host 确认的逻辑标识
    if (std::filesystem::is_regular_file(cache_path)) {
        const nlohmann::json cache = load_json(cache_path);
        if (cache.value("schema_version", 0) != 1) {
            throw Error("Unsupported hardware identity cache schema version");
        }
        const std::string cached_hardware = normalize_hardware_id(
            cache.value("hardware_id", std::string{}));
        if (cached_hardware != config.hardware_id) {
            config.previous_hardware_id = cached_hardware;
            config.previous_vm_id = cache.value("vm_id", std::string{});
        } else {
            cached_vm_id = cache.value("vm_id", std::string{});
            if (!cached_vm_id.empty()) {
                validate_identifier(cached_vm_id, "cached Agent vm_id");
            }
        }
    }

    // 手工构造的旧配置也按显式 vm_id 处理，保持库调用兼容。
    config.vm_id_configured = config.vm_id_configured || !config.vm_id.empty();
    if (config.vm_id_configured) {
        validate_identifier(config.vm_id, "vm_id");
        config.identity_unbound = false;
    } else if (std::filesystem::is_regular_file(binding_path(config))) {
        config.vm_id = load_binding(config);
        config.identity_unbound = false;
    } else if (!cached_vm_id.empty()) {
        config.vm_id = cached_vm_id;
        config.identity_unbound = false;
    } else {
        config.vm_id = config.hardware_id;
        config.identity_unbound = true;
    }
    write_hardware_cache(config);
}

bool refresh_agent_binding(AgentConfig& config) {
    if (config.vm_id_configured || !std::filesystem::is_regular_file(binding_path(config))) {
        return false;
    }
    const std::string vm_id = load_binding(config);
    const bool changed = config.identity_unbound || config.vm_id != vm_id;
    config.vm_id = vm_id;
    config.identity_unbound = false;
    if (changed) {
        write_hardware_cache(config);
    }
    return changed;
}

std::filesystem::path hardware_presence_path(const AgentConfig& config) {
    if (config.hardware_id.empty()) {
        return config.channel_root / L"agents" / path_from_utf8(config.vm_id + ".json");
    }
    return config.channel_root / L"agents" /
        path_from_utf8(config.hardware_id + ".json");
}

void write_hardware_migration_marker(const AgentConfig& config) {
    if (config.previous_hardware_id.empty()) {
        return;
    }
    nlohmann::json marker = {
        {"schema_version", 1},
        {"lab_id", config.lab_id},
        {"old_hardware_id", config.previous_hardware_id},
        {"hardware_id", config.hardware_id},
        {"status", "migrated"},
        {"observed_at", utc_timestamp()},
    };
    if (!config.previous_vm_id.empty()) {
        marker["old_vm_id"] = config.previous_vm_id;
    }
    write_json_atomic(
        config.channel_root / L"agents" /
            path_from_utf8(config.previous_hardware_id + ".migrated.json"),
        marker);
}

}  // namespace satsuma::vm
