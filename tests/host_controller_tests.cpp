// Host 任务报告规范路径和执行身份校验测试。
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

#include "controller.hpp"
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

// 创建只包含 Host 文件任务所需字段的实验室配置。
[[nodiscard]] satsuma::LabConfig make_config(const std::filesystem::path& root) {
    satsuma::LabConfig config;
    config.lab_id = "host_controller_test";
    config.shared_folder.host_root = root / L"share";
    satsuma::VmConfig vm;
    vm.id = "client";
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
        step.vm = "client";
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
        run_root / L"results" / L"client" / L"first" /
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
    mismatched.vm_id = "gateway";
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
