// SatsumaVM 计划任务参数的无副作用单元测试。
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

#include "autostart.hpp"
#include "satsuma/core/id.hpp"
#include "satsuma/core/path.hpp"

namespace {

// 在条件不满足时终止当前测试。
void expect(const bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

// 创建计划任务输入文件并验证最终动作定义。
void test_autostart_spec(const std::filesystem::path& root) {
    const std::filesystem::path bin_directory = root / L"Agent Bin";
    const std::filesystem::path config_directory = root / L"Agent Config";
    std::filesystem::create_directories(bin_directory);
    std::filesystem::create_directories(config_directory);

    const std::filesystem::path executable = bin_directory / L"SatsumaVM.exe";
    const std::filesystem::path config = config_directory / L"agent.json";
    std::ofstream(executable, std::ios::binary) << "test executable";
    std::ofstream(config, std::ios::binary) << "{}";

    const satsuma::vm::AgentAutostartSpec spec =
        satsuma::vm::make_agent_autostart_spec(executable, config);
    const std::filesystem::path expected_executable = std::filesystem::canonical(executable);
    const std::filesystem::path expected_config = std::filesystem::canonical(config);
    expect(spec.executable == expected_executable, "autostart executable was not canonicalized");
    expect(spec.config == expected_config, "autostart config was not canonicalized");
    expect(
        spec.working_directory == expected_executable.parent_path(),
        "autostart working directory does not match the executable directory");
    expect(
        spec.arguments == L"--config \"" + expected_config.native() + L"\" --watch",
        "autostart arguments did not preserve a config path containing spaces");
}

}  // namespace

// 顺序运行无副作用测试并清理临时文件。
int main() {
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        satsuma::path_from_utf8(satsuma::make_id("satsuma autostart test"));
    try {
        test_autostart_spec(root);
        std::filesystem::remove_all(root);
        std::cout << "SatsumaVmAutostartTests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "SatsumaVmAutostartTests failed: " << error.what() << '\n';
        std::error_code cleanup_error;
        std::filesystem::remove_all(root, cleanup_error);
        return 1;
    }
}
