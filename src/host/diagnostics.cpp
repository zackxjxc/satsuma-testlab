// Host 自动化环境检测实现。
#include "diagnostics.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "satsuma/core/errors.hpp"
#include "satsuma/core/id.hpp"
#include "satsuma/core/json_io.hpp"
#include "satsuma/core/path.hpp"
#include "satsuma/core/task.hpp"
#include "controller.hpp"
#include "vmrun_provider.hpp"

namespace satsuma::host {
namespace {

// 向机器可读报告追加一项独立检查。
void add_check(
    nlohmann::json& checks,
    const std::string& name,
    const std::string& status,
    const std::string& message,
    nlohmann::json details = nlohmann::json::object()) {
    checks.push_back({
        {"name", name},
        {"status", status},
        {"message", message},
        {"details", std::move(details)},
    });
}

// 创建并删除一个原子 JSON 探针，确认目录实际可写。
void probe_writable_directory(const std::filesystem::path& root) {
    if (!std::filesystem::is_directory(root)) {
        throw Error("Directory does not exist: " + path_to_utf8(root));
    }
    const std::filesystem::path probe = resolve_under_root(
        root,
        path_from_utf8(make_id("satsuma-diagnostic") + ".json"));
    try {
        write_json_atomic(probe, {{"probe", true}});
        if (!std::filesystem::remove(probe)) {
            throw Error("Cannot remove diagnostic probe: " + path_to_utf8(probe));
        }
    } catch (...) {
        std::error_code cleanup_error;
        std::filesystem::remove(probe, cleanup_error);
        throw;
    }
}

// 解析调用方目标；未指定时检查配置中的全部 VM。
[[nodiscard]] std::vector<const VmConfig*> select_targets(
    const LabConfig& config,
    const std::optional<std::string>& vm_id) {
    std::vector<const VmConfig*> targets;
    if (vm_id.has_value()) {
        const VmConfig* vm = find_vm(config, *vm_id);
        if (vm == nullptr) {
            throw Error("Unknown VM id: " + *vm_id);
        }
        targets.push_back(vm);
        return targets;
    }
    for (const VmConfig& vm : config.vms) {
        targets.push_back(&vm);
    }
    return targets;
}

// 读取 Agent 已原子发布的 UTF-8 日志字节。
[[nodiscard]] std::string read_text(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw Error("Cannot read diagnostic output: " + path_to_utf8(path));
    }
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

}  // namespace

Diagnostics::Diagnostics(LabConfig config) : config_(std::move(config)) {}

nlohmann::json Diagnostics::inspect_environment(const std::optional<std::string>& vm_id) const {
    const std::vector<const VmConfig*> targets = select_targets(config_, vm_id);

    nlohmann::json checks = nlohmann::json::array();
    const auto inspect_directory = [&checks](const std::string& name, const std::filesystem::path& path) {
        try {
            probe_writable_directory(path);
            add_check(checks, name, "passed", "Directory exists and accepts atomic writes", {
                {"path", path_to_utf8(path)},
            });
        } catch (const std::exception& error) {
            add_check(checks, name, "failed", error.what(), {{"path", path_to_utf8(path)}});
        }
    };
    inspect_directory("shared_folder", config_.shared_folder.host_root);
    inspect_directory("archive", config_.host.archive_root);

    std::unique_ptr<vmware::VmrunProvider> provider;
    try {
        provider = std::make_unique<vmware::VmrunProvider>(config_.provider.vmrun);
        add_check(checks, "vmrun", "passed", "vmrun executable is available", {
            {"path", path_to_utf8(config_.provider.vmrun)},
        });
    } catch (const std::exception& error) {
        add_check(checks, "vmrun", "failed", error.what(), {
            {"path", path_to_utf8(config_.provider.vmrun)},
        });
    }

    if (provider != nullptr) {
        try {
            const std::vector<std::filesystem::path> running = provider->list_running();
            nlohmann::json paths = nlohmann::json::array();
            for (const auto& path : running) {
                paths.push_back(path_to_utf8(path));
            }
            add_check(checks, "vmware_control", "passed", "vmrun list completed", {
                {"running_vms", std::move(paths)},
            });
        } catch (const std::exception& error) {
            add_check(checks, "vmware_control", "failed", error.what());
        }
    }

    for (const VmConfig* vm : targets) {
        if (std::filesystem::is_regular_file(vm->vmx)) {
            add_check(checks, "vmx", "passed", "Configured VMX exists", {
                {"vm_id", vm->id},
                {"path", path_to_utf8(vm->vmx)},
            });
        } else {
            add_check(checks, "vmx", "failed", "Configured VMX is not a regular file", {
                {"vm_id", vm->id},
                {"path", path_to_utf8(vm->vmx)},
            });
        }

        if (provider == nullptr || !std::filesystem::is_regular_file(vm->vmx)) {
            add_check(checks, "snapshots", "skipped", "VMware control or VMX check failed", {
                {"vm_id", vm->id},
            });
            continue;
        }
        try {
            const std::vector<std::string> snapshots = provider->list_snapshots(vm->vmx);
            const bool base_present =
                std::find(snapshots.begin(), snapshots.end(), vm->snapshots.base) != snapshots.end();
            add_check(
                checks,
                "snapshots",
                base_present ? "passed" : "failed",
                base_present ? "Snapshot list contains the configured base" : "Configured base snapshot is missing",
                {
                {"vm_id", vm->id},
                {"base", vm->snapshots.base},
                {"count", snapshots.size()},
                {"base_present", base_present},
                });
        } catch (const std::exception& error) {
            add_check(checks, "snapshots", "failed", error.what(), {{"vm_id", vm->id}});
        }
    }

    std::size_t passed = 0;
    std::size_t failed = 0;
    std::size_t skipped = 0;
    for (const auto& check : checks) {
        const std::string status = check.at("status").get<std::string>();
        passed += status == "passed" ? 1 : 0;
        failed += status == "failed" ? 1 : 0;
        skipped += status == "skipped" ? 1 : 0;
    }
    return {
        {"schema_version", 1},
        {"mode", "environment"},
        {"status", failed == 0 ? "ready" : "failed"},
        {"checked_at", utc_timestamp()},
        {"summary", {{"passed", passed}, {"failed", failed}, {"skipped", skipped}}},
        {"checks", std::move(checks)},
    };
}

nlohmann::json Diagnostics::run_probe(
    const std::optional<std::string>& vm_id,
    const std::chrono::seconds timeout) const {
    if (timeout.count() < 1 || timeout.count() > 300) {
        throw Error("Diagnostic timeout must be between 1 and 300 seconds");
    }
    const std::vector<const VmConfig*> targets = select_targets(config_, vm_id);
    nlohmann::json report = inspect_environment(vm_id);
    const std::string environment_status = report.at("status").get<std::string>();

    TaskPlan plan;
    plan.name = "satsuma-automation-diagnostic";
    plan.run_id = make_id("check");
    for (const VmConfig* vm : targets) {
        TaskStep step;
        step.id = vm->id;
        step.vm = vm->id;
        step.type = "echo";
        step.message = "satsuma-diagnostic:" + *plan.run_id + ":" + vm->id;
        step.timeout_seconds = static_cast<int>(timeout.count());
        step.retry_safe = true;
        plan.steps.push_back(std::move(step));
    }

    Controller controller(config_);
    const RunManifest manifest = controller.create_run(plan);
    const std::filesystem::path run_directory = resolve_under_root(
        config_.shared_folder.host_root,
        std::filesystem::path(L"runs") / path_from_utf8(manifest.run_id));
    nlohmann::json agents = nlohmann::json::array();
    std::set<std::string> reported;
    const auto deadline = std::chrono::steady_clock::now() + timeout;

    while (reported.size() < targets.size() && std::chrono::steady_clock::now() < deadline) {
        for (const VmConfig* vm : targets) {
            if (reported.contains(vm->id)) {
                continue;
            }
            const std::filesystem::path result_path = resolve_under_root(
                run_directory,
                std::filesystem::path(L"results") /
                    path_from_utf8(vm->id) /
                    path_from_utf8(vm->id) /
                    L"execution.json");
            if (!std::filesystem::is_regular_file(result_path)) {
                continue;
            }

            nlohmann::json agent = {
                {"vm_id", vm->id},
                {"status", "failed"},
            };
            try {
                const ExecutionResult result = load_json(result_path).get<ExecutionResult>();
                const std::filesystem::path stdout_path = resolve_under_root(
                    run_directory,
                    path_from_utf8(result.stdout_path));
                const std::string expected = "satsuma-diagnostic:" + manifest.run_id + ":" + vm->id + "\n";
                const std::string actual = read_text(stdout_path);
                const bool passed =
                    result.status == "exited" &&
                    result.exit_code == std::optional<std::uint32_t>(0) &&
                    !result.timed_out &&
                    actual == expected;
                agent["status"] = passed ? "passed" : "failed";
                agent["execution_status"] = result.status;
                agent["exit_code"] = result.exit_code.has_value()
                    ? nlohmann::json(*result.exit_code)
                    : nlohmann::json(nullptr);
                agent["duration_ms"] = result.duration_ms;
                agent["message"] = passed
                    ? "Agent executed and returned the diagnostic echo"
                    : "Agent result or stdout did not match the diagnostic task";
                if (!result.error.empty()) {
                    agent["error"] = result.error;
                }
            } catch (const std::exception& error) {
                agent["message"] = "Cannot validate Agent diagnostic result";
                agent["error"] = error.what();
            }
            agents.push_back(std::move(agent));
            reported.insert(vm->id);
        }
        if (reported.size() < targets.size()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

    for (const VmConfig* vm : targets) {
        if (!reported.contains(vm->id)) {
            agents.push_back({
                {"vm_id", vm->id},
                {"status", "timeout"},
                {"message", "Agent did not return the diagnostic task before the deadline"},
            });
        }
    }

    std::size_t agent_passed = 0;
    for (const auto& agent : agents) {
        agent_passed += agent.at("status") == "passed" ? 1 : 0;
    }
    const std::size_t agent_failed = agents.size() - agent_passed;
    const std::string status = agent_failed > 0
        ? "failed"
        : (environment_status == "ready" ? "ready" : "degraded");
    report["mode"] = "full";
    report["environment_status"] = environment_status;
    report["status"] = status;
    report["run_id"] = manifest.run_id;
    report["finished_at"] = utc_timestamp();
    report["probe_summary"] = {
        {"expected_agents", targets.size()},
        {"passed", agent_passed},
        {"failed", agent_failed},
    };
    report["agents"] = std::move(agents);
    return report;
}

}  // namespace satsuma::host
