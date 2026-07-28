// Host/VM 集成测试使用的无害被测程序。
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <windows.h>
#include <tlhelp32.h>

#include "satsuma/core/json_io.hpp"
#include "satsuma/core/path.hpp"
#include "satsuma/core/windows_command_line.hpp"

namespace {

// 返回当前进程 Token 是否属于 LocalSystem。
[[nodiscard]] bool current_user_is_local_system() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        throw std::runtime_error("failed to open fixture process Token");
    }
    DWORD size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    std::vector<unsigned char> buffer(size);
    if (size == 0 || !GetTokenInformation(
            token,
            TokenUser,
            buffer.data(),
            static_cast<DWORD>(buffer.size()),
            &size)) {
        CloseHandle(token);
        throw std::runtime_error("failed to query fixture Token user");
    }
    CloseHandle(token);

    DWORD system_sid_size = SECURITY_MAX_SID_SIZE;
    std::vector<unsigned char> system_sid(system_sid_size);
    if (!CreateWellKnownSid(
            WinLocalSystemSid,
            nullptr,
            system_sid.data(),
            &system_sid_size)) {
        throw std::runtime_error("failed to create LocalSystem SID");
    }
    const auto* user = reinterpret_cast<const TOKEN_USER*>(buffer.data());
    return EqualSid(user->User.Sid, system_sid.data()) != FALSE;
}

// 写入测试进程 PID 或延迟标记。
void write_text_file(
    const std::filesystem::path& path,
    const std::string& content) {
    if (!path.parent_path().empty()) {
        std::filesystem::create_directories(path.parent_path());
    }
    std::ofstream output(path, std::ios::binary);
    output << content;
    if (!output) {
        throw std::runtime_error("failed to write fixture process marker");
    }
}

// 返回当前进程的父进程 ID，并验证快照中的进程身份。
[[nodiscard]] DWORD satsuma_agent_parent_process_id() {
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        throw std::runtime_error("failed to snapshot fixture parent process");
    }

    DWORD parent_process_id = 0; // 当前 Fixture 的直接父进程
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (entry.th32ProcessID == GetCurrentProcessId()) {
                parent_process_id = entry.th32ParentProcessID;
                break;
            }
        } while (Process32NextW(snapshot, &entry));
    }
    if (parent_process_id == 0) {
        CloseHandle(snapshot);
        throw std::runtime_error("fixture could not resolve its parent process");
    }

    bool parent_name_matches = false; // 防止误终止非 Satsuma 父进程
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (entry.th32ProcessID == parent_process_id) {
                parent_name_matches = _wcsicmp(entry.szExeFile, L"SatsumaVM.exe") == 0;
                break;
            }
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    if (!parent_name_matches) {
        throw std::runtime_error("fixture parent is not SatsumaVM.exe");
    }
    return parent_process_id;
}

