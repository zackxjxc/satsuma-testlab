// Host 任务物化和报告汇总实现。
#include "controller.hpp"

#include <algorithm>
#include <chrono>
#include <limits>
#include <set>
#include <thread>
#include <vector>

#include <windows.h>

#include "satsuma/core/errors.hpp"
#include "satsuma/core/id.hpp"
#include "satsuma/core/json_io.hpp"
#include "satsuma/core/path.hpp"
#include "satsuma/core/sha256.hpp"
#include "satsuma/core/update.hpp"
#include "identity.hpp"

namespace satsuma::host {
namespace {

// 验证计划只引用 lab.json 中登记的 VM。
void validate_vm_references(const LabConfig& config, const TaskPlan& plan) {
    for (const auto& artifact : plan.artifacts) {
        if (find_vm(config, artifact.vm) == nullptr) {
            throw Error("Artifact references an unknown VM: " + artifact.vm);
        }
    }
    const auto validate_step = [&config, &plan](const TaskStep& step) {
        if (find_vm(config, step.vm) == nullptr) {
            throw Error("Step references an unknown VM: " + step.vm);
        }
        if (step.type == "execute" || step.type == "script") {
            const std::filesystem::path& executable = step.type == "script"
                ? step.script
                : step.program;
            const auto artifact = std::find_if(
                plan.artifacts.begin(),
                plan.artifacts.end(),
                [&step, &executable](const ArtifactInput& input) {
                    return input.vm == step.vm && input.destination == executable;
                });
            if (artifact == plan.artifacts.end()) {
                throw Error("Executable file is not a registered Artifact: " + path_to_utf8(executable));
            }
        }
        if (step.type == "script") {
            const VmConfig& vm = *find_vm(config, step.vm);
            const nlohmann::json presence = load_vm_presence(config, vm);
            if (presence.value("protocol_version", 0) != kRunManifestProtocolVersion) {
                throw Error("Script step requires Agent VMCI protocol version 3 for VM " + step.vm);
            }
            const nlohmann::json inventory = load_vm_inventory(config, vm);
            const std::string engine = std::string(script_engine_name(step.engine));
            const bool available = std::any_of(
                inventory.at("script_engines").begin(),
                inventory.at("script_engines").end(),
                [&engine](const nlohmann::json& capability) {
                    return capability.value("engine", std::string{}) == engine &&
                        capability.value("available", false);
                });
            if (!available) {
                throw Error("Script engine is unavailable for VM " + step.vm + ": " + engine);
            }
        }
    };
    for (const auto& step : plan.steps) {
        validate_step(step);
    }
    if (plan.lifecycle.has_value()) {
        for (const auto& step : plan.lifecycle->finally_steps) {
            validate_step(step);
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

// 拒绝把目录联接或符号链接当成可枚举、可删除的运行目录。
[[nodiscard]] bool is_reparse_point(const std::filesystem::path& path) noexcept {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes == INVALID_FILE_ATTRIBUTES ||
        (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}

// 发布新版本前收敛已结束更新，并阻止未结束或损坏的旧更新被新请求越过。
void prepare_agent_update_queue(
    const std::filesystem::path& updates_root,
    const std::string& lab_id,
    const std::string& vm_id) {
    std::vector<std::filesystem::path> update_directories;
    for (const auto& entry : std::filesystem::directory_iterator(updates_root)) {
        if (entry.path().filename().native().starts_with(L".")) {
            continue;
        }
        if (entry.is_directory()) {
            update_directories.push_back(entry.path());
        }
    }
    std::sort(update_directories.begin(), update_directories.end());

    for (const std::filesystem::path& update_directory : update_directories) {
        const std::string directory_id = path_to_utf8(update_directory.filename());
        if (is_reparse_point(update_directory)) {
            throw Error("Agent update queue contains a reparse point: " + directory_id);
        }

        AgentUpdateManifest previous;
        try {
            previous = load_agent_update_manifest(update_directory / L"update.json");
        } catch (const std::exception& error) {
            throw Error(
                "Agent update queue contains an invalid update " + directory_id +
                ": " + error.what());
        }
        if (previous.lab_id != lab_id ||
            previous.vm_id != vm_id ||
            previous.update_id != directory_id) {
            throw Error(
                "Agent update queue manifest identity does not match directory: " +
                directory_id);
        }

        const std::filesystem::path result_path = update_directory / L"result.json";
        if (!std::filesystem::is_regular_file(result_path)) {
            throw Error(
                "Agent update is still pending; wait for or resolve it before publishing another: " +
                previous.update_id);
        }

        AgentUpdateResult result;
        try {
            result = load_agent_update_result(result_path);
        } catch (const std::exception& error) {
            throw Error(
                "Agent update queue contains an invalid result for " + previous.update_id +
                ": " + error.what());
        }
        if (result.vm_id != vm_id ||
            result.update_id != previous.update_id ||
            result.version != previous.version) {
            throw Error(
                "Agent update result identity does not match its directory: " +
                previous.update_id);
        }
        if (result.status == "failed") {
            continue;
        }

        std::error_code cleanup_error;
        std::filesystem::remove_all(update_directory, cleanup_error);
        if (cleanup_error || std::filesystem::exists(update_directory)) {
            throw Error(
                "Successful agent update could not clean its state directory before publishing another: " +
                previous.update_id + ": " + cleanup_error.message());
        }
    }
}

struct ReportRunLocation {
    std::filesystem::path directory;
    std::string source;
};

// Active 状态优先；不存在时读取已经完成发布的编排证据归档。
[[nodiscard]] ReportRunLocation locate_report_run(
    const LabConfig& config,
    const std::string& run_id) {
    const std::filesystem::path active = resolve_under_root(
        config.transport.state_root,
        std::filesystem::path(L"runs") / path_from_utf8(run_id));
    if (std::filesystem::is_regular_file(active / L"task.json")) {
        return {active, "active"};
    }
    if (!config.host.archive_root.empty()) {
        const std::filesystem::path archived = resolve_under_root(
            config.host.archive_root,
            std::filesystem::path(L"runs") / path_from_utf8(run_id) /
                L"evidence" / L"main");
        const std::filesystem::path marker_path = archived / L".archive-complete.json";
        if (std::filesystem::is_regular_file(archived / L"task.json") &&
            std::filesystem::is_regular_file(marker_path)) {
            const nlohmann::json marker = load_json(marker_path);
            if (marker.value("schema_version", 0) != 1 ||
                marker.value("status", std::string{}) != "complete" ||
                marker.value("source_run_id", std::string{}) != run_id) {
                throw Error("Archived run completion marker is invalid: " + run_id);
            }
            return {archived, "archive"};
        }
    }
    throw Error("Unknown run_id: " + run_id);
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
    manifest.name = plan.name;
    manifest.created_at = utc_timestamp();
    manifest.steps = plan.steps;
    validate_identifier(manifest.run_id, "run_id");

    const std::filesystem::path runs_root = config_.transport.state_root / L"runs";
    std::filesystem::create_directories(runs_root);
    const std::filesystem::path final_directory = resolve_under_root(
        config_.transport.state_root,
        std::filesystem::path(L"runs") / path_from_utf8(manifest.run_id));
    if (std::filesystem::exists(final_directory)) {
        throw Error("Run directory already exists: " + path_to_utf8(final_directory));
    }

    // 点号前缀目录不会被 Agent 扫描，完成后再整体原子改名。
    const std::string staging_name = ".preparing-" + make_id("stage");
    const std::filesystem::path staging_directory = resolve_under_root(
        config_.transport.state_root,
        std::filesystem::path(L"runs") / path_from_utf8(staging_name));
    try {
        std::filesystem::create_directories(staging_directory / L"state");
        std::filesystem::create_directories(staging_directory / L"results");
        for (const auto& input : plan.artifacts) {
            validate_artifact_destination(input.destination);
            if (!std::filesystem::is_regular_file(input.source)) {
                throw Error("Artifact source is not a regular file: " + path_to_utf8(input.source));
            }
            if (std::filesystem::file_size(input.source) > kMaxArtifactBytes) {
                throw Error("Artifact exceeds the 2 GiB size limit: " + path_to_utf8(input.source));
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
        rename_path_with_retry(staging_directory, final_directory);
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
    const std::string& version,
    const std::optional<std::string> next_vm_id) const {
    validate_identifier(vm_id, "update VM id");
    if (find_vm(config_, vm_id) == nullptr) {
        throw Error("Agent update references an unknown VM: " + vm_id);
    }
    if (!std::filesystem::is_regular_file(binary)) {
        throw Error("Agent update binary is not a regular file: " + path_to_utf8(binary));
    }
    if (next_vm_id.has_value()) {
        validate_identifier(*next_vm_id, "next update VM id");
        if (*next_vm_id == vm_id) {
            throw Error("Agent rebind target must differ from its current VM id");
        }
        if (find_vm(config_, *next_vm_id) == nullptr) {
            throw Error("Agent rebind references an unknown target VM: " + *next_vm_id);
        }
        const std::filesystem::path target_presence = resolve_under_root(
            config_.transport.state_root,
            std::filesystem::path(L"agents") /
                path_from_utf8(*next_vm_id + ".json"));
        if (std::filesystem::exists(target_presence)) {
            throw Error(
                "Agent rebind target presence already exists: " +
                path_to_utf8(target_presence));
        }
    }

    AgentUpdateManifest manifest;
    manifest.protocol_version = next_vm_id.has_value() ? 2 : 1;
    manifest.lab_id = config_.lab_id;
    manifest.vm_id = vm_id;
    manifest.next_vm_id = next_vm_id;
    manifest.update_id = make_id("update");
    manifest.version = version;
    manifest.binary = L"SatsumaVM.exe";
    manifest.created_at = utc_timestamp();

    const std::filesystem::path updates_root = resolve_under_root(
        config_.transport.state_root,
        std::filesystem::path(L"updates") / path_from_utf8(vm_id));
    std::filesystem::create_directories(updates_root);
    prepare_agent_update_queue(updates_root, config_.lab_id, vm_id);
    const std::filesystem::path final_directory = resolve_under_root(
        updates_root,
        path_from_utf8(manifest.update_id));
    const std::filesystem::path staging_directory = resolve_under_root(
        updates_root,
        path_from_utf8(".preparing-" + make_id("stage")));
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
        rename_path_with_retry(staging_directory, final_directory);
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
        config_.transport.state_root,
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
                    "Successful agent update could not clean its state directory: " +
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
    const ReportRunLocation location = locate_report_run(config_, run_id);
    const std::filesystem::path& run_directory = location.directory;

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
    const bool complete = executions.size() == manifest.steps.size(); // 是否已收到全部规范结果
    const bool manual_intervention_required = !blocked_steps.empty(); // 是否存在人工恢复门禁
    std::string status;
    if (manual_intervention_required) {
        status = "manual_intervention_required";
    } else if (!complete) {
        status = "pending";
    } else if (failed != 0) {
        status = "failed";
    } else {
        status = "succeeded";
    }
    return {
        {"schema_version", 1},
        {"run_id", run_id},
        {"source", location.source},
        {"name", manifest.name},
        {"status", status},
        {"expected_steps", manifest.steps.size()},
        {"reported_steps", executions.size()},
        {"successful_steps", completed},
        {"failed_steps", failed},
        {"complete", complete},
        {"manual_intervention_required", manual_intervention_required},
        {"blocked_steps", std::move(blocked_steps)},
        {"executions", std::move(executions)},
    };
}

nlohmann::json Controller::list_runs() const {
    const std::filesystem::path runs_root = config_.transport.state_root / L"runs";
    nlohmann::json runs = nlohmann::json::array();
    std::set<std::string> run_ids;
    if (std::filesystem::is_directory(runs_root)) {
        for (const auto& entry : std::filesystem::directory_iterator(runs_root)) {
            if (entry.is_directory() && !is_reparse_point(entry.path()) &&
                !entry.path().filename().native().starts_with(L".")) {
                run_ids.insert(path_to_utf8(entry.path().filename()));
            }
        }
    }
    const std::filesystem::path archive_runs_root = config_.host.archive_root / L"runs";
    if (!config_.host.archive_root.empty() &&
        std::filesystem::is_directory(archive_runs_root)) {
        for (const auto& entry : std::filesystem::directory_iterator(archive_runs_root)) {
            if (entry.is_directory() && !is_reparse_point(entry.path()) &&
                !entry.path().filename().native().starts_with(L".") &&
                std::filesystem::is_regular_file(
                    entry.path() / L"evidence" / L"main" / L"task.json")) {
                run_ids.insert(path_to_utf8(entry.path().filename()));
            }
        }
    }
    for (const std::string& run_id : run_ids) {
        try {
            nlohmann::json report = build_report(run_id);
            runs.push_back({
                {"run_id", run_id},
                {"source", report.at("source")},
                {"name", report.at("name")},
                {"status", report.at("status")},
                {"complete", report.at("complete")},
            });
        } catch (const std::exception& error) {
            runs.push_back({
                {"run_id", run_id},
                {"status", "invalid"},
                {"complete", false},
                {"error", error.what()},
            });
        }
    }
    return {{"schema_version", 1}, {"runs", std::move(runs)}};
}

nlohmann::json Controller::cancel_run(
    const std::string& run_id,
    const std::string& reason) const {
    validate_identifier(run_id, "run_id");
    if (reason.empty() || reason.size() > 512 || reason.find('\0') != std::string::npos) {
        throw Error("Cancellation reason must contain between 1 and 512 non-NUL characters");
    }
    const std::filesystem::path run_directory = resolve_under_root(
        config_.transport.state_root,
        std::filesystem::path(L"runs") / path_from_utf8(run_id));
    if (!std::filesystem::is_regular_file(run_directory / L"task.json")) {
        throw Error("Unknown run_id: " + run_id);
    }
    const nlohmann::json report = build_report(run_id);
    if (report.at("complete").get<bool>()) {
        return {
            {"schema_version", 1},
            {"run_id", run_id},
            {"status", "already_complete"},
        };
    }
    write_json_atomic(run_directory / L"cancel.json", {
        {"schema_version", 1},
        {"run_id", run_id},
        {"reason", reason},
        {"requested_at", utc_timestamp()},
    });
    return {
        {"schema_version", 1},
        {"run_id", run_id},
        {"status", "cancellation_requested"},
    };
}

nlohmann::json Controller::prune_runs(const std::size_t keep) const {
    if (keep > 10'000) {
        throw Error("Run retention must be between 0 and 10000");
    }
    const std::filesystem::path runs_root = config_.transport.state_root / L"runs";
    std::vector<std::filesystem::directory_entry> entries;
    if (std::filesystem::is_directory(runs_root)) {
        for (const auto& entry : std::filesystem::directory_iterator(runs_root)) {
            if (entry.is_directory() && !is_reparse_point(entry.path()) &&
                !entry.path().filename().native().starts_with(L".")) {
                entries.push_back(entry);
            }
        }
    }
    std::sort(entries.begin(), entries.end(), [](const auto& left, const auto& right) {
        return left.last_write_time() > right.last_write_time();
    });
    nlohmann::json removed = nlohmann::json::array();
    nlohmann::json retained = nlohmann::json::array();
    for (std::size_t index = 0; index < entries.size(); ++index) {
        const std::string run_id = path_to_utf8(entries[index].path().filename());
        if (index < keep) {
            retained.push_back(run_id);
            continue;
        }
        try {
            const nlohmann::json report = build_report(run_id);
            if (!report.at("complete").get<bool>()) {
                retained.push_back(run_id);
                continue;
            }
            std::filesystem::remove_all(entries[index].path());
            removed.push_back(run_id);
        } catch (...) {
            retained.push_back(run_id);
        }
    }
    return {
        {"schema_version", 1},
        {"status", "pruned"},
        {"keep", keep},
        {"removed", std::move(removed)},
        {"retained", std::move(retained)},
    };
}

}  // namespace satsuma::host
