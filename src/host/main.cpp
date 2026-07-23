// SatsumaHost 命令行入口。
#include <filesystem>
#include <iostream>
#include <map>
#include <string>

#include "controller.hpp"
#include "rpc_server.hpp"
#include "satsuma/core/config.hpp"
#include "satsuma/core/errors.hpp"
#include "satsuma/core/json_io.hpp"
#include "satsuma/core/path.hpp"
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

// 输出当前首个增量支持的 CLI 用法。
void print_usage() {
    std::cout
        << "SatsumaHost 0.1.0\n"
        << "Usage:\n"
        << "  SatsumaHost serve --config lab.json\n"
        << "  SatsumaHost vm start --id <vm-id> [--config lab.json]\n"
        << "  SatsumaHost vm stop --id <vm-id> [--mode soft|hard] [--config lab.json]\n"
        << "  SatsumaHost vm restore --id <vm-id> --snapshot <name> [--config lab.json]\n"
        << "  SatsumaHost run --config lab.json --plan task.json\n"
        << "  SatsumaHost report --config lab.json --run <run-id>\n";
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
        std::wstring vm_command;
        int options_start = 2;
        if (command == L"vm") {
            if (argc < 3) {
                print_usage();
                return 2;
            }
            vm_command = argv[2];
            options_start = 3;
        }

        const auto options = parse_options(argc, argv, options_start);
        const std::filesystem::path config_path =
            command == L"vm" && !options.contains(L"config")
                ? std::filesystem::path(L"lab.json")
                : std::filesystem::path(require_option(options, L"config"));
        satsuma::LabConfig config = satsuma::load_lab_config(config_path);

        if (command == L"serve") {
            satsuma::host::RpcServer server(std::move(config));
            server.start();
            return 0;
        }

        if (command == L"vm") {
            if (vm_command != L"start" && vm_command != L"stop" && vm_command != L"restore") {
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
            if (vm_command == L"start") {
                provider.start(vm->vmx);
                status = "started";
            } else if (vm_command == L"stop") {
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
            std::cout << controller.build_report(run_id).dump(2) << '\n';
            return 0;
        }

        print_usage();
        return 2;
    } catch (const std::exception& error) {
        std::cerr << "SatsumaHost error: " << error.what() << '\n';
        return 1;
    }
}
