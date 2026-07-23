// Satsuma 任务清单和执行结果 JSON 实现。
#include "satsuma/core/task.hpp"

#include <algorithm>
#include <cctype>
#include <set>

#include <nlohmann/json.hpp>

#include "satsuma/core/errors.hpp"
#include "satsuma/core/json_io.hpp"
#include "satsuma/core/path.hpp"

namespace satsuma {
namespace {

// 读取非空必需字符串字段。
[[nodiscard]] std::string required_string(const nlohmann::json& value, const char* field) {
    if (!value.contains(field) || !value.at(field).is_string()) {
        throw Error(std::string("Missing or invalid string field: ") + field);
    }
    const std::string result = value.at(field).get<std::string>();
    if (result.empty()) {
        throw Error(std::string("Task field must not be empty: ") + field);
    }
    return result;
}

// 验证 SHA-256 使用 64 位小写十六进制格式。
void validate_sha256(const std::string& hash) {
    const bool valid = hash.size() == 64 && std::all_of(
        hash.begin(),
        hash.end(),
        [](const unsigned char value) { return std::isdigit(value) || (value >= 'a' && value <= 'f'); });
    if (!valid) {
        throw Error("sha256 must contain 64 lowercase hexadecimal characters");
    }
}

// 限制任务 ID 和 VM ID 为安全的 ASCII 标识符。
void validate_identifier(const std::string& identifier, const char* field) {
    const bool valid = !identifier.empty() && identifier.size() <= 128 && std::all_of(
        identifier.begin(),
        identifier.end(),
        [](const unsigned char value) {
            return std::isalnum(value) || value == '-' || value == '_';
        });
    if (!valid) {
        throw Error(std::string(field) + " may contain only letters, numbers, '-' and '_'");
    }
}

// 从 JSON 解析并验证一个任务步骤。
[[nodiscard]] TaskStep parse_step(const nlohmann::json& value) {
    TaskStep step;
    step.id = required_string(value, "id");
    step.vm = required_string(value, "vm");
    validate_identifier(step.id, "step id");
    validate_identifier(step.vm, "step VM id");
    step.type = required_string(value, "type");
    step.timeout_seconds = value.value("timeout_seconds", 120);
    if (step.timeout_seconds < 1 || step.timeout_seconds > 86'400) {
        throw Error("timeout_seconds must be between 1 and 86400 for step " + step.id);
    }

    if (value.contains("arguments")) {
        step.arguments = value.at("arguments").get<std::vector<std::string>>();
    }
    if (value.contains("collect_files")) {
        for (const std::string& file : value.at("collect_files").get<std::vector<std::string>>()) {
            std::filesystem::path relative = path_from_utf8(file);
            validate_relative_path(relative);
            step.collect_files.push_back(std::move(relative));
        }
    }

    if (step.type == "execute") {
        step.program = path_from_utf8(required_string(value, "program"));
        validate_relative_path(step.program);
    } else if (step.type == "echo") {
        step.message = required_string(value, "message");
        if (!step.arguments.empty() || !step.collect_files.empty()) {
            throw Error("echo step does not accept arguments or collect_files: " + step.id);
        }
    } else {
        throw Error("Unsupported task step type: " + step.type);
    }
    return step;
}

// 将一个任务步骤转换为稳定的 JSON 表示。
[[nodiscard]] nlohmann::json serialize_step(const TaskStep& step) {
    nlohmann::json value = {
        {"id", step.id},
        {"vm", step.vm},
        {"type", step.type},
        {"timeout_seconds", step.timeout_seconds},
    };
    if (step.type == "execute") {
        value["program"] = path_to_utf8(step.program);
        value["arguments"] = step.arguments;
        std::vector<std::string> files;
        files.reserve(step.collect_files.size());
        for (const auto& file : step.collect_files) {
            files.push_back(path_to_utf8(file));
        }
        value["collect_files"] = files;
    } else {
        value["message"] = step.message;
    }
    return value;
}

// 验证步骤 ID 和 Artifact 目标不重复。
void validate_plan_uniqueness(const TaskPlan& plan) {
    std::set<std::string> step_ids;
    for (const auto& step : plan.steps) {
        if (!step_ids.insert(step.id).second) {
            throw Error("Duplicate task step id: " + step.id);
        }
    }

    std::set<std::string> artifact_paths;
    for (const auto& artifact : plan.artifacts) {
        const std::string key = artifact.vm + "\n" + path_to_utf8(artifact.destination);
        if (!artifact_paths.insert(key).second) {
            throw Error("Duplicate artifact destination: " + path_to_utf8(artifact.destination));
        }
    }
}

}  // namespace

TaskPlan load_task_plan(const std::filesystem::path& path) {
    const nlohmann::json value = load_json(path);
    if (value.value("schema_version", 0) != 1) {
        throw Error("Task plan requires schema_version 1");
    }

    TaskPlan plan;
    plan.schema_version = 1;
    plan.name = required_string(value, "name");
    if (value.contains("run_id")) {
        plan.run_id = required_string(value, "run_id");
    }

    if (value.contains("artifacts")) {
        if (!value.at("artifacts").is_array()) {
            throw Error("artifacts must be an array");
        }
        for (const auto& artifact_value : value.at("artifacts")) {
            ArtifactInput artifact;
            artifact.source = path_from_utf8(required_string(artifact_value, "source"));
            artifact.vm = required_string(artifact_value, "vm");
            validate_identifier(artifact.vm, "Artifact VM id");
            artifact.destination = path_from_utf8(required_string(artifact_value, "shared_destination"));
            validate_relative_path(artifact.destination);
            if (!artifact.source.is_absolute()) {
                throw Error("Artifact source must be an absolute Host path: " + path_to_utf8(artifact.source));
            }
            if (artifact_value.contains("sha256")) {
                artifact.sha256 = required_string(artifact_value, "sha256");
                validate_sha256(*artifact.sha256);
            }
            plan.artifacts.push_back(std::move(artifact));
        }
    }

    if (!value.contains("steps") || !value.at("steps").is_array() || value.at("steps").empty()) {
        throw Error("Task plan must contain at least one step");
    }
    for (const auto& step_value : value.at("steps")) {
        plan.steps.push_back(parse_step(step_value));
    }

    validate_plan_uniqueness(plan);
    return plan;
}

RunManifest load_run_manifest(const std::filesystem::path& path) {
    try {
        return load_json(path).get<RunManifest>();
    } catch (const nlohmann::json::exception& error) {
        throw Error("Invalid run manifest: " + std::string(error.what()));
    }
}

void to_json(nlohmann::json& value, const RunManifest& manifest) {
    value = {
        {"schema_version", manifest.schema_version},
        {"protocol_version", manifest.protocol_version},
        {"lab_id", manifest.lab_id},
        {"run_id", manifest.run_id},
        {"request_id", manifest.request_id},
        {"name", manifest.name},
        {"created_at", manifest.created_at},
        {"artifacts", nlohmann::json::array()},
        {"steps", nlohmann::json::array()},
    };
    for (const auto& artifact : manifest.artifacts) {
        value["artifacts"].push_back({
            {"vm", artifact.vm},
            {"path", path_to_utf8(artifact.path)},
            {"sha256", artifact.sha256},
        });
    }
    for (const auto& step : manifest.steps) {
        value["steps"].push_back(serialize_step(step));
    }
}

void from_json(const nlohmann::json& value, RunManifest& manifest) {
    manifest.schema_version = value.value("schema_version", 0);
    manifest.protocol_version = value.value("protocol_version", 0);
    manifest.lab_id = required_string(value, "lab_id");
    manifest.run_id = required_string(value, "run_id");
    manifest.request_id = required_string(value, "request_id");
    manifest.name = required_string(value, "name");
    manifest.created_at = required_string(value, "created_at");
    if (manifest.schema_version != 1 || manifest.protocol_version != 1) {
        throw Error("Unsupported run manifest schema or protocol version");
    }

    manifest.artifacts.clear();
    for (const auto& artifact_value : value.at("artifacts")) {
        ArtifactManifest artifact;
        artifact.vm = required_string(artifact_value, "vm");
        validate_identifier(artifact.vm, "Artifact VM id");
        artifact.path = path_from_utf8(required_string(artifact_value, "path"));
        artifact.sha256 = required_string(artifact_value, "sha256");
        validate_relative_path(artifact.path);
        validate_sha256(artifact.sha256);
        manifest.artifacts.push_back(std::move(artifact));
    }

    manifest.steps.clear();
    for (const auto& step_value : value.at("steps")) {
        manifest.steps.push_back(parse_step(step_value));
    }
}

void to_json(nlohmann::json& value, const ExecutionResult& result) {
    value = {
        {"schema_version", result.schema_version},
        {"run_id", result.run_id},
        {"vm_id", result.vm_id},
        {"job_id", result.job_id},
        {"step_id", result.step_id},
        {"status", result.status},
        {"timed_out", result.timed_out},
        {"duration_ms", result.duration_ms},
        {"stdout", result.stdout_path},
        {"stderr", result.stderr_path},
        {"files", nlohmann::json::array()},
        {"error", result.error},
        {"started_at", result.started_at},
        {"finished_at", result.finished_at},
    };
    value["exit_code"] = result.exit_code.has_value()
        ? nlohmann::json(*result.exit_code)
        : nlohmann::json(nullptr);
    for (const auto& file : result.files) {
        value["files"].push_back({{"path", file.path}, {"sha256", file.sha256}});
    }
}

void from_json(const nlohmann::json& value, ExecutionResult& result) {
    result.schema_version = value.value("schema_version", 0);
    result.run_id = required_string(value, "run_id");
    result.vm_id = required_string(value, "vm_id");
    result.job_id = required_string(value, "job_id");
    result.step_id = required_string(value, "step_id");
    result.status = required_string(value, "status");
    if (value.contains("exit_code") && !value.at("exit_code").is_null()) {
        result.exit_code = value.at("exit_code").get<std::uint32_t>();
    } else {
        result.exit_code.reset();
    }
    result.timed_out = value.value("timed_out", false);
    result.duration_ms = value.value("duration_ms", 0LL);
    result.stdout_path = value.value("stdout", "");
    result.stderr_path = value.value("stderr", "");
    result.files.clear();
    for (const auto& file_value : value.value("files", nlohmann::json::array())) {
        CollectedFile file;
        file.path = required_string(file_value, "path");
        file.sha256 = required_string(file_value, "sha256");
        validate_sha256(file.sha256);
        result.files.push_back(std::move(file));
    }
    result.error = value.value("error", "");
    result.started_at = required_string(value, "started_at");
    result.finished_at = required_string(value, "finished_at");
    if (result.schema_version != 1) {
        throw Error("Unsupported execution result schema version");
    }
}

}  // namespace satsuma
