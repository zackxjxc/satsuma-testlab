// SatsumaVM Windows Service 策略和状态机无副作用测试。
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include <windows.h>

#include <nlohmann/json.hpp>

#include "process_runner.hpp"
#include "service.hpp"
#include "satsuma/core/id.hpp"
#include "satsuma/core/path.hpp"

namespace {

// 在条件不满足时终止当前测试。
void expect(const bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

// 验证带空格路径、固定命令和基础恢复策略。
void test_service_spec(const std::filesystem::path& root) {
    const std::filesystem::path bin_directory = root / L"Agent Bin";
    const std::filesystem::path config_directory = root / L"Agent Config";
    std::filesystem::create_directories(bin_directory);
    std::filesystem::create_directories(config_directory);
    const std::filesystem::path executable = bin_directory / L"SatsumaVM.exe";
    const std::filesystem::path config = config_directory / L"agent.json";
    std::ofstream(executable, std::ios::binary) << "test executable";
    std::ofstream(config, std::ios::binary) << "{}";

    const satsuma::vm::AgentServiceSpec spec =
        satsuma::vm::make_agent_service_spec(executable, config);
    const std::filesystem::path expected_executable =
        std::filesystem::canonical(executable);
    const std::filesystem::path expected_config =
        std::filesystem::canonical(config);
    expect(spec.executable == expected_executable, "service executable was not canonicalized");
    expect(spec.config == expected_config, "service config was not canonicalized");
    expect(
        spec.working_directory == expected_executable.parent_path(),
        "service working directory does not match the executable directory");
    expect(
        spec.binary_path ==
            L"\"" + expected_executable.native() + L"\" --config \"" +
            expected_config.native() + L"\" --service",
        "service binary path was not fully quoted");
    expect(
        spec.restart_delays_ms ==
            std::array<std::uint32_t, 3>{5'000, 15'000, 60'000},
        "service restart delays changed");
    expect(spec.failure_reset_seconds == 86'400, "service failure reset period changed");
    expect(spec.delayed_auto_start, "service delayed auto-start was disabled");
    expect(spec.restart_on_non_crash, "service non-crash recovery was disabled");
    expect(
        satsuma::vm::agent_service_startup_log_path_for_test(root / L"agent.json") ==
            std::filesystem::absolute(root) / L"agent-startup-error.log",
        "service startup log ignored the configured install root");

    expect(
        satsuma::vm::agent_service_binary_belongs_for_test(spec.binary_path, spec),
        "service rejected its own command");
    const std::filesystem::path external = bin_directory / L"ExternalService.exe";
    std::ofstream(external, std::ios::binary) << "external executable";
    const std::wstring external_binary =
        L"\"" + external.native() + L"\" --config \"" +
        expected_config.native() + L"\" --service";
    expect(
        !satsuma::vm::agent_service_binary_belongs_for_test(external_binary, spec),
        "service accepted a different executable");
    const std::filesystem::path external_config = config_directory / L"external.json";
    std::ofstream(external_config, std::ios::binary) << "{}";
    const std::wstring wrong_config_binary =
        L"\"" + expected_executable.native() + L"\" --config \"" +
        external_config.native() + L"\" --service";
    expect(
        !satsuma::vm::agent_service_binary_belongs_for_test(wrong_config_binary, spec),
        "service accepted a different config");
    const std::wstring wrong_mode_binary =
        L"\"" + expected_executable.native() + L"\" --config \"" +
        expected_config.native() + L"\" --watch";
    expect(
        !satsuma::vm::agent_service_binary_belongs_for_test(wrong_mode_binary, spec),
        "service accepted watch mode as its ownership signature");
}

// 验证 START_PENDING 到 STOPPED 的正常和异常状态序列。
void test_service_status_sequences() {
    const std::vector<satsuma::vm::AgentServiceStatusSnapshot> normal =
        satsuma::vm::agent_service_status_sequence_for_test(false);
    expect(normal.size() == 4, "normal service status sequence length changed");
    expect(
        normal[0].state == SERVICE_START_PENDING &&
            normal[0].checkpoint == 1 &&
            normal[0].controls_accepted == 0,
        "service did not begin in START_PENDING");
    expect(
        normal[1].state == SERVICE_RUNNING &&
            normal[1].controls_accepted ==
                (SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN),
        "service RUNNING controls changed");
    expect(
        normal[2].state == SERVICE_STOP_PENDING &&
            normal[2].checkpoint == 1 &&
            normal[2].controls_accepted == 0,
        "service did not report STOP_PENDING");
    expect(
        normal[3].state == SERVICE_STOPPED &&
            normal[3].win32_exit_code == NO_ERROR,
        "service normal stop status changed");

    const std::vector<satsuma::vm::AgentServiceStatusSnapshot> failed =
        satsuma::vm::agent_service_status_sequence_for_test(true);
    expect(failed.size() == 3, "failed service status sequence length changed");
    expect(
        failed.back().state == SERVICE_STOPPED &&
            failed.back().win32_exit_code == ERROR_SERVICE_SPECIFIC_ERROR &&
            failed.back().service_exit_code != 0,
        "service failure did not report a nonzero STOPPED status");

    const std::vector<satsuma::vm::AgentServiceStatusSnapshot> registration_window =
        satsuma::vm::agent_service_registration_window_sequence_for_test();
    expect(
        registration_window.size() == 3 &&
            registration_window[0].state == SERVICE_START_PENDING &&
            registration_window[1].state == SERVICE_STOP_PENDING &&
            registration_window[2].state == SERVICE_STOPPED,
        "service registration-window STOP sequence changed");
    for (const auto& status : registration_window) {
        expect(status.state != 0, "service registration window exposed state zero");
    }
    expect(
        satsuma::vm::agent_service_final_status_order_for_test(),
        "service reported STOPPED before latching or while holding the mutex");
}

// 验证控制处理器只在 STOP 和 SHUTDOWN 时请求取消。
void test_service_controls() {
    for (const DWORD control : {SERVICE_CONTROL_STOP, SERVICE_CONTROL_SHUTDOWN}) {
        const satsuma::vm::AgentServiceControlTestResult result =
            satsuma::vm::agent_service_control_for_test(control);
        expect(result.handler_result == NO_ERROR, "service stop control was rejected");
        expect(result.stop_requested, "service stop control did not request cancellation");
        expect(
            result.states.size() == 2 &&
                result.states.back().state == SERVICE_STOP_PENDING,
            "service stop control did not report STOP_PENDING");
    }

    const satsuma::vm::AgentServiceControlTestResult interrogate =
        satsuma::vm::agent_service_control_for_test(SERVICE_CONTROL_INTERROGATE);
    expect(interrogate.handler_result == NO_ERROR, "service INTERROGATE was rejected");
    expect(!interrogate.stop_requested, "service INTERROGATE unexpectedly stopped Agent");
    expect(
        interrogate.states.size() == 2 &&
            interrogate.states.back().state == SERVICE_RUNNING,
        "service INTERROGATE did not repeat current status");

    const satsuma::vm::AgentServiceControlTestResult unsupported =
        satsuma::vm::agent_service_control_for_test(255);
    expect(
        unsupported.handler_result == ERROR_CALL_NOT_IMPLEMENTED &&
            !unsupported.stop_requested,
        "service accepted an unsupported control code");
    expect(
        satsuma::vm::agent_service_stop_survives_report_failure_for_test(),
        "service lost STOP when status reporting failed");
}

// 验证控制台不能绕过 SCM 直接进入 Service Agent。
void test_console_dispatcher_rejection(const std::filesystem::path& root) {
    const int result = satsuma::vm::run_agent_service_dispatcher(
        root / L"unused-agent.json");
    expect(
        result == static_cast<int>(ERROR_FAILED_SERVICE_CONTROLLER_CONNECT),
        "service dispatcher unexpectedly accepted a console process");
}

// 返回本机是否没有同名 Service，避免测试接触真实安装。
bool agent_service_is_absent() {
    const SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (manager == nullptr) {
        throw std::runtime_error(
            "cannot open SCM for absent service test: " +
            std::to_string(GetLastError()));
    }
    const SC_HANDLE service = OpenServiceW(manager, L"SatsumaVM", SERVICE_QUERY_STATUS);
    const DWORD open_error = service == nullptr ? GetLastError() : ERROR_SUCCESS;
    if (service != nullptr) {
        CloseServiceHandle(service);
    }
    CloseServiceHandle(manager);
    if (open_error == ERROR_SUCCESS) {
        return false;
    }
    if (open_error != ERROR_SERVICE_DOES_NOT_EXIST) {
        throw std::runtime_error(
            "cannot query SatsumaVM for absent service test: " +
            std::to_string(open_error));
    }
    return true;
}

// 验证 Service 不存在时卸载不解析配置且返回幂等结果。
void test_remove_service_absent_cli(
    const std::filesystem::path& root,
    const std::filesystem::path& vm_executable) {
    if (!agent_service_is_absent()) {
        return;
    }

    const std::filesystem::path invalid_config = root / L"invalid-agent.json";
    std::ofstream(invalid_config, std::ios::binary) << "{ invalid json";
    satsuma::vm::ProcessRequest request;
    request.program = vm_executable;
    request.arguments = {
        "--config",
        satsuma::path_to_utf8(invalid_config),
        "--remove-service",
    };
    request.working_directory = root;
    request.stdout_path = root / L"remove-service-stdout.log";
    request.stderr_path = root / L"remove-service-stderr.log";
    request.timeout = std::chrono::seconds(10);
    const satsuma::vm::ProcessResult result =
        satsuma::vm::ProcessRunner().run(request);
    expect(
        result.exit_code.has_value() && *result.exit_code == 0,
        "absent remove-service path failed");

    std::ifstream output(request.stdout_path, std::ios::binary);
    const nlohmann::json summary = nlohmann::json::parse(output);
    expect(
        summary.value("status", std::string{}) == "absent" &&
            summary.value("service_name", std::string{}) == "SatsumaVM",
        "remove-service absent output changed");
}

}  // namespace

// 顺序运行 Service 策略测试并清理临时目录。
int main(const int argc, char* argv[]) {
    if (argc != 2) {
        std::cerr << "Usage: SatsumaVmServiceTests <SatsumaVM.exe>\n";
        return 2;
    }
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        satsuma::path_from_utf8(satsuma::make_id("satsuma-service-test"));
    try {
        test_service_spec(root);
        test_service_status_sequences();
        test_service_controls();
        test_console_dispatcher_rejection(root);
        test_remove_service_absent_cli(
            root,
            std::filesystem::absolute(argv[1]));
        std::filesystem::remove_all(root);
        std::cout << "SatsumaVmServiceTests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "SatsumaVmServiceTests failed: " << error.what() << '\n';
        std::error_code cleanup_error;
        std::filesystem::remove_all(root, cleanup_error);
        return 1;
    }
}
