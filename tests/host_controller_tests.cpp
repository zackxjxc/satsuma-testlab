// Host 任务报告规范路径和执行身份校验测试。
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

#include "controller.hpp"
#include "identity.hpp"
#include "artifact_store.hpp"
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
    config.transport.state_root = root / L"state";
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
        step.run_as = satsuma::TaskRunAs::System;
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
    return config.transport.state_root / L"runs" /
        satsuma::path_from_utf8(run_id) / L"results" /
        satsuma::path_from_utf8(step.vm) /
        satsuma::path_from_utf8(step.id) / L"execution.json";
}

// 新运行必须预建每个步骤的规范结果目录，供 Host 安全轮询和 Agent 原子发布。
void test_create_run_precreates_step_result_directories(const std::filesystem::path& root) {
    const satsuma::LabConfig config = make_config(root);
    const satsuma::host::Controller controller(config);
    const satsuma::RunManifest manifest = controller.create_run(make_plan("precreated_result_paths"));
    for (const satsuma::TaskStep& step : manifest.steps) {
        const std::filesystem::path execution = result_path(config, manifest.run_id, step);
        expect(
            std::filesystem::is_directory(execution.parent_path()) &&
                !std::filesystem::exists(execution),
            "run creation did not precreate an empty canonical step result directory");
    }
}

// 验证收集文件中的同名 JSON 不会伪造步骤完成数量。
// Guest 目标按 VM 独立；Host 物化不再把两个目标复制到同一个路径。
void test_artifact_destinations_are_scoped_to_vm(const std::filesystem::path& root) {
    satsuma::LabConfig config = make_config(root);
    satsuma::VmConfig other;
    other.id = "vm_02";
    config.vms.push_back(other);
    const satsuma::host::Controller controller(config);
    const auto first_source = root / L"first.json";
    const auto second_source = root / L"second.json";
    satsuma::write_json_atomic(first_source, {{"vm", 1}});
    satsuma::write_json_atomic(second_source, {{"vm", 2}});
    for (const bool same_content : {true, false}) {
        auto plan = make_plan(same_content ? "same_content" : "different_content");
        plan.steps[1].vm = "vm_02";
        plan.artifacts = {
            {first_source, "vm_01", L"artifacts/preflight.json", std::nullopt},
            {same_content ? first_source : second_source, "vm_02",
                L"artifacts/preflight.json", std::nullopt},
        };
        const auto manifest = controller.create_run(plan);
        const auto run = config.transport.state_root / L"runs" /
            satsuma::path_from_utf8(manifest.run_id);
        expect(manifest.artifacts.size() == 2 &&
                manifest.artifacts[0].path == manifest.artifacts[1].path,
            "cross-VM isolation changed the Guest destination");
        for (std::size_t index = 0; index < 2; ++index) {
            const auto stored = satsuma::host::artifact_storage_path(run, manifest, index);
            expect(stored.parent_path() == run / L".artifacts" &&
                    satsuma::sha256_file(stored) == satsuma::sha256_file(plan.artifacts[index].source),
                "cross-VM Artifact was overwritten or reused from the wrong source");
        }
        expect(!std::filesystem::exists(run / L"artifacts"),
            "Host repeated the Guest destination hierarchy in its storage");
    }

    auto duplicate = make_plan("duplicate_artifacts");
    duplicate.artifacts = {
        {first_source, "vm_01", L"artifacts/preflight.json", std::nullopt},
        {second_source, "vm_01", L"artifacts/preflight.json", std::nullopt},
    };
    for (const auto destination : {
            L"artifacts/preflight.json", L"ARTIFACTS\\PREFLIGHT.JSON",
            L"artifacts//preflight.json", L"artifacts/preflight.json/child"}) {
        duplicate.artifacts[1].destination = destination;
        expect_error([&] { controller.validate_plan(duplicate, false); },
            "same-VM duplicate or overlapping Artifact destination was accepted");
    }
    duplicate.artifacts[0].destination = L"artifacts/\u00c9preuve.json";
    duplicate.artifacts[1].destination = L"artifacts/\u00e9preuve.json";
    expect_error([&] { controller.validate_plan(duplicate, false); },
        "non-ASCII Windows case alias was accepted");
    duplicate.artifacts.resize(1);
    for (const auto destination : {
            L"artifacts/preflight.json.", L"artifacts/preflight.json ",
            L"artifacts/name:stream", L"artifacts/NUL.json", L"artifacts/COM1",
            L"artifacts/PREFLI~1.JSON", L"artifacts/dir./file", L"artifacts/file?"}) {
        duplicate.artifacts[0].destination = destination;
        expect_error([&] { controller.validate_plan(duplicate, false); },
            "ambiguous Windows Artifact filename was accepted");
    }

    const auto plan_path = root / L"duplicate-plan.json";
    satsuma::write_json_atomic(plan_path, {
        {"schema_version", 3}, {"name", "duplicate JSON plan"},
        {"artifacts", {
            {{"source", satsuma::path_to_utf8(first_source)}, {"vm", "vm_01"},
                {"destination", "artifacts/preflight.json"}},
            {{"source", satsuma::path_to_utf8(first_source)}, {"vm", "vm_01"},
                {"destination", "ARTIFACTS/PREFLIGHT.JSON"}},
        }},
        {"steps", {{{"id", "echo"}, {"vm", "vm_01"}, {"type", "echo"},
            {"message", "validation only"}, {"retry_safe", true}}}},
    });
    expect_error([&] { static_cast<void>(satsuma::load_task_plan(plan_path)); },
        "plan loading did not reject a Windows-equivalent Artifact destination");
}

