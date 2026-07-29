// Host 多 VM 生命周期编排和不可变证据归档实现。
#include "orchestrator.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

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
    std::vector<std::string> vm_ids; // 按准备顺序冻结的生命周期 VM
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

constexpr std::chrono::seconds kVmStateReconciliationTimeout{5}; // 启停报错后的状态收敛等待上限
constexpr std::chrono::milliseconds kVmStateReconciliationDelay{100}; // 状态对账轮询间隔
constexpr std::chrono::seconds kSharedRunDeleteTimeout{5}; // 等待 Agent 释放共享目录句柄的上限
constexpr std::chrono::milliseconds kSharedRunDeleteDelay{100}; // 共享目录删除重试间隔

// 返回指定编排在 Host 归档中的稳定根目录。
[[nodiscard]] std::filesystem::path orchestration_archive_root(
    const LabConfig& config,
    const std::string& run_id) {
    const std::filesystem::path runs_root = resolve_under_root(
        config.host.archive_root,
        L"runs");
    return resolve_under_root(runs_root, path_from_utf8(run_id));
}

// 创建不依赖 VMware 当前状态的编排输出公共字段。
[[nodiscard]] nlohmann::json make_orchestration_output(
    const OrchestrationArchive& archive) {
    nlohmann::json output = {
        {"schema_version", 1},
        {"run_id", archive.identity.run_id},
        {"lifecycle_path", path_to_utf8(archive.state_path)},
        {"resumed", archive.resumed},
    };
    if (archive.identity.vm_ids.size() == 1) {
        output["vm_id"] = archive.identity.vm_ids.front();
    } else {
        output["vm_ids"] = archive.identity.vm_ids;
    }
    return output;
}

// 返回编排计划声明的生命周期策略。
[[nodiscard]] const std::vector<VmLifecyclePolicy>& require_lifecycle_policies(
    const TaskPlan& plan) {
    if (!plan.lifecycle.has_value()) {
        throw Error("Host orchestrate requires a lifecycle policy");
    }
    return plan.lifecycle->vms;
}

