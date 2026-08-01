// Host 任务报告规范路径和执行身份校验测试。
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

#include "controller.hpp"
#include "identity.hpp"
#include "lab_lease.hpp"
#include "satsuma/core/config.hpp"
#include "satsuma/core/errors.hpp"
#include "satsuma/core/id.hpp"
#include "satsuma/core/json_io.hpp"
#include "satsuma/core/path.hpp"
#include "satsuma/core/sha256.hpp"
#include "satsuma/core/task.hpp"

namespace {

// 在条件不满足时终止当前测试。
void expect(const bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

// 断言操作被安全门禁拒绝。
template <typename Operation>
void expect_error(Operation operation, const std::string& message) {
    try {
        operation();
    } catch (const satsuma::Error&) {
        return;
    }
    throw std::runtime_error(message);
}

// 创建只包含 Host 文件任务所需字段的实验室配置。
[[nodiscard]] satsuma::LabConfig make_config(const std::filesystem::path& root) {
    satsuma::LabConfig config;
    config.lab_id = "host_controller_test";
    config.shared_folder.host_root = root / L"share";
    satsuma::VmConfig vm;
    vm.id = "vm_01";
    config.vms.push_back(std::move(vm));
    return config;
}

// 创建两个 echo 步骤的固定测试计划。
[[nodiscard]] satsuma::TaskPlan make_plan(const std::string& run_id) {
    satsuma::TaskPlan plan;
    plan.name = "host report canonical results";
    plan.run_id = run_id;
    for (const std::string& step_id : {"first", "second"}) {
        satsuma::TaskStep step;
        step.id = step_id;
        step.vm = "vm_01";
        step.type = "echo";
        step.message = step_id;
        step.retry_safe = true;
        plan.steps.push_back(std::move(step));
    }
    return plan;
}

// 创建属于指定清单步骤的成功执行结果。
[[nodiscard]] satsuma::ExecutionResult make_execution(
    const satsuma::RunManifest& manifest,
    const satsuma::TaskStep& step,
    const std::string& job_id) {
    satsuma::ExecutionResult result;
    result.run_id = manifest.run_id;
    result.vm_id = step.vm;
    result.job_id = job_id;
    result.step_id = step.id;
    result.status = "exited";
    result.run_as = step.run_as;
    result.exit_code = 0;
    result.stdout_path = "stdout.log";
    result.stderr_path = "stderr.log";
    result.started_at = "2026-07-27T00:00:00.000Z";
    result.finished_at = "2026-07-27T00:00:01.000Z";
    return result;
}

// 返回某步骤的规范 execution.json 路径。
[[nodiscard]] std::filesystem::path result_path(
    const satsuma::LabConfig& config,
    const std::string& run_id,
    const satsuma::TaskStep& step) {
    return config.shared_folder.host_root / L"runs" /
        satsuma::path_from_utf8(run_id) / L"results" /
        satsuma::path_from_utf8(step.vm) /
        satsuma::path_from_utf8(step.id) / L"execution.json";
}

// 验证收集文件中的同名 JSON 不会伪造步骤完成数量。
void test_report_uses_canonical_result_paths(const std::filesystem::path& root) {
    const satsuma::LabConfig config = make_config(root);
    const satsuma::host::Controller controller(config);
    const satsuma::RunManifest manifest = controller.create_run(make_plan("report_paths"));
    const std::filesystem::path run_root =
        config.shared_folder.host_root / L"runs" / L"report_paths";
    const std::filesystem::path collected =
        run_root / L"results" / L"vm_01" / L"first" /
        L"files" / L"nested" / L"execution.json";
    satsuma::write_json_atomic(collected, {
        {"status", "exited"},
        {"exit_code", 0},
    });

    satsuma::ExecutionResult first = make_execution(
        manifest,
        manifest.steps.at(0),
        "job_report_first");
    first.files.push_back({
        satsuma::path_to_utf8(std::filesystem::relative(collected, run_root)),
        satsuma::sha256_file(collected),
    });
    satsuma::write_json_atomic(
        result_path(config, manifest.run_id, manifest.steps.at(0)),
        first);

    const nlohmann::json incomplete = controller.build_report(manifest.run_id);
    expect(
        incomplete.at("reported_steps") == 1 &&
            incomplete.at("successful_steps") == 1 &&
            !incomplete.at("complete").get<bool>() &&
            incomplete.at("status") == "pending",
        "collected execution.json changed the canonical completion count");

    satsuma::write_json_atomic(
        result_path(config, manifest.run_id, manifest.steps.at(1)),
        make_execution(manifest, manifest.steps.at(1), "job_report_second"));
    const nlohmann::json complete = controller.build_report(manifest.run_id);
    expect(
        complete.at("reported_steps") == 2 &&
            complete.at("successful_steps") == 2 &&
            complete.at("complete").get<bool>() &&
            complete.at("status") == "succeeded",
        "canonical execution results did not complete the report");

    satsuma::ExecutionResult failed = make_execution(
        manifest,
        manifest.steps.at(1),
        "job_report_second");
    failed.exit_code = 7;
    satsuma::write_json_atomic(
        result_path(config, manifest.run_id, manifest.steps.at(1)),
        failed);
    const nlohmann::json failed_report = controller.build_report(manifest.run_id);
    expect(
        failed_report.at("complete").get<bool>() &&
            failed_report.at("failed_steps") == 1 &&
            failed_report.at("status") == "failed",
        "complete report did not expose its failed business status");
}

// 验证危险 claim 恢复门禁优先成为报告顶层状态。
void test_report_exposes_manual_intervention(const std::filesystem::path& root) {
    const satsuma::LabConfig config = make_config(root);
    const satsuma::host::Controller controller(config);
    satsuma::TaskPlan plan = make_plan("report_manual_intervention");
    plan.steps.resize(1);
    const satsuma::RunManifest manifest = controller.create_run(plan);
    const satsuma::TaskStep& step = manifest.steps.front();
    const std::filesystem::path recovery_path =
        config.shared_folder.host_root / L"runs" /
        satsuma::path_from_utf8(manifest.run_id) / L"state" /
        satsuma::path_from_utf8(step.vm) /
        satsuma::path_from_utf8(step.id + ".claim-recovery.json");
    satsuma::write_json_atomic(recovery_path, {
        {"status", "manual_intervention_required"},
        {"error", "unsafe expired claim"},
    });

    const nlohmann::json report = controller.build_report(manifest.run_id);
    expect(
        report.at("status") == "manual_intervention_required" &&
            report.at("manual_intervention_required").get<bool>() &&
            !report.at("complete").get<bool>(),
        "manual intervention gate did not become the report status");
}

// 验证规范路径中的串属结果会阻止报告完成。
void test_report_rejects_mismatched_identity(const std::filesystem::path& root) {
    const satsuma::LabConfig config = make_config(root);
    const satsuma::host::Controller controller(config);
    satsuma::TaskPlan plan = make_plan("report_identity");
    plan.steps.resize(1);
    const satsuma::RunManifest manifest = controller.create_run(plan);
    satsuma::ExecutionResult mismatched = make_execution(
        manifest,
        manifest.steps.front(),
        "job_report_mismatch");
    mismatched.vm_id = "vm_02";
    satsuma::write_json_atomic(
        result_path(config, manifest.run_id, manifest.steps.front()),
        mismatched);

    bool rejected = false;
    try {
        static_cast<void>(controller.build_report(manifest.run_id));
    } catch (const satsuma::Error&) {
        rejected = true;
    }
    expect(rejected, "Host report accepted a mismatched canonical execution identity");
}

// 验证运行列表、取消请求和只删除终态运行的保留策略。
void test_run_management(const std::filesystem::path& root) {
    const satsuma::LabConfig config = make_config(root);
    const satsuma::host::Controller controller(config);
    satsuma::TaskPlan complete_plan = make_plan("completed_for_prune");
    complete_plan.steps.resize(1);
    const satsuma::RunManifest complete = controller.create_run(complete_plan);
    satsuma::write_json_atomic(
        result_path(config, complete.run_id, complete.steps.front()),
        make_execution(complete, complete.steps.front(), "job_completed_for_prune"));
    const satsuma::RunManifest pending = controller.create_run(make_plan("pending_for_cancel"));

    const nlohmann::json listed = controller.list_runs();
    expect(listed.at("runs").size() == 2, "run list omitted a shared run");
    const nlohmann::json cancelled = controller.cancel_run(
        pending.run_id,
        "controller test cancellation");
    expect(
        cancelled.at("status") == "cancellation_requested" &&
            std::filesystem::is_regular_file(
                config.shared_folder.host_root / L"runs" / L"pending_for_cancel" / L"cancel.json"),
        "run cancellation was not published atomically");

    const nlohmann::json pruned = controller.prune_runs(0);
    expect(pruned.at("removed").size() == 1, "run pruning did not remove one completed run");
    expect(
        !std::filesystem::exists(
            config.shared_folder.host_root / L"runs" / L"completed_for_prune") &&
            std::filesystem::is_directory(
                config.shared_folder.host_root / L"runs" / L"pending_for_cancel"),
        "run pruning removed a pending run or retained a completed run");
}

// 验证硬件发现、Host 配置写回和共享绑定发布。
void test_agent_hardware_discovery_and_binding(const std::filesystem::path& root) {
    constexpr char hardware_id[] = "564d1234-abcd-4321-9876-001122334455";
    const std::filesystem::path shared_root = root / L"share";
    const std::filesystem::path config_path = root / L"lab.json";
    satsuma::write_json_atomic(config_path, {
        {"schema_version", 1},
        {"lab_id", "host_identity_test"},
        {"provider", {
            {"type", "vmware_workstation"},
            {"vmrun", satsuma::path_to_utf8(root / L"vmrun.exe")},
        }},
        {"host", {{"archive_root", satsuma::path_to_utf8(root / L"archive")}}},
        {"shared_folder", {{"host_root", satsuma::path_to_utf8(shared_root)}}},
        {"vms", nlohmann::json::array({{
            {"id", "vm_01"},
            {"vmx", satsuma::path_to_utf8(root / L"vm_01.vmx")},
            {"agent_version", "0.1.0"},
            {"snapshots", {
                {"base", "clean"},
                {"ai_prefix", "satsuma-ai-"},
                {"max_ai_snapshots", 4},
            }},
        }})},
    });
    satsuma::write_json_atomic(
        shared_root / L"agents" / L"564d1234-abcd-4321-9876-001122334455.json",
        {
            {"schema_version", 1},
            {"protocol_version", 2},
            {"lab_id", "host_identity_test"},
            {"vm_id", hardware_id},
            {"hardware_id", hardware_id},
            {"agent_version", "0.1.0"},
            {"status", "unbound"},
            {"updated_at", "2026-07-29T00:00:00.000Z"},
        });

    const satsuma::LabConfig config = satsuma::load_lab_config(config_path);
    const nlohmann::json discovered = satsuma::host::discover_agents(config);
    expect(
        discovered.at("count") == 1 &&
            discovered.at("agents").at(0).at("hardware_id") == hardware_id &&
            discovered.at("agents").at(0).at("configured_vm_id").is_null(),
        "Host discovery did not expose the unbound hardware presence");

    satsuma::write_json_atomic(
        shared_root / L"agents" / L"vm_02.json",
        {
            {"schema_version", 1},
            {"protocol_version", 2},
            {"lab_id", "host_identity_test"},
            {"vm_id", "vm_02"},
            {"hardware_id", hardware_id},
            {"status", "idle"},
        });
    const nlohmann::json conflicted = satsuma::host::discover_agents(config);
    expect(
        conflicted.at("status") == "identity_conflict" &&
            conflicted.at("collisions").size() == 1,
        "Host discovery did not reject a duplicated SMBIOS UUID");
    bool conflict_rejected = false;
    try {
        static_cast<void>(satsuma::host::bind_agent_hardware(
            config_path,
            config,
            "vm_01",
            hardware_id));
    } catch (const satsuma::Error&) {
        conflict_rejected = true;
    }
    expect(conflict_rejected, "Host binding accepted a duplicated SMBIOS UUID");
    std::filesystem::remove(shared_root / L"agents" / L"vm_02.json");

    const std::filesystem::path sessions =
        shared_root / L"agents" / L"sessions" /
            L"564d1234-abcd-4321-9876-001122334455";
    satsuma::write_json_atomic(
        sessions / L"session-before-restart.json",
        {
            {"schema_version", 1},
            {"protocol_version", 2},
            {"lab_id", "host_identity_test"},
            {"vm_id", hardware_id},
            {"hardware_id", hardware_id},
            {"session_id", "session-before-restart"},
            {"status", "unbound"},
            {"runtime", {{"started_at", "2026-08-01T00:00:00.000Z"}}},
            {"updated_at", "2026-08-01T00:01:00.000Z"},
        });
    satsuma::write_json_atomic(
        sessions / L"session-after-restart.json",
        {
            {"schema_version", 1},
            {"protocol_version", 2},
            {"lab_id", "host_identity_test"},
            {"vm_id", hardware_id},
            {"hardware_id", hardware_id},
            {"session_id", "session-after-restart"},
            {"status", "unbound"},
            {"runtime", {{"started_at", "2026-08-01T00:01:01.000Z"}}},
            {"updated_at", "2026-08-01T00:02:00.000Z"},
        });
    const nlohmann::json restarted = satsuma::host::discover_agents(config);
    expect(
        restarted.at("status") == "discovered" && restarted.at("collisions").empty(),
        "Host treated sequential Agent sessions as a duplicated SMBIOS UUID");
    std::filesystem::remove_all(sessions);

    for (const std::string& session_id : {"session-one", "session-two"}) {
        satsuma::write_json_atomic(
            sessions / satsuma::path_from_utf8(session_id + ".json"),
            {
                {"schema_version", 1},
                {"protocol_version", 2},
                {"lab_id", "host_identity_test"},
                {"vm_id", hardware_id},
                {"hardware_id", hardware_id},
                {"session_id", session_id},
                {"status", "unbound"},
            });
    }
    const nlohmann::json session_conflict = satsuma::host::discover_agents(config);
    expect(
        session_conflict.at("status") == "identity_conflict" &&
            session_conflict.at("collisions").at(0).at("active_session_ids").size() == 2,
        "Host discovery did not detect duplicate unbound Agent sessions");
    std::filesystem::remove_all(sessions);

    const nlohmann::json bound = satsuma::host::bind_agent_hardware(
        config_path,
        config,
        "vm_01",
        hardware_id);
    expect(bound.at("status") == "bound", "Host hardware binding did not succeed");
    const satsuma::LabConfig updated = satsuma::load_lab_config(config_path);
    expect(
        updated.vms.at(0).hardware_id == hardware_id &&
            std::filesystem::is_regular_file(
                shared_root / L"agents" /
                    L"564d1234-abcd-4321-9876-001122334455.binding.json"),
        "Host hardware binding did not persist both sides of the mapping");
}

// 验证进程互斥、死亡租约恢复和普通 run finalize。
void test_lab_lease_lifecycle(const std::filesystem::path& root) {
    satsuma::LabConfig config = make_config(root);
    config.host.archive_root = root / L"archive";
    const std::filesystem::path config_path = root / L"lab.json";
    auto first = satsuma::host::LabLease::acquire(config, config_path, "test");
    expect(
        satsuma::host::LabLease::status(config).at("status") == "busy",
        "active lab lease was not reported as busy");
    expect_error(
        [&] { static_cast<void>(satsuma::host::LabLease::acquire(config, config_path, "other")); },
        "second Host acquired the same lab process mutex");
    first->release("released");
    first.reset();

    const std::filesystem::path lease_path =
        config.host.archive_root / L"coordination" / L"lab-lease.json";
    satsuma::write_json_atomic(lease_path, {
        {"schema_version", 1},
        {"lab_id", config.lab_id},
        {"lease_id", "lease_stale"},
        {"host_process_id", 0},
        {"command", "orchestrate"},
        {"run_id", "recover_run"},
        {"state", "active"},
        {"acquired_at", "2026-07-29T00:00:00.000Z"},
        {"renewed_at", "2026-07-29T00:00:00.000Z"},
    });
    expect_error(
        [&] { static_cast<void>(satsuma::host::LabLease::acquire(config, config_path, "new")); },
        "ordinary write command discarded a stale active lease");
    auto recovered = satsuma::host::LabLease::acquire(
        config, config_path, "lab recover", "recover_run", true);
    recovered->release("released");
    recovered.reset();

    satsuma::host::Controller controller(config);
    const satsuma::RunManifest manifest = controller.create_run(make_plan("finalize_run"));
    for (const satsuma::TaskStep& step : manifest.steps) {
        satsuma::write_json_atomic(
            result_path(config, manifest.run_id, step),
            make_execution(manifest, step, "job_" + step.id));
    }
    satsuma::write_json_atomic(lease_path, {
        {"schema_version", 1},
        {"lab_id", config.lab_id},
        {"lease_id", "lease_finalize"},
        {"host_process_id", 0},
        {"command", "run"},
        {"run_id", manifest.run_id},
        {"state", "active"},
        {"acquired_at", "2026-07-29T00:00:00.000Z"},
        {"renewed_at", "2026-07-29T00:00:00.000Z"},
    });
    const nlohmann::json finalized = satsuma::host::LabLease::finalize_run(
        config, config_path, manifest.run_id);
    expect(
        finalized.at("status") == "finalized" &&
            satsuma::host::LabLease::status(config).at("status") == "available",
        "terminal ordinary run did not release its persistent lease");
}

}  // namespace

// 运行 Host Controller 测试并清理专用临时目录。
int main() {
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        satsuma::path_from_utf8(satsuma::make_id("satsuma-host-controller-test"));
    try {
        test_report_uses_canonical_result_paths(root / L"paths");
        test_report_exposes_manual_intervention(root / L"manual");
        test_report_rejects_mismatched_identity(root / L"identity");
        test_run_management(root / L"run-management");
        test_agent_hardware_discovery_and_binding(root / L"hardware-binding");
        test_lab_lease_lifecycle(root / L"lab-lease");
        std::filesystem::remove_all(root);
        std::cout << "SatsumaHostControllerTests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "SatsumaHostControllerTests failed: " << error.what() << '\n';
        std::error_code cleanup_error;
        std::filesystem::remove_all(root, cleanup_error);
        return 1;
    }
}
