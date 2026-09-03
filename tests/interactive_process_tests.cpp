// 交互用户 Session helper、日志、超时和取消本机测试。
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

#include <windows.h>

#include "interactive_process.hpp"
#include "satsuma/core/errors.hpp"
#include "satsuma/core/id.hpp"
#include "satsuma/core/path.hpp"
#include "satsuma/core/sha256.hpp"

namespace {

// 在条件不满足时终止当前测试。
void expect(const bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

// 读取一个测试文本文件。
[[nodiscard]] std::string read_text(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>(),
    };
}

// 验证 helper 请求和结果文件已被清理。
void expect_helper_files_removed(const std::filesystem::path& workspace) {
    for (const auto& entry : std::filesystem::directory_iterator(workspace)) {
        expect(!entry.path().filename().native().starts_with(L".satsuma-process-helper"),
            "interactive helper retained a request or result file");
    }
}

// 保存一次目标进程树测试使用的路径和参数。
struct ProcessTreeProbe {
    std::filesystem::path target_pid;   // 目标进程 PID
    std::filesystem::path child_pid;    // 子进程 PID
    std::filesystem::path child_marker; // 未被终止时写入的延迟标记
    std::vector<std::string> arguments; // 传给测试夹具的参数
};

// 创建一个目标和子进程都会长时间运行的测试请求。
[[nodiscard]] ProcessTreeProbe make_process_tree_probe(
    const std::filesystem::path& root,
    const std::string& name) {
    ProcessTreeProbe probe;
    probe.target_pid = root / satsuma::path_from_utf8(name + "-target.pid");
    probe.child_pid = root / satsuma::path_from_utf8(name + "-child.pid");
    probe.child_marker = root / satsuma::path_from_utf8(name + "-child-survived.txt");
    probe.arguments = {
        "--pid-file",
        satsuma::path_to_utf8(probe.target_pid),
        "--child-pid-file",
        satsuma::path_to_utf8(probe.child_pid),
        "--child-marker",
        satsuma::path_to_utf8(probe.child_marker),
        "--child-delay-ms",
        "-1",
        "--sleep-ms",
        "5000",
    };
    return probe;
}

// 读取测试夹具输出的 PID。
[[nodiscard]] DWORD read_process_id(const std::filesystem::path& path) {
    expect(std::filesystem::is_regular_file(path), "fixture did not publish a process PID");
    return static_cast<DWORD>(std::stoul(read_text(path)));
}

// 判断指定 PID 是否已经退出。
[[nodiscard]] bool process_has_exited(const DWORD process_id) {
    HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, process_id);
    if (process == nullptr) {
        if (GetLastError() == ERROR_INVALID_PARAMETER) {
            return true;
        }
        throw std::runtime_error(
            "cannot open process " + std::to_string(process_id) +
            " for termination verification");
    }
    const DWORD wait_result = WaitForSingleObject(process, 0);
    CloseHandle(process);
    if (wait_result == WAIT_OBJECT_0) {
        return true;
    }
    if (wait_result == WAIT_TIMEOUT) {
        return false;
    }
    throw std::runtime_error("cannot query process termination state");
}

// 验证 helper、目标和子进程均退出，且子进程未活到延迟标记时间。
void expect_process_tree_stopped(
    const ProcessTreeProbe& probe,
    const DWORD helper_pid,
    const bool require_target_tree) {
    expect(helper_pid != 0 && process_has_exited(helper_pid),
        "interactive helper survived Job termination");
    const bool target_started = std::filesystem::is_regular_file(probe.target_pid);
    const bool child_started = std::filesystem::is_regular_file(probe.child_pid);
    if (require_target_tree) {
        expect(target_started && child_started,
            "interactive target tree did not publish both process IDs");
    }
    if (target_started) {
        expect(process_has_exited(read_process_id(probe.target_pid)),
            "interactive target survived Job termination");
    }
    if (child_started) {
        expect(process_has_exited(read_process_id(probe.child_pid)),
            "interactive target child survived Job termination");
    }
}

// 清理上次异常中断留下的专用交互测试工作目录。
void cleanup_abandoned_test_workspaces() {
    satsuma::vm::InteractiveUserSession cleanup =
        satsuma::vm::InteractiveUserSession::acquire(
            "interactive_test",
            "cleanup_probe");
    std::error_code cleanup_error;
    std::filesystem::remove_all(
        cleanup.working_directory().parent_path(),
        cleanup_error);
}