void test_report_uses_canonical_result_paths(const std::filesystem::path& root) {
    const satsuma::LabConfig config = make_config(root);
    const satsuma::host::Controller controller(config);
    const satsuma::RunManifest manifest = controller.create_run(make_plan("report_paths"));
    const std::filesystem::path run_root =
        config.transport.state_root / L"runs" / L"report_paths";
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
        config.transport.state_root / L"runs" /
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
    expect(listed.at("runs").size() == 2, "run list omitted a Host run");
    const nlohmann::json cancelled = controller.cancel_run(
        pending.run_id,
        "controller test cancellation");
    expect(
        cancelled.at("status") == "cancellation_requested" &&
            std::filesystem::is_regular_file(
                config.transport.state_root / L"runs" / L"pending_for_cancel" / L"cancel.json"),
        "run cancellation was not published atomically");

    const nlohmann::json pruned = controller.prune_runs(0);
    expect(pruned.at("removed").size() == 1, "run pruning did not remove one completed run");
    expect(
        !std::filesystem::exists(
            config.transport.state_root / L"runs" / L"completed_for_prune") &&
            std::filesystem::is_directory(
                config.transport.state_root / L"runs" / L"pending_for_cancel"),
        "run pruning removed a pending run or retained a completed run");
}

// 验证只读预检可成功返回，并在创建运行目录前拒绝缺失 Artifact。
void test_plan_preflight_validation(const std::filesystem::path& root) {
    const satsuma::LabConfig config = make_config(root);
    const satsuma::host::Controller controller(config);
    controller.validate_plan(make_plan("preflight_valid"), false);

    satsuma::TaskPlan missing = make_plan("preflight_missing_artifact");
    missing.artifacts.push_back({
        root / L"missing.exe",
        "vm_01",
        satsuma::path_from_utf8("artifacts/vm_01/missing.exe"),
        std::nullopt,
    });
    satsuma::TaskStep execute;
    execute.id = "missing";
    execute.vm = "vm_01";
    execute.type = "execute";
    execute.program = satsuma::path_from_utf8("artifacts/vm_01/missing.exe");
    missing.steps = {execute};
    expect_error(
        [&] { controller.validate_plan(missing, false); },
        "plan preflight accepted a missing Artifact source");
    expect(
        !std::filesystem::exists(
            config.transport.state_root / L"runs" / L"preflight_missing_artifact"),
        "plan preflight created a run directory");
}

// 验证 active 状态删除后，报告和列表仍可读取已完成的证据归档。
void test_archived_run_reporting(const std::filesystem::path& root) {
    satsuma::LabConfig config = make_config(root);
    config.host.archive_root = root / L"archive";
    const satsuma::host::Controller controller(config);
    satsuma::TaskPlan plan = make_plan("archived_report");
    plan.steps.resize(1);
    const satsuma::RunManifest manifest = controller.create_run(plan);
    satsuma::write_json_atomic(
        result_path(config, manifest.run_id, manifest.steps.front()),
        make_execution(manifest, manifest.steps.front(), "job_archived_report"));

    const std::filesystem::path active =
        config.transport.state_root / L"runs" / L"archived_report";
    const std::filesystem::path archived =
        config.host.archive_root / L"runs" / L"archived_report" /
            L"evidence" / L"main";
    std::filesystem::create_directories(archived.parent_path());
    std::filesystem::copy(active, archived, std::filesystem::copy_options::recursive);
    satsuma::write_json_atomic(archived / L".archive-complete.json", {
        {"schema_version", 1},
        {"status", "complete"},
        {"source_run_id", "archived_report"},
        {"files", nlohmann::json::array()},
    });
    std::filesystem::remove_all(active);

    const nlohmann::json report = controller.build_report("archived_report");
    expect(
        report.at("source") == "archive" && report.at("status") == "succeeded" &&
            report.at("complete").get<bool>(),
        "report did not read the completed archive after active-state deletion");
    const nlohmann::json listed = controller.list_runs();
    expect(
        listed.at("runs").size() == 1 &&
            listed.at("runs").front().at("run_id") == "archived_report" &&
            listed.at("runs").front().at("source") == "archive",
        "runs list omitted the archived run or its source");
}

