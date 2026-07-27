// Host 任务物化和报告汇总实现。
#include "controller.hpp"

#include <algorithm>
#include <chrono>
#include <limits>
#include <set>
#include <thread>

#include <windows.h>

#include "satsuma/core/errors.hpp"
#include "satsuma/core/id.hpp"
#include "satsuma/core/json_io.hpp"
#include "satsuma/core/path.hpp"
#include "satsuma/core/sha256.hpp"
#include "satsuma/core/update.hpp"

namespace satsuma::host {
namespace {

// 验证计划只引用 lab.json 中登记的 VM。
void validate_vm_references(const LabConfig& config, const TaskPlan& plan) {
    for (const auto& artifact : plan.artifacts) {
        if (find_vm(config, artifact.vm) == nullptr) {
            throw Error("Artifact references an unknown VM: " + artifact.vm);
        }
    }
    for (const auto& step : plan.steps) {
        if (find_vm(config, step.vm) == nullptr) {
            throw Error("Step references an unknown VM: " + step.vm);
        }
        if (step.type == "execute") {
            const auto artifact = std::find_if(
                plan.artifacts.begin(),
                plan.artifacts.end(),
                [&step](const ArtifactInput& input) {
                    return input.vm == step.vm && input.destination == step.program;
                });
            if (artifact == plan.artifacts.end()) {
                throw Error("Execute program is not a registered Artifact: " + path_to_utf8(step.program));
            }
        }
    }
}

// 确保 Artifact 只能部署到运行目录的 artifacts 子目录。
void validate_artifact_destination(const std::filesystem::path& destination) {
    validate_relative_path(destination);
    const auto first = destination.begin();
    if (first == destination.end() || _wcsicmp(first->native().c_str(), L"artifacts") != 0) {
        throw Error("Artifact destination must be under artifacts/: " + path_to_utf8(destination));
    }
}

}  // namespace

Controller::Controller(LabConfig config) : config_(std::move(config)) {}

RunManifest Controller::create_run(const std::filesystem::path& plan_path) const {
    const TaskPlan plan = load_task_plan(plan_path);
    return create_run(plan);
}

RunManifest Controller::create_run(const TaskPlan& plan) const {
    if (plan.lifecycle.has_value()) {
        throw Error("Task lifecycle policies require the Host orchestrator and cannot use run");
    }
    validate_vm_references(config_, plan);

    RunManifest manifest;
    manifest.protocol_version = kRunManifestProtocolVersion;
    manifest.lab_id = config_.lab_id;
    manifest.run_id = plan.run_id.value_or(make_id("run"));
    manifest.request_id = make_id("request");
    manifest.name = plan.name;
    manifest.created_at = utc_timestamp();
    manifest.steps = plan.steps;
    validate_identifier(manifest.run_id, "run_id");

    const std::filesystem::path runs_root = config_.shared_folder.host_root / L"runs";
    std::filesystem::create_directories(runs_root);
    const std::filesystem::path final_directory = resolve_under_root(
        config_.shared_folder.host_root,
        std::filesystem::path(L"runs") / path_from_utf8(manifest.run_id));
    if (std::filesystem::exists(final_directory)) {
        throw Error("Run directory already exists: " + path_to_utf8(final_directory));
    }

    // 点号前缀目录不会被 Agent 扫描，完成后再整体原子改名。
    const std::string staging_name = ".preparing-" + manifest.run_id + "-" + make_id("stage");
    const std::filesystem::path staging_directory = resolve_under_root(
        config_.shared_folder.host_root,
        std::filesystem::path(L"runs") / path_from_utf8(staging_name));
    std::filesystem::create_directories(staging_directory / L"state");
    std::filesystem::create_directories(staging_directory / L"results");

    try {
        for (const auto& input : plan.artifacts) {
            validate_artifact_destination(input.destination);
            if (!std::filesystem::is_regular_file(input.source)) {
                throw Error("Artifact source is not a regular file: " + path_to_utf8(input.source));
            }

            const std::filesystem::path target = resolve_under_root(staging_directory, input.destination);
            std::filesystem::create_directories(target.parent_path());
            std::filesystem::copy_file(input.source, target, std::filesystem::copy_options::none);

            const std::string actual_hash = sha256_file(target);
            if (input.sha256.has_value() && *input.sha256 != actual_hash) {
                throw Error("Artifact SHA-256 mismatch: " + path_to_utf8(input.source));
            }
            manifest.artifacts.push_back({input.vm, input.destination, actual_hash});
        }

        write_json_atomic(staging_directory / L"task.json", manifest);
        std::filesystem::rename(staging_directory, final_directory);
    } catch (...) {
        std::error_code cleanup_error;
        std::filesystem::remove_all(staging_directory, cleanup_error);
        throw;
    }
    return manifest;
}

AgentUpdateManifest Controller::publish_agent_update(
    const std::string& vm_id,
    const std::filesystem::path& binary,
    const std::string& version) const {
    validate_identifier(vm_id, "update VM id");
    if (find_vm(config_, vm_id) == nullptr) {
        throw Error("Agent update references an unknown VM: " + vm_id);
    }
    if (!std::filesystem::is_regular_file(binary)) {
        throw Error("Agent update binary is not a regular file: " + path_to_utf8(binary));
    }

    AgentUpdateManifest manifest;
    manifest.lab_id = config_.lab_id;
    manifest.vm_id = vm_id;
    manifest.update_id = make_id("update");
    manifest.version = version;
    manifest.binary = L"SatsumaVM.exe";
    manifest.created_at = utc_timestamp();

    const std::filesystem::path updates_root = resolve_under_root(
        config_.shared_folder.host_root,
        std::filesystem::path(L"updates") / path_from_utf8(vm_id));
    std::filesystem::create_directories(updates_root);
    const std::filesystem::path final_directory = resolve_under_root(
        updates_root,
        path_from_utf8(manifest.update_id));
    const std::filesystem::path staging_directory = resolve_under_root(
        updates_root,
        path_from_utf8(".preparing-" + manifest.update_id + "-" + make_id("stage")));
    if (std::filesystem::exists(final_directory)) {
        throw Error("Agent update directory already exists: " + path_to_utf8(final_directory));
    }

    try {
        std::filesystem::create_directories(staging_directory);
        const std::filesystem::path staged_binary =
            resolve_under_root(staging_directory, manifest.binary);
        std::filesystem::copy_file(
            binary,
            staged_binary,
            std::filesystem::copy_options::none);
        const std::uintmax_t size = std::filesystem::file_size(staged_binary);
        if (size == 0 || size > std::numeric_limits<std::uint64_t>::max()) {
            throw Error("Agent update binary has an invalid size");
        }
        manifest.size = static_cast<std::uint64_t>(size);
        manifest.sha256 = sha256_file(staged_binary);
        manifest = nlohmann::json(manifest).get<AgentUpdateManifest>();
        write_json_atomic(staging_directory / L"update.json", manifest);
        std::filesystem::rename(staging_directory, final_directory);
    } catch (...) {
        std::error_code cleanup_error;
        std::filesystem::remove_all(staging_directory, cleanup_error);
        throw;
    }
    return manifest;
}

AgentUpdateResult Controller::wait_agent_update(
    const std::string& vm_id,
    const std::string& update_id,
    const std::chrono::seconds timeout) const {
    validate_identifier(vm_id, "update VM id");
    validate_identifier(update_id, "update_id");
    if (timeout < std::chrono::seconds(1) || timeout > std::chrono::hours(1)) {
        throw Error("Agent update timeout must be between 1 and 3600 seconds");
    }
    const std::filesystem::path update_directory = resolve_under_root(
        config_.shared_folder.host_root,
        std::filesystem::path(L"updates") /
            path_from_utf8(vm_id) /
            path_from_utf8(update_id));
    const std::filesystem::path manifest_path = update_directory / L"update.json";
    if (!std::filesystem::is_regular_file(manifest_path)) {
        throw Error("Unknown agent update: " + update_id);
    }
    const AgentUpdateManifest manifest = load_agent_update_manifest(manifest_path);
    if (manifest.lab_id != config_.lab_id ||
        manifest.vm_id != vm_id ||
        manifest.update_id != update_id) {
        throw Error("Agent update manifest identity does not match its directory");
    }

    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        const std::filesystem::path result_path = update_directory / L"result.json";
        if (std::filesystem::is_regular_file(result_path)) {
            const AgentUpdateResult result = load_agent_update_result(result_path);
            if (result.vm_id != vm_id ||
                result.update_id != update_id ||
                result.version != manifest.version) {
                throw Error("Agent update result identity does not match its directory");
            }
            if (result.status == "succeeded") {
                const auto cleanup_deadline =
                    std::chrono::steady_clock::now() + std::chrono::seconds(5);
                std::error_code cleanup_error;
                do {
                    cleanup_error.clear();
                    std::filesystem::remove_all(update_directory, cleanup_error);
                    if (!std::filesystem::exists(update_directory)) {
                        return result;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                } while (std::chrono::steady_clock::now() < cleanup_deadline);
                throw Error(
                    "Successful agent update could not clean its shared directory: " +
                    cleanup_error.message());
            }
            return result;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            throw Error("Timed out while waiting for the Agent update result");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

nlohmann::json Controller::build_report(const std::string& run_id) const {
    validate_identifier(run_id, "run_id");
    const std::filesystem::path run_directory = resolve_under_root(
        config_.shared_folder.host_root,
        std::filesystem::path(L"runs") / path_from_utf8(run_id));
    if (!std::filesystem::is_regular_file(run_directory / L"task.json")) {
        throw Error("Unknown run_id: " + run_id);
    }

    const RunManifest manifest = load_run_manifest(run_directory / L"task.json");
    nlohmann::json executions = nlohmann::json::array();
    std::size_t completed = 0;
    std::size_t failed = 0;
    nlohmann::json blocked_steps = nlohmann::json::array();
    for (const TaskStep& step : manifest.steps) {
        const std::filesystem::path result_path = resolve_under_root(
            run_directory,
            std::filesystem::path(L"results") /
                path_from_utf8(step.vm) /
                path_from_utf8(step.id) /
                L"execution.json");
        if (std::filesystem::exists(result_path)) {
            if (!std::filesystem::is_regular_file(result_path)) {
                throw Error("Canonical step result path is not a regular file");
            }
            const nlohmann::json execution_json = load_json(result_path);
            const ExecutionResult execution = execution_json.get<ExecutionResult>();
            if (execution.run_id != manifest.run_id ||
                execution.vm_id != step.vm ||
                execution.step_id != step.id ||
                execution.run_as != step.run_as) {
                throw Error("Canonical step result identity does not match its manifest step");
            }
            if (execution.status == "exited" &&
                execution.exit_code.value_or(1) == 0) {
                ++completed;
            } else {
                ++failed;
            }
            executions.push_back(execution_json);
            continue;
        }
        const std::filesystem::path recovery_path = resolve_under_root(
            run_directory,
            std::filesystem::path(L"state") /
                path_from_utf8(step.vm) /
                path_from_utf8(step.id + ".claim-recovery.json"));
        if (!std::filesystem::is_regular_file(recovery_path)) {
            continue;
        }
        const nlohmann::json recovery = load_json(recovery_path);
        if (recovery.value("status", "") == "manual_intervention_required") {
            blocked_steps.push_back({
                {"vm_id", step.vm},
                {"step_id", step.id},
                {"recovery", recovery},
            });
        }
    }
    return {
        {"schema_version", 1},
        {"run_id", run_id},
        {"name", manifest.name},
        {"expected_steps", manifest.steps.size()},
        {"reported_steps", executions.size()},
        {"successful_steps", completed},
        {"failed_steps", failed},
        {"complete", executions.size() == manifest.steps.size()},
        {"manual_intervention_required", !blocked_steps.empty()},
        {"blocked_steps", std::move(blocked_steps)},
        {"executions", std::move(executions)},
    };
}

}  // namespace satsuma::host
