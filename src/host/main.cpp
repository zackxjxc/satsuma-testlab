// SatsumaHost 命令行入口。
#include <charconv>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "controller.hpp"
#include "diagnostics.hpp"
#include "orchestrator.hpp"
#include "rpc_server.hpp"
#include "satsuma/core/config.hpp"
#include "satsuma/core/errors.hpp"
#include "satsuma/core/id.hpp"
#include "satsuma/core/json_io.hpp"
#include "satsuma/core/path.hpp"
#include "satsuma/core/snapshot.hpp"
#include "vmrun_provider.hpp"

namespace {

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
        if (!options.emplace(name, argv[index + 1]).second) {
            throw satsuma::Error("Duplicate command-line option");
        }
    }
    return options;
}

// 读取必需命令行选项。
[[nodiscard]] std::wstring require_option(
    const std::map<std::wstring, std::wstring>& options,
    const std::wstring& name) {
    const auto match = options.find(name);
    if (match == options.end() || match->second.empty()) {
        throw satsuma::Error("Missing required command-line option");
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

// 输出当前首个增量支持的 CLI 用法。
void print_usage() {
    std::cout
        << "SatsumaHost 0.1.0\n"
        << "Usage:\n"
        << "  SatsumaHost check --config lab.json [--vm <vm-id>] [--timeout-seconds <1-300>]\n"
        << "  SatsumaHost serve --config lab.json\n"
        << "  SatsumaHost vm start --id <vm-id> [--config lab.json]\n"
        << "  SatsumaHost vm stop --id <vm-id> [--mode soft|hard] [--config lab.json]\n"
        << "  SatsumaHost vm restore --id <vm-id> --snapshot <name> [--config lab.json]\n"
        << "  SatsumaHost snapshot list --vm <vm-id> [--config lab.json]\n"
        << "  SatsumaHost snapshot create-ai --vm <vm-id> --name <purpose> [--config lab.json]\n"
        << "  SatsumaHost snapshot delete-ai --vm <vm-id> --snapshot <name> [--config lab.json]\n"
        << "  SatsumaHost agent update --vm <vm-id> --binary SatsumaVM.exe --version <version> "
           "[--timeout-seconds <1-3600>] [--config lab.json]\n"
        << "  SatsumaHost agent rebind --vm <current-id> --next-vm <new-id> "
           "--binary SatsumaVM.exe --version <version> "
           "[--timeout-seconds <1-3600>] [--config lab.json]\n"
        << "  SatsumaHost run --config lab.json --plan task.json\n"
        << "  SatsumaHost orchestrate --config lab.json --plan task.json [--timeout-seconds <1-86400>]\n"
        << "  SatsumaHost report --config lab.json --run <run-id> [--wait-seconds <1-86400>]\n";
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
        std::wstring subcommand;
        int options_start = 2;
        const bool grouped_command =
            command == L"vm" || command == L"snapshot" || command == L"agent";
        if (grouped_command) {
            if (argc < 3) {
                print_usage();
                return 2;
            }
            subcommand = argv[2];
            options_start = 3;
        }

        const auto options = parse_options(argc, argv, options_start);
        const std::filesystem::path config_path =
            grouped_command && !options.contains(L"config")
                ? std::filesystem::path(L"lab.json")
                : std::filesystem::path(require_option(options, L"config"));
        satsuma::LabConfig config = satsuma::load_lab_config(config_path);

        if (command == L"serve") {
            satsuma::host::RpcServer server(std::move(config));
            server.start();
            return 0;
        }

        if (command == L"check") {
            std::optional<std::string> vm_id;
            const auto vm_option = options.find(L"vm");
            if (vm_option != options.end()) {
                vm_id = satsuma::path_to_utf8(vm_option->second);
            }
            const std::chrono::seconds timeout = parse_diagnostic_timeout(options);
            satsuma::host::Diagnostics diagnostics(std::move(config));
            const nlohmann::json report = diagnostics.run_probe(vm_id, timeout);
            std::cout << report.dump(2) << '\n';
            const std::string status = report.at("status").get<std::string>();
            if (status == "ready") {
                return 0;
            }
            return status == "degraded" ? 3 : 1;
        }

        if (command == L"agent") {
            if (subcommand != L"update" && subcommand != L"rebind") {
                print_usage();
                return 2;
            }
            const std::string vm_id = satsuma::path_to_utf8(
                require_option(options, L"vm"));
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
                satsuma::validate_ai_snapshot_deletion(vm->snapshots, existing, snapshot_name);
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
                metadata["status"] = "deleting";
                satsuma::write_json_atomic(metadata_path, metadata);
                try {
                    provider.delete_snapshot(vm->vmx, snapshot_name);
                } catch (const std::exception& error) {
                    metadata["status"] = "delete_failed";
                    metadata["error"] = error.what();
                    satsuma::write_json_atomic(metadata_path, metadata);
                    throw;
                }
                metadata["status"] = "deleted";
                metadata["deleted_at"] = satsuma::utc_timestamp();
                satsuma::write_json_atomic(metadata_path, metadata);
                std::cout << nlohmann::json({
                    {"status", "deleted"},
                    {"vm_id", vm_id},
                    {"snapshot", snapshot_name},
                }).dump(2) << '\n';
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
            try {
                provider.create_snapshot(vm->vmx, snapshot_name);
            } catch (const std::exception& error) {
                metadata["status"] = "failed";
                metadata["error"] = error.what();
                satsuma::write_json_atomic(metadata_path, metadata);
                throw;
            }
            metadata["status"] = "created";
            satsuma::write_json_atomic(metadata_path, metadata);
            std::cout << nlohmann::json({
                {"status", "created"},
                {"vm_id", vm_id},
                {"snapshot", snapshot_name},
            }).dump(2) << '\n';
            return 0;
        }

        if (command == L"orchestrate") {
            const std::filesystem::path plan_path = require_option(options, L"plan");
            const std::chrono::seconds timeout = parse_orchestration_timeout(options);
            satsuma::host::Orchestrator orchestrator(std::move(config));
            const nlohmann::json output = orchestrator.execute(plan_path, timeout);
            std::cout << output.dump(2) << '\n';
            const std::string status = output.at("status").get<std::string>();
            if (status == "COMPLETED") {
                return 0;
            }
            if (status == "RECOVERY_FAILED") {
                return 4;
            }
            return status == "MANUAL_INTERVENTION_REQUIRED" ? 5 : 1;
        }

        satsuma::host::Controller controller(std::move(config));

        if (command == L"run") {
            const std::filesystem::path plan_path = require_option(options, L"plan");
            const satsuma::RunManifest manifest = controller.create_run(plan_path);
            nlohmann::json output = {
                {"status", "prepared"},
                {"run_id", manifest.run_id},
                {"request_id", manifest.request_id},
            };
            std::cout << output.dump(2) << '\n';
            return 0;
        }
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
            if (wait_status == "manual_intervention_required") {
                return 5;
            }
            return wait_status == "timeout" ? 3 : 0;
        }

        print_usage();
        return 2;
    } catch (const std::exception& error) {
        std::cerr << "SatsumaHost error: " << error.what() << '\n';
        return 1;
    }
}
