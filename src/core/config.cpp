// Satsuma Host 和 VM 配置解析实现。
#include "satsuma/core/config.hpp"

#include <algorithm>
#include <set>

#include <nlohmann/json.hpp>

#include "satsuma/core/errors.hpp"
#include "satsuma/core/endpoint.hpp"
#include "satsuma/core/id.hpp"
#include "satsuma/core/json_io.hpp"
#include "satsuma/core/path.hpp"

namespace satsuma {
namespace {

// 读取必需字符串字段并统一空值错误。
[[nodiscard]] std::string required_string(const nlohmann::json& value, const char* field) {
    if (!value.contains(field) || !value.at(field).is_string()) {
        throw Error(std::string("Missing or invalid string field: ") + field);
    }
    const std::string result = value.at(field).get<std::string>();
    if (result.empty()) {
        throw Error(std::string("Configuration field must not be empty: ") + field);
    }
    return result;
}

// 读取必需整数并拒绝 JSON 隐式类型转换。
[[nodiscard]] int required_integer(const nlohmann::json& value, const char* field) {
    if (!value.contains(field) || !value.at(field).is_number_integer()) {
        throw Error(std::string("Missing or invalid integer field: ") + field);
    }
    return value.at(field).get<int>();
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
    validate_schema_version(value, "lab.json");

    LabConfig config;
    config.schema_version = 1;
    config.lab_id = required_string(value, "lab_id");
    validate_identifier(config.lab_id, "lab_id");

    const auto& provider = value.at("provider");
    config.provider.type = required_string(provider, "type");
    config.provider.vmrun = path_from_utf8(required_string(provider, "vmrun"));
    if (config.provider.type != "vmware_workstation") {
        throw Error("Unsupported provider type: " + config.provider.type);
    }

    const auto& host = value.at("host");
    config.host.listen = required_string(host, "listen");
    config.host.archive_root = path_from_utf8(required_string(host, "archive_root"));

    const auto& shared_folder = value.at("shared_folder");
    config.shared_folder.host_root = path_from_utf8(required_string(shared_folder, "host_root"));
    config.shared_folder.guest_root = required_string(shared_folder, "guest_root");

    if (!value.contains("vms") || !value.at("vms").is_array() || value.at("vms").empty()) {
        throw Error("lab.json must contain at least one VM");
    }

    std::set<std::string> vm_ids;
    for (const auto& vm_value : value.at("vms")) {
        VmConfig vm;
        vm.id = required_string(vm_value, "id");
        validate_identifier(vm.id, "VM id");
        vm.role = vm_value.value("role", vm.id);
        vm.vmx = path_from_utf8(required_string(vm_value, "vmx"));
        vm.agent_version = required_string(vm_value, "agent_version");
        if (!vm_value.contains("snapshots") || !vm_value.at("snapshots").is_object()) {
            throw Error("Missing or invalid snapshots object for VM: " + vm.id);
        }
        const auto& snapshots = vm_value.at("snapshots");
        vm.snapshots.base = required_string(snapshots, "base");
        vm.snapshots.ai_prefix = required_string(snapshots, "ai_prefix");
        vm.snapshots.max_ai_snapshots = required_integer(snapshots, "max_ai_snapshots");
        validate_snapshot_config(vm.snapshots);
        if (vm_value.contains("management_ip")) {
            vm.management_ip = required_string(vm_value, "management_ip");
        }
        if (!vm_ids.insert(vm.id).second) {
            throw Error("Duplicate VM id in lab.json: " + vm.id);
        }
        config.vms.push_back(std::move(vm));
    }
    return config;
}

AgentConfig load_agent_config(const std::filesystem::path& path) {
    const nlohmann::json value = load_json(path);
    validate_schema_version(value, "agent.json");

    AgentConfig config;
    config.schema_version = 1;
    config.protocol_version = value.value("protocol_version", 0);
    config.lab_id = required_string(value, "lab_id");
    config.vm_id = required_string(value, "vm_id");
    config.agent_version = required_string(value, "agent_version");
    config.last_update_id = value.value("last_update_id", std::string{});
    config.host = required_string(value, "host");
    validate_identifier(config.lab_id, "lab_id");
    validate_identifier(config.vm_id, "vm_id");
    config.shared_root = path_from_utf8(required_string(value, "shared_root"));
    config.local_work_root = path_from_utf8(required_string(value, "local_work_root"));
    config.poll_interval_ms = value.value("poll_interval_ms", 1000);
    config.reconnect_interval_ms = value.value("reconnect_interval_ms", 1000);
    config.rpc_timeout_ms = value.value("rpc_timeout_ms", 5000);

    if (config.protocol_version != 1) {
        throw Error("agent.json requires protocol_version 1");
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
    static_cast<void>(parse_tcp_endpoint(config.host));
    if (config.reconnect_interval_ms < 100 || config.reconnect_interval_ms > 60'000) {
        throw Error("reconnect_interval_ms must be between 100 and 60000");
    }
    if (config.rpc_timeout_ms < 100 || config.rpc_timeout_ms > 300'000) {
        throw Error("rpc_timeout_ms must be between 100 and 300000");
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
