// Host 自动化环境检测实现。
#include "diagnostics.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <windows.h>

#include "satsuma/core/errors.hpp"
#include "satsuma/core/id.hpp"
#include "satsuma/core/json_io.hpp"
#include "satsuma/core/path.hpp"
#include "satsuma/core/task.hpp"
#include "controller.hpp"
#include "identity.hpp"
#include "vmrun_provider.hpp"

namespace satsuma::host {
namespace {

constexpr std::chrono::seconds kEnvironmentRecheckTimeout{30}; // Agent 上线后的环境收敛等待上限
constexpr std::chrono::seconds kEnvironmentRecheckDelay{1}; // 环境复检间隔

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

// 返回环境报告中同名检查是否全部通过。
[[nodiscard]] bool check_group_passed(const nlohmann::json& report, const std::string_view name) {
    bool found = false;
    for (const auto& check : report.at("checks")) {
        if (check.at("name").get<std::string>() == name) {
            found = true;
            if (check.at("status") != "passed") {
                return false;
            }
        }
    }
    return found;
}

// 仅在 VMware Tools 是唯一未通过项时等待冷启动状态收敛。
[[nodiscard]] bool only_vmware_tools_pending(const nlohmann::json& report) {
    bool tools_pending = false; // 至少存在一项未就绪的 Tools 检查
    for (const auto& check : report.at("checks")) {
        if (check.at("status") == "passed") {
            continue;
        }
        if (check.at("name") != "vmware_tools" || check.at("status") != "failed") {
            return false;
        }
        tools_pending = true;
    }
    return tools_pending;
}

}  // namespace

Diagnostics::Diagnostics(LabConfig config) : config_(std::move(config)) {}

nlohmann::json Diagnostics::inspect_environment(const std::optional<std::string>& vm_id) const {
    const std::vector<const VmConfig*> targets = select_targets(config_, vm_id);

    nlohmann::json checks = nlohmann::json::array();
    const auto inspect_directory = [&checks](const std::string& name, const std::filesystem::path& path) {
        nlohmann::json details = {{"path", path_to_utf8(path)}};
        try {
            if (!std::filesystem::is_directory(path)) {
                throw Error("Directory does not exist: " + path_to_utf8(path));
            }
            const std::filesystem::space_info space = std::filesystem::space(path);
            details["capacity_bytes"] = space.capacity;
            details["free_bytes"] = space.free;
            details["available_bytes"] = space.available;
            if (space.available == 0) {
                throw Error("Directory volume reports no available space: " + path_to_utf8(path));
            }
            probe_writable_directory(path);
            add_check(
                checks,
                name,
                "passed",
                "Directory exists, reports capacity, and accepts atomic writes",
                std::move(details));
        } catch (const std::exception& error) {
            add_check(checks, name, "failed", error.what(), std::move(details));
        }
    };
    inspect_directory("transport_state", config_.transport.state_root);
    inspect_directory("archive", config_.host.archive_root);

    std::unique_ptr<vmware::VmrunProvider> provider;
    std::optional<std::vector<std::filesystem::path>> running_vms;
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
            running_vms = provider->list_running();
            nlohmann::json paths = nlohmann::json::array();
            for (const auto& path : *running_vms) {
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
            add_check(checks, "vm_power", "skipped", "VMware control or VMX check failed", {
                {"vm_id", vm->id},
                {"state", "unknown"},
            });
            add_check(checks, "snapshots", "skipped", "VMware control or VMX check failed", {
                {"vm_id", vm->id},
            });
            add_check(checks, "vmware_tools", "skipped", "VMware control or VMX check failed", {
                {"vm_id", vm->id},
            });
            continue;
        }

        bool vm_running = false;
        if (running_vms.has_value()) {
            const std::filesystem::path expected =
                std::filesystem::absolute(vm->vmx).lexically_normal();
            vm_running = std::any_of(
                running_vms->begin(),
                running_vms->end(),
                [&expected](const std::filesystem::path& candidate) {
                    const std::filesystem::path actual =
                        std::filesystem::absolute(candidate).lexically_normal();
                    return _wcsicmp(expected.native().c_str(), actual.native().c_str()) == 0;
                });
            add_check(
                checks,
                "vm_power",
                vm_running ? "passed" : "failed",
                vm_running ? "VM is running" : "VM is stopped",
                {{"vm_id", vm->id}, {"state", vm_running ? "running" : "stopped"}});
        } else {
            add_check(checks, "vm_power", "skipped", "VMware running list is unavailable", {
                {"vm_id", vm->id},
                {"state", "unknown"},
            });
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

        if (!running_vms.has_value() || !vm_running) {
            add_check(
                checks,
                "vmware_tools",
                "skipped",
                running_vms.has_value()
                    ? "VMware Tools cannot run while the VM is stopped"
                    : "VMware running list is unavailable",
                {
                    {"vm_id", vm->id},
                    {"state", "not_checked"},
                    {"power_state", running_vms.has_value() ? "stopped" : "unknown"},
                });
            continue;
        }
        try {
            const std::string tools_state = provider->check_tools_state(vm->vmx);
            bool running = tools_state == "running";
            std::string tools_message = running
                ? "VMware Tools is running"
                : "VMware Tools is installed but its Guest service is not ready";
            nlohmann::json tools_details = {
                {"vm_id", vm->id},
                {"state", tools_state},
                {"power_state", "running"},
            };
            if (tools_state == "installed") {
                running = true;
                tools_message =
                    "VMware Tools is installed; VMCI Agent probe determines channel readiness";
            }
            add_check(
                checks,
                "vmware_tools",
                running ? "passed" : "failed",
                tools_message,
                std::move(tools_details));
        } catch (const std::exception& error) {
            add_check(checks, "vmware_tools", "failed", error.what(), {{"vm_id", vm->id}});
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
    std::string environment_status = report.at("status").get<std::string>();
    const std::string initial_environment_status = environment_status; // Agent 等待前的环境状态
    const nlohmann::json initial_environment = {
        {"status", initial_environment_status},
        {"checked_at", report.at("checked_at")},
        {"summary", report.at("summary")},
        {"checks", report.at("checks")},
    }; // 冷启动复检时保留最初的机器可读证据

    if (!check_group_passed(report, "transport_state")) {
        nlohmann::json agents = nlohmann::json::array();
        for (const VmConfig* vm : targets) {
            agents.push_back({
                {"vm_id", vm->id},
                {"status", "skipped"},
                {"message", "Agent diagnostic was skipped because the transport state is unavailable"},
            });
        }
        report["mode"] = "full";
        report["environment_status"] = environment_status;
        report["status"] = "failed";
        report["run_id"] = nullptr;
        report["finished_at"] = utc_timestamp();
        report["probe_summary"] = {
            {"expected_agents", targets.size()},
            {"passed", 0},
            {"failed", 0},
            {"skipped", targets.size()},
        };
        report["agents"] = std::move(agents);
        return report;
    }

    if (!check_group_passed(report, "vm_power")) {
        nlohmann::json agents = nlohmann::json::array();
        for (const VmConfig* vm : targets) {
            agents.push_back({
                {"vm_id", vm->id},
                {"status", "skipped"},
                {"message", "Agent diagnostic was skipped because the VM is not running"},
            });
        }
        report["mode"] = "full";
        report["environment_status"] = environment_status;
        report["status"] = "failed";
        report["run_id"] = nullptr;
        report["finished_at"] = utc_timestamp();
        report["probe_summary"] = {
            {"expected_agents", targets.size()},
            {"passed", 0},
            {"failed", 0},
            {"skipped", targets.size()},
        };
        report["agents"] = std::move(agents);
        return report;
    }

    const std::chrono::seconds diagnostic_timeout = timeout;
    const auto deadline = std::chrono::steady_clock::now() + diagnostic_timeout;
    std::map<std::string, std::string> presence_errors;
    std::set<std::string> ready_presence;
    while (ready_presence.size() < targets.size() &&
           std::chrono::steady_clock::now() < deadline) {
        for (const VmConfig* vm : targets) {
            if (ready_presence.contains(vm->id)) {
                continue;
            }
            try {
                static_cast<void>(load_vm_presence(config_, *vm));
                ready_presence.insert(vm->id);
                presence_errors.erase(vm->id);
            } catch (const std::exception& error) {
                presence_errors[vm->id] = error.what();
            }
        }
        if (ready_presence.size() < targets.size()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
    if (ready_presence.size() < targets.size()) {
        nlohmann::json waits = nlohmann::json::array();
        for (const VmConfig* vm : targets) {
            const bool ready = ready_presence.contains(vm->id);
            nlohmann::json wait = {
                {"vm_id", vm->id},
                {"status", ready ? "ready" : "timeout"},
                {"waited_seconds", diagnostic_timeout.count()},
            };
            if (!ready && presence_errors.contains(vm->id)) {
                wait["last_error"] = presence_errors.at(vm->id);
            }
            waits.push_back(std::move(wait));
        }
        report["mode"] = "full";
        report["environment_status"] = environment_status;
        report["status"] = "failed";
        report["run_id"] = nullptr;
        report["finished_at"] = utc_timestamp();
        report["agent_wait"] = std::move(waits);
        report["agents"] = nlohmann::json::array();
        return report;
    }

    nlohmann::json inventories = nlohmann::json::array();
    for (const VmConfig* vm : targets) {
        try {
            nlohmann::json inventory;
            bool refreshed = false;
            try {
                inventory = load_vm_inventory(config_, *vm);
            } catch (...) {
                const auto remaining = std::chrono::duration_cast<std::chrono::seconds>(
                    deadline - std::chrono::steady_clock::now());
                if (remaining.count() < 1) {
                    throw Error("Agent readiness deadline expired before inventory refresh");
                }
                inventory = refresh_vm_inventory(config_, *vm, remaining);
                refreshed = true;
            }
            inventories.push_back({
                {"vm_id", vm->id},
                {"status", "passed"},
                {"observed_at", inventory.at("observed_at")},
                {"refreshed", refreshed},
            });
        } catch (const std::exception& error) {
            inventories.push_back({
                {"vm_id", vm->id},
                {"status", "failed"},
                {"error", error.what()},
            });
        }
    }
    report["inventories"] = inventories;
    if (std::any_of(inventories.begin(), inventories.end(), [](const nlohmann::json& inventory) {
            return inventory.at("status") != "passed";
        })) {
        report["mode"] = "full";
        report["environment_status"] = environment_status;
        report["status"] = "failed";
        report["run_id"] = nullptr;
        report["finished_at"] = utc_timestamp();
        report["agents"] = nlohmann::json::array();
        return report;
    }

    TaskPlan plan;
    plan.name = "satsuma-automation-diagnostic";
    plan.run_id = make_id("check");
    for (const VmConfig* vm : targets) {
        TaskStep step;
        step.id = vm->id;
        step.vm = vm->id;
        step.type = "echo";
        step.message = "satsuma-diagnostic:" + *plan.run_id + ":" + vm->id;
        step.timeout_seconds = static_cast<int>(diagnostic_timeout.count());
        step.retry_safe = true;
        plan.steps.push_back(std::move(step));
    }

    Controller controller(config_);
    const RunManifest manifest = controller.create_run(plan);
    const std::filesystem::path run_directory = resolve_under_root(
        config_.transport.state_root,
        std::filesystem::path(L"runs") / path_from_utf8(manifest.run_id));
    nlohmann::json agents = nlohmann::json::array();
    std::map<std::string, nlohmann::json> latest_presence; // 与本次 echo 对应的 Agent 会话证据
    std::set<std::string> reported;
    std::map<std::string, std::string> validation_errors; // 瞬断期间保留最近一次读取错误
    while (reported.size() < targets.size() && std::chrono::steady_clock::now() < deadline) {
        for (const VmConfig* vm : targets) {
            if (reported.contains(vm->id)) {
                continue;
            }
            const std::filesystem::path presence_path = vm_presence_path(config_, *vm);
            std::error_code presence_error;
            if (std::filesystem::is_regular_file(presence_path, presence_error)) {
                try {
                    const nlohmann::json presence = load_vm_presence(config_, *vm);
                    latest_presence[vm->id] = presence;
                    validation_errors.erase(vm->id);
                    static_cast<void>(presence);
                } catch (const JsonIoError& error) {
                    validation_errors[vm->id] = error.what();
                    continue;
                } catch (const std::exception& error) {
                    agents.push_back({
                        {"vm_id", vm->id},
                        {"hardware_id", vm->hardware_id.empty()
                            ? nlohmann::json(nullptr)
                            : nlohmann::json(vm->hardware_id)},
                        {"status", "failed"},
                        {"message", "Agent identity validation failed"},
                        {"error", error.what()},
                    });
                    reported.insert(vm->id);
                    continue;
                }
            } else if (presence_error &&
                       presence_error != std::errc::no_such_file_or_directory) {
                validation_errors[vm->id] =
                    "Cannot inspect Agent presence: " + presence_error.message();
                continue;
            }
            const std::filesystem::path result_path = resolve_under_root(
                run_directory,
                std::filesystem::path(L"results") /
                    path_from_utf8(vm->id) /
                    path_from_utf8(vm->id) /
                    L"execution.json");
            std::error_code availability_error;
            const bool result_available = std::filesystem::is_regular_file(
                result_path,
                availability_error);
            if (availability_error) {
                validation_errors[vm->id] =
                    "Cannot inspect Agent diagnostic result: " + availability_error.message();
                continue;
            }
            if (!result_available) {
                continue;
            }

            nlohmann::json agent = {
                {"vm_id", vm->id},
                {"hardware_id", vm->hardware_id.empty()
                    ? nlohmann::json(nullptr)
                    : nlohmann::json(vm->hardware_id)},
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
                const auto presence_it = latest_presence.find(vm->id);
                if (presence_it != latest_presence.end()) {
                    const nlohmann::json& presence = presence_it->second;
                    agent["presence"] = {
                        {"agent_version", presence.value("agent_version", std::string{})},
                        {"binary_sha256", presence.value("binary_sha256", std::string{})},
                        {"boot_id", presence.value("boot_id", std::string{})},
                        {"process_id", presence.value("process_id", 0)},
                        {"runtime", presence.value("runtime", nlohmann::json(nullptr))},
                        {"updated_at", presence.value("updated_at", std::string{})},
                    };
                }
            } catch (const std::exception& error) {
                validation_errors[vm->id] = error.what();
                continue;
            }
            validation_errors.erase(vm->id);
            agents.push_back(std::move(agent));
            reported.insert(vm->id);
        }
        if (reported.size() < targets.size()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

    const nlohmann::json discovery = discover_agents(config_);
    for (const VmConfig* vm : targets) {
        if (!reported.contains(vm->id)) {
            const auto validation_error = validation_errors.find(vm->id);
            if (validation_error != validation_errors.end()) {
                agents.push_back({
                    {"vm_id", vm->id},
                    {"status", "failed"},
                    {"message", "Cannot validate Agent diagnostic result before the deadline"},
                    {"error", validation_error->second},
                });
            } else {
                nlohmann::json unbound_hardware_ids = nlohmann::json::array();
                if (vm->hardware_id.empty()) {
                    for (const auto& discovered : discovery.at("agents")) {
                        if (discovered.at("status") == "unbound") {
                            unbound_hardware_ids.push_back(discovered.at("hardware_id"));
                        }
                    }
                }
                nlohmann::json agent = {
                    {"vm_id", vm->id},
                    {"hardware_id", vm->hardware_id.empty()
                        ? nlohmann::json(nullptr)
                        : nlohmann::json(vm->hardware_id)},
                    {"status", unbound_hardware_ids.empty() ? "timeout" : "failed"},
                    {"message", unbound_hardware_ids.empty()
                        ? "Agent did not return the diagnostic task before the deadline"
                        : "Unbound Agents detected; run agent rebind with one discovered hardware_id"},
                };
                if (!unbound_hardware_ids.empty()) {
                    agent["unbound_hardware_ids"] = std::move(unbound_hardware_ids);
                }
                agents.push_back(std::move(agent));
            }
        }
    }

    std::size_t agent_passed = 0;
    std::size_t agent_skipped = 0;
    for (const auto& agent : agents) {
        agent_passed += agent.at("status") == "passed" ? 1 : 0;
        agent_skipped += agent.at("status") == "skipped" ? 1 : 0;
    }
    const std::size_t agent_failed = agents.size() - agent_passed - agent_skipped;
    if (agent_failed == 0 &&
        environment_status != "ready" &&
        only_vmware_tools_pending(report)) {
        // Agent 上线后 VMware Tools heartbeat 仍可能延迟，有限等待环境状态收敛。
        const auto recheck_deadline =
            std::chrono::steady_clock::now() + kEnvironmentRecheckTimeout;
        std::size_t recheck_attempts = 0; // 实际执行的环境复检次数
        do {
            report = inspect_environment(vm_id);
            ++recheck_attempts;
            environment_status = report.at("status").get<std::string>();
            if (environment_status == "ready" ||
                std::chrono::steady_clock::now() >= recheck_deadline) {
                break;
            }
            std::this_thread::sleep_for(kEnvironmentRecheckDelay);
        } while (true);
        report["environment_rechecked_after_agent"] = true;
        report["environment_recheck_attempts"] = recheck_attempts;
        report["initial_environment_status"] = initial_environment_status;
        report["initial_environment"] = initial_environment;
    }
    const std::string status = agent_failed > 0 || agent_skipped > 0
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
        {"skipped", agent_skipped},
    };
    report["agents"] = std::move(agents);
    return report;
}

}  // namespace satsuma::host