// 运行真实当前控制台用户的参数、日志和 Session 测试。
void test_interactive_execution(
    const std::filesystem::path& helper,
    const std::filesystem::path& fixture,
    const std::filesystem::path& root) {
    satsuma::vm::InteractiveUserSession session =
        satsuma::vm::InteractiveUserSession::acquire(
            "interactive_test",
            satsuma::make_id("run"));
    expect(session.session_id() != 0xFFFFFFFF,
        "interactive session retained the invalid WTS sentinel");
    expect(!session.user_sid().empty(), "interactive session did not record a user SID");

    const std::filesystem::path deployed = session.deploy_file(
        fixture,
        L"bin/SatsumaTestFixture.exe");
    expect(satsuma::sha256_file(deployed) == satsuma::sha256_file(fixture),
        "interactive Artifact deployment changed the fixture");
    const std::filesystem::path output =
        session.working_directory() / L"collected/result.json";
    const std::filesystem::path session_file =
        session.working_directory() / L"collected/session.txt";
    const std::filesystem::path identity_file =
        session.working_directory() / L"collected/identity.txt";

    satsuma::vm::ProcessRequest request;
    request.program = deployed;
    request.arguments = {
        "--message",
        "interactive argument with spaces",
        "--output",
        satsuma::path_to_utf8(output),
        "--session-file",
        satsuma::path_to_utf8(session_file),
        "--identity-file",
        satsuma::path_to_utf8(identity_file),
    };
    request.working_directory = session.working_directory();
    request.stdout_path = root / L"stdout.log";
    request.stderr_path = root / L"stderr.log";
    request.timeout = std::chrono::seconds(10);
    const satsuma::vm::ProcessResult result = session.run(helper, request);
    expect(result.exit_code == 0 && !result.timed_out,
        "interactive fixture did not exit successfully");
    const std::string stdout_text = read_text(request.stdout_path);
    expect(stdout_text == "interactive argument with spaces\r\n" ||
           stdout_text == "interactive argument with spaces\n",
        "interactive stdout or argument boundaries changed");
    expect(read_text(request.stderr_path).empty(),
        "interactive fixture wrote unexpected stderr");
    expect(std::filesystem::is_regular_file(output),
        "interactive fixture did not create its collected file");
    expect(std::stoul(read_text(session_file)) == session.session_id(),
        "interactive target ran in the wrong Windows Session");
    expect(read_text(identity_file) == "user\r\n" ||
           read_text(identity_file) == "user\n",
        "interactive target unexpectedly ran as LocalSystem");
    expect_helper_files_removed(session.working_directory());

    const ProcessTreeProbe timeout_probe = make_process_tree_probe(root, "timeout");
    request.arguments = timeout_probe.arguments;
    request.stdout_path = root / L"timeout-stdout.log";
    request.stderr_path = root / L"timeout-stderr.log";
    request.timeout = std::chrono::milliseconds(750);
    const satsuma::vm::ProcessResult timeout = session.run(helper, request);
    expect(timeout.timed_out && !timeout.exit_code.has_value(),
        "interactive timeout did not terminate the Job tree");
    expect_process_tree_stopped(
        timeout_probe,
        satsuma::vm::last_interactive_helper_pid_for_test(),
        false);
    expect_helper_files_removed(session.working_directory());

    const ProcessTreeProbe stop_probe = make_process_tree_probe(root, "stop");
    request.arguments = stop_probe.arguments;
    std::stop_source stop_source;
    request.stdout_path = root / L"stop-stdout.log";
    request.stderr_path = root / L"stop-stderr.log";
    request.timeout = std::chrono::seconds(5);
    request.stop_token = stop_source.get_token();
    std::jthread stopper([&stop_source, &stop_probe] {
        for (int attempt = 0; attempt < 250; ++attempt) {
            if (std::filesystem::is_regular_file(stop_probe.target_pid) &&
                std::filesystem::is_regular_file(stop_probe.child_pid)) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        stop_source.request_stop();
    });
    bool stop_reported = false;
    try {
        static_cast<void>(session.run(helper, request));
    } catch (const satsuma::Error& error) {
        stop_reported = std::string(error.what()) == "Agent stop requested";
    }
    expect(stop_reported, "interactive stop did not return the stable Agent error");
    expect_process_tree_stopped(
        stop_probe,
        satsuma::vm::last_interactive_helper_pid_for_test(),
        true);
    expect_helper_files_removed(session.working_directory());

    request.arguments = {"--sleep-ms", "1000"};
    request.stdout_path = root / L"identity-change-stdout.log";
    request.stderr_path = root / L"identity-change-stderr.log";
    request.stop_token = {};
    satsuma::vm::set_interactive_resume_identity_changed_for_test(true);
    bool resume_identity_change_rejected = false;
    try {
        static_cast<void>(session.run(helper, request));
    } catch (const satsuma::Error& error) {
        resume_identity_change_rejected = std::string(error.what()) ==
            "Interactive user identity changed before process launch";
    }
    satsuma::vm::set_interactive_resume_identity_changed_for_test(false);
    expect(resume_identity_change_rejected,
        "resume-time identity change did not block the suspended helper");
    expect(process_has_exited(
            satsuma::vm::last_interactive_helper_pid_for_test()),
        "suspended helper survived the resume-time identity rejection");
    expect_helper_files_removed(session.working_directory());

    request.stop_token = {};
    satsuma::vm::set_interactive_identity_changed_for_test(true);
    bool identity_change_rejected = false;
    try {
        static_cast<void>(session.run(helper, request));
    } catch (const satsuma::Error& error) {
        identity_change_rejected = std::string(error.what()) ==
            "Interactive user identity changed before process launch";
    }
    satsuma::vm::set_interactive_identity_changed_for_test(false);
    expect(identity_change_rejected,
        "interactive identity change did not block process launch");
    expect_helper_files_removed(session.working_directory());
    std::filesystem::remove_all(session.working_directory());
}

// 验证无活动 Session 的稳定错误。
void test_no_active_session_error() {
    try {
        satsuma::vm::validate_interactive_session_id_for_test(0xFFFFFFFF);
    } catch (const satsuma::Error& error) {
        expect(std::string(error.what()) ==
               satsuma::vm::kNoInteractiveUserSessionError,
            "no-session error text changed");
        return;
    }
    throw std::runtime_error("invalid WTS Session sentinel was accepted");
}

// 验证生产身份门禁不会允许普通交互用户调用方。
void test_production_caller_policy() {
    bool rejected = false;
    try {
        satsuma::vm::validate_interactive_caller_for_test(false);
    } catch (const satsuma::Error& error) {
        rejected = std::string(error.what()) ==
            "Interactive user execution requires LocalSystem";
    }
    expect(rejected, "production interactive caller policy accepted a non-SYSTEM caller");
    satsuma::vm::validate_interactive_caller_for_test(true);
}

}  // namespace