// 验证 Host 可用配置中的精确哈希拒绝同版本的错误 Agent 构建。
void test_agent_build_hash_validation(const std::filesystem::path& root) {
    satsuma::LabConfig config = make_config(root);
    config.vms.front().agent_version = "0.3.1";
    config.vms.front().agent_sha256 = std::string(64, 'a');
    const std::filesystem::path presence_path =
        config.transport.state_root / L"agents" / L"vm_01.json";
    satsuma::write_json_atomic(presence_path, {
        {"lab_id", config.lab_id},
        {"vm_id", "vm_01"},
        {"agent_version", "0.3.1"},
        {"binary_sha256", std::string(64, 'a')},
    });
    expect(
        satsuma::host::load_vm_presence(config, config.vms.front())
                .at("binary_sha256") == std::string(64, 'a'),
        "Host rejected the configured Agent build hash");

    nlohmann::json mismatched = satsuma::load_json(presence_path);
    mismatched["binary_sha256"] = std::string(64, 'b');
    satsuma::write_json_atomic(presence_path, mismatched);
    expect_error(
        [&] {
            static_cast<void>(
                satsuma::host::load_vm_presence(config, config.vms.front()));
        },
        "Host accepted an Agent build hash different from lab configuration");
}

// 验证硬件发现、Host 配置写回和共享绑定发布。
void test_agent_hardware_discovery_and_binding(const std::filesystem::path& root) {
    constexpr char hardware_id[] = "564d1234-abcd-4321-9876-001122334455";
    const std::filesystem::path mirror_root = root / L"mirror";
    const std::filesystem::path config_path = root / L"lab.json";
    satsuma::write_json_atomic(config_path, {
        {"schema_version", 1},
        {"lab_id", "host_identity_test"},
        {"provider", {
            {"type", "vmware_workstation"},
            {"vmrun", satsuma::path_to_utf8(root / L"vmrun.exe")},
        }},
        {"host", {{"archive_root", satsuma::path_to_utf8(root / L"archive")}}},
        {"transport", {
            {"state_root", satsuma::path_to_utf8(mirror_root)},
            {"vmci_port", 42510},
        }},
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
        mirror_root / L"agents" / L"564d1234-abcd-4321-9876-001122334455.json",
        {
            {"schema_version", 2},
            {"protocol_version", satsuma::kRunManifestProtocolVersion},
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
        mirror_root / L"agents" / L"vm_02.json",
        {
            {"schema_version", 2},
            {"protocol_version", satsuma::kRunManifestProtocolVersion},
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
    std::filesystem::remove(mirror_root / L"agents" / L"vm_02.json");

    const std::filesystem::path sessions =
        mirror_root / L"agents" / L"sessions" /
            L"564d1234-abcd-4321-9876-001122334455";
    satsuma::write_json_atomic(
        sessions / L"session-before-restart.json",
        {
            {"schema_version", 2},
            {"protocol_version", satsuma::kRunManifestProtocolVersion},
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
            {"schema_version", 2},
            {"protocol_version", satsuma::kRunManifestProtocolVersion},
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
                {"schema_version", 2},
                {"protocol_version", satsuma::kRunManifestProtocolVersion},
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
                mirror_root / L"agents" /
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
    bool conflict_diagnosed = false;
    try {
        static_cast<void>(satsuma::host::LabLease::acquire(config, config_path, "other"));
    } catch (const satsuma::Error& error) {
        const std::string message = error.what();
        conflict_diagnosed =
            message.find("Another SatsumaHost write session") != std::string::npos &&
            message.find("Command: test") != std::string::npos &&
            message.find("PID:") != std::string::npos &&
            message.find("Lease:") != std::string::npos &&
            message.find("retry sequentially") != std::string::npos;
    }
    expect(conflict_diagnosed, "write-session conflict omitted the active lease diagnosis");
    first->release("released");
    first.reset();

    auto scoped = satsuma::host::LabLease::acquire(config, config_path, "check");
    scoped->release_on_scope_exit("failed");
    scoped.reset();
    const nlohmann::json scoped_status = satsuma::host::LabLease::status(config);
    expect(
        scoped_status.at("status") == "available" &&
            scoped_status.at("lease").at("state") == "failed",
        "safe command scope exit retained an active lab lease");

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
        test_create_run_precreates_step_result_directories(root / L"precreated-result-paths");
        test_report_uses_canonical_result_paths(root / L"paths");
        test_report_exposes_manual_intervention(root / L"manual");
        test_report_rejects_mismatched_identity(root / L"identity");
        test_plan_preflight_validation(root / L"plan-preflight");
        test_run_management(root / L"run-management");
        test_archived_run_reporting(root / L"archived-reporting");
        test_agent_build_hash_validation(root / L"agent-build-hash");
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
        test_artifact_destinations_are_scoped_to_vm(root / L"artifact-isolation");
