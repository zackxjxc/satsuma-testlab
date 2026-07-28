// Windows Job Object 进程树执行实现。
#include "process_runner.hpp"

#include <algorithm>
#include <limits>
#include <string>
#include <vector>

#include <windows.h>

#include "satsuma/core/errors.hpp"
#include "satsuma/core/path.hpp"
#include "satsuma/core/windows_command_line.hpp"

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

// 终止已分配到 Job Object 的进程树并返回稳定停止错误。
[[noreturn]] void cancel_job(const HANDLE job, const HANDLE process) {
    if (!TerminateJobObject(job, ERROR_OPERATION_ABORTED)) {
        throw Error(
            "Agent stop requested; TerminateJobObject failed with Win32 error " +
            std::to_string(GetLastError()));
    }
    WaitForSingleObject(process, 5'000);
    throw Error("Agent stop requested");
}

// 判断两个持续写入日志的当前总大小是否超过请求上限。
[[nodiscard]] bool output_limit_exceeded(
    const HANDLE standard_output,
    const HANDLE standard_error,
    const std::uint64_t limit) {
    LARGE_INTEGER stdout_size{};
    LARGE_INTEGER stderr_size{};
    ensure_win32(GetFileSizeEx(standard_output, &stdout_size), "GetFileSizeEx(stdout)");
    ensure_win32(GetFileSizeEx(standard_error, &stderr_size), "GetFileSizeEx(stderr)");
    return stdout_size.QuadPart < 0 || stderr_size.QuadPart < 0 ||
        static_cast<std::uint64_t>(stdout_size.QuadPart) > limit ||
        static_cast<std::uint64_t>(stderr_size.QuadPart) >
            limit - static_cast<std::uint64_t>(stdout_size.QuadPart);
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
    if (request.max_output_bytes == 0) {
        throw Error("Process output limit must be greater than zero");
    }
    if (request.stop_token.stop_requested()) {
        throw Error("Agent stop requested");
    }

    UniqueHandle standard_output = open_log(request.stdout_path);
    UniqueHandle standard_error = open_log(request.stderr_path);
    UniqueHandle standard_input = open_null_input();
    UniqueHandle job(CreateJobObjectW(nullptr, nullptr));
    if (!job) {
        throw Error("CreateJobObjectW failed with Win32 error " + std::to_string(GetLastError()));
    }
    UniqueHandle cancel_event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!cancel_event) {
        throw Error("CreateEventW failed with Win32 error " + std::to_string(GetLastError()));
    }
    std::stop_callback stop_callback(
        request.stop_token,
        [event = cancel_event.get()] { SetEvent(event); });

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
    std::vector<wchar_t> command_line = build_windows_command_line(request.program, request.arguments);
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
        if (WaitForSingleObject(cancel_event.get(), 0) == WAIT_OBJECT_0) {
            cancel_job(job.get(), process.get());
        }
        if (ResumeThread(thread.get()) == static_cast<DWORD>(-1)) {
            throw Error("ResumeThread failed with Win32 error " + std::to_string(GetLastError()));
        }

        ProcessResult result;
        const HANDLE wait_handles[] = {process.get(), cancel_event.get()};
        const auto deadline = start_time + request.timeout;
        for (;;) {
            const auto now = std::chrono::steady_clock::now();
            const auto remaining = now < deadline
                ? std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now)
                : std::chrono::milliseconds::zero();
            const DWORD wait_ms = static_cast<DWORD>(std::min(remaining, std::chrono::milliseconds(100)).count());
            const DWORD wait_result = WaitForMultipleObjects(2, wait_handles, FALSE, wait_ms);
            if (wait_result == WAIT_OBJECT_0) {
                result.output_limit_exceeded = output_limit_exceeded(
                    standard_output.get(), standard_error.get(), request.max_output_bytes);
                break;
            }
            if (wait_result == WAIT_OBJECT_0 + 1) {
                cancel_job(job.get(), process.get());
            }
            if (wait_result != WAIT_TIMEOUT) {
                throw Error("WaitForMultipleObjects failed with Win32 error " + std::to_string(GetLastError()));
            }
            if (output_limit_exceeded(
                    standard_output.get(),
                    standard_error.get(),
                    request.max_output_bytes)) {
                result.output_limit_exceeded = true;
                ensure_win32(TerminateJobObject(job.get(), ERROR_FILE_TOO_LARGE), "TerminateJobObject");
                WaitForSingleObject(process.get(), 5'000);
                break;
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                result.timed_out = true;
                ensure_win32(TerminateJobObject(job.get(), ERROR_TIMEOUT), "TerminateJobObject");
                WaitForSingleObject(process.get(), 5'000);
                break;
            }
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