// 运行本机交互 Session 测试，非桌面环境使用 CTest skip 返回码。
int main(const int argc, char* argv[]) {
    if (argc != 3) {
        std::cerr << "Expected SatsumaVM and fixture executable paths\n";
        return 1;
    }
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        satsuma::path_from_utf8(satsuma::make_id("satsuma-interactive-test"));
    try {
        test_no_active_session_error();
        test_production_caller_policy();
        cleanup_abandoned_test_workspaces();
        std::filesystem::create_directories(root);
        test_interactive_execution(argv[1], argv[2], root);
        std::filesystem::remove_all(root);
        std::cout << "SatsumaVmInteractiveTests passed\n";
        return 0;
    } catch (const satsuma::Error& error) {
        std::error_code cleanup_error;
        std::filesystem::remove_all(root, cleanup_error);
        if (std::string(error.what()) ==
            satsuma::vm::kNoInteractiveUserSessionError) {
            std::cout << "SatsumaVmInteractiveTests skipped: " << error.what() << '\n';
            return 77;
        }
        std::cerr << "SatsumaVmInteractiveTests failed: " << error.what() << '\n';
        return 1;
    } catch (const std::exception& error) {
        std::error_code cleanup_error;
        std::filesystem::remove_all(root, cleanup_error);
        std::cerr << "SatsumaVmInteractiveTests failed: " << error.what() << '\n';
        return 1;
    }
}
