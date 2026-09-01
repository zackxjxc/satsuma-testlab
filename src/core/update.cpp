// SatsumaVM 独立自更新清单和结果 JSON 实现。
#include "satsuma/core/update.hpp"

#include <algorithm>
#include <cctype>
#include <limits>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "satsuma/core/errors.hpp"
#include "satsuma/core/id.hpp"
#include "satsuma/core/json_contract.hpp"
#include "satsuma/core/json_io.hpp"
#include "satsuma/core/path.hpp"

namespace satsuma {
namespace {

// 读取非空必需字符串字段。
[[nodiscard]] std::string required_string(
    const nlohmann::json& value,
    const char* field) {
    return required_non_empty_string(value, field, "Update field");
}

// 验证 SHA-256 使用 64 位小写十六进制格式。
void validate_sha256(const std::string& hash) {
    const bool valid = hash.size() == 64 && std::all_of(
        hash.begin(),
        hash.end(),
        [](const unsigned char value) {
            return std::isdigit(value) != 0 || (value >= 'a' && value <= 'f');
        });
    if (!valid) {
        throw Error("Update sha256 must contain 64 lowercase hexadecimal characters");
    }
}

// 读取正数候选大小。
[[nodiscard]] std::uint64_t required_size(const nlohmann::json& value) {
    if (!value.contains("size") || !value.at("size").is_number_integer()) {
        throw Error("Update size must be a positive integer");
    }
    const std::int64_t size = value.at("size").get<std::int64_t>();
    if (size <= 0) {
        throw Error("Update size must be a positive integer");
    }
    return static_cast<std::uint64_t>(size);
}

// 验证固定版本的更新清单。
void validate_manifest(const AgentUpdateManifest& manifest) {
    if (manifest.schema_version != 1 || manifest.type != "update_agent") {
        throw Error("Agent update requires schema_version 1 and type update_agent");
    }
    if (manifest.protocol_version == 1) {
        if (manifest.next_vm_id.has_value()) {
            throw Error("Agent update protocol 1 does not support next_vm_id");
        }
    } else if (manifest.protocol_version == 2) {
        if (!manifest.next_vm_id.has_value()) {
            throw Error("Agent update protocol 2 requires next_vm_id");
        }
    } else {
        throw Error("Agent update protocol_version must be 1 or 2");
    }
    validate_identifier(manifest.lab_id, "update lab_id");
    validate_identifier(manifest.vm_id, "update vm_id");
    if (manifest.next_vm_id.has_value()) {
        validate_identifier(*manifest.next_vm_id, "update next_vm_id");
        if (*manifest.next_vm_id == manifest.vm_id) {
            throw Error("Agent update next_vm_id must differ from vm_id");
        }
    }
    validate_identifier(manifest.update_id, "update_id");
    if (manifest.version.empty() ||
        manifest.version.size() > 64 ||
        manifest.version.find('\0') != std::string::npos) {
        throw Error("Update version must contain between 1 and 64 characters");
    }
    validate_relative_path(manifest.binary);
    if (manifest.binary.parent_path() != std::filesystem::path{}) {
        throw Error("Update binary must be a file name within its update directory");
    }
    if (manifest.size == 0) {
        throw Error("Update size must be positive");
    }
    validate_sha256(manifest.sha256);
    if (manifest.created_at.empty()) {
        throw Error("Update created_at must not be empty");
    }
}

// 验证更新终态结果。
void validate_result(const AgentUpdateResult& result) {
    if (result.schema_version != 1) {
        throw Error("Agent update result requires schema_version 1");
    }
    validate_identifier(result.update_id, "update result update_id");
    validate_identifier(result.vm_id, "update result vm_id");
    if (result.version.empty() || result.version.size() > 64) {
        throw Error("Update result version must contain between 1 and 64 characters");
    }
    if (result.status != "succeeded" && result.status != "failed") {
        throw Error("Update result status must be succeeded or failed");
    }
    if (result.rollback_status != "none" &&
        result.rollback_status != "succeeded" &&
        result.rollback_status != "failed") {
        throw Error("Update rollback_status must be none, succeeded or failed");
    }
    if (result.status == "succeeded" &&
        (result.process_id == 0 ||
         result.rollback_status != "none" ||
         !result.error.empty())) {
        throw Error("Successful update result has inconsistent fields");
    }
    if (result.status == "failed" && result.error.empty()) {
        throw Error("Failed update result requires an error");
    }
    if (result.completed_at.empty()) {
        throw Error("Update result completed_at must not be empty");
    }
}

}  // namespace

AgentUpdateManifest load_agent_update_manifest(
    const std::filesystem::path& path) {
    try {
        return load_json(path).get<AgentUpdateManifest>();
    } catch (const nlohmann::json::exception& error) {
        throw Error("Invalid agent update manifest: " + std::string(error.what()));
    }
}

AgentUpdateResult load_agent_update_result(
    const std::filesystem::path& path) {
    try {
        return load_json(path).get<AgentUpdateResult>();
    } catch (const nlohmann::json::exception& error) {
        throw Error("Invalid agent update result: " + std::string(error.what()));
    }
}

void to_json(nlohmann::json& value, const AgentUpdateManifest& manifest) {
    value = {
        {"schema_version", manifest.schema_version},
        {"protocol_version", manifest.protocol_version},
        {"type", manifest.type},
        {"lab_id", manifest.lab_id},
        {"vm_id", manifest.vm_id},
        {"update_id", manifest.update_id},
        {"version", manifest.version},
        {"binary", path_to_utf8(manifest.binary)},
        {"size", manifest.size},
        {"sha256", manifest.sha256},
        {"created_at", manifest.created_at},
    };
    if (manifest.next_vm_id.has_value()) {
        value["next_vm_id"] = *manifest.next_vm_id;
    }
}

void from_json(const nlohmann::json& value, AgentUpdateManifest& manifest) {
    reject_unknown_fields(
        value,
        {"schema_version", "protocol_version", "type", "lab_id", "vm_id", "next_vm_id",
         "update_id", "version", "binary", "size", "sha256", "created_at"},
        "agent update manifest");
    manifest.schema_version = value.value("schema_version", 0);
    manifest.protocol_version = value.value("protocol_version", 0);
    manifest.type = required_string(value, "type");
    manifest.lab_id = required_string(value, "lab_id");
    manifest.vm_id = required_string(value, "vm_id");
    if (value.contains("next_vm_id")) {
        manifest.next_vm_id = required_string(value, "next_vm_id");
    } else {
        manifest.next_vm_id.reset();
    }
    manifest.update_id = required_string(value, "update_id");
    manifest.version = required_string(value, "version");
    manifest.binary = path_from_utf8(required_string(value, "binary"));
    manifest.size = required_size(value);
    manifest.sha256 = required_string(value, "sha256");
    manifest.created_at = required_string(value, "created_at");
    validate_manifest(manifest);
}

void to_json(nlohmann::json& value, const AgentUpdateResult& result) {
    value = {
        {"schema_version", result.schema_version},
        {"update_id", result.update_id},
        {"vm_id", result.vm_id},
        {"version", result.version},
        {"status", result.status},
        {"rollback_status", result.rollback_status},
        {"process_id", result.process_id},
        {"error", result.error},
        {"completed_at", result.completed_at},
    };
}

void from_json(const nlohmann::json& value, AgentUpdateResult& result) {
    reject_unknown_fields(
        value,
        {"schema_version", "update_id", "vm_id", "version", "status", "rollback_status",
         "process_id", "error", "completed_at"},
        "agent update result");
    result.schema_version = value.value("schema_version", 0);
    result.update_id = required_string(value, "update_id");
    result.vm_id = required_string(value, "vm_id");
    result.version = required_string(value, "version");
    result.status = required_string(value, "status");
    result.rollback_status = required_string(value, "rollback_status");
    result.process_id = value.value("process_id", std::uint32_t{});
    result.error = value.value("error", std::string{});
    result.completed_at = required_string(value, "completed_at");
    validate_result(result);
}

}  // namespace satsuma
