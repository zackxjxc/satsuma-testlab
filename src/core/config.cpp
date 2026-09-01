// Satsuma Host 和 VM 配置解析实现。
#include "satsuma/core/config.hpp"

#include <algorithm>
#include <limits>
#include <set>
#include <string_view>

#include <nlohmann/json.hpp>
#include <windows.h>

#include "satsuma/core/errors.hpp"
#include "satsuma/core/hardware_identity.hpp"
#include "satsuma/core/id.hpp"
#include "satsuma/core/json_contract.hpp"
#include "satsuma/core/json_io.hpp"
#include "satsuma/core/path.hpp"

namespace satsuma {
namespace {

// 读取必需字符串字段并统一空值错误。
[[nodiscard]] std::string required_string(const nlohmann::json& value, const char* field) {
    return required_non_empty_string(value, field, "Configuration field");
}

// 读取必需整数并拒绝 JSON 隐式类型转换。
[[nodiscard]] int required_integer(const nlohmann::json& value, const char* field) {
    if (!value.contains(field) || !value.at(field).is_number_integer()) {
        throw Error(std::string("Missing or invalid integer field: ") + field);
    }
    return value.at(field).get<int>();
}

// 配置中的运行路径必须独立于 Host 或 Windows Service 的当前目录。
void require_absolute_path(const std::filesystem::path& path, const std::string_view field) {
    if (!path.is_absolute()) {
        throw Error("Configuration path must be absolute: " + std::string(field));
    }
}

// 验证基础快照不会被 AI 快照所有权规则覆盖。
void validate_snapshot_config(const SnapshotConfig& snapshots) {
    if (snapshots.base.size() > 128 || snapshots.base.find('\0') != std::string::npos) {
        throw Error("Snapshot base name must contain between 1 and 128 non-NUL characters");
    }
    validate_identifier(snapshots.ai_prefix, "snapshot ai_prefix");
    if (snapshots.base.starts_with(snapshots.ai_prefix)) {
        throw Error("Snapshot base name must not use the AI snapshot prefix");
    }
    if (snapshots.max_ai_snapshots < 1 || snapshots.max_ai_snapshots > 64) {
        throw Error("max_ai_snapshots must be between 1 and 64");
    }
}

// 检查当前仅支持的 schema 版本。
void validate_schema_version(const nlohmann::json& value, const char* source) {
    const int version = value.value("schema_version", 0);
    if (version != 1) {
        throw Error(std::string(source) + " requires schema_version 1");
    }
}

}  // namespace

LabConfig load_lab_config(const std::filesystem::path& path) {
    const nlohmann::json value = load_json(path);
    reject_unknown_fields(
        value,
        {"$schema", "schema_version", "lab_id", "provider", "host", "transport", "vms"},
        "lab configuration");
    validate_schema_version(value, "lab.json");

    LabConfig config;
    config.lab_id = required_string(value, "lab_id");
    validate_identifier(config.lab_id, "lab_id");

    const auto& provider = value.at("provider");
    reject_unknown_fields(provider, {"type", "vmrun"}, "provider configuration");
    config.provider.type = required_string(provider, "type");
    config.provider.vmrun = path_from_utf8(required_string(provider, "vmrun"));
    require_absolute_path(config.provider.vmrun, "provider.vmrun");
    if (config.provider.type != "vmware_workstation") {
        throw Error("Unsupported provider type: " + config.provider.type);
    }

    const auto& host = value.at("host");
    reject_unknown_fields(host, {"archive_root"}, "host configuration");
    config.host.archive_root = path_from_utf8(required_string(host, "archive_root"));
    require_absolute_path(config.host.archive_root, "host.archive_root");

    const auto& transport = value.at("transport");
    reject_unknown_fields(
        transport,
        {"state_root", "vmci_port"},
        "Host transport configuration");
    config.transport.state_root = path_from_utf8(required_string(transport, "state_root"));
    require_absolute_path(config.transport.state_root, "transport.state_root");
    const std::uint64_t vmci_port = transport.at("vmci_port").get<std::uint64_t>();
    if (vmci_port == 0 || vmci_port >= std::numeric_limits<std::uint32_t>::max()) {
        throw Error("transport.vmci_port must be between 1 and 4294967294");
    }
    config.transport.vmci_port = static_cast<std::uint32_t>(vmci_port);

    if (!value.contains("vms") || !value.at("vms").is_array() || value.at("vms").empty()) {
        throw Error("lab.json must contain at least one VM");
    }

    std::set<std::string> vm_ids;
    std::set<std::string> hardware_ids;
    for (const auto& vm_value : value.at("vms")) {
        reject_unknown_fields(
            vm_value,
            {"id", "hardware_id", "vmx", "agent_version", "snapshots"},
            "VM configuration");
        VmConfig vm;
        vm.id = required_string(vm_value, "id");
        validate_identifier(vm.id, "VM id");
        if (vm_value.contains("hardware_id")) {
            if (!vm_value.at("hardware_id").is_string()) {
                throw Error("Invalid string field: hardware_id");
            }
            vm.hardware_id = normalize_hardware_id(
                vm_value.at("hardware_id").get<std::string>());
        }
        vm.vmx = path_from_utf8(required_string(vm_value, "vmx"));
        require_absolute_path(vm.vmx, "vms[].vmx for " + vm.id);
        vm.agent_version = required_string(vm_value, "agent_version");
        if (!vm_value.contains("snapshots") || !vm_value.at("snapshots").is_object()) {
            throw Error("Missing or invalid snapshots object for VM: " + vm.id);
        }
        const auto& snapshots = vm_value.at("snapshots");
        reject_unknown_fields(
            snapshots,
            {"base", "ai_prefix", "max_ai_snapshots"},
            "snapshot configuration");
        vm.snapshots.base = required_string(snapshots, "base");
        vm.snapshots.ai_prefix = required_string(snapshots, "ai_prefix");
        vm.snapshots.max_ai_snapshots = required_integer(snapshots, "max_ai_snapshots");
        validate_snapshot_config(vm.snapshots);
        if (!vm_ids.insert(vm.id).second) {
            throw Error("Duplicate VM id in lab.json: " + vm.id);
        }
        if (!vm.hardware_id.empty() && !hardware_ids.insert(vm.hardware_id).second) {
            throw Error("Duplicate hardware_id in lab.json: " + vm.hardware_id);
        }
        config.vms.push_back(std::move(vm));
    }
    return config;
}

AgentConfig load_agent_config(const std::filesystem::path& path) {
    const nlohmann::json value = load_json(path);
    reject_unknown_fields(
        value,
        {"$schema", "schema_version", "protocol_version", "lab_id", "vm_id",
         "agent_version", "last_update_id", "transport",
         "storage_root", "mirror_root", "poll_interval_ms", "reconnect_interval_ms"},
        "Agent configuration");
    validate_schema_version(value, "agent.json");

    AgentConfig config;
    config.protocol_version = value.value("protocol_version", 0);
    config.lab_id = required_string(value, "lab_id");
    config.vm_id_configured = value.contains("vm_id");
    if (config.vm_id_configured) {
        config.vm_id = required_string(value, "vm_id");
    }
    config.agent_version = required_string(value, "agent_version");
    config.last_update_id = value.value("last_update_id", std::string{});
    validate_identifier(config.lab_id, "lab_id");
    if (config.vm_id_configured) {
        validate_identifier(config.vm_id, "vm_id");
    }
    config.storage_root = path_from_utf8(required_string(value, "storage_root"));
    require_absolute_path(config.storage_root, "storage_root");
    config.local_work_root = config.storage_root / L"work";
    config.mirror_root = path_from_utf8(required_string(value, "mirror_root"));
    require_absolute_path(config.mirror_root, "mirror_root");
    const std::filesystem::path storage_drive = config.storage_root.root_path();
    if (storage_drive.empty() || GetDriveTypeW(storage_drive.c_str()) != DRIVE_FIXED) {
        throw Error("storage_root must be located on a local fixed drive");
    }
    const std::filesystem::path mirror_drive = config.mirror_root.root_path();
    if (mirror_drive.empty() || GetDriveTypeW(mirror_drive.c_str()) != DRIVE_FIXED) {
        throw Error("mirror_root must be located on a local fixed drive");
    }
    const auto& transport = value.at("transport");
    reject_unknown_fields(
        transport,
        {"host_cid", "vmci_port", "request_timeout_ms"},
        "Agent transport configuration");
    const std::uint64_t host_cid = transport.at("host_cid").get<std::uint64_t>();
    const std::uint64_t vmci_port = transport.at("vmci_port").get<std::uint64_t>();
    if (host_cid >= std::numeric_limits<std::uint32_t>::max()) {
        throw Error("transport.host_cid must be between 0 and 4294967294");
    }
    if (vmci_port == 0 || vmci_port >= std::numeric_limits<std::uint32_t>::max()) {
        throw Error("transport.vmci_port must be between 1 and 4294967294");
    }
    config.transport.host_cid = static_cast<std::uint32_t>(host_cid);
    config.transport.vmci_port = static_cast<std::uint32_t>(vmci_port);
    config.transport.request_timeout_ms = transport.value("request_timeout_ms", 10'000);
    if (config.transport.request_timeout_ms < 100 ||
        config.transport.request_timeout_ms > 60'000) {
        throw Error("transport.request_timeout_ms must be between 100 and 60000");
    }
    config.poll_interval_ms = value.value("poll_interval_ms", 1000);
    config.reconnect_interval_ms = value.value("reconnect_interval_ms", 1000);

    if (config.protocol_version != kRunManifestProtocolVersion) {
        throw Error("agent.json requires protocol_version 4");
    }
    if (config.poll_interval_ms < 100 || config.poll_interval_ms > 60'000) {
        throw Error("poll_interval_ms must be between 100 and 60000");
    }
    if (config.agent_version.size() > 64) {
        throw Error("agent_version must not exceed 64 characters");
    }
    if (!config.last_update_id.empty()) {
        validate_identifier(config.last_update_id, "last_update_id");
    }
    if (config.reconnect_interval_ms < 100 || config.reconnect_interval_ms > 60'000) {
        throw Error("reconnect_interval_ms must be between 100 and 60000");
    }
    return config;
}

const VmConfig* find_vm(const LabConfig& config, const std::string& vm_id) {
    const auto match = std::find_if(
        config.vms.begin(),
        config.vms.end(),
        [&vm_id](const VmConfig& vm) { return vm.id == vm_id; });
    return match == config.vms.end() ? nullptr : &*match;
}

}  // namespace satsuma
