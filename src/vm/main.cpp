// SatsumaVM 命令行入口。
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

#include "agent.hpp"
#include "autostart.hpp"
#include "satsuma/core/config.hpp"
#include "satsuma/core/errors.hpp"
#include "satsuma/core/id.hpp"
#include "satsuma/core/path.hpp"

namespace {

// 输出 VM Agent 的首个增量用法。
void print_usage() {
    std::cout
        << "SatsumaVM 0.1.0\n"
        << "Usage:\n"
        << "  SatsumaVM --config agent.json --once\n"
        << "  SatsumaVM --config agent.json --rpc-once\n"
        << "  SatsumaVM --config agent.json --watch\n"
        << "  SatsumaVM --config agent.json --validate-config\n"
        << "  SatsumaVM --config agent.json --install-autostart\n";
}

// 将计划任务变更转换为稳定机器文本。
[[nodiscard]] const char* autostart_change_name(const satsuma::vm::AutostartChange change) {
    switch (change) {
        case satsuma::vm::AutostartChange::Created:
            return "created";
        case satsuma::vm::AutostartChange::Updated:
            return "updated";
        case satsuma::vm::AutostartChange::Unchanged:
            return "unchanged";
        default:
            throw satsuma::Error("Unknown autostart change");
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
        if (argc != 4 || std::wstring(argv[1]) != L"--config") {
            print_usage();
            return 2;
        }

        config_path = std::filesystem::path(argv[2]);
        mode = argv[3];
        satsuma::AgentConfig config = satsuma::load_agent_config(config_path);
        if (mode == L"--validate-config") {
            std::cout << nlohmann::json({
                {"status", "valid"},
                {"vm_id", config.vm_id},
            }).dump() << '\n';
            return 0;
        }
        if (mode == L"--install-autostart") {
            const satsuma::vm::AgentAutostartResult result =
                satsuma::vm::ensure_agent_autostart(config_path, config.local_work_root, true);
            std::cout << nlohmann::json({
                {"status", autostart_change_name(result.change)},
                {"task_path", result.task_path},
                {"start_requested", result.start_requested},
                {"engine_process_id", result.engine_process_id},
            }).dump() << '\n';
            return 0;
        }

        satsuma::vm::Agent agent(std::move(config));
        if (mode == L"--once") {
            const int executed = agent.run_once();
            std::cout << "{\"executed_steps\":" << executed << "}\n";
            return 0;
        }
        if (mode == L"--rpc-once") {
            const bool has_task = agent.synchronize_rpc();
            std::cout << "{\"rpc_connected\":true,\"has_task\":"
                      << (has_task ? "true" : "false") << "}\n";
            return 0;
        }
        if (mode == L"--watch") {
            const satsuma::vm::AgentAutostartResult result =
                satsuma::vm::ensure_agent_autostart(config_path, config.local_work_root, false);
            std::cerr << "SatsumaVM autostart " << autostart_change_name(result.change)
                      << ": " << result.task_path << '\n';
            agent.run_watch();
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
