// SatsumaHost 命令行入口。
#include <algorithm>
#include <charconv>
#include <chrono>
#include <exception>
#include <filesystem>
#include <initializer_list>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "controller.hpp"
#include "diagnostics.hpp"
#include "identity.hpp"
#include "lab_lease.hpp"
#include "orchestrator.hpp"
#include "satsuma/core/config.hpp"
#include "satsuma/core/errors.hpp"
#include "satsuma/core/id.hpp"
#include "satsuma/core/json_io.hpp"
#include "satsuma/core/path.hpp"
#include "satsuma/core/snapshot.hpp"
#include "satsuma/core/version.hpp"
#include "vmrun_provider.hpp"

namespace {

// 将宽字符选项名转换为稳定的 UTF-8 错误文本。
[[nodiscard]] std::string option_name_utf8(const std::wstring_view name) {
    return satsuma::path_to_utf8(std::filesystem::path(std::wstring(name)));
}

// 解析 --name value 形式的严格命令行选项。
[[nodiscard]] std::map<std::wstring, std::wstring> parse_options(
    const int argc,
    wchar_t* argv[],
    const int start) {
    std::map<std::wstring, std::wstring> options;
    for (int index = start; index < argc; index += 2) {
        if (index + 1 >= argc || std::wstring(argv[index]).rfind(L"--", 0) != 0) {
            throw satsuma::Error("Options must use --name value pairs");
        }
        const std::wstring name = std::wstring(argv[index]).substr(2);
        if (name.empty()) {
            throw satsuma::Error("Command-line option name must not be empty");
        }
        if (!options.emplace(name, argv[index + 1]).second) {
            throw satsuma::Error("Duplicate command-line option: --" + option_name_utf8(name));
        }
    }
    return options;
}

// 拒绝当前命令不认识的选项，避免拼写错误静默回落为默认行为。
void validate_options(
    const std::map<std::wstring, std::wstring>& options,
    const std::initializer_list<std::wstring_view> allowed) {
    for (const auto& [name, value] : options) {
        static_cast<void>(value);
        if (std::find(allowed.begin(), allowed.end(), name) == allowed.end()) {
            throw satsuma::Error("Unknown command-line option: --" + option_name_utf8(name));
        }
    }
}

// 判断命令和子命令是否属于当前稳定 CLI。
[[nodiscard]] bool is_supported_command(
    const std::wstring_view command,
    const std::wstring_view subcommand) {
    if (command == L"check" || command == L"discover" || command == L"run" ||
        command == L"orchestrate" || command == L"report") {
        return subcommand.empty();
    }
    if (command == L"vm") {
        return subcommand == L"start" || subcommand == L"stop" || subcommand == L"restore";
    }
    if (command == L"snapshot") {
        return subcommand == L"list" || subcommand == L"create-ai" || subcommand == L"delete-ai";
    }
    if (command == L"agent") {
        return subcommand == L"update" || subcommand == L"rebind" ||
            subcommand == L"inventory" || subcommand == L"inventory refresh";
    }
    if (command == L"runs") {
        return subcommand == L"list" || subcommand == L"cancel" || subcommand == L"prune" ||
            subcommand == L"finalize";
    }
    if (command == L"lab") {
        return subcommand == L"status" || subcommand == L"recover" || subcommand == L"unlock";
    }
    return false;
}

// 校验每个稳定命令允许的完整选项集合。
void validate_command_options(
    const std::wstring_view command,
    const std::wstring_view subcommand,
    const std::map<std::wstring, std::wstring>& options) {
    if (command == L"check") {
        validate_options(options, {L"config", L"vm", L"timeout-seconds"});
    } else if (command == L"discover") {
        validate_options(options, {L"config"});
    } else if (command == L"run") {
        validate_options(options, {L"config", L"plan", L"timeout-seconds"});
    } else if (command == L"orchestrate") {
        validate_options(options, {L"config", L"plan", L"timeout-seconds"});
    } else if (command == L"report") {
        validate_options(options, {L"config", L"run", L"wait-seconds"});
    } else if (command == L"vm" && subcommand == L"start") {
        validate_options(options, {L"config", L"id"});
    } else if (command == L"vm" && subcommand == L"stop") {
        validate_options(options, {L"config", L"id", L"mode"});
    } else if (command == L"vm" && subcommand == L"restore") {
        validate_options(options, {L"config", L"id", L"snapshot"});
    } else if (command == L"snapshot" && subcommand == L"list") {
        validate_options(options, {L"config", L"vm"});
    } else if (command == L"snapshot" && subcommand == L"create-ai") {
        validate_options(options, {L"config", L"vm", L"name"});
    } else if (command == L"snapshot" && subcommand == L"delete-ai") {
        validate_options(options, {L"config", L"vm", L"snapshot"});
    } else if (command == L"agent" && subcommand == L"update") {
        validate_options(options, {L"config", L"vm", L"binary", L"version", L"timeout-seconds"});
    } else if (command == L"agent" && subcommand == L"rebind") {
        validate_options(
            options,
            {L"config", L"vm", L"hardware-id", L"next-vm", L"binary", L"version",
             L"timeout-seconds"});
    } else if (command == L"agent" && subcommand == L"inventory") {
        validate_options(options, {L"config", L"vm"});
    } else if (command == L"agent" && subcommand == L"inventory refresh") {
        validate_options(options, {L"config", L"vm", L"timeout-seconds"});
    } else if (command == L"runs" && subcommand == L"list") {
        validate_options(options, {L"config"});
    } else if (command == L"runs" && subcommand == L"cancel") {
        validate_options(options, {L"config", L"run", L"reason"});
    } else if (command == L"runs" && subcommand == L"prune") {
        validate_options(options, {L"config", L"keep"});
    } else if (command == L"runs" && subcommand == L"finalize") {
        validate_options(options, {L"config", L"run"});
    } else if (command == L"lab" && subcommand == L"status") {
        validate_options(options, {L"config"});
    } else if (command == L"lab" && subcommand == L"recover") {
        validate_options(options, {L"config", L"plan", L"timeout-seconds"});
    } else if (command == L"lab" && subcommand == L"unlock") {
        validate_options(options, {L"config", L"force"});
    }
}

// 读取必需命令行选项。
[[nodiscard]] std::wstring require_option(
    const std::map<std::wstring, std::wstring>& options,
    const std::wstring& name) {
    const auto match = options.find(name);
    if (match == options.end() || match->second.empty()) {
        throw satsuma::Error("Missing required command-line option: --" + option_name_utf8(name));
    }
    return match->second;
}

// 解析检测模式等待 Agent 的秒数。
[[nodiscard]] std::chrono::seconds parse_diagnostic_timeout(
    const std::map<std::wstring, std::wstring>& options) {
    const auto match = options.find(L"timeout-seconds");
    if (match == options.end()) {
        return std::chrono::seconds(30);
    }

    const std::string value = satsuma::path_to_utf8(match->second);
    int seconds = 0;
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), seconds);
    if (error != std::errc{} || end != value.data() + value.size() || seconds < 1 || seconds > 300) {
        throw satsuma::Error("Diagnostic timeout must be an integer between 1 and 300 seconds");
    }
    return std::chrono::seconds(seconds);
}