// 验证任务涉及的全部 VM 都由当前生命周期策略覆盖。
void validate_plan_scope(
    const TaskPlan& plan,
    const std::vector<VmLifecyclePolicy>& policies) {
    std::unordered_set<std::string> vm_ids;
    for (const VmLifecyclePolicy& policy : policies) {
        vm_ids.insert(policy.vm);
    }
    const auto require_vm = [&vm_ids](
                                const std::string& referenced_vm,
                                const std::string& field) {
        if (!vm_ids.contains(referenced_vm)) {
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
    nlohmann::json value = {
        {"schema_version", identity.schema_version},
        {"run_id", identity.run_id},
        {"main_run_id", identity.main_run_id},
        {"finally_run_id", identity.finally_run_id},
        {"plan_sha256", identity.plan_sha256},
    };
    if (identity.schema_version == 1) {
        value["vm_id"] = identity.vm_ids.front();
    } else {
        value["vm_ids"] = identity.vm_ids;
    }
    return value;
}

// 读取并验证 Host 重启所需的不可变编排身份。
[[nodiscard]] OrchestrationIdentity load_orchestration_identity(
    const std::filesystem::path& path) {
    try {
        const nlohmann::json value = load_json(path);
        OrchestrationIdentity identity;
        identity.schema_version = value.value("schema_version", 0);
        identity.run_id = value.at("run_id").get<std::string>();
        if (identity.schema_version == 1) {
            identity.vm_ids.push_back(value.at("vm_id").get<std::string>());
        } else if (identity.schema_version == 2) {
            identity.vm_ids = value.at("vm_ids").get<std::vector<std::string>>();
        } else {
            throw Error("Orchestration identity requires schema_version 1 or 2");
        }
        identity.main_run_id = value.at("main_run_id").get<std::string>();
        identity.finally_run_id = value.at("finally_run_id").get<std::string>();
        identity.plan_sha256 = value.at("plan_sha256").get<std::string>();
        validate_identifier(identity.run_id, "orchestration identity run_id");
        if (identity.vm_ids.empty()) {
            throw Error("Orchestration identity requires at least one VM");
        }
        std::unordered_set<std::string> unique_vm_ids;
        for (const std::string& vm_id : identity.vm_ids) {
            validate_identifier(vm_id, "orchestration identity VM id");
            if (!unique_vm_ids.insert(vm_id).second) {
                throw Error("Orchestration identity contains a duplicate VM: " + vm_id);
            }
        }
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
    const std::vector<std::string>& vm_ids) {
    const std::filesystem::path runs_root = resolve_under_root(
        config.host.archive_root,
        L"runs");
    const std::filesystem::path root = orchestration_archive_root(config, run_id);
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
        if (identity.run_id != run_id || identity.vm_ids != vm_ids ||
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
        vm_ids.size() == 1 ? 1 : 2,
        run_id,
        vm_ids,
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
        rename_path_with_retry(staging, root);
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

// 在有限时间内等待 vmrun 运行列表收敛到目标电源状态。
[[nodiscard]] bool wait_for_vm_running_state(
    const vmware::VmrunProvider& provider,
    const std::filesystem::path& vmx,
    const bool expected_running) {
    const auto deadline = std::chrono::steady_clock::now() + kVmStateReconciliationTimeout;
    std::string last_error; // 最后一次运行列表查询错误
    for (;;) {
        try {
            if (is_vm_running(provider, vmx) == expected_running) {
                return true;
            }
            last_error.clear();
        } catch (const std::exception& error) {
            last_error = error.what();
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            break;
        }
        std::this_thread::sleep_for(kVmStateReconciliationDelay);
    }
    if (!last_error.empty()) {
        throw Error("VM state reconciliation failed: " + last_error);
    }
    return false;
}

// vmrun 启动报错后以运行列表对账，避免晚到成功被误判为失败。
void start_vm_with_reconciliation(
    const vmware::VmrunProvider& provider,
    const std::filesystem::path& vmx) {
    try {
        provider.start(vmx);
    } catch (const std::exception& operation_error) {
        try {
            if (wait_for_vm_running_state(provider, vmx, true)) {
                return;
            }
        } catch (const std::exception& reconciliation_error) {
            throw Error(
                std::string(operation_error.what()) +
                "; cannot reconcile VM start: " + reconciliation_error.what());
        }
        throw Error(
            std::string(operation_error.what()) +
            "; VM remained stopped after start reconciliation");
    }
}

// vmrun 关闭报错后以运行列表对账，确认已关机时继续清理流程。
void stop_vm_with_reconciliation(
    const vmware::VmrunProvider& provider,
    const std::filesystem::path& vmx,
    const vmware::VmStopMode mode) {
    try {
        provider.stop(vmx, mode);
    } catch (const std::exception& operation_error) {
        try {
            if (wait_for_vm_running_state(provider, vmx, false)) {
                return;
            }
        } catch (const std::exception& reconciliation_error) {
            throw Error(
                std::string(operation_error.what()) +
                "; cannot reconcile VM stop: " + reconciliation_error.what());
        }
        throw Error(
            std::string(operation_error.what()) +
            "; VM remained running after stop reconciliation");
    }
}

// 有限等待运行目录中的全部预期结果完成。
[[nodiscard]] nlohmann::json wait_for_report(
    const Controller& controller,
    const std::string& run_id,
    const std::chrono::seconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    nlohmann::json report;
    std::string last_io_error; // 共享层瞬时读取失败的最后一条诊断
    do {
        try {
            report = controller.build_report(run_id);
            last_io_error.clear();
            if (report.at("complete").get<bool>() ||
                report.value("manual_intervention_required", false)) {
                return report;
            }
        } catch (const JsonIoError& error) {
            // 已原子发布的结果可能被安全软件短暂占用，下一轮重新读取完整证据。
            last_io_error = error.what();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    } while (std::chrono::steady_clock::now() < deadline);
    throw Error(
        "Run did not complete before orchestration timeout: " + run_id +
        (last_io_error.empty() ? "" : "; last JSON I/O error: " + last_io_error));
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

// 生成证据目录内普通文件的稳定路径、大小和哈希清单。
[[nodiscard]] nlohmann::json build_evidence_file_manifest(
    const std::filesystem::path& root) {
    struct EvidenceFile {
        std::string path;
        std::uintmax_t size;
        std::string sha256;
    };
    std::vector<EvidenceFile> files;
    std::filesystem::recursive_directory_iterator iterator(root);
    const std::filesystem::recursive_directory_iterator end;
    for (; iterator != end; ++iterator) {
        const DWORD attributes = GetFileAttributesW(iterator->path().c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES) {
            throw Error("Cannot inspect archived evidence path");
        }
        if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
            iterator.disable_recursion_pending();
            throw Error("Archived evidence contains a forbidden reparse point");
        }
        if (iterator->is_directory()) {
            continue;
        }
        if (!iterator->is_regular_file()) {
            throw Error("Archived evidence contains an unsupported file type");
        }
        const std::filesystem::path relative = iterator->path().lexically_relative(root);
        if (relative == L".archive-complete.json") {
            continue;
        }
        files.push_back({
            path_to_utf8(relative),
            std::filesystem::file_size(iterator->path()),
            sha256_file(iterator->path()),
        });
    }
    std::sort(files.begin(), files.end(), [](const EvidenceFile& left, const EvidenceFile& right) {
        return left.path < right.path;
    });
    nlohmann::json manifest = nlohmann::json::array();
    for (const EvidenceFile& file : files) {
        manifest.push_back({
            {"path", file.path},
            {"size", file.size},
            {"sha256", file.sha256},
        });
    }
    return manifest;
}

// 验证已发布归档的完成标记和全部文件摘要。
void validate_archived_evidence(const std::filesystem::path& destination) {
    const nlohmann::json marker = load_json(destination / L".archive-complete.json");
    if (marker.value("schema_version", 0) != 1 ||
        marker.value("status", std::string{}) != "complete" ||
        !marker.contains("files") ||
        marker.at("files") != build_evidence_file_manifest(destination)) {
        throw Error("Run evidence archive failed validation: " + path_to_utf8(destination));
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
        validate_archived_evidence(destination);
        return;
    }

    const std::filesystem::path staging = resolve_under_root(
        archive_root,
        path_from_utf8("." + label + "-" + make_id("archive")));
    try {
        const nlohmann::json source_files = build_evidence_file_manifest(source);
        copy_tree_without_reparse_points(source, staging);
        if (source_files != build_evidence_file_manifest(staging)) {
            throw Error("Run evidence changed while it was being archived");
        }
        write_json_atomic(staging / L".archive-complete.json", {
            {"schema_version", 1},
            {"status", "complete"},
            {"source_run_id", execution_run_id},
            {"archived_at", utc_timestamp()},
            {"files", source_files},
        });
        rename_path_with_retry(staging, destination);
        validate_archived_evidence(destination);
    } catch (...) {
        std::error_code cleanup_error;
        std::filesystem::remove_all(staging, cleanup_error);
        throw;
    }
}

// 请求指定 Agent 删除当前运行的统一 Guest 工作目录并等待回执。
[[nodiscard]] nlohmann::json request_guest_work_cleanup(
    const LabConfig& config,
    const std::string& run_id,
    const std::string& vm_id,
    const std::chrono::seconds timeout) {
    const std::filesystem::path run_directory = resolve_under_root(
        config.shared_folder.host_root,
        std::filesystem::path(L"runs") / path_from_utf8(run_id));
    const std::filesystem::path state_directory = run_directory / L"state";
    const std::filesystem::path request_path =
        state_directory / path_from_utf8(vm_id + "-cleanup-request.json");
    const std::filesystem::path result_path =
        state_directory / path_from_utf8(vm_id + "-cleanup.json");

    std::string request_id;
    if (std::filesystem::is_regular_file(request_path)) {
        const nlohmann::json request = load_json(request_path);
        request_id = request.value("request_id", std::string{});
        validate_identifier(request_id, "cleanup request_id");
        if (request.value("schema_version", 0) != 1 ||
            request.value("lab_id", std::string{}) != config.lab_id ||
            request.value("run_id", std::string{}) != run_id ||
            request.value("vm_id", std::string{}) != vm_id ||
            request.value("target", std::string{}) != "guest_work") {
            throw Error("Persisted Guest cleanup request identity is invalid");
        }
    } else {
        request_id = make_id("cleanup");
        write_json_atomic(request_path, {
            {"schema_version", 1},
            {"lab_id", config.lab_id},
            {"run_id", run_id},
            {"vm_id", vm_id},
            {"request_id", request_id},
            {"target", "guest_work"},
            {"requested_at", utc_timestamp()},
        });
    }

    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (true) {
        if (std::filesystem::is_regular_file(result_path)) {
            const nlohmann::json result = load_json(result_path);
            if (result.value("schema_version", 0) != 1 ||
                result.value("lab_id", std::string{}) != config.lab_id ||
                result.value("run_id", std::string{}) != run_id ||
                result.value("vm_id", std::string{}) != vm_id ||
                result.value("request_id", std::string{}) != request_id ||
                result.value("target", std::string{}) != "guest_work") {
                throw Error("Guest cleanup result identity is invalid");
            }
            const std::string status = result.value("status", std::string{});
            if (status == "deleted") {
                return result;
            }
            if (status == "failed") {
                throw Error(
                    "Guest cleanup failed for VM " + vm_id + " in run " + run_id +
                    ": " + result.value("error", std::string("unknown error")));
            }
            throw Error("Guest cleanup result status is invalid");
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            throw Error("Timed out while waiting for Guest cleanup: " + vm_id + "/" + run_id);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

// 删除已经归档并完成 Guest 清理的 Shared Folder 运行目录。
void delete_shared_run(const LabConfig& config, const std::string& run_id) {
    const std::filesystem::path run_directory = resolve_under_root(
        config.shared_folder.host_root,
        std::filesystem::path(L"runs") / path_from_utf8(run_id));
    const DWORD attributes = GetFileAttributesW(run_directory.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES ||
        (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        throw Error("Shared run directory is missing or unsafe: " + run_id);
    }

    const auto deadline = std::chrono::steady_clock::now() + kSharedRunDeleteTimeout;
    std::error_code last_error;
    while (true) {
        std::error_code remove_error;
        std::filesystem::remove_all(run_directory, remove_error);

        std::error_code exists_error;
        const bool still_exists = std::filesystem::exists(run_directory, exists_error);
        if (!still_exists && !exists_error) {
            return;
        }
        last_error = remove_error ? remove_error : exists_error;
        if (std::chrono::steady_clock::now() >= deadline) {
            const std::string detail = last_error
                ? last_error.message()
                : "directory still exists";
            throw Error(
                "Failed to delete Shared run directory within retry timeout: " +
                run_id + ": " + detail);
        }
        std::this_thread::sleep_for(kSharedRunDeleteDelay);
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
    const std::vector<VmLifecyclePolicy>& policies,
    const TaskCleanupPolicy& task_cleanup) {
    const bool business_success = state.phase == RunPhase::Completed;
    switch (state.phase) {
    case RunPhase::Completed:
        output["status"] = "COMPLETED";
        break;
    case RunPhase::Failed:
        output["status"] = "FAILED";
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
    if (state.phase == RunPhase::Completed || state.phase == RunPhase::Failed) {
        const GuestWorkCleanupAction guest_action = business_success
            ? task_cleanup.guest_work_on_success
            : task_cleanup.guest_work_on_failure;
        const SharedRunCleanupAction shared_action = business_success
            ? task_cleanup.shared_run_on_success
            : task_cleanup.shared_run_on_failure;
        output["guest_work_cleanup"] = guest_work_cleanup_action_name(guest_action);
        output["shared_run_cleanup"] = shared_run_cleanup_action_name(shared_action);
        if (policies.size() == 1) {
            const VmCleanupPolicy& cleanup = business_success
                ? policies.front().on_success
                : policies.front().on_failure;
            output["cleanup_action"] = vm_cleanup_action_name(cleanup.action);
        } else {
            output["cleanup_actions"] = nlohmann::json::array();
            for (const VmLifecyclePolicy& policy : policies) {
                const VmCleanupPolicy& cleanup = business_success
                    ? policy.on_success
                    : policy.on_failure;
                output["cleanup_actions"].push_back({
                    {"vm_id", policy.vm},
                    {"action", vm_cleanup_action_name(cleanup.action)},
                });
            }
        }
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
            stop_vm_with_reconciliation(provider, vm.vmx, vmware::VmStopMode::Soft);
        }
        return false;
    }
    if (assumed_running) {
        stop_vm_with_reconciliation(provider, vm.vmx, vmware::VmStopMode::Hard);
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

    // 新运行依次准备、执行、取证、finally 和逆序清理；恢复只接管可安全重入的后半阶段。
    TaskPlan plan = load_task_plan(plan_path);
    const std::vector<VmLifecyclePolicy>& policies = require_lifecycle_policies(plan);
    validate_plan_scope(plan, policies);
    if (!plan.run_id.has_value()) {
        throw Error("Host orchestrate requires an explicit plan run_id for crash recovery");
    }
    const std::string& run_id = *plan.run_id;
    validate_identifier(run_id, "orchestration run_id");

    std::vector<std::string> vm_ids; // 按生命周期声明顺序冻结的 VM 身份
    std::vector<const VmConfig*> vms; // 与策略下标一一对应的实验室配置
    vm_ids.reserve(policies.size());
    vms.reserve(policies.size());
    for (const VmLifecyclePolicy& policy : policies) {
        const VmConfig* vm = find_vm(config_, policy.vm);
        if (vm == nullptr) {
            throw Error("Lifecycle policy references an unknown VM: " + policy.vm);
        }
        vm_ids.push_back(policy.vm);
        vms.push_back(vm);
    }

    std::optional<OrchestrationArchive> archive;
    if (std::filesystem::exists(orchestration_archive_root(config_, run_id))) {
        archive = prepare_or_load_archive(
            config_,
            plan_path,
            run_id,
            vm_ids);
        if (is_terminal_run_phase(archive->state.phase)) {
            nlohmann::json output = make_orchestration_output(*archive);
            apply_terminal_state_output(output, archive->state, policies, plan.cleanup);
            return output;
        }
    }

    vmware::VmrunProvider provider(config_.provider.vmrun);
    std::vector<bool> initially_running; // 外部操作前逐台采样的运行状态
    initially_running.reserve(vms.size());
    for (std::size_t index = 0; index < vms.size(); ++index) {
        const VmConfig& vm = *vms[index];
        const VmLifecyclePolicy& policy = policies[index];
        const std::vector<std::string> snapshots = provider.list_snapshots(vm.vmx);
        if (policy.restore_before.has_value()) {
            validate_managed_snapshot(vm, snapshots, *policy.restore_before);
        }
        if (policy.on_success.action == VmCleanupAction::Restore) {
            validate_managed_snapshot(vm, snapshots, *policy.on_success.snapshot);
        }
        if (policy.on_failure.action == VmCleanupAction::Restore) {
            validate_managed_snapshot(vm, snapshots, *policy.on_failure.snapshot);
        }
        initially_running.push_back(is_vm_running(provider, vm.vmx));
    }

    if (!archive.has_value()) {
        archive = prepare_or_load_archive(
            config_,
            plan_path,
            run_id,
            vm_ids);
    }
    OrchestrationArchive& active_archive = *archive;
    RunLifecycleState& state = active_archive.state;
    const std::filesystem::path& state_path = active_archive.state_path;
    nlohmann::json output = make_orchestration_output(active_archive);
    if (active_archive.resumed && state.phase != RunPhase::Executing &&
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
    std::vector<bool> assumed_running = initially_running; // 编排期间逐台维护的预期状态
    bool agent_ready = active_archive.resumed;
    bool main_published = active_archive.resumed;
    bool finally_published = false;
    bool business_success = false;
    bool manual_gate = false;
    std::string business_error;

    if (active_archive.resumed) {
        output["execution_run_id"] = active_archive.identity.main_run_id;
        try {
            output["report"] = state.phase == RunPhase::Executing
                ? wait_for_report(controller, active_archive.identity.main_run_id, timeout)
                : controller.build_report(active_archive.identity.main_run_id);
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
            const bool requires_restore = std::any_of( // 是否需要进入统一恢复阶段
                policies.begin(),
                policies.end(),
                [](const VmLifecyclePolicy& policy) {
                    return policy.restore_before.has_value();
                });
            if (requires_restore) {
                persist_run_transition(
                    state_path,
                    state,
                    RunPhase::RestoringBefore,
                    utc_timestamp(),
                    "restore target VMs before execution");
                for (std::size_t index = 0; index < vms.size(); ++index) {
                    const VmLifecyclePolicy& policy = policies[index];
                    if (!policy.restore_before.has_value()) {
                        continue;
                    }
                    const VmConfig& vm = *vms[index];
                    if (assumed_running[index]) {
                        stop_vm_with_reconciliation(
                            provider,
                            vm.vmx,
                            vmware::VmStopMode::Hard);
                        assumed_running[index] = false;
                    }
                    provider.revert_to_snapshot(vm.vmx, *policy.restore_before);
                }
            }

            persist_run_transition(
                state_path,
                state,
                RunPhase::StartingVm,
                utc_timestamp(),
                "start target VMs in lifecycle order");
            for (std::size_t index = 0; index < vms.size(); ++index) {
                if (!assumed_running[index]) {
                    start_vm_with_reconciliation(provider, vms[index]->vmx);
                    assumed_running[index] = true;
                }
            }
            persist_run_transition(
                state_path,
                state,
                RunPhase::WaitingAgent,
                utc_timestamp(),
                "wait for Agents in lifecycle order");
            Diagnostics diagnostics(config_);
            const std::chrono::seconds diagnostic_timeout = std::min(
                timeout,
                std::chrono::seconds(300)); // 编排总等待可超过独立诊断上限
            if (vms.size() > 1) {
                output["diagnostics"] = nlohmann::json::array();
            }
            for (const VmConfig* vm : vms) {
                nlohmann::json diagnostic = diagnostics.run_probe(vm->id, diagnostic_timeout);
                if (vms.size() == 1) {
                    output["diagnostic"] = diagnostic;
                } else {
                    output["diagnostics"].push_back({
                        {"vm_id", vm->id},
                        {"result", diagnostic},
                    });
                }
                if (diagnostic.at("status") != "ready") {
                    throw Error("Agent diagnostic did not return ready for VM: " + vm->id);
                }
            }
            agent_ready = true;

            persist_run_transition(
                state_path,
                state,
                RunPhase::Deploying,
                utc_timestamp(),
                "publish main task");
            const RunManifest manifest = controller.create_run(
                make_main_plan(plan, active_archive.identity.main_run_id));
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
                output["report"] = controller.build_report(
                    active_archive.identity.main_run_id);
            }
            archive_run_evidence(
                config_,
                run_id,
                active_archive.identity.main_run_id,
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
                    active_archive.identity.finally_run_id);
                const RunManifest finally_manifest = controller.create_run(finally_plan);
                finally_published = true;
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

    const GuestWorkCleanupAction guest_cleanup = business_success
        ? plan.cleanup.guest_work_on_success
        : plan.cleanup.guest_work_on_failure;
    const SharedRunCleanupAction shared_cleanup = business_success
        ? plan.cleanup.shared_run_on_success
        : plan.cleanup.shared_run_on_failure;
    output["guest_work_cleanup"] = guest_work_cleanup_action_name(guest_cleanup);
    output["shared_run_cleanup"] = shared_run_cleanup_action_name(shared_cleanup);

    std::vector<std::string> execution_run_ids;
    if (main_published) {
        execution_run_ids.push_back(active_archive.identity.main_run_id);
    }
    if (finally_published) {
        execution_run_ids.push_back(active_archive.identity.finally_run_id);
    }

    bool cleanup_failed = false; // 任一目录或 VM 清理失败后仍继续处理其余目标
    if (guest_cleanup == GuestWorkCleanupAction::Delete) {
        output["guest_cleanup_results"] = nlohmann::json::array();
        for (std::size_t reverse_index = policies.size(); reverse_index > 0; --reverse_index) {
            const std::size_t index = reverse_index - 1;
            const VmLifecyclePolicy& policy = policies[index];
            const VmCleanupPolicy& vm_cleanup = business_success
                ? policy.on_success
                : policy.on_failure;
            if (vm_cleanup.action == VmCleanupAction::Restore) {
                continue;
            }
            for (const std::string& execution_run_id : execution_run_ids) {
                try {
                    output["guest_cleanup_results"].push_back(request_guest_work_cleanup(
                        config_, execution_run_id, policy.vm, timeout));
                } catch (const std::exception& error) {
                    cleanup_failed = true;
                    append_error(business_error, error.what());
                }
            }
        }
    }

    if (!cleanup_failed && shared_cleanup == SharedRunCleanupAction::ArchiveThenDelete) {
        try {
            for (const std::string& execution_run_id : execution_run_ids) {
                delete_shared_run(config_, execution_run_id);
            }
        } catch (const std::exception& error) {
            cleanup_failed = true;
            append_error(business_error, error.what());
        }
    }

    for (std::size_t reverse_index = policies.size(); reverse_index > 0; --reverse_index) {
        const std::size_t index = reverse_index - 1;
        const VmLifecyclePolicy& policy = policies[index];
        const VmCleanupPolicy& cleanup = business_success
            ? policy.on_success
            : policy.on_failure;
        try {
            assumed_running[index] = apply_cleanup_policy(
                provider,
                *vms[index],
                cleanup,
                assumed_running[index]);
        } catch (const std::exception& error) {
            cleanup_failed = true;
            append_error(
                business_error,
                "VM lifecycle cleanup failed for " + policy.vm + ": " + error.what());
        }
    }

    if (cleanup_failed) {
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

    if (policies.size() == 1) {
        const VmCleanupPolicy& cleanup = business_success
            ? policies.front().on_success
            : policies.front().on_failure;
        output["cleanup_action"] = vm_cleanup_action_name(cleanup.action);
    } else {
        output["cleanup_actions"] = nlohmann::json::array();
        for (const VmLifecyclePolicy& policy : policies) {
            const VmCleanupPolicy& cleanup = business_success
                ? policy.on_success
                : policy.on_failure;
            output["cleanup_actions"].push_back({
                {"vm_id", policy.vm},
                {"action", vm_cleanup_action_name(cleanup.action)},
            });
        }
    }

    persist_run_transition(
        state_path,
        state,
        business_success ? RunPhase::Completed : RunPhase::Failed,
        utc_timestamp(),
        business_success ? "orchestration completed" : business_error);
    output["status"] = business_success ? "COMPLETED" : "FAILED";
    if (!business_error.empty()) {
        output["error"] = business_error;
    }
    return output;
}

}  // namespace satsuma::host
