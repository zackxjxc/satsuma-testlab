// VMware vmrun 结构化调用实现。
#include "vmrun_provider.hpp"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <limits>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <windows.h>

#include "satsuma/core/errors.hpp"
#include "satsuma/core/id.hpp"
#include "satsuma/core/path.hpp"
#include "satsuma/core/windows_command_line.hpp"

namespace satsuma::vmware {
namespace {

constexpr std::chrono::seconds kSnapshotRestoreRetryTimeout{30}; // 停机后等待 VMware 释放 VM 文件锁
constexpr std::chrono::seconds kSnapshotRestoreRetryDelay{1}; // 快照恢复重试间隔
constexpr std::chrono::seconds kPowerStateReconciliationTimeout{30}; // 等待运行列表收敛的上限
constexpr std::chrono::milliseconds kPowerStateReconciliationDelay{100}; // 电源状态轮询间隔

// 只重试 VMware 明确报告的瞬时文件锁冲突。
[[nodiscard]] bool snapshot_restore_error_is_retryable(const Error& error) {
    std::string message = error.what();
    std::transform(message.begin(), message.end(), message.begin(), [](const unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return message.find("busy") != std::string::npos ||
           message.find("locked") != std::string::npos ||
           message.find("in use") != std::string::npos ||
           message.find("being used") != std::string::npos;
}

// 负责自动关闭 Win32 HANDLE。
class UniqueHandle {
public:
    // 接管一个可空 Win32 HANDLE。
    explicit UniqueHandle(HANDLE handle = nullptr) : handle_(handle) {}

    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;

    // 支持从句柄工厂转移所有权。
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

// 管理单次命令的专用临时目录。
class TemporaryDirectory {
public:
    // 在系统临时目录下创建不可复用的子目录。
    TemporaryDirectory() {
        path_ = std::filesystem::temp_directory_path() /
                path_from_utf8(make_id("satsuma-vmrun"));
        std::filesystem::create_directories(path_);
    }

    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

    // 尽力删除本次命令产生的日志。
    ~TemporaryDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    // 返回临时目录绝对路径。
    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

private:
    std::filesystem::path path_;  // 本次命令专用目录
};

// 打开可继承的命令重定向文件。
[[nodiscard]] UniqueHandle open_inheritable_file(
    const std::filesystem::path& path,
    const DWORD access,
    const DWORD creation) {
    SECURITY_ATTRIBUTES security{};
    security.nLength = sizeof(security);
    security.bInheritHandle = TRUE;
    UniqueHandle handle(CreateFileW(
        path.c_str(),
        access,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        &security,
        creation,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH,
        nullptr));
    if (!handle) {
        throw Error("Cannot open vmrun redirection file: " + std::to_string(GetLastError()));
    }
    return handle;
}

// 检查 Win32 BOOL 返回值并统一错误文本。
void ensure_win32(const BOOL success, const char* operation) {
    if (!success) {
        throw Error(std::string(operation) + " failed with Win32 error " + std::to_string(GetLastError()));
    }
}

// 读取命令输出的原始 UTF-8 字节。
[[nodiscard]] std::string read_output(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw Error("Cannot read vmrun output: " + path_to_utf8(path));
    }
    return std::string(
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>());
}

// 移除 getline 保留的 CR。
void trim_carriage_return(std::string& value) {
    if (!value.empty() && value.back() == '\r') {
        value.pop_back();
    }
}

// 拒绝不存在或不是绝对路径的 VMX。
void validate_vmx_file(const std::filesystem::path& vmx) {
    if (!vmx.is_absolute() || !std::filesystem::is_regular_file(vmx)) {
        throw Error("VMX is not an absolute regular file: " + path_to_utf8(vmx));
    }
}

// 拒绝空快照名和会截断 CreateProcessW 命令行的 NUL。
void validate_snapshot_name(const std::string_view snapshot_name) {
    if (snapshot_name.empty() || snapshot_name.find('\0') != std::string_view::npos) {
        throw Error("Snapshot name must be non-empty and contain no NUL character");
    }
}

}  // namespace

VmrunProvider::VmrunProvider(
    std::filesystem::path executable,
    const std::chrono::milliseconds timeout)
    : executable_(std::move(executable)), timeout_(timeout) {
    if (!executable_.is_absolute() || !std::filesystem::is_regular_file(executable_)) {
        throw Error("vmrun executable is not an absolute regular file: " + path_to_utf8(executable_));
    }
    if (timeout_.count() <= 0 ||
        timeout_.count() > static_cast<std::int64_t>(std::numeric_limits<DWORD>::max())) {
        throw Error("vmrun timeout is outside the supported range");
    }
}

std::vector<std::filesystem::path> VmrunProvider::list_running() const {
    const std::string output = invoke({"list"});
    std::istringstream lines(output);
    std::string header;
    if (!std::getline(lines, header)) {
        throw Error("vmrun list returned an empty response");
    }
    trim_carriage_return(header);

    constexpr std::string_view prefix = "Total running VMs: ";
    if (!header.starts_with(prefix)) {
        throw Error("vmrun list returned an invalid header: " + header);
    }
    std::size_t expected_count = 0;
    const std::string_view count_text(header.data() + prefix.size(), header.size() - prefix.size());
    const auto [end, parse_error] = std::from_chars(
        count_text.data(),
        count_text.data() + count_text.size(),
        expected_count);
    if (parse_error != std::errc{} || end != count_text.data() + count_text.size()) {
        throw Error("vmrun list returned an invalid VM count");
    }

    std::vector<std::filesystem::path> paths;
    std::string line;
    while (std::getline(lines, line)) {
        trim_carriage_return(line);
        if (!line.empty()) {
            paths.push_back(path_from_utf8(line));
        }
    }
    if (paths.size() != expected_count) {
        throw Error("vmrun list VM count does not match its output");
    }
    return paths;
}

bool VmrunProvider::is_running(const std::filesystem::path& vmx) const {
    validate_vmx_file(vmx);
    const std::filesystem::path expected = std::filesystem::absolute(vmx).lexically_normal();
    const std::vector<std::filesystem::path> running = list_running();
    return std::any_of(
        running.begin(),
        running.end(),
        [&expected](const std::filesystem::path& candidate) {
            const std::filesystem::path actual =
                std::filesystem::absolute(candidate).lexically_normal();
            return _wcsicmp(expected.native().c_str(), actual.native().c_str()) == 0;
        });
}

std::vector<std::string> VmrunProvider::list_snapshots(const std::filesystem::path& vmx) const {
    validate_vmx_file(vmx);
    const std::string output = invoke({"listSnapshots", path_to_utf8(vmx)});
    std::istringstream lines(output);
    std::string header;
    if (!std::getline(lines, header)) {
        throw Error("vmrun listSnapshots returned an empty response");
    }
    trim_carriage_return(header);

    constexpr std::string_view prefix = "Total snapshots: ";
    if (!header.starts_with(prefix)) {
        throw Error("vmrun listSnapshots returned an invalid header: " + header);
    }
    std::size_t expected_count = 0;
    const std::string_view count_text(header.data() + prefix.size(), header.size() - prefix.size());
    const auto [end, parse_error] = std::from_chars(
        count_text.data(),
        count_text.data() + count_text.size(),
        expected_count);
    if (parse_error != std::errc{} || end != count_text.data() + count_text.size()) {
        throw Error("vmrun listSnapshots returned an invalid snapshot count");
    }

    std::vector<std::string> snapshots;
    std::string line;
    while (std::getline(lines, line)) {
        trim_carriage_return(line);
        if (!line.empty()) {
            snapshots.push_back(line);
        }
    }
    if (snapshots.size() != expected_count) {
        throw Error("vmrun listSnapshots count does not match its output");
    }
    return snapshots;
}

std::string VmrunProvider::check_tools_state(const std::filesystem::path& vmx) const {
    validate_vmx_file(vmx);
    const std::string output = invoke({"checkToolsState", path_to_utf8(vmx)});
    std::istringstream lines(output);
    std::string state;
    if (!std::getline(lines, state)) {
        throw Error("vmrun checkToolsState returned an empty response");
    }
    trim_carriage_return(state);
    if (state.empty()) {
        throw Error("vmrun checkToolsState returned an empty state");
    }

    std::string extra;
    while (std::getline(lines, extra)) {
        trim_carriage_return(extra);
        if (!extra.empty()) {
            throw Error("vmrun checkToolsState returned multiple states");
        }
    }
    return state;
}

std::string VmrunProvider::get_guest_ip_address(const std::filesystem::path& vmx) const {
    validate_vmx_file(vmx);
    const std::string output = invoke({"getGuestIPAddress", path_to_utf8(vmx), "-wait"});
    std::istringstream lines(output);
    std::string address;
    if (!std::getline(lines, address)) {
        throw Error("vmrun getGuestIPAddress returned an empty response");
    }
    trim_carriage_return(address);
    if (address.empty()) {
        throw Error("vmrun getGuestIPAddress returned an empty address");
    }
    std::string extra;
    while (std::getline(lines, extra)) {
        trim_carriage_return(extra);
        if (!extra.empty()) {
            throw Error("vmrun getGuestIPAddress returned multiple addresses");
        }
    }
    return address;
}

void VmrunProvider::start(const std::filesystem::path& vmx) const {
    validate_vmx_file(vmx);
    std::string operation_error;
    try {
        static_cast<void>(invoke({"start", path_to_utf8(vmx), "nogui"}));
    } catch (const std::exception& error) {
        operation_error = error.what();
    }
    try {
        wait_for_running_state(vmx, true);
    } catch (const std::exception& error) {
        throw Error(
            operation_error.empty()
                ? error.what()
                : operation_error + "; start reconciliation failed: " + error.what());
    }
}

void VmrunProvider::stop(const std::filesystem::path& vmx, const VmStopMode mode) const {
    validate_vmx_file(vmx);
    const char* power_mode = nullptr;  // vmrun 接受的固定电源模式
    switch (mode) {
        case VmStopMode::Soft:
            power_mode = "soft";
            break;
        case VmStopMode::Hard:
            power_mode = "hard";
            break;
        default:
            throw Error("Unsupported vmrun stop mode");
    }
    std::string operation_error;
    try {
        static_cast<void>(invoke({"stop", path_to_utf8(vmx), power_mode}));
    } catch (const std::exception& error) {
        operation_error = error.what();
    }
    try {
        wait_for_running_state(vmx, false);
    } catch (const std::exception& error) {
        throw Error(
            operation_error.empty()
                ? error.what()
                : operation_error + "; stop reconciliation failed: " + error.what());
    }
}

void VmrunProvider::wait_for_running_state(
    const std::filesystem::path& vmx,
    const bool expected_running) const {
    const std::chrono::milliseconds reconciliation_timeout = std::min(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            kPowerStateReconciliationTimeout),
        timeout_);
    const auto deadline = std::chrono::steady_clock::now() +
        reconciliation_timeout;
    std::string last_error;
    for (;;) {
        try {
            if (is_running(vmx) == expected_running) {
                return;
            }
            last_error.clear();
        } catch (const std::exception& error) {
            last_error = error.what();
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            break;
        }
        std::this_thread::sleep_for(kPowerStateReconciliationDelay);
    }
    throw Error(
        std::string("VM did not reach power state ") +
        (expected_running ? "running" : "stopped") +
        " before the reconciliation deadline" +
        (last_error.empty() ? "" : "; last vmrun list error: " + last_error));
}

void VmrunProvider::revert_to_snapshot(
    const std::filesystem::path& vmx,
    const std::string_view snapshot_name) const {
    validate_vmx_file(vmx);
    validate_snapshot_name(snapshot_name);
    const auto deadline = std::chrono::steady_clock::now() + kSnapshotRestoreRetryTimeout;
    for (;;) {
        try {
            static_cast<void>(invoke({
                "revertToSnapshot",
                path_to_utf8(vmx),
                std::string(snapshot_name),
            }));
            return;
        } catch (const Error& error) {
            if (!snapshot_restore_error_is_retryable(error) ||
                std::chrono::steady_clock::now() >= deadline) {
                throw;
            }
            std::this_thread::sleep_for(kSnapshotRestoreRetryDelay);
        }
    }
}

void VmrunProvider::create_snapshot(
    const std::filesystem::path& vmx,
    const std::string_view snapshot_name) const {
    validate_vmx_file(vmx);
    validate_snapshot_name(snapshot_name);
    static_cast<void>(invoke({"snapshot", path_to_utf8(vmx), std::string(snapshot_name)}));
}

void VmrunProvider::delete_snapshot(
    const std::filesystem::path& vmx,
    const std::string_view snapshot_name) const {
    validate_vmx_file(vmx);
    validate_snapshot_name(snapshot_name);
    static_cast<void>(invoke({"deleteSnapshot", path_to_utf8(vmx), std::string(snapshot_name)}));
}

std::string VmrunProvider::invoke(const std::vector<std::string>& arguments) const {
    TemporaryDirectory temporary;
    const std::filesystem::path stdout_path = temporary.path() / L"stdout.log";
    const std::filesystem::path stderr_path = temporary.path() / L"stderr.log";
    UniqueHandle standard_output = open_inheritable_file(stdout_path, GENERIC_WRITE, CREATE_ALWAYS);
    UniqueHandle standard_error = open_inheritable_file(stderr_path, GENERIC_WRITE, CREATE_ALWAYS);
    UniqueHandle standard_input = open_inheritable_file(L"NUL", GENERIC_READ, OPEN_EXISTING);
    UniqueHandle job(CreateJobObjectW(nullptr, nullptr));
    if (!job) {
        throw Error("CreateJobObjectW failed for vmrun: " + std::to_string(GetLastError()));
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
    std::vector<wchar_t> command_line = build_windows_command_line(executable_, arguments);
    ensure_win32(
        CreateProcessW(
            executable_.c_str(),
            command_line.data(),
            nullptr,
            nullptr,
            TRUE,
            CREATE_SUSPENDED | CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT,
            nullptr,
            executable_.parent_path().c_str(),
            &startup,
            &process_info),
        "CreateProcessW(vmrun)");

    UniqueHandle process(process_info.hProcess);
    UniqueHandle thread(process_info.hThread);
    try {
        ensure_win32(AssignProcessToJobObject(job.get(), process.get()), "AssignProcessToJobObject(vmrun)");
        if (ResumeThread(thread.get()) == static_cast<DWORD>(-1)) {
            throw Error("ResumeThread(vmrun) failed: " + std::to_string(GetLastError()));
        }

        const DWORD wait_result = WaitForSingleObject(process.get(), static_cast<DWORD>(timeout_.count()));
        if (wait_result == WAIT_TIMEOUT) {
            TerminateJobObject(job.get(), ERROR_TIMEOUT);
            WaitForSingleObject(process.get(), 5'000);
            throw Error("vmrun command timed out");
        }
        if (wait_result != WAIT_OBJECT_0) {
            throw Error("WaitForSingleObject(vmrun) failed: " + std::to_string(GetLastError()));
        }

        DWORD exit_code = 0;
        ensure_win32(GetExitCodeProcess(process.get(), &exit_code), "GetExitCodeProcess(vmrun)");
        job.reset();
        standard_input.reset();
        standard_output.reset();
        standard_error.reset();
        const std::string standard_out = read_output(stdout_path);
        const std::string standard_err = read_output(stderr_path);
        if (exit_code != 0) {
            throw Error(
                "vmrun exited with code " + std::to_string(exit_code) +
                (standard_err.empty() ? "" : ": " + standard_err));
        }
        return standard_out;
    } catch (...) {
        TerminateJobObject(job.get(), ERROR_PROCESS_ABORTED);
        TerminateProcess(process.get(), ERROR_PROCESS_ABORTED);
        throw;
    }
}

}  // namespace satsuma::vmware