// 解析生命周期编排的有限等待秒数。
[[nodiscard]] std::chrono::seconds parse_orchestration_timeout(
    const std::map<std::wstring, std::wstring>& options) {
    const auto match = options.find(L"timeout-seconds");
    if (match == options.end()) {
        return std::chrono::seconds(300);
    }

    const std::string value = satsuma::path_to_utf8(match->second);
    int seconds = 0;
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), seconds);
    if (error != std::errc{} || end != value.data() + value.size() || seconds < 1 || seconds > 86'400) {
        throw satsuma::Error("Orchestration timeout must be an integer between 1 and 86400 seconds");
    }
    return std::chrono::seconds(seconds);
}

// 解析 report 的可选有限等待秒数。
[[nodiscard]] std::optional<std::chrono::seconds> parse_report_wait(
    const std::map<std::wstring, std::wstring>& options) {
    const auto match = options.find(L"wait-seconds");
    if (match == options.end()) {
        return std::nullopt;
    }

    const std::string value = satsuma::path_to_utf8(match->second);
    int seconds = 0;
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), seconds);
    if (error != std::errc{} || end != value.data() + value.size() || seconds < 1 || seconds > 86'400) {
        throw satsuma::Error("Report wait must be an integer between 1 and 86400 seconds");
    }
    return std::chrono::seconds(seconds);
}

