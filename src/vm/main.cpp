// SatsumaVM 命令行入口。
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

#include "agent.hpp"
#include "hardware_identity.hpp"
#include "interactive_process.hpp"
#include "service.hpp"
#include "update.hpp"
#include "satsuma/core/config.hpp"
#include "satsuma/core/errors.hpp"
#include "satsuma/core/id.hpp"
#include "satsuma/core/path.hpp"
#include "satsuma/core/version.hpp"

namespace {

// 输出 VM Agent 的首个增量用法。
void print_usage() {
    std::cout
        << "SatsumaVM " << satsuma::kVersion << '\n'
        << "Usage:\n"
        << "  SatsumaVM --help\n"
        << "  SatsumaVM --version [--json]\n"
        << "  SatsumaVM --config agent.json --once\n"
        << "  SatsumaVM --config agent.json --watch\n"
        << "  SatsumaVM --config agent.json --service\n"
        << "  SatsumaVM --config agent.json --validate-config\n"
        << "  SatsumaVM --config agent.json --install-service\n"
        << "  SatsumaVM --config agent.json --remove-service\n"
        << "  SatsumaVM --config agent.json --apply-update update-manifest.json\n";
}

// 将 Windows Service 变更转换为稳定机器文本。
[[nodiscard]] const char* service_change_name(const satsuma::vm::ServiceChange change) {
    switch (change) {
        case satsuma::vm::ServiceChange::Created:
            return "created";
        case satsuma::vm::ServiceChange::Updated:
            return "updated";
        case satsuma::vm::ServiceChange::Unchanged:
            return "unchanged";
        default:
            throw satsuma::Error("Unknown service change");
    }
}

// 尽力记录计划任务后台启动错误，供安装脚本回传。
void append_startup_error(
    const std::filesystem::path& config_path,
    const std::string& message) noexcept {
    try {
        const std::filesystem::path install_root = std::filesystem::absolute(config_path).parent_path();
        if (!std::filesystem::is_regular_file(install_root / L"bin" / L"SatsumaVM.exe")) {
            return;
        }
        const std::filesystem::path log_path = install_root / L"agent-startup-error.log";
        std::ofstream output(log_path, std::ios::binary | std::ios::app);
        if (output) {
            output << satsuma::utc_timestamp() << ' ' << message << '\n';
        }
    } catch (...) {
    }
}

}  // namespace

// 运行 VM Agent CLI 并把业务错误转换为稳定退出码。
int wmain(const int argc, wchar_t* argv[]) {
    std::filesystem::path config_path;
    std::wstring mode;
    try {
        if (argc == 3 && std::wstring(argv[1]) == L"--process-helper") {
            return satsuma::vm::run_interactive_process_helper(
                std::filesystem::path(argv[2]));
        }
        if ((argc == 2 || argc == 3) && std::wstring(argv[1]) == L"--version") {
            if (argc == 3 && std::wstring_view(argv[2]) != L"--json") {
                print_usage();
                return 2;
            }
            if (argc == 3) {
                std::cout << nlohmann::json({
                    {"component", "SatsumaVM"},
                    {"version", satsuma::kVersion},
                    {"build_number", satsuma::kBuildNumber},
                    {"build_attempt", satsuma::kBuildAttempt},
                    {"git_commit", satsuma::kGitCommit},
                }).dump() << '\n';
            } else {
                std::cout << satsuma::kVersion << '\n';
            }
            return 0;
        }
        if (argc == 2 && (std::wstring(argv[1]) == L"--help" || std::wstring(argv[1]) == L"help")) {
            print_usage();
            return 0;
        }
        if ((argc != 4 && argc != 5) || std::wstring(argv[1]) != L"--config") {
            print_usage();
            return 2;
        }

        config_path = std::filesystem::path(argv[2]);
        mode = argv[3];
        if (mode == L"--apply-update") {
            if (argc != 5) {
                print_usage();
                return 2;
            }
            return satsuma::vm::apply_agent_update_helper(
                config_path,
                std::filesystem::path(argv[4]));
        }
        if (argc != 4) {
            print_usage();
            return 2;
        }
        if (mode == L"--service") {
            const int service_result = satsuma::vm::run_agent_service_dispatcher(config_path);
            if (service_result != 0) {
                std::cerr << "SatsumaVM service dispatcher failed with Win32 error "
                          << service_result << '\n';
            }
            return service_result;
        }
        if (mode == L"--remove-service") {
            const bool removed = satsuma::vm::remove_agent_service(config_path);
            std::cout << nlohmann::json({
                {"status", removed ? "removed" : "absent"},
                {"service_name", "SatsumaVM"},
            }).dump() << '\n';
            return 0;
        }

        satsuma::AgentConfig config = satsuma::load_agent_config(config_path);
        if (mode == L"--validate-config") {
            nlohmann::json output = {
                {"status", "valid"},
                {"storage_root", satsuma::path_to_utf8(config.storage_root)},
            };
            output["vm_id"] = config.vm_id_configured
                ? nlohmann::json(config.vm_id)
                : nlohmann::json(nullptr);
            std::cout << output.dump() << '\n';
            return 0;
        }
        if (mode == L"--install-service") {
            const satsuma::vm::AgentServiceResult result =
                satsuma::vm::ensure_agent_service(config_path, config.local_work_root, true);
            std::cout << nlohmann::json({
                {"status", service_change_name(result.change)},
                {"service_name", result.service_name},
                {"start_requested", result.start_requested},
                {"process_id", result.process_id},
            }).dump() << '\n';
            return 0;
        }
        satsuma::vm::prepare_agent_hardware_identity(config);
        satsuma::vm::Agent agent(std::move(config));
        if (mode == L"--once") {
            const int executed = agent.run_once();
            std::cout << "{\"executed_steps\":" << executed << "}\n";
            return 0;
        }
        if (mode == L"--watch") {
            agent.run_watch();
            return 0;
        }

        print_usage();
        return 2;
    } catch (const std::exception& error) {
        if (mode == L"--watch" && !config_path.empty()) {
            append_startup_error(config_path, error.what());
        }
        std::cerr << "SatsumaVM error: " << error.what() << '\n';
        return 1;
    }
}