// 使用独占标记保证同一工作目录只强杀一次父 Agent。
[[noreturn]] void terminate_satsuma_parent_once(
    const std::filesystem::path& marker_path) {
    const DWORD parent_process_id = satsuma_agent_parent_process_id();
    const HANDLE parent = OpenProcess(
        PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE | SYNCHRONIZE,
        FALSE,
        parent_process_id);
    if (parent == nullptr) {
        throw std::runtime_error("fixture could not open its SatsumaVM parent");
    }

    std::wstring parent_path(32'768, L'\0'); // 再次核对 PID 当前对应的可执行文件
    DWORD parent_path_size = static_cast<DWORD>(parent_path.size());
    if (!QueryFullProcessImageNameW(
            parent,
            0,
            parent_path.data(),
            &parent_path_size)) {
        CloseHandle(parent);
        throw std::runtime_error("fixture could not query its SatsumaVM parent path");
    }
    parent_path.resize(parent_path_size);
    if (_wcsicmp(
            std::filesystem::path(parent_path).filename().c_str(),
            L"SatsumaVM.exe") != 0) {
        CloseHandle(parent);
        throw std::runtime_error("fixture parent PID changed before termination");
    }

    if (!marker_path.parent_path().empty()) {
        std::filesystem::create_directories(marker_path.parent_path());
    }
    const HANDLE marker = CreateFileW(
        marker_path.c_str(),
        GENERIC_WRITE,
        FILE_SHARE_READ,
        nullptr,
        CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH,
        nullptr);
    if (marker == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        CloseHandle(parent);
        if (error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS) {
            throw std::runtime_error("fixture crash marker appeared during arming");
        }
        throw std::runtime_error("fixture could not create its crash marker");
    }
    const std::string marker_text =
        "parent_process_id=" + std::to_string(parent_process_id) + "\n";
    DWORD written = 0;
    const BOOL marker_written = WriteFile(
        marker,
        marker_text.data(),
        static_cast<DWORD>(marker_text.size()),
        &written,
        nullptr);
    const BOOL marker_flushed = marker_written ? FlushFileBuffers(marker) : FALSE;
    CloseHandle(marker);
    if (!marker_written || !marker_flushed || written != marker_text.size()) {
        CloseHandle(parent);
        throw std::runtime_error("fixture could not persist its crash marker");
    }

    if (!TerminateProcess(parent, ERROR_PROCESS_ABORTED)) {
        CloseHandle(parent);
        throw std::runtime_error("fixture could not terminate its SatsumaVM parent");
    }
    const DWORD wait_result = WaitForSingleObject(parent, 10'000);
    CloseHandle(parent);
    if (wait_result != WAIT_OBJECT_0) {
        throw std::runtime_error("fixture SatsumaVM parent did not exit after termination");
    }

    // Agent 已退出，当前进程也必须立即结束，不能留下脱离 Job 的测试进程。
    TerminateProcess(GetCurrentProcess(), ERROR_PROCESS_ABORTED);
    std::abort();
}

// 在可选独占锁下连续替换同一 JSON 文件，复现实机共享目录原子写问题。
void run_atomic_json_probe(
    const std::filesystem::path& target_path,
    const std::filesystem::path& lock_path) {
    HANDLE lock = nullptr; // 探针执行期间保持的可选独占锁
    if (!lock_path.empty()) {
        if (!lock_path.parent_path().empty()) {
            std::filesystem::create_directories(lock_path.parent_path());
        }
        lock = CreateFileW(
            lock_path.c_str(),
            GENERIC_READ | GENERIC_WRITE,
            0,
            nullptr,
            OPEN_ALWAYS,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH,
            nullptr);
        if (lock == INVALID_HANDLE_VALUE) {
            throw std::runtime_error(
                "failed to open fixture probe lock: " +
                std::to_string(GetLastError()));
        }
    }

    try {
        satsuma::write_json_atomic(target_path, {{"sequence", 1}});
        if (satsuma::load_json(target_path).value("sequence", 0) != 1) {
            throw std::runtime_error("fixture atomic JSON probe read back an invalid value");
        }
        satsuma::write_json_atomic(target_path, {{"sequence", 2}});
        if (satsuma::load_json(target_path).value("sequence", 0) != 2) {
            throw std::runtime_error("fixture atomic JSON probe did not publish its final value");
        }
    } catch (...) {
        if (lock != nullptr) {
            CloseHandle(lock);
        }
        throw;
    }
    if (lock != nullptr) {
        CloseHandle(lock);
    }
}

// 创建一个继承当前 Job 的子进程，供进程树终止测试使用。
void spawn_child_process(
    const std::filesystem::path& executable,
    const std::filesystem::path& pid_path,
    const std::filesystem::path& marker_path,
    const int delay_ms) {
    std::vector<wchar_t> command = satsuma::build_windows_command_line(
        executable,
        {
            "--child-mode",
            "--child-marker",
            satsuma::path_to_utf8(marker_path),
            "--child-delay-ms",
            std::to_string(delay_ms),
        });
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(
            executable.c_str(),
            command.data(),
            nullptr,
            nullptr,
            FALSE,
            CREATE_NO_WINDOW,
            nullptr,
            nullptr,
            &startup,
            &process)) {
        throw std::runtime_error(
            "failed to create fixture child process: " +
            std::to_string(GetLastError()));
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    write_text_file(pid_path, std::to_string(process.dwProcessId) + "\n");
}

}  // namespace

// 解析测试参数、输出日志并生成声明的结果文件。
int main(const int argc, char* argv[]) {
    try {
        std::string message = "fixture-ok";
        std::filesystem::path output_path;
        std::filesystem::path ready_path;  // 进程进入测试主体后的同步标记
        std::filesystem::path rename_self_path;  // 更新助手自重命名探针目标
        std::filesystem::path replace_backup_path;  // 完整更新探针使用的旧版备份
        std::filesystem::path session_path;  // 目标进程 Session ID 输出
        std::filesystem::path identity_path;  // 目标进程 Token 身份输出
        std::filesystem::path pid_path;  // 当前测试进程 PID 输出
        std::filesystem::path child_pid_path;  // 子进程 PID 输出
        std::filesystem::path child_marker_path;  // 子进程延迟标记
        std::filesystem::path atomic_json_path; // 连续原子写探针目标
        std::filesystem::path atomic_json_lock_path; // 连续原子写探针锁文件
        std::filesystem::path print_json_path; // 只读输出的 JSON 路径
        std::filesystem::path crash_parent_once_path; // 首次运行时强杀父 Agent 的本地标记
        bool child_mode = false;  // 是否作为进程树测试的子进程运行
        int child_delay_ms = 0;
        int sleep_ms = 0;
        int stdout_bytes = 0; // 输出容量门禁测试使用的字节数
        for (int index = 1; index < argc; ++index) {
            const std::string argument = argv[index];
            if (argument == "--message" && index + 1 < argc) {
                message = argv[++index];
            } else if (argument == "--output" && index + 1 < argc) {
                output_path = argv[++index];
            } else if (argument == "--ready-file" && index + 1 < argc) {
                ready_path = argv[++index];
            } else if (argument == "--sleep-ms" && index + 1 < argc) {
                sleep_ms = std::stoi(argv[++index]);
            } else if (argument == "--stdout-bytes" && index + 1 < argc) {
                stdout_bytes = std::stoi(argv[++index]);
            } else if (argument == "--rename-self" && index + 1 < argc) {
                rename_self_path = argv[++index];
            } else if (argument == "--replace-self" && index + 2 < argc) {
                rename_self_path = argv[++index];
                replace_backup_path = argv[++index];
            } else if (argument == "--session-file" && index + 1 < argc) {
                session_path = argv[++index];
            } else if (argument == "--identity-file" && index + 1 < argc) {
                identity_path = argv[++index];
            } else if (argument == "--pid-file" && index + 1 < argc) {
                pid_path = argv[++index];
            } else if (argument == "--child-pid-file" && index + 1 < argc) {
                child_pid_path = argv[++index];
            } else if (argument == "--child-marker" && index + 1 < argc) {
                child_marker_path = argv[++index];
            } else if (argument == "--child-delay-ms" && index + 1 < argc) {
                child_delay_ms = std::stoi(argv[++index]);
            } else if (argument == "--atomic-json-probe" && index + 1 < argc) {
                atomic_json_path = argv[++index];
            } else if (argument == "--atomic-json-lock" && index + 1 < argc) {
                atomic_json_lock_path = argv[++index];
            } else if (argument == "--print-json" && index + 1 < argc) {
                print_json_path = argv[++index];
            } else if (argument == "--crash-parent-once" && index + 1 < argc) {
                crash_parent_once_path = satsuma::path_from_utf8(argv[++index]);
            } else if (argument == "--child-mode") {
                child_mode = true;
            } else {
                throw std::runtime_error("unknown fixture argument: " + argument);
            }
        }

        if (child_mode) {
            if (child_marker_path.empty() || child_delay_ms <= 0) {
                throw std::runtime_error("fixture child mode requires marker and delay");
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(child_delay_ms));
            write_text_file(child_marker_path, "child-survived\n");
            return 0;
        }

        if (!pid_path.empty()) {
            write_text_file(pid_path, std::to_string(GetCurrentProcessId()) + "\n");
        }
        if (!atomic_json_lock_path.empty() && atomic_json_path.empty()) {
            throw std::runtime_error("fixture atomic JSON lock requires a probe target");
        }
        if (!atomic_json_path.empty()) {
            run_atomic_json_probe(atomic_json_path, atomic_json_lock_path);
        }
        if (!print_json_path.empty()) {
            std::cout << satsuma::load_json(print_json_path).dump(2) << '\n';
            return 0;
        }
        if (!child_pid_path.empty() || !child_marker_path.empty() || child_delay_ms > 0) {
            if (child_pid_path.empty() || child_marker_path.empty() || child_delay_ms <= 0) {
                throw std::runtime_error(
                    "fixture child process requires PID path, marker, and delay");
            }
            spawn_child_process(
                std::filesystem::absolute(argv[0]),
                child_pid_path,
                child_marker_path,
                child_delay_ms);
        }

        if (!ready_path.empty()) {
            if (!ready_path.parent_path().empty()) {
                std::filesystem::create_directories(ready_path.parent_path());
            }
            std::ofstream ready(ready_path, std::ios::binary);
            ready << "ready\n";
            if (!ready) {
                throw std::runtime_error("failed to write fixture ready marker");
            }
        }
        bool crash_parent_pending = false; // attempt 2 看到标记后直接正常完成
        if (!crash_parent_once_path.empty()) {
            satsuma::validate_relative_path(crash_parent_once_path);
            if (std::filesystem::exists(crash_parent_once_path)) {
                if (!std::filesystem::is_regular_file(crash_parent_once_path)) {
                    throw std::runtime_error("fixture crash marker is not a regular file");
                }
            } else {
                crash_parent_pending = true;
            }
        }
        if (sleep_ms > 0 && (crash_parent_once_path.empty() || crash_parent_pending)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));
        }
        if (crash_parent_pending) {
            terminate_satsuma_parent_once(crash_parent_once_path);
        }
        if (!rename_self_path.empty()) {
            if (!replace_backup_path.empty()) {
                std::filesystem::rename(
                    rename_self_path,
                    replace_backup_path);
            }
            std::filesystem::rename(
                std::filesystem::absolute(argv[0]),
                rename_self_path);
            if (!replace_backup_path.empty()) {
                std::filesystem::remove(replace_backup_path);
            }
        }
        if (stdout_bytes < 0 || stdout_bytes > 16 * 1024 * 1024) {
            throw std::runtime_error("fixture stdout byte count is outside the test limit");
        }
        if (stdout_bytes > 0) {
            const std::string chunk(4096, 'x');
            for (int written = 0; written < stdout_bytes; written += static_cast<int>(chunk.size())) {
                std::cout.write(chunk.data(), std::min<int>(
                    static_cast<int>(chunk.size()), stdout_bytes - written));
            }
            std::cout.flush();
        }
        std::cout << message << '\n';
        if (!session_path.empty()) {
            DWORD session_id = 0;
            if (!ProcessIdToSessionId(GetCurrentProcessId(), &session_id)) {
                throw std::runtime_error("failed to query fixture Session ID");
            }
            std::filesystem::create_directories(session_path.parent_path());
            std::ofstream session_output(session_path, std::ios::binary);
            session_output << session_id << '\n';
            if (!session_output) {
                throw std::runtime_error("failed to write fixture Session ID");
            }
        }
        if (!identity_path.empty()) {
            std::filesystem::create_directories(identity_path.parent_path());
            std::ofstream identity_output(identity_path, std::ios::binary);
            identity_output << (current_user_is_local_system() ? "system\n" : "user\n");
            if (!identity_output) {
                throw std::runtime_error("failed to write fixture Token identity");
            }
        }
        if (!output_path.empty()) {
            std::filesystem::create_directories(output_path.parent_path());
            std::ofstream output(output_path, std::ios::binary);
            output << "{\"status\":\"passed\",\"message\":\"" << message << "\"}\n";
            if (!output) {
                throw std::runtime_error("failed to write fixture output");
            }
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "SatsumaTestFixture error: " << error.what() << '\n';
        return 1;
    }
}