// 解析共享运行保留数量。
[[nodiscard]] std::size_t parse_run_retention(
    const std::map<std::wstring, std::wstring>& options) {
    const std::string value = satsuma::path_to_utf8(require_option(options, L"keep"));
    unsigned int keep = 0;
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), keep);
    if (error != std::errc{} || end != value.data() + value.size() || keep > 10'000) {
        throw satsuma::Error("Run retention must be an integer between 0 and 10000");
    }
    return keep;
}

// 解析 Agent 更新结果的有限等待秒数。
[[nodiscard]] std::chrono::seconds parse_update_timeout(
    const std::map<std::wstring, std::wstring>& options) {
    const auto match = options.find(L"timeout-seconds");
    if (match == options.end()) {
        return std::chrono::seconds(180);
    }

    const std::string value = satsuma::path_to_utf8(match->second);
    int seconds = 0;
    const auto [end, error] = std::from_chars(
        value.data(),
        value.data() + value.size(),
        seconds);
    if (error != std::errc{} ||
        end != value.data() + value.size() ||
        seconds < 1 ||
        seconds > 3'600) {
        throw satsuma::Error(
            "Agent update timeout must be an integer between 1 and 3600 seconds");
    }
    return std::chrono::seconds(seconds);
}

// 在控制命令报错后重新读取 VMware 状态，判断目标快照是否存在。
[[nodiscard]] bool snapshot_exists(
    const satsuma::vmware::VmrunProvider& provider,
    const std::filesystem::path& vmx,
    const std::string& snapshot_name) {
    const std::vector<std::string> snapshots = provider.list_snapshots(vmx);
    return std::find(snapshots.begin(), snapshots.end(), snapshot_name) != snapshots.end();
}

// 开始新的快照操作前清除上一事务留下的终态诊断字段。
void clear_snapshot_operation_diagnostics(nlohmann::json& metadata) {
    metadata.erase("error");
    metadata.erase("reconciliation_error");
    metadata.erase("reconciled_after_error");
    metadata.erase("operation_error");
    metadata.erase("reconciled_at");
    metadata.erase("deleted_at");
}

// 输出当前首个增量支持的 CLI 用法。
void print_usage() {
    std::cout
        << "SatsumaHost " << satsuma::kVersion << '\n'
        << "Usage:\n"
        << "  SatsumaHost --help\n"
        << "  SatsumaHost --version\n"
        << "  SatsumaHost check --config lab.local.json [--vm <vm-id>] [--timeout-seconds <1-300>]\n"
        << "  SatsumaHost discover --config lab.local.json\n"
        << "  SatsumaHost vm start --config lab.local.json --id <vm-id>\n"
        << "  SatsumaHost vm stop --config lab.local.json --id <vm-id> [--mode soft|hard]\n"
        << "  SatsumaHost vm restore --config lab.local.json --id <vm-id> --snapshot <name>\n"
        << "  SatsumaHost snapshot list --config lab.local.json --vm <vm-id>\n"
        << "  SatsumaHost snapshot create-ai --config lab.local.json --vm <vm-id> --name <purpose>\n"
        << "  SatsumaHost snapshot delete-ai --config lab.local.json --vm <vm-id> --snapshot <name>\n"
        << "  SatsumaHost agent update --vm <vm-id> --binary SatsumaVM.exe --version <version> "
           "[--timeout-seconds <1-3600>] --config lab.local.json\n"
        << "  SatsumaHost agent rebind --vm <current-id> --next-vm <new-id> "
           "--binary SatsumaVM.exe --version <version> "
           "[--timeout-seconds <1-3600>] --config lab.local.json\n"
        << "  SatsumaHost agent rebind --vm <vm-id> --hardware-id <uuid> "
           "--config lab.local.json\n"
        << "  SatsumaHost agent inventory --config lab.local.json --vm <vm-id>\n"
        << "  SatsumaHost agent inventory refresh --config lab.local.json --vm <vm-id> "
           "[--timeout-seconds <1-300>]\n"
        << "  SatsumaHost run --config lab.local.json --plan task.json "
           "[--timeout-seconds <1-300>]\n"
        << "  SatsumaHost orchestrate --config lab.local.json --plan task.json "
           "[--timeout-seconds <1-86400>]\n"
        << "  SatsumaHost report --config lab.local.json --run <run-id> [--wait-seconds <1-86400>]\n"
        << "  SatsumaHost runs list --config lab.local.json\n"
        << "  SatsumaHost runs cancel --config lab.local.json --run <run-id> [--reason <text>]\n"
        << "  SatsumaHost runs finalize --config lab.local.json --run <run-id>\n"
        << "  SatsumaHost runs prune --config lab.local.json --keep <0-10000>\n"
        << "  SatsumaHost lab status --config lab.local.json\n"
        << "  SatsumaHost lab recover --config lab.local.json --plan task.json "
           "[--timeout-seconds <1-86400>]\n"
        << "  SatsumaHost lab unlock --config lab.local.json --force true\n";
}

}  // namespace

