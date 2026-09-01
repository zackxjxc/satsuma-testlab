// Satsuma 任务清单和执行结果 JSON 实现。
#include "satsuma/core/task.hpp"

#include <algorithm>
#include <cctype>
#include <cwctype>
#include <set>
#include <string_view>

#include <nlohmann/json.hpp>

#include "satsuma/core/errors.hpp"
#include "satsuma/core/id.hpp"
#include "satsuma/core/json_contract.hpp"
#include "satsuma/core/json_io.hpp"
#include "satsuma/core/path.hpp"

namespace satsuma {
namespace {

// 区分用户任务与 Agent 可见的物化清单。
enum class StepParseMode {
    TaskPlan,
    RunManifest,
};

// 生成 Windows 等价相对路径的大小写无关比较键。
[[nodiscard]] std::wstring windows_path_key(std::filesystem::path value) {
    value = value.lexically_normal();
    value.make_preferred();
    std::wstring key = value.native();
    std::transform(key.begin(), key.end(), key.begin(), [](const wchar_t character) {
        return static_cast<wchar_t>(std::towlower(character));
    });
    return key;
}

// 读取非空必需字符串字段。
[[nodiscard]] std::string required_string(const nlohmann::json& value, const char* field) {
    return required_non_empty_string(value, field, "Task field");
}

// 解析仅允许 SYSTEM 或当前交互用户的运行身份。
[[nodiscard]] TaskRunAs parse_task_run_as(const nlohmann::json& value) {
    const std::string name = required_string(value, "run_as");
    if (name == "system") {
        return TaskRunAs::System;
    }
    if (name == "interactive_user") {
        return TaskRunAs::InteractiveUser;
    }
    throw Error("Unsupported task run_as: " + name);
}

// 解析 script 步骤的固定解释器。
[[nodiscard]] ScriptEngine parse_script_engine(const nlohmann::json& value) {
    const std::string name = required_string(value, "engine");
    if (name == "cmd") {
        return ScriptEngine::Cmd;
    }
    if (name == "windows_powershell") {
        return ScriptEngine::WindowsPowerShell;
    }
    if (name == "pwsh") {
        return ScriptEngine::Pwsh;
    }
    throw Error("Unsupported script engine: " + name);
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

// 按任务计划或 VMCI 运行清单解析并验证一个步骤。
[[nodiscard]] TaskStep parse_step(const nlohmann::json& value, const StepParseMode mode) {
    const std::string type = required_string(value, "type");
    if (type == "execute") {
        reject_unknown_fields(
            value,
            {"id", "vm", "type", "program", "arguments", "run_as", "timeout_seconds",
             "retry_safe", "collect_files"},
            "execute step");
    } else if (type == "script") {
        reject_unknown_fields(
            value,
            {"id", "vm", "type", "engine", "script", "arguments", "run_as",
             "timeout_seconds", "retry_safe", "collect_files"},
            "script step");
    } else if (type == "echo") {
        reject_unknown_fields(
            value,
            {"id", "vm", "type", "message", "retry_safe"},
            "echo step");
    }

    TaskStep step;
    step.id = required_string(value, "id");
    step.vm = required_string(value, "vm");
    validate_identifier(step.id, "step id");
    validate_identifier(step.vm, "step VM id");
    step.type = type;
    step.retry_safe = value.value("retry_safe", step.type == "echo");

    if (value.contains("arguments")) {
        step.arguments = value.at("arguments").get<std::vector<std::string>>();
        if (step.arguments.size() > kMaxArgumentsPerStep) {
            throw Error("Task step exceeds the argument count limit: " + step.id);
        }
    }
    if (value.contains("collect_files")) {
        std::set<std::wstring> collect_paths; // Windows 等价的结果文件目标
        for (const std::string& file : value.at("collect_files").get<std::vector<std::string>>()) {
            std::filesystem::path relative = path_from_utf8(file);
            validate_relative_path(relative);
            if (!collect_paths.insert(windows_path_key(relative)).second) {
                throw Error("Duplicate collect_files path for step " + step.id + ": " + file);
            }
            step.collect_files.push_back(std::move(relative));
        }
        if (step.collect_files.size() > kMaxCollectedFilesPerStep) {
            throw Error("Task step exceeds the collected file count limit: " + step.id);
        }
    }

    if (step.type == "execute" || step.type == "script") {
        step.timeout_seconds = value.value("timeout_seconds", 120);
        if (step.timeout_seconds < 1 || step.timeout_seconds > 86'400) {
            throw Error("timeout_seconds must be between 1 and 86400 for step " + step.id);
        }
        const bool has_run_as = value.contains("run_as");
        if (mode == StepParseMode::RunManifest && !has_run_as) {
            throw Error("Run manifest requires run_as for executable step " + step.id);
        }
        step.run_as = has_run_as ? parse_task_run_as(value) : TaskRunAs::System;
        if (step.type == "execute") {
            step.program = path_from_utf8(required_string(value, "program"));
            validate_relative_path(step.program);
        } else {
            step.engine = parse_script_engine(value);
            step.script = path_from_utf8(required_string(value, "script"));
            validate_relative_path(step.script);
            std::wstring extension = step.script.extension().native();
            std::transform(extension.begin(), extension.end(), extension.begin(), [](const wchar_t character) {
                return static_cast<wchar_t>(std::towlower(character));
            });
            const bool valid_extension = step.engine == ScriptEngine::Cmd
                ? extension == L".bat" || extension == L".cmd"
                : extension == L".ps1";
            if (!valid_extension) {
                throw Error("Script extension does not match engine for step " + step.id);
            }
        }
    } else if (step.type == "echo") {
        if (value.contains("run_as")) {
            throw Error("echo step does not accept run_as: " + step.id);
        }
        step.message = required_string(value, "message");
        if (!step.arguments.empty() || !step.collect_files.empty()) {
            throw Error("echo step does not accept arguments or collect_files: " + step.id);
        }
    } else {
        throw Error("Unsupported task step type: " + step.type);
    }
    return step;
}

// 验证任务中的快照名称可安全传给 VMware Provider。
void validate_snapshot_name(const std::string& snapshot, const std::string_view field) {
    if (snapshot.empty() || snapshot.size() > 128 || snapshot.find('\0') != std::string::npos) {
        throw Error(std::string(field) + " must contain between 1 and 128 characters");
    }
}

// 解析成功或失败后的单台 VM 清理策略。
[[nodiscard]] VmCleanupPolicy parse_cleanup_policy(
    const nlohmann::json& value,
    const std::string_view field) {
    reject_unknown_fields(value, {"action", "snapshot"}, field);
    VmCleanupPolicy policy;
    const std::string action = required_string(value, "action");
    if (action == "leave_running") {
        policy.action = VmCleanupAction::LeaveRunning;
    } else if (action == "stop") {
        policy.action = VmCleanupAction::Stop;
    } else if (action == "restore") {
        policy.action = VmCleanupAction::Restore;
    } else {
        throw Error("Unsupported VM cleanup action: " + action);
    }

    if (value.contains("snapshot")) {
        policy.snapshot = required_string(value, "snapshot");
        validate_snapshot_name(*policy.snapshot, field);
    }
    if (policy.action == VmCleanupAction::Restore && !policy.snapshot.has_value()) {
        throw Error(std::string(field) + " restore action requires snapshot");
    }
    if (policy.action != VmCleanupAction::Restore && policy.snapshot.has_value()) {
        throw Error(std::string(field) + " only accepts snapshot for restore action");
    }
    return policy;
}

// 解析 Host 编排器专用的任务生命周期策略。
[[nodiscard]] TaskLifecyclePolicy parse_lifecycle_policy(
    const nlohmann::json& value,
    const StepParseMode step_mode) {
    reject_unknown_fields(value, {"vms", "finally"}, "lifecycle");
    TaskLifecyclePolicy lifecycle;
    if (!value.contains("vms") || !value.at("vms").is_array() || value.at("vms").empty()) {
        throw Error("lifecycle.vms must contain at least one VM policy");
    }

    std::set<std::string> vm_ids;
    for (const auto& vm_value : value.at("vms")) {
        reject_unknown_fields(
            vm_value,
            {"vm", "restore_before", "on_success", "on_failure"},
            "lifecycle VM policy");
        VmLifecyclePolicy policy;
        policy.vm = required_string(vm_value, "vm");
        validate_identifier(policy.vm, "lifecycle VM id");
        if (!vm_ids.insert(policy.vm).second) {
            throw Error("Duplicate lifecycle VM policy: " + policy.vm);
        }
        if (vm_value.contains("restore_before")) {
            policy.restore_before = required_string(vm_value, "restore_before");
            validate_snapshot_name(*policy.restore_before, "restore_before");
        }
        if (!vm_value.contains("on_success") || !vm_value.contains("on_failure")) {
            throw Error("Lifecycle VM policy requires on_success and on_failure: " + policy.vm);
        }
        policy.on_success = parse_cleanup_policy(vm_value.at("on_success"), "on_success");
        policy.on_failure = parse_cleanup_policy(vm_value.at("on_failure"), "on_failure");
        lifecycle.vms.push_back(std::move(policy));
    }

    if (value.contains("finally")) {
        if (!value.at("finally").is_array()) {
            throw Error("lifecycle.finally must be an array");
        }
        if (value.at("finally").size() > kMaxStepsPerRun) {
            throw Error("lifecycle.finally exceeds the step count limit");
        }
        for (const auto& step_value : value.at("finally")) {
            lifecycle.finally_steps.push_back(parse_step(step_value, step_mode));
        }
    }
    return lifecycle;
}

// 解析 Guest 工作目录和 Host 状态运行目录的结束清理策略。
[[nodiscard]] TaskCleanupPolicy parse_task_cleanup_policy(const nlohmann::json& value) {
    reject_unknown_fields(value, {"guest_work", "host_run"}, "cleanup");
    if (!value.contains("guest_work") || !value.contains("host_run")) {
        throw Error("cleanup requires guest_work and host_run policies");
    }

    const auto parse_guest_action = [](const nlohmann::json& action_value) {
        const std::string action = action_value.get<std::string>();
        if (action == "delete") {
            return GuestWorkCleanupAction::Delete;
        }
        if (action == "retain") {
            return GuestWorkCleanupAction::Retain;
        }
        throw Error("Unsupported Guest work cleanup action: " + action);
    };
    const auto parse_host_action = [](const nlohmann::json& action_value) {
        const std::string action = action_value.get<std::string>();
        if (action == "retain") {
            return HostRunCleanupAction::Retain;
        }
        if (action == "archive_then_delete") {
            return HostRunCleanupAction::ArchiveThenDelete;
        }
        throw Error("Unsupported Host run cleanup action: " + action);
    };

    const nlohmann::json& guest = value.at("guest_work");
    const nlohmann::json& host = value.at("host_run");
    reject_unknown_fields(guest, {"on_success", "on_failure"}, "cleanup.guest_work");
    reject_unknown_fields(host, {"on_success", "on_failure"}, "cleanup.host_run");
    if (!guest.contains("on_success") || !guest.contains("on_failure") ||
        !host.contains("on_success") || !host.contains("on_failure")) {
        throw Error("cleanup policies require on_success and on_failure actions");
    }

    TaskCleanupPolicy cleanup;
    cleanup.guest_work_on_success = parse_guest_action(guest.at("on_success"));
    cleanup.guest_work_on_failure = parse_guest_action(guest.at("on_failure"));
    cleanup.host_run_on_success = parse_host_action(host.at("on_success"));
    cleanup.host_run_on_failure = parse_host_action(host.at("on_failure"));
    return cleanup;
}

// 将一个任务步骤转换为稳定的 JSON 表示。
[[nodiscard]] nlohmann::json serialize_step(const TaskStep& step) {
    nlohmann::json value = {
        {"id", step.id},
        {"vm", step.vm},
        {"type", step.type},
        {"retry_safe", step.retry_safe},
    };
    if (step.type == "execute" || step.type == "script") {
        value["timeout_seconds"] = step.timeout_seconds;
        value["run_as"] = std::string(task_run_as_name(step.run_as));
        if (step.type == "execute") {
            value["program"] = path_to_utf8(step.program);
        } else {
            value["engine"] = std::string(script_engine_name(step.engine));
            value["script"] = path_to_utf8(step.script);
        }
        value["arguments"] = step.arguments;
        std::vector<std::string> files;
        files.reserve(step.collect_files.size());
        for (const auto& file : step.collect_files) {
            files.push_back(path_to_utf8(file));
        }
        value["collect_files"] = files;
    } else if (step.type == "echo") {
        if (step.run_as != TaskRunAs::System) {
            throw Error("echo step only supports the system run identity");
        }
        value["message"] = step.message;
    } else {
        throw Error("Unsupported task step type: " + step.type);
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
    if (plan.lifecycle.has_value()) {
        for (const auto& step : plan.lifecycle->finally_steps) {
            if (!step_ids.insert(step.id).second) {
                throw Error("Duplicate task or finally step id: " + step.id);
            }
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

// 验证执行结果的身份与交互 Session 证据一致。
void validate_execution_identity(const ExecutionResult& result) {
    if (result.run_as == TaskRunAs::System &&
        result.interactive_session_id.has_value()) {
        throw Error("SYSTEM execution result cannot contain an interactive Session ID");
    }
    if (result.run_as == TaskRunAs::InteractiveUser &&
        result.status != "failed" &&
        !result.interactive_session_id.has_value()) {
        throw Error("Successful interactive execution result requires a Session ID");
    }
}

}  // namespace

TaskPlan load_task_plan(const std::filesystem::path& path) {
    const nlohmann::json value = load_json(path);
    reject_unknown_fields(
        value,
        {"$schema", "schema_version", "name", "run_id", "artifacts", "steps", "lifecycle", "cleanup"},
        "task plan");
    const int schema_version = value.value("schema_version", 0);
    if (schema_version != 3) {
        throw Error("Task plan requires schema_version 3");
    }

    TaskPlan plan;
    plan.name = required_string(value, "name");
    if (value.contains("run_id")) {
        plan.run_id = required_string(value, "run_id");
    }

    if (value.contains("artifacts")) {
        if (!value.at("artifacts").is_array()) {
            throw Error("artifacts must be an array");
        }
        if (value.at("artifacts").size() > kMaxArtifactsPerRun) {
            throw Error("Task plan exceeds the Artifact count limit");
        }
        for (const auto& artifact_value : value.at("artifacts")) {
            reject_unknown_fields(
                artifact_value,
                {"source", "vm", "destination", "sha256"},
                "task artifact");
            ArtifactInput artifact;
            artifact.source = path_from_utf8(required_string(artifact_value, "source"));
            artifact.vm = required_string(artifact_value, "vm");
            validate_identifier(artifact.vm, "Artifact VM id");
            artifact.destination = path_from_utf8(required_string(artifact_value, "destination"));
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
    if (value.at("steps").size() > kMaxStepsPerRun) {
        throw Error("Task plan exceeds the step count limit");
    }
    for (const auto& step_value : value.at("steps")) {
        plan.steps.push_back(parse_step(step_value, StepParseMode::TaskPlan));
    }
    if (value.contains("lifecycle")) {
        plan.lifecycle = parse_lifecycle_policy(value.at("lifecycle"), StepParseMode::TaskPlan);
    }
    if (value.contains("cleanup")) {
        plan.cleanup = parse_task_cleanup_policy(value.at("cleanup"));
    }

    validate_plan_uniqueness(plan);
    return plan;
}

std::string_view task_run_as_name(const TaskRunAs run_as) {
    switch (run_as) {
    case TaskRunAs::System: return "system";
    case TaskRunAs::InteractiveUser: return "interactive_user";
    }
    throw Error("Unknown task run identity");
}

std::string_view script_engine_name(const ScriptEngine engine) {
    switch (engine) {
    case ScriptEngine::Cmd: return "cmd";
    case ScriptEngine::WindowsPowerShell: return "windows_powershell";
    case ScriptEngine::Pwsh: return "pwsh";
    case ScriptEngine::None: break;
    }
    throw Error("Task step does not define a script engine");
}

std::string_view vm_cleanup_action_name(const VmCleanupAction action) {
    switch (action) {
    case VmCleanupAction::LeaveRunning: return "leave_running";
    case VmCleanupAction::Stop: return "stop";
    case VmCleanupAction::Restore: return "restore";
    }
    throw Error("Unknown VM cleanup action");
}

std::string_view guest_work_cleanup_action_name(const GuestWorkCleanupAction action) {
    switch (action) {
    case GuestWorkCleanupAction::Delete: return "delete";
    case GuestWorkCleanupAction::Retain: return "retain";
    }
    throw Error("Unknown Guest work cleanup action");
}

std::string_view host_run_cleanup_action_name(const HostRunCleanupAction action) {
    switch (action) {
    case HostRunCleanupAction::Retain: return "retain";
    case HostRunCleanupAction::ArchiveThenDelete: return "archive_then_delete";
    }
    throw Error("Unknown Host run cleanup action");
}

RunManifest load_run_manifest(const std::filesystem::path& path) {
    try {
        return load_json(path).get<RunManifest>();
    } catch (const nlohmann::json::exception& error) {
        throw Error("Invalid run manifest: " + std::string(error.what()));
    }
}

void to_json(nlohmann::json& value, const RunManifest& manifest) {
    if (manifest.schema_version != 2 ||
        manifest.protocol_version != kRunManifestProtocolVersion) {
        throw Error("Unsupported run manifest schema or protocol version");
    }
    value = {
        {"schema_version", manifest.schema_version},
        {"protocol_version", manifest.protocol_version},
        {"lab_id", manifest.lab_id},
        {"run_id", manifest.run_id},
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
    reject_unknown_fields(
        value,
        {"schema_version", "protocol_version", "lab_id", "run_id", "name", "created_at",
         "artifacts", "steps"},
        "run manifest");
    manifest.schema_version = value.value("schema_version", 0);
    manifest.protocol_version = value.value("protocol_version", 0);
    manifest.lab_id = required_string(value, "lab_id");
    manifest.run_id = required_string(value, "run_id");
    manifest.name = required_string(value, "name");
    manifest.created_at = required_string(value, "created_at");
    if (manifest.schema_version != 2 ||
        manifest.protocol_version != kRunManifestProtocolVersion) {
        throw Error("Unsupported run manifest schema or protocol version");
    }

    manifest.artifacts.clear();
    for (const auto& artifact_value : value.at("artifacts")) {
        reject_unknown_fields(
            artifact_value,
            {"vm", "path", "sha256"},
            "run manifest artifact");
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
        manifest.steps.push_back(parse_step(step_value, StepParseMode::RunManifest));
    }
}

void to_json(nlohmann::json& value, const ExecutionResult& result) {
    validate_execution_identity(result);
    value = {
        {"schema_version", result.schema_version},
        {"run_id", result.run_id},
        {"vm_id", result.vm_id},
        {"job_id", result.job_id},
        {"step_id", result.step_id},
        {"status", result.status},
        {"run_as", std::string(task_run_as_name(result.run_as))},
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
    if (result.interactive_session_id.has_value()) {
        value["interactive_session_id"] = *result.interactive_session_id;
    }
    for (const auto& file : result.files) {
        value["files"].push_back({{"path", file.path}, {"sha256", file.sha256}});
    }
}

void from_json(const nlohmann::json& value, ExecutionResult& result) {
    reject_unknown_fields(
        value,
        {"schema_version", "run_id", "vm_id", "job_id", "step_id", "status", "run_as",
         "interactive_session_id", "exit_code", "duration_ms", "stdout", "stderr", "files",
         "error", "started_at", "finished_at"},
        "execution result");
    result.schema_version = value.value("schema_version", 0);
    result.run_id = required_string(value, "run_id");
    result.vm_id = required_string(value, "vm_id");
    result.job_id = required_string(value, "job_id");
    result.step_id = required_string(value, "step_id");
    result.status = required_string(value, "status");
    result.run_as = parse_task_run_as(value);
    if (value.contains("interactive_session_id") &&
        !value.at("interactive_session_id").is_null()) {
        result.interactive_session_id = value.at("interactive_session_id").get<std::uint32_t>();
    } else {
        result.interactive_session_id.reset();
    }
    if (value.contains("exit_code") && !value.at("exit_code").is_null()) {
        result.exit_code = value.at("exit_code").get<std::uint32_t>();
    } else {
        result.exit_code.reset();
    }
    result.duration_ms = value.value("duration_ms", 0LL);
    result.stdout_path = value.value("stdout", "");
    result.stderr_path = value.value("stderr", "");
    result.files.clear();
    for (const auto& file_value : value.value("files", nlohmann::json::array())) {
        reject_unknown_fields(file_value, {"path", "sha256"}, "collected result file");
        CollectedFile file;
        file.path = required_string(file_value, "path");
        file.sha256 = required_string(file_value, "sha256");
        validate_sha256(file.sha256);
        result.files.push_back(std::move(file));
    }
    result.error = value.value("error", "");
    result.started_at = required_string(value, "started_at");
    result.finished_at = required_string(value, "finished_at");
    if (result.schema_version != 2) {
        throw Error("Unsupported execution result schema version");
    }
    validate_execution_identity(result);
}

}  // namespace satsuma
