// Host 单 VM 生命周期编排和不可变证据归档实现。
#include "orchestrator.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <thread>
#include <utility>

#include <windows.h>

#include "controller.hpp"
#include "diagnostics.hpp"
#include "satsuma/core/errors.hpp"
#include "satsuma/core/id.hpp"
#include "satsuma/core/json_io.hpp"
#include "satsuma/core/lifecycle.hpp"
#include "satsuma/core/path.hpp"
#include "satsuma/core/sha256.hpp"
#include "satsuma/core/task.hpp"
#include "vmrun_provider.hpp"

namespace satsuma::host {
namespace {

// Host 重启时验证和恢复同一次编排所需的不可变身份。
struct OrchestrationIdentity {
    int schema_version{1}; // 编排身份 schema 版本
    std::string run_id; // 生命周期运行 ID
    std::string vm_id; // 唯一生命周期 VM
    std::string main_run_id; // Agent 主任务运行 ID
    std::string finally_run_id; // Agent finally 任务运行 ID
    std::string plan_sha256; // 原始计划文件哈希
};

// 已初始化或重新加载的编排归档。
struct OrchestrationArchive {
    std::filesystem::path root; // Host 归档根目录
    std::filesystem::path state_path; // 生命周期状态文件
    OrchestrationIdentity identity; // 不可变编排身份
    RunLifecycleState state; // 当前持久化状态
    bool resumed{false}; // 是否从已有归档恢复
};

// 返回单 VM 编排所需的唯一生命周期策略。
[[nodiscard]] const VmLifecyclePolicy& require_single_vm_policy(const TaskPlan& plan) {
    if (!plan.lifecycle.has_value()) {
        throw Error("Host orchestrate requires a lifecycle policy");
    }
    if (plan.lifecycle->vms.size() != 1) {
        throw Error("Host orchestrate currently requires exactly one lifecycle VM policy");
    }
    return plan.lifecycle->vms.front();
}

// 验证任务涉及的全部 VM 都由当前单 VM 策略覆盖。
void validate_plan_scope(const TaskPlan& plan, const std::string& vm_id) {
    const auto require_vm = [&vm_id](const std::string& referenced_vm, const std::string& field) {
        if (referenced_vm != vm_id) {
            throw Error(field + " is outside the lifecycle VM scope: " + referenced_vm);
        }
    };
    for (const ArtifactInput& artifact : plan.artifacts) {
        require_vm(artifact.vm, "Artifact VM");
    }
    for (const TaskStep& step : plan.steps) {
        require_vm(step.vm, "Task step VM");
    }
    for (const TaskStep& step : plan.lifecycle->finally_steps) {
        require_vm(step.vm, "Finally step VM");
    }
}

// 序列化不可变编排身份。
[[nodiscard]] nlohmann::json orchestration_identity_json(
    const OrchestrationIdentity& identity) {
    return {
        {"schema_version", identity.schema_version},
        {"run_id", identity.run_id},
        {"vm_id", identity.vm_id},
        {"main_run_id", identity.main_run_id},
        {"finally_run_id", identity.finally_run_id},
        {"plan_sha256", identity.plan_sha256},
    };
}

// 读取并验证 Host 重启所需的不可变编排身份。
[[nodiscard]] OrchestrationIdentity load_orchestration_identity(
    const std::filesystem::path& path) {
    try {
        const nlohmann::json value = load_json(path);
        OrchestrationIdentity identity;
        identity.schema_version = value.value("schema_version", 0);
        identity.run_id = value.at("run_id").get<std::string>();
        identity.vm_id = value.at("vm_id").get<std::string>();
        identity.main_run_id = value.at("main_run_id").get<std::string>();
        identity.finally_run_id = value.at("finally_run_id").get<std::string>();
        identity.plan_sha256 = value.at("plan_sha256").get<std::string>();
        if (identity.schema_version != 1) {
            throw Error("Orchestration identity requires schema_version 1");
        }
        validate_identifier(identity.run_id, "orchestration identity run_id");
        validate_identifier(identity.vm_id, "orchestration identity vm_id");
        validate_identifier(identity.main_run_id, "orchestration identity main_run_id");
        validate_identifier(identity.finally_run_id, "orchestration identity finally_run_id");
        if (identity.plan_sha256.size() != 64) {
            throw Error("Orchestration identity plan SHA-256 is invalid");
        }
        return identity;
    } catch (const nlohmann::json::exception& error) {
        throw Error("Invalid orchestration identity: " + std::string(error.what()));
    }
}

// 原子初始化归档，或在计划身份完全一致时加载已有编排。
[[nodiscard]] OrchestrationArchive prepare_or_load_archive(
    const LabConfig& config,
    const std::filesystem::path& plan_path,
    const std::string& run_id,
    const std::string& vm_id) {
    const std::filesystem::path runs_root = resolve_under_root(
        config.host.archive_root,
        L"runs");
    const std::filesystem::path root = resolve_under_root(
        runs_root,
        path_from_utf8(run_id));
    const std::filesystem::path state_path = root / L"lifecycle.json";
    const std::filesystem::path identity_path = root / L"orchestration.json";
    const std::filesystem::path archived_plan_path = root / L"plan.json";
    const std::string plan_sha256 = sha256_file(plan_path); // 本次调用使用的原始计划身份

    if (std::filesystem::exists(root)) {
        if (!std::filesystem::is_regular_file(state_path) ||
            !std::filesystem::is_regular_file(identity_path) ||
            !std::filesystem::is_regular_file(archived_plan_path)) {
            throw Error("Existing orchestration archive is incomplete: " + path_to_utf8(root));
        }
        OrchestrationIdentity identity = load_orchestration_identity(identity_path);
        if (identity.run_id != run_id || identity.vm_id != vm_id ||
            identity.main_run_id != run_id || identity.plan_sha256 != plan_sha256 ||
            sha256_file(archived_plan_path) != plan_sha256) {
            throw Error("Existing orchestration archive does not match the requested plan");
        }
        RunLifecycleState state = load_run_lifecycle_state(state_path);
        if (state.run_id != run_id) {
            throw Error("Existing orchestration lifecycle belongs to another run");
        }
        return {root, state_path, std::move(identity), std::move(state), true};
    }

    std::filesystem::create_directories(runs_root);
    const std::filesystem::path staging = resolve_under_root(
        runs_root,
        path_from_utf8(".preparing-" + run_id + "-" + make_id("orchestration")));
    OrchestrationIdentity identity{
        1,
        run_id,
        vm_id,
        run_id,
        make_id("finally"),
        plan_sha256,
    };
    RunLifecycleState state = make_run_lifecycle_state(run_id, utc_timestamp());
    try {
        std::filesystem::create_directories(staging);
        const std::filesystem::path staged_plan = staging / L"plan.json";
        std::filesystem::copy_file(plan_path, staged_plan, std::filesystem::copy_options::none);
        if (sha256_file(staged_plan) != plan_sha256) {
            throw Error("Archived orchestration plan SHA-256 changed during copy");
        }
        write_json_atomic_existing_parent(
            staging / L"orchestration.json",
            orchestration_identity_json(identity));
        write_json_atomic_existing_parent(staging / L"lifecycle.json", state);
        std::filesystem::rename(staging, root);
    } catch (...) {
        std::error_code cleanup_error;
        std::filesystem::remove_all(staging, cleanup_error);
        throw;
    }
    return {root, state_path, std::move(identity), std::move(state), false};
}

// 验证自动恢复只使用配置中的基础快照或 AI 所有权快照。
void validate_managed_snapshot(
    const VmConfig& vm,
    const std::vector<std::string>& existing,
    const std::string& snapshot) {
    if (snapshot != vm.snapshots.base && !snapshot.starts_with(vm.snapshots.ai_prefix)) {
        throw Error("Lifecycle snapshot is not managed by this VM: " + snapshot);
    }
    if (std::find(existing.begin(), existing.end(), snapshot) == existing.end()) {
        throw Error("Lifecycle snapshot does not exist: " + snapshot);
    }
}

// 判断 vmrun 返回的运行列表是否包含目标 VMX。
[[nodiscard]] bool is_vm_running(
    const vmware::VmrunProvider& provider,
    const std::filesystem::path& vmx) {
    const std::filesystem::path expected = std::filesystem::absolute(vmx).lexically_normal();
    for (const std::filesystem::path& running : provider.list_running()) {
        const std::filesystem::path candidate = std::filesystem::absolute(running).lexically_normal();
        if (_wcsicmp(expected.native().c_str(), candidate.native().c_str()) == 0) {
            return true;
        }
    }
    return false;
}

// 有限等待运行目录中的全部预期结果完成。
[[nodiscard]] nlohmann::json wait_for_report(
    const Controller& controller,
    const std::string& run_id,
    const std::chrono::seconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    nlohmann::json report;
    do {
        report = controller.build_report(run_id);
        if (report.at("complete").get<bool>() ||
            report.value("manual_intervention_required", false)) {
            return report;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    } while (std::chrono::steady_clock::now() < deadline);
    throw Error("Run did not complete before orchestration timeout: " + run_id);
}

// 复制目录时拒绝任何重解析点，避免归档越过运行根目录。
void copy_tree_without_reparse_points(
    const std::filesystem::path& source,
    const std::filesystem::path& destination) {
    std::filesystem::create_directories(destination);
    std::filesystem::recursive_directory_iterator iterator(source);
    const std::filesystem::recursive_directory_iterator end;
    for (; iterator != end; ++iterator) {
        const DWORD attributes = GetFileAttributesW(iterator->path().c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES) {
            throw Error("Cannot inspect run evidence path");
        }
        if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
            iterator.disable_recursion_pending();
            throw Error("Run evidence contains a forbidden reparse point");
        }
        const std::filesystem::path relative = iterator->path().lexically_relative(source);
        const std::filesystem::path target = resolve_under_root(destination, relative);
        if (iterator->is_directory()) {
            std::filesystem::create_directories(target);
        } else if (iterator->is_regular_file()) {
            std::filesystem::create_directories(target.parent_path());
            std::filesystem::copy_file(iterator->path(), target, std::filesystem::copy_options::none);
        } else {
            throw Error("Run evidence contains an unsupported file type");
        }
    }
}

// 将共享目录运行证据一次性发布到 Guest 不可见的归档目录。
void archive_run_evidence(
    const LabConfig& config,
    const std::string& lifecycle_run_id,
    const std::string& execution_run_id,
    const std::string& label) {
    const std::filesystem::path source = resolve_under_root(
        config.shared_folder.host_root,
        std::filesystem::path(L"runs") / path_from_utf8(execution_run_id));
    const std::filesystem::path archive_root = resolve_under_root(
        config.host.archive_root,
        std::filesystem::path(L"runs") / path_from_utf8(lifecycle_run_id) / L"evidence");
    const std::filesystem::path destination = resolve_under_root(archive_root, path_from_utf8(label));
    if (std::filesystem::exists(destination)) {
        if (std::filesystem::is_regular_file(destination / L"task.json")) {
            return;
        }
        throw Error("Run evidence archive is incomplete: " + path_to_utf8(destination));
    }

    const std::filesystem::path staging = resolve_under_root(
        archive_root,
        path_from_utf8("." + label + "-" + make_id("archive")));
    try {
        copy_tree_without_reparse_points(source, staging);
        std::filesystem::rename(staging, destination);
    } catch (...) {
        std::error_code cleanup_error;
        std::filesystem::remove_all(staging, cleanup_error);
        throw;
    }
}

// 生成不含 Host 生命周期字段的 Agent 可见主任务计划。
[[nodiscard]] TaskPlan make_main_plan(TaskPlan plan, const std::string& run_id) {
    plan.run_id = run_id;
    plan.lifecycle.reset();
    return plan;
}

// 生成单独发布、始终在主任务后执行的 finally 计划。
[[nodiscard]] TaskPlan make_finally_plan(
    const TaskPlan& source,
    const std::string& finally_run_id) {
    TaskPlan plan;
    plan.name = source.name + " finally";
    plan.run_id = finally_run_id;
    plan.artifacts = source.artifacts;
    plan.steps = source.lifecycle->finally_steps;
    return plan;
}

// 将异常文本追加到已有业务错误，保留最先失败的上下文。
void append_error(std::string& target, const std::string& message) {
    if (!target.empty()) {
        target += "; ";
    }
    target += message;
}

// 把完成报告折叠为业务结果和人工门禁状态。
void apply_main_report(
    const nlohmann::json& report,
    bool& business_success,
    bool& manual_gate,
    std::string& business_error) {
    manual_gate = report.value("manual_intervention_required", false);
    business_success = !manual_gate && report.at("failed_steps") == 0;
    if (manual_gate) {
        append_error(business_error, "Main task requires manual intervention");
    } else if (!business_success) {
        append_error(business_error, "Main task reported failed steps");
    }
}

// 将已有终态转换为幂等的 CLI 返回值。
void apply_terminal_state_output(
    nlohmann::json& output,
    const RunLifecycleState& state,
    const VmLifecyclePolicy& policy) {
    switch (state.phase) {
    case RunPhase::Completed:
        output["status"] = "COMPLETED";
        output["cleanup_action"] = vm_cleanup_action_name(policy.on_success.action);
        break;
    case RunPhase::Failed:
        output["status"] = "FAILED";
        output["cleanup_action"] = vm_cleanup_action_name(policy.on_failure.action);
        break;
    case RunPhase::RecoveryFailed:
        output["status"] = "RECOVERY_FAILED";
        break;
    case RunPhase::ManualInterventionRequired:
        output["status"] = "MANUAL_INTERVENTION_REQUIRED";
        break;
    default:
        throw Error("Cannot return a non-terminal orchestration state");
    }
    if (!state.transitions.empty() && state.phase != RunPhase::Completed) {
        output["error"] = state.transitions.back().message;
    }
}

// 执行成功或失败策略，并返回 VM 是否仍被认为处于运行状态。
[[nodiscard]] bool apply_cleanup_policy(
    const vmware::VmrunProvider& provider,
    const VmConfig& vm,
    const VmCleanupPolicy& policy,
    const bool assumed_running) {
    if (policy.action == VmCleanupAction::LeaveRunning) {
        return assumed_running;
    }
    if (policy.action == VmCleanupAction::Stop) {
        if (assumed_running) {
            provider.stop(vm.vmx, vmware::VmStopMode::Soft);
        }
        return false;
    }
    if (assumed_running) {
        provider.stop(vm.vmx, vmware::VmStopMode::Hard);
    }
    provider.revert_to_snapshot(vm.vmx, *policy.snapshot);
    return false;
}

}  // namespace

Orchestrator::Orchestrator(LabConfig config) : config_(std::move(config)) {}

nlohmann::json Orchestrator::execute(
    const std::filesystem::path& plan_path,
    const std::chrono::seconds timeout) const {
    if (timeout.count() < 1 || timeout.count() > 86'400) {
        throw Error("Orchestration timeout must be between 1 and 86400 seconds");
    }

    TaskPlan plan = load_task_plan(plan_path);
    const VmLifecyclePolicy& policy = require_single_vm_policy(plan);
    validate_plan_scope(plan, policy.vm);
    const VmConfig* vm = find_vm(config_, policy.vm);
    if (vm == nullptr) {
        throw Error("Lifecycle policy references an unknown VM: " + policy.vm);
    }

    vmware::VmrunProvider provider(config_.provider.vmrun);
    const std::vector<std::string> snapshots = provider.list_snapshots(vm->vmx);
    if (policy.restore_before.has_value()) {
        validate_managed_snapshot(*vm, snapshots, *policy.restore_before);
    }
    if (policy.on_success.action == VmCleanupAction::Restore) {
        validate_managed_snapshot(*vm, snapshots, *policy.on_success.snapshot);
    }
    if (policy.on_failure.action == VmCleanupAction::Restore) {
        validate_managed_snapshot(*vm, snapshots, *policy.on_failure.snapshot);
    }
    const bool initially_running = is_vm_running(provider, vm->vmx);

    const std::string run_id = plan.run_id.value_or(make_id("run"));
    validate_identifier(run_id, "orchestration run_id");
    OrchestrationArchive archive = prepare_or_load_archive(
        config_,
        plan_path,
        run_id,
        vm->id);
    RunLifecycleState& state = archive.state;
    const std::filesystem::path& state_path = archive.state_path;
    nlohmann::json output = {
        {"schema_version", 1},
        {"run_id", run_id},
        {"vm_id", vm->id},
        {"lifecycle_path", path_to_utf8(state_path)},
        {"resumed", archive.resumed},
    };
    if (archive.resumed && is_terminal_run_phase(state.phase)) {
        apply_terminal_state_output(output, state, policy);
        return output;
    }
    if (archive.resumed && state.phase != RunPhase::Executing &&
        state.phase != RunPhase::CollectingEvidence) {
        const std::string error =
            "Host restart cannot safely resume orchestration phase: " +
            std::string(run_phase_name(state.phase));
        persist_run_transition(
            state_path,
            state,
            RunPhase::ManualInterventionRequired,
            utc_timestamp(),
            error);
        output["status"] = "MANUAL_INTERVENTION_REQUIRED";
        output["error"] = error;
        return output;
    }

    Controller controller(config_);
    bool assumed_running = initially_running;
    bool agent_ready = archive.resumed;
    bool main_published = archive.resumed;
    bool business_success = false;
    bool manual_gate = false;
    std::string business_error;

    if (archive.resumed) {
        output["execution_run_id"] = archive.identity.main_run_id;
        try {
            output["report"] = state.phase == RunPhase::Executing
                ? wait_for_report(controller, archive.identity.main_run_id, timeout)
                : controller.build_report(archive.identity.main_run_id);
            if (!output["report"].at("complete").get<bool>() &&
                !output["report"].value("manual_intervention_required", false)) {
                throw Error("Persisted evidence collection phase has an incomplete main report");
            }
            apply_main_report(
                output["report"],
                business_success,
                manual_gate,
                business_error);
        } catch (const std::exception& error) {
            append_error(business_error, error.what());
        }
    } else {
        try {
            if (policy.restore_before.has_value()) {
                persist_run_transition(
                    state_path,
                    state,
                    RunPhase::RestoringBefore,
                    utc_timestamp(),
                    "restore before execution");
                if (assumed_running) {
                    provider.stop(vm->vmx, vmware::VmStopMode::Hard);
                    assumed_running = false;
                }
                provider.revert_to_snapshot(vm->vmx, *policy.restore_before);
            }

            persist_run_transition(
                state_path,
                state,
                RunPhase::StartingVm,
                utc_timestamp(),
                "start target VM");
            if (!assumed_running) {
                provider.start(vm->vmx);
                assumed_running = true;
            }
            persist_run_transition(
                state_path,
                state,
                RunPhase::WaitingAgent,
                utc_timestamp(),
                "wait for Agent diagnostic echo");
            Diagnostics diagnostics(config_);
            const nlohmann::json diagnostic = diagnostics.run_probe(vm->id, timeout);
            output["diagnostic"] = diagnostic;
            if (diagnostic.at("status") != "ready") {
                throw Error("Agent diagnostic did not return ready");
            }
            agent_ready = true;

            persist_run_transition(
                state_path,
                state,
                RunPhase::Deploying,
                utc_timestamp(),
                "publish main task");
            const RunManifest manifest = controller.create_run(
                make_main_plan(plan, archive.identity.main_run_id));
            main_published = true;
            output["execution_run_id"] = manifest.run_id;
            persist_run_transition(
                state_path,
                state,
                RunPhase::Executing,
                utc_timestamp(),
                "wait for main task results");
            output["report"] = wait_for_report(controller, manifest.run_id, timeout);
            apply_main_report(
                output["report"],
                business_success,
                manual_gate,
                business_error);
        } catch (const std::exception& error) {
            append_error(business_error, error.what());
        }
    }

    if (state.phase == RunPhase::RestoringBefore) {
        persist_run_transition(
            state_path,
            state,
            RunPhase::RecoveryFailed,
            utc_timestamp(),
            business_error);
        output["status"] = "RECOVERY_FAILED";
        output["error"] = business_error;
        return output;
    }

    if (main_published) {
        if (state.phase == RunPhase::Executing) {
            persist_run_transition(
                state_path,
                state,
                RunPhase::CollectingEvidence,
                utc_timestamp(),
                "archive main task evidence");
        }
        try {
            if (!output.contains("report")) {
                output["report"] = controller.build_report(archive.identity.main_run_id);
            }
            archive_run_evidence(
                config_,
                run_id,
                archive.identity.main_run_id,
                "main");
        } catch (const std::exception& error) {
            business_success = false;
            append_error(business_error, error.what());
        }
    }

    if (manual_gate) {
        persist_run_transition(
            state_path,
            state,
            RunPhase::ManualInterventionRequired,
            utc_timestamp(),
            business_error);
        output["status"] = "MANUAL_INTERVENTION_REQUIRED";
        output["error"] = business_error;
        return output;
    }

    if (agent_ready &&
        (state.phase == RunPhase::CollectingEvidence || state.phase == RunPhase::Deploying)) {
        persist_run_transition(
            state_path,
            state,
            RunPhase::RunningFinally,
            utc_timestamp(),
            "execute finally steps");
        if (!plan.lifecycle->finally_steps.empty()) {
            try {
                const TaskPlan finally_plan = make_finally_plan(
                    plan,
                    archive.identity.finally_run_id);
                const RunManifest finally_manifest = controller.create_run(finally_plan);
                output["finally_run_id"] = finally_manifest.run_id;
                output["finally_report"] = wait_for_report(controller, finally_manifest.run_id, timeout);
                archive_run_evidence(config_, run_id, finally_manifest.run_id, "finally");
                if (output["finally_report"].at("failed_steps") != 0) {
                    business_success = false;
                    append_error(business_error, "Finally steps failed");
                }
            } catch (const std::exception& error) {
                business_success = false;
                append_error(business_error, error.what());
            }
        }
    }

    if (state.phase != RunPhase::RunningFinally &&
        state.phase != RunPhase::StartingVm &&
        state.phase != RunPhase::WaitingAgent &&
        state.phase != RunPhase::Deploying &&
        state.phase != RunPhase::CollectingEvidence) {
        throw Error("Orchestration reached an unsupported recovery phase");
    }
    persist_run_transition(
        state_path,
        state,
        RunPhase::Recovering,
        utc_timestamp(),
        business_success ? "apply success cleanup policy" : "apply failure cleanup policy");

    const VmCleanupPolicy& cleanup = business_success ? policy.on_success : policy.on_failure;
    try {
        assumed_running = apply_cleanup_policy(provider, *vm, cleanup, assumed_running);
    } catch (const std::exception& error) {
        append_error(business_error, error.what());
        persist_run_transition(
            state_path,
            state,
            RunPhase::RecoveryFailed,
            utc_timestamp(),
            business_error);
        output["status"] = "RECOVERY_FAILED";
        output["error"] = business_error;
        return output;
    }

    persist_run_transition(
        state_path,
        state,
        business_success ? RunPhase::Completed : RunPhase::Failed,
        utc_timestamp(),
        business_success ? "orchestration completed" : business_error);
    output["status"] = business_success ? "COMPLETED" : "FAILED";
    output["cleanup_action"] = vm_cleanup_action_name(cleanup.action);
    if (!business_error.empty()) {
        output["error"] = business_error;
    }
    return output;
}

}  // namespace satsuma::host