// 运行 Host CLI 并把业务错误转换为稳定退出码。
int wmain(const int argc, wchar_t* argv[]) {
    try {
        if (argc < 2) {
            print_usage();
            return 2;
        }

        const std::wstring command = argv[1];
        if (argc == 2 && (command == L"--help" || command == L"help")) {
            print_usage();
            return 0;
        }
        if (argc == 2 && command == L"--version") {
            std::cout << satsuma::kVersion << '\n';
            return 0;
        }

        std::wstring subcommand;
        int options_start = 2;
        const bool grouped_command =
            command == L"vm" || command == L"snapshot" || command == L"agent" ||
            command == L"runs" || command == L"lab";
        if (grouped_command) {
            if (argc < 3) {
                print_usage();
                return 2;
            }
            subcommand = argv[2];
            options_start = 3;
            if (command == L"agent" && subcommand == L"inventory" && argc >= 4 &&
                std::wstring_view(argv[3]) == L"refresh") {
                subcommand = L"inventory refresh";
                options_start = 4;
            }
        }

        if (!is_supported_command(command, subcommand)) {
            print_usage();
            return 2;
        }

        const auto options = parse_options(argc, argv, options_start);
        validate_command_options(command, subcommand, options);
        const std::filesystem::path config_path = require_option(options, L"config");
        satsuma::LabConfig config = satsuma::load_lab_config(config_path);

        if (command == L"lab" && subcommand == L"status") {
            std::cout << satsuma::host::LabLease::status(config).dump(2) << '\n';
            return 0;
        }
        if (command == L"lab" && subcommand == L"unlock") {
            if (satsuma::path_to_utf8(require_option(options, L"force")) != "true") {
                throw satsuma::Error("lab unlock requires --force true");
            }
            std::cout << satsuma::host::LabLease::force_unlock(config, config_path).dump(2) << '\n';
            return 0;
        }
        if (command == L"lab" && subcommand == L"recover") {
            const std::filesystem::path plan_path = require_option(options, L"plan");
            const satsuma::TaskPlan plan = satsuma::load_task_plan(plan_path);
            if (!plan.lifecycle.has_value() || !plan.run_id.has_value()) {
                throw satsuma::Error("lab recover requires a lifecycle plan with an explicit run_id");
            }
            auto lease = satsuma::host::LabLease::acquire(
                config, config_path, "lab recover", plan.run_id, true);
            satsuma::host::Orchestrator orchestrator(config);
            const nlohmann::json output = orchestrator.execute(
                plan_path, parse_orchestration_timeout(options));
            const std::string status = output.at("status").get<std::string>();
            if (status == "COMPLETED" || status == "FAILED") {
                lease->release("released");
            }
            std::cout << output.dump(2) << '\n';
            return status == "COMPLETED" ? 0 : 1;
        }

        if (command == L"discover") {
            std::cout << satsuma::host::discover_agents(config).dump(2) << '\n';
            return 0;
        }

        const bool short_write_operation =
            command == L"vm" ||
            (command == L"snapshot" && subcommand != L"list") ||
            (command == L"agent" && subcommand != L"inventory") ||
            (command == L"runs" && (subcommand == L"cancel" || subcommand == L"prune"));
        std::unique_ptr<satsuma::host::LabLease> operation_lease;
        if (short_write_operation) {
            operation_lease = satsuma::host::LabLease::acquire(
                config,
                config_path,
                satsuma::path_to_utf8(command + (subcommand.empty() ? L"" : L" " + subcommand)));
        }

        if (command == L"runs") {
            if (subcommand == L"finalize") {
                const std::string run_id = satsuma::path_to_utf8(require_option(options, L"run"));
                std::cout << satsuma::host::LabLease::finalize_run(
                    config, config_path, run_id).dump(2) << '\n';
                return 0;
            }
            satsuma::host::Controller controller(std::move(config));
            if (subcommand == L"list") {
                std::cout << controller.list_runs().dump(2) << '\n';
                return 0;
            }
            if (subcommand == L"cancel") {
                const std::string run_id = satsuma::path_to_utf8(require_option(options, L"run"));
                const auto reason_option = options.find(L"reason");
                const std::string reason = reason_option == options.end()
                    ? "Cancellation requested by SatsumaHost"
                    : satsuma::path_to_utf8(reason_option->second);
                const nlohmann::json output = controller.cancel_run(run_id, reason);
                operation_lease->release("released");
                std::cout << output.dump(2) << '\n';
                return 0;
            }
            const nlohmann::json output = controller.prune_runs(parse_run_retention(options));
            operation_lease->release("released");
            std::cout << output.dump(2) << '\n';
            return 0;
        }

        if (command == L"check") {
            std::optional<std::string> vm_id;
            const auto vm_option = options.find(L"vm");
            if (vm_option != options.end()) {
                vm_id = satsuma::path_to_utf8(vm_option->second);
            }
            const std::chrono::seconds timeout = parse_diagnostic_timeout(options);
            auto lease = satsuma::host::LabLease::acquire(config, config_path, "check");
            satsuma::host::Diagnostics diagnostics(std::move(config));
            const nlohmann::json report = diagnostics.run_probe(vm_id, timeout);
            lease->release("released");
            std::cout << report.dump(2) << '\n';
            const std::string status = report.at("status").get<std::string>();
            if (status == "ready") {
                return 0;
            }
            return status == "degraded" ? 3 : 1;
        }

        if (command == L"agent") {
            const std::string vm_id = satsuma::path_to_utf8(
                require_option(options, L"vm"));
            const satsuma::VmConfig* vm = satsuma::find_vm(config, vm_id);
            if (vm == nullptr) {
                throw satsuma::Error("Unknown VM id: " + vm_id);
            }
            if (subcommand == L"inventory") {
                std::cout << satsuma::host::load_vm_inventory(config, *vm).dump(2) << '\n';
                return 0;
            }
            if (subcommand == L"inventory refresh") {
                const nlohmann::json output = satsuma::host::refresh_vm_inventory(
                    config, *vm, parse_diagnostic_timeout(options));
                operation_lease->release("released");
                std::cout << output.dump(2) << '\n';
                return 0;
            }
            const auto hardware_option = options.find(L"hardware-id");
            if (subcommand == L"rebind" && hardware_option != options.end()) {
                if (options.contains(L"next-vm") || options.contains(L"binary") ||
                    options.contains(L"version") || options.contains(L"timeout-seconds")) {
                    throw satsuma::Error(
                        "Hardware rebind cannot be combined with update rebind options");
                }
                std::cout << satsuma::host::bind_agent_hardware(
                    config_path,
                    config,
                    vm_id,
                    satsuma::path_to_utf8(hardware_option->second)).dump(2) << '\n';
                operation_lease->release("released");
                return 0;
            }
            const std::filesystem::path binary = require_option(options, L"binary");
            const std::string version = satsuma::path_to_utf8(
                require_option(options, L"version"));
            std::optional<std::string> next_vm_id;
            if (subcommand == L"rebind") {
                next_vm_id = satsuma::path_to_utf8(
                    require_option(options, L"next-vm"));
            }
            const std::chrono::seconds timeout = parse_update_timeout(options);
            satsuma::host::Controller controller(std::move(config));
            const satsuma::AgentUpdateManifest manifest =
                controller.publish_agent_update(vm_id, binary, version, next_vm_id);
            const satsuma::AgentUpdateResult result = controller.wait_agent_update(
                vm_id,
                manifest.update_id,
                timeout);
            nlohmann::json output = result;
            output["manifest"] = manifest;
            std::cout << output.dump(2) << '\n';
            if (result.status == "succeeded") {
                operation_lease->release("released");
            }
            return result.status == "succeeded" ? 0 : 1;
        }

        if (command == L"vm") {
            if (subcommand != L"start" && subcommand != L"stop" && subcommand != L"restore") {
                print_usage();
                return 2;
            }
            const std::string vm_id = satsuma::path_to_utf8(require_option(options, L"id"));
            const satsuma::VmConfig* vm = satsuma::find_vm(config, vm_id);
            if (vm == nullptr) {
                throw satsuma::Error("Unknown VM id: " + vm_id);
            }
            satsuma::vmware::VmrunProvider provider(config.provider.vmrun);
            std::string status;  // 返回给调用方的电源操作状态
            std::string snapshot_name;  // 恢复操作使用的快照名
            std::string stop_mode;  // 关闭操作使用的 vmrun 模式
            if (subcommand == L"start") {
                provider.start(vm->vmx);
                status = "started";
            } else if (subcommand == L"stop") {
                const auto mode_option = options.find(L"mode");
                stop_mode = mode_option == options.end()
                    ? "soft"
                    : satsuma::path_to_utf8(mode_option->second);
                satsuma::vmware::VmStopMode mode;
                if (stop_mode == "soft") {
                    mode = satsuma::vmware::VmStopMode::Soft;
                } else if (stop_mode == "hard") {
                    mode = satsuma::vmware::VmStopMode::Hard;
                } else {
                    throw satsuma::Error("VM stop mode must be soft or hard");
                }
                provider.stop(vm->vmx, mode);
                status = "stopped";
            } else {
                snapshot_name = satsuma::path_to_utf8(require_option(options, L"snapshot"));
                provider.revert_to_snapshot(vm->vmx, snapshot_name);
                status = "restored";
            }
            nlohmann::json output = {
                {"status", status},
                {"vm_id", vm->id},
            };
            if (!snapshot_name.empty()) {
                output["snapshot"] = snapshot_name;
            }
            if (!stop_mode.empty()) {
                output["mode"] = stop_mode;
            }
            std::cout << output.dump(2) << '\n';
            operation_lease->release("released");
            return 0;
        }

        if (command == L"snapshot") {
            if (subcommand != L"list" && subcommand != L"create-ai" && subcommand != L"delete-ai") {
                print_usage();
                return 2;
            }
            const std::string vm_id = satsuma::path_to_utf8(require_option(options, L"vm"));
            const satsuma::VmConfig* vm = satsuma::find_vm(config, vm_id);
            if (vm == nullptr) {
                throw satsuma::Error("Unknown VM id: " + vm_id);
            }
            satsuma::vmware::VmrunProvider provider(config.provider.vmrun);
            const std::vector<std::string> existing = provider.list_snapshots(vm->vmx);
            if (subcommand == L"list") {
                nlohmann::json snapshots = nlohmann::json::array();
                for (const std::string& name : existing) {
                    std::string ownership = "external";  // 不受 Satsuma 管理的快照
                    if (name == vm->snapshots.base) {
                        ownership = "user_base";
                    } else if (name.starts_with(vm->snapshots.ai_prefix)) {
                        ownership = "ai";
                    }
                    snapshots.push_back({{"name", name}, {"ownership", ownership}});
                }
                std::cout << nlohmann::json({
                    {"status", "listed"},
                    {"vm_id", vm_id},
                    {"snapshots", std::move(snapshots)},
                }).dump(2) << '\n';
                return 0;
            }
            if (subcommand == L"delete-ai") {
                const std::string snapshot_name = satsuma::path_to_utf8(require_option(options, L"snapshot"));
                try {
                    satsuma::validate_ai_snapshot_deletion(vm->snapshots, existing, snapshot_name);
                } catch (...) {
                    operation_lease->release("rejected");
                    throw;
                }
                const std::filesystem::path metadata_path = satsuma::resolve_under_root(
                    config.host.archive_root,
                    std::filesystem::path(L"snapshots") /
                        satsuma::path_from_utf8(vm_id) /
                        satsuma::path_from_utf8(snapshot_name + ".json"));
                nlohmann::json metadata = std::filesystem::is_regular_file(metadata_path)
                    ? satsuma::load_json(metadata_path)
                    : nlohmann::json({
                        {"schema_version", 1},
                        {"type", "ai_snapshot"},
                        {"vm_id", vm_id},
                        {"snapshot", snapshot_name},
                    });
                clear_snapshot_operation_diagnostics(metadata);
                metadata["status"] = "deleting";
                satsuma::write_json_atomic(metadata_path, metadata);
                bool reconciled = false; // vmrun 报错后由实际快照状态确认操作结果
                try {
                    provider.delete_snapshot(vm->vmx, snapshot_name);
                } catch (const std::exception& error) {
                    std::string reconciliation_error;
                    try {
                        reconciled = !snapshot_exists(provider, vm->vmx, snapshot_name);
                    } catch (const std::exception& reconciliation_failure) {
                        reconciliation_error = reconciliation_failure.what();
                    }
                    if (!reconciled) {
                        metadata["status"] = "delete_failed";
                        metadata["error"] = error.what();
                        if (!reconciliation_error.empty()) {
                            metadata["reconciliation_error"] = reconciliation_error;
                        }
                        satsuma::write_json_atomic(metadata_path, metadata);
                        throw;
                    }
                    metadata["reconciled_after_error"] = true;
                    metadata["operation_error"] = error.what();
                    metadata["reconciled_at"] = satsuma::utc_timestamp();
                }
                metadata["status"] = "deleted";
                metadata["deleted_at"] = satsuma::utc_timestamp();
                satsuma::write_json_atomic(metadata_path, metadata);
                nlohmann::json output = {
                    {"status", "deleted"},
                    {"vm_id", vm_id},
                    {"snapshot", snapshot_name},
                };
                if (reconciled) {
                    output["reconciled"] = true;
                }
                std::cout << output.dump(2) << '\n';
                operation_lease->release("released");
                return 0;
            }

            const std::string purpose = satsuma::path_to_utf8(require_option(options, L"name"));
            const std::string snapshot_name = satsuma::plan_ai_snapshot_name(
                vm->snapshots,
                existing,
                purpose,
                satsuma::utc_timestamp_compact());
            const std::filesystem::path metadata_path = satsuma::resolve_under_root(
                config.host.archive_root,
                std::filesystem::path(L"snapshots") /
                    satsuma::path_from_utf8(vm_id) /
                    satsuma::path_from_utf8(snapshot_name + ".json"));
            nlohmann::json metadata = {
                {"schema_version", 1},
                {"type", "ai_snapshot"},
                {"status", "creating"},
                {"vm_id", vm_id},
                {"snapshot", snapshot_name},
                {"purpose", purpose},
                {"parent_snapshot", nullptr},
                {"agent_version", vm->agent_version},
                {"created_at", satsuma::utc_timestamp()},
            };
            satsuma::write_json_atomic(metadata_path, metadata);
            bool reconciled = false; // vmrun 报错后由实际快照状态确认操作结果
            try {
                provider.create_snapshot(vm->vmx, snapshot_name);
            } catch (const std::exception& error) {
                std::string reconciliation_error;
                try {
                    reconciled = snapshot_exists(provider, vm->vmx, snapshot_name);
                } catch (const std::exception& reconciliation_failure) {
                    reconciliation_error = reconciliation_failure.what();
                }
                if (!reconciled) {
                    metadata["status"] = "failed";
                    metadata["error"] = error.what();
                    if (!reconciliation_error.empty()) {
                        metadata["reconciliation_error"] = reconciliation_error;
                    }
                    satsuma::write_json_atomic(metadata_path, metadata);
                    throw;
                }
                metadata["reconciled_after_error"] = true;
                metadata["operation_error"] = error.what();
                metadata["reconciled_at"] = satsuma::utc_timestamp();
            }
            metadata["status"] = "created";
            satsuma::write_json_atomic(metadata_path, metadata);
            nlohmann::json output = {
                {"status", "created"},
                {"vm_id", vm_id},
                {"snapshot", snapshot_name},
            };
            if (reconciled) {
                output["reconciled"] = true;
            }
            std::cout << output.dump(2) << '\n';
            operation_lease->release("released");
            return 0;
        }

        if (command == L"orchestrate") {
            const std::filesystem::path plan_path = require_option(options, L"plan");
            const std::chrono::seconds timeout = parse_orchestration_timeout(options);
            const satsuma::TaskPlan plan = satsuma::load_task_plan(plan_path);
            if (!plan.lifecycle.has_value() || !plan.run_id.has_value()) {
                throw satsuma::Error(
                    "Host orchestrate requires an explicit plan run_id for crash recovery");
            }
            auto lease = satsuma::host::LabLease::acquire(
                config, config_path, "orchestrate", plan.run_id);
            satsuma::host::Orchestrator orchestrator(std::move(config));
            const nlohmann::json output = orchestrator.execute(plan_path, timeout);
            const std::string status = output.at("status").get<std::string>();
            if (status == "COMPLETED" || status == "FAILED") {
                lease->release("released");
            }
            std::cout << output.dump(2) << '\n';
            if (status == "COMPLETED") {
                return 0;
            }
            if (status == "RECOVERY_FAILED") {
                return 4;
            }
            return status == "MANUAL_INTERVENTION_REQUIRED" ? 5 : 1;
        }

        if (command == L"run") {
            const std::filesystem::path plan_path = require_option(options, L"plan");
            const satsuma::TaskPlan plan = satsuma::load_task_plan(plan_path);
            if (plan.lifecycle.has_value()) {
                throw satsuma::Error(
                    "Task lifecycle policies require the Host orchestrator and cannot use run");
            }
            auto lease = satsuma::host::LabLease::acquire(config, config_path, "run");
            satsuma::host::Diagnostics diagnostics(config);
            const std::chrono::seconds check_timeout = parse_diagnostic_timeout(options);
            std::vector<std::string> checked_vms;
            nlohmann::json checks = nlohmann::json::array();
            bool checks_ready = true;
            for (const satsuma::TaskStep& step : plan.steps) {
                if (std::find(checked_vms.begin(), checked_vms.end(), step.vm) != checked_vms.end()) {
                    continue;
                }
                checked_vms.push_back(step.vm);
                nlohmann::json check = diagnostics.run_probe(step.vm, check_timeout);
                checks_ready = checks_ready && check.at("status") == "ready";
                checks.push_back({{"vm_id", step.vm}, {"result", std::move(check)}});
            }
            if (!checks_ready) {
                std::cout << nlohmann::json({
                    {"status", "check_failed"},
                    {"checks", checks},
                }).dump(2) << '\n';
                return 1;
            }
            satsuma::host::Controller controller(config);
            const satsuma::RunManifest manifest = controller.create_run(plan);
            lease->attach_run(manifest.run_id);
            nlohmann::json output = {
                {"status", "prepared"},
                {"run_id", manifest.run_id},
                {"request_id", manifest.request_id},
            };
            std::cout << output.dump(2) << '\n';
            return 0;
        }
        satsuma::host::Controller controller(std::move(config));
        if (command == L"report") {
            const std::string run_id = satsuma::path_to_utf8(require_option(options, L"run"));
            const std::optional<std::chrono::seconds> wait = parse_report_wait(options);
            const auto started = std::chrono::steady_clock::now();
            const auto deadline = wait.has_value()
                ? started + *wait
                : started;
            nlohmann::json report;
            std::string wait_status;
            do {
                report = controller.build_report(run_id);
                if (report.at("complete").get<bool>()) {
                    if (wait.has_value()) {
                        wait_status = "completed";
                    }
                    break;
                }
                if (report.value("manual_intervention_required", false)) {
                    if (wait.has_value()) {
                        wait_status = "manual_intervention_required";
                    }
                    break;
                }
                if (!wait.has_value()) {
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            } while (std::chrono::steady_clock::now() < deadline);

            if (wait.has_value()) {
                if (wait_status.empty()) {
                    wait_status = "timeout";
                }
                report["wait_status"] = wait_status;
                report["waited_ms"] = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - started).count();
            }
            std::cout << report.dump(2) << '\n';
            if (report.at("status") == "manual_intervention_required") {
                return 5;
            }
            if (wait_status == "timeout") {
                return 3;
            }
            return report.at("status") == "failed" ? 1 : 0;
        }

        print_usage();
        return 2;
    } catch (const std::exception& error) {
        std::cerr << "SatsumaHost error: " << error.what() << '\n';
        return 1;
    }
}
