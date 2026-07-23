// Windows Job Object 进程树执行实现。
#include "process_runner.hpp"

#include <algorithm>
#include <limits>
#include <string>
#include <vector>

#include <windows.h>

#include "satsuma/core/errors.hpp"
#include "satsuma/core/path.hpp"

namespace satsuma::vm {
namespace {

// 负责自动关闭 Win32 HANDLE。
class UniqueHandle {
public:
    // 接管一个可空 Win32 HANDLE。
    explicit UniqueHandle(HANDLE handle = nullptr) : handle_(handle) {}

    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;

    // 支持在创建进程后转移 HANDLE 所有权。
    UniqueHandle(UniqueHandle&& other) noexcept : handle_(other.release()) {}

    // 关闭当前 HANDLE 后接管新值。
    UniqueHandle& operator=(UniqueHandle&& other) noexcept {
        if (this != &other) {
            reset(other.release());
        }
        return *this;
    }

    // 离开作用域时关闭有效 HANDLE。
    ~UniqueHandle() {
        reset();
    }

    // 返回底层 HANDLE。
    [[nodiscard]] HANDLE get() const noexcept {
        return handle_;
    }

    // 返回 HANDLE 是否有效。
    [[nodiscard]] explicit operator bool() const noexcept {
        return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE;
    }

    // 放弃所有权但不关闭 HANDLE。
    [[nodiscard]] HANDLE release() noexcept {
        const HANDLE value = handle_;
        handle_ = nullptr;
        return value;
    }

    // 关闭旧 HANDLE 并保存新值。
    void reset(HANDLE handle = nullptr) noexcept {
        if (handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(handle_);
        }
        handle_ = handle;
    }

private:
    HANDLE handle_;  // 被管理的 Win32 HANDLE
};

// 按 CommandLineToArgvW 规则引用单个 Windows 参数。
[[nodiscard]] std::wstring quote_argument(const std::wstring& argument) {
    if (!argument.empty() && argument.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
        return argument;
    }

    std::wstring quoted = L"\"";
    std::size_t backslashes = 0;
    for (const wchar_t value : argument) {
        if (value == L'\\') {
            ++backslashes;
            continue;
        }
        if (value == L'\"') {
            quoted.append(backslashes * 2 + 1, L'\\');
            quoted.push_back(L'\"');
            backslashes = 0;
            continue;
        }
        quoted.append(backslashes, L'\\');
        backslashes = 0;
        quoted.push_back(value);
    }
    quoted.append(backslashes * 2, L'\\');
    quoted.push_back(L'\"');
    return quoted;
}

// 生成 CreateProcessW 所需的可修改命令行缓冲区。
[[nodiscard]] std::vector<wchar_t> build_command_line(const ProcessRequest& request) {
    std::wstring command = quote_argument(request.program.native());
    for (const auto& argument : request.arguments) {
        command.push_back(L' ');
        command += quote_argument(path_from_utf8(argument).native());
    }
    std::vector<wchar_t> buffer(command.begin(), command.end());
    buffer.push_back(L'\0');
    return buffer;
}

// 打开允许 Host 实时读取的共享日志文件。
[[nodiscard]] UniqueHandle open_log(const std::filesystem::path& path) {
    std::filesystem::create_directories(path.parent_path());
    SECURITY_ATTRIBUTES security{};
    security.nLength = sizeof(security);
    security.bInheritHandle = TRUE;
    UniqueHandle handle(CreateFileW(
        path.c_str(),
        GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        &security,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH,
        nullptr));
    if (!handle) {
        throw Error(
            "Cannot create process log " + path_to_utf8(path) +
            " (Win32 error " + std::to_string(GetLastError()) + ")");
    }
    return handle;
}

// 打开可继承的 NUL 输入，避免继承无效控制台句柄。
[[nodiscard]] UniqueHandle open_null_input() {
    SECURITY_ATTRIBUTES security{};
    security.nLength = sizeof(security);
    security.bInheritHandle = TRUE;
    UniqueHandle handle(CreateFileW(
        L"NUL",
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        &security,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr));
    if (!handle) {
        throw Error("Cannot open NUL input (Win32 error " + std::to_string(GetLastError()) + ")");
    }
    return handle;
}

// 检查 Win32 BOOL 返回值并统一错误文本。
void ensure_win32(const BOOL success, const char* operation) {
    if (!success) {
        throw Error(std::string(operation) + " failed with Win32 error " + std::to_string(GetLastError()));
    }
}

}  // namespace

ProcessResult ProcessRunner::run(const ProcessRequest& request) const {
    if (!std::filesystem::is_regular_file(request.program)) {
        throw Error("Program is not a regular file: " + path_to_utf8(request.program));
    }
    if (!std::filesystem::is_directory(request.working_directory)) {
        throw Error("Working directory does not exist: " + path_to_utf8(request.working_directory));
    }
    if (request.timeout.count() <= 0 ||
        request.timeout.count() > static_cast<std::int64_t>(std::numeric_limits<DWORD>::max())) {
        throw Error("Process timeout is outside the supported range");
    }

    UniqueHandle standard_output = open_log(request.stdout_path);
    UniqueHandle standard_error = open_log(request.stderr_path);
    UniqueHandle standard_input = open_null_input();
    UniqueHandle job(CreateJobObjectW(nullptr, nullptr));
    if (!job) {
        throw Error("CreateJobObjectW failed with Win32 error " + std::to_string(GetLastError()));
    }

    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    ensure_win32(
        SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits)),
        "SetInformationJobObject");

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = standard_input.get();
    startup.hStdOutput = standard_output.get();
    startup.hStdError = standard_error.get();

    PROCESS_INFORMATION process_info{};
    std::vector<wchar_t> command_line = build_command_line(request);
    const auto start_time = std::chrono::steady_clock::now();
    ensure_win32(
        CreateProcessW(
            request.program.c_str(),
            command_line.data(),
            nullptr,
            nullptr,
            TRUE,
            CREATE_SUSPENDED | CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT,
            nullptr,
            request.working_directory.c_str(),
            &startup,
            &process_info),
        "CreateProcessW");

    UniqueHandle process(process_info.hProcess);
    UniqueHandle thread(process_info.hThread);
    try {
        ensure_win32(AssignProcessToJobObject(job.get(), process.get()), "AssignProcessToJobObject");
        if (ResumeThread(thread.get()) == static_cast<DWORD>(-1)) {
            throw Error("ResumeThread failed with Win32 error " + std::to_string(GetLastError()));
        }

        const DWORD wait_result = WaitForSingleObject(process.get(), static_cast<DWORD>(request.timeout.count()));
        ProcessResult result;
        if (wait_result == WAIT_TIMEOUT) {
            result.timed_out = true;
            ensure_win32(TerminateJobObject(job.get(), ERROR_TIMEOUT), "TerminateJobObject");
            WaitForSingleObject(process.get(), 5'000);
        } else if (wait_result != WAIT_OBJECT_0) {
            throw Error("WaitForSingleObject failed with Win32 error " + std::to_string(GetLastError()));
        }

        DWORD exit_code = 0;
        if (GetExitCodeProcess(process.get(), &exit_code) && exit_code != STILL_ACTIVE) {
            result.exit_code = exit_code;
        }
        result.duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start_time).count();
        return result;
    } catch (...) {
        TerminateJobObject(job.get(), ERROR_PROCESS_ABORTED);
        TerminateProcess(process.get(), ERROR_PROCESS_ABORTED);
        throw;
    }
}

}  // namespace satsuma::vm
