// Host 任务物化和报告汇总实现。
#include "controller.hpp"

#include <algorithm>
#include <set>

#include <windows.h>

#include "satsuma/core/errors.hpp"
#include "satsuma/core/id.hpp"
#include "satsuma/core/json_io.hpp"
#include "satsuma/core/path.hpp"
#include "satsuma/core/sha256.hpp"

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
    validate_vm_references(config_, plan);

    RunManifest manifest;
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

nlohmann::json Controller::build_report(const std::string& run_id) const {
    validate_identifier(run_id, "run_id");
    const std::filesystem::path run_directory = resolve_under_root(
        config_.shared_folder.host_root,
        std::filesystem::path(L"runs") / path_from_utf8(run_id));
    if (!std::filesystem::is_regular_file(run_directory / L"task.json")) {
        throw Error("Unknown run_id: " + run_id);
    }

    nlohmann::json executions = nlohmann::json::array();
    const std::filesystem::path result_root = run_directory / L"results";
    if (std::filesystem::exists(result_root)) {
        std::filesystem::recursive_directory_iterator iterator(result_root);
        const std::filesystem::recursive_directory_iterator end;
        for (; iterator != end; ++iterator) {
            const DWORD attributes = GetFileAttributesW(iterator->path().c_str());
            if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
                iterator.disable_recursion_pending();
                throw Error("Result directory contains a forbidden reparse point");
            }
            if (iterator->is_regular_file() && iterator->path().filename() == L"execution.json") {
                executions.push_back(load_json(iterator->path()));
            }
        }
    }

    std::size_t completed = 0;
    std::size_t failed = 0;
    for (const auto& execution : executions) {
        const std::string status = execution.value("status", "unknown");
        if (status == "exited" && execution.value("exit_code", 1) == 0) {
            ++completed;
        } else {
            ++failed;
        }
    }

    const RunManifest manifest = load_run_manifest(run_directory / L"task.json");
    return {
        {"schema_version", 1},
        {"run_id", run_id},
        {"name", manifest.name},
        {"expected_steps", manifest.steps.size()},
        {"reported_steps", executions.size()},
        {"successful_steps", completed},
        {"failed_steps", failed},
        {"complete", executions.size() == manifest.steps.size()},
        {"executions", std::move(executions)},
    };
}

}  // namespace satsuma::host
