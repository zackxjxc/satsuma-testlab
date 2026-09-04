// 交互用户 Session 令牌、跨 Session 句柄传递和隐藏 helper 实现。
#include "interactive_process.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <exception>
#include <functional>
#include <limits>
#include <map>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <windows.h>
#include <aclapi.h>
#include <sddl.h>
#include <userenv.h>
#include <wtsapi32.h>

#include <nlohmann/json.hpp>

#include "satsuma/core/errors.hpp"
#include "satsuma/core/id.hpp"
#include "satsuma/core/json_io.hpp"
#include "satsuma/core/path.hpp"
#include "satsuma/core/windows_command_line.hpp"
#include "process_environment.hpp"

namespace satsuma::vm {
namespace {

constexpr DWORD kInvalidSessionId = 0xFFFFFFFF;  // WTS 无活动控制台 Session

#ifdef SATSUMA_INTERACTIVE_TESTS
bool g_interactive_session_unavailable = false;  // 测试注入的无 Session 状态
bool g_interactive_identity_changed = false;     // 测试注入的 Session/SID 变化
bool g_resume_identity_changed = false;          // 测试注入的恢复前身份变化
DWORD g_last_helper_pid = 0;                     // 最近一次测试 helper PID
#endif

// 自动关闭普通 Win32 HANDLE。
class UniqueHandle {
public:
    // 接管一个可空 HANDLE。
    explicit UniqueHandle(HANDLE value = nullptr) : value_(value) {}

    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;

    // 转移 HANDLE 所有权。
    UniqueHandle(UniqueHandle&& other) noexcept : value_(other.release()) {}

    // 关闭旧 HANDLE 并接管新值。
    UniqueHandle& operator=(UniqueHandle&& other) noexcept {
        if (this != &other) {
            reset(other.release());
        }
        return *this;
    }

    // 关闭有效 HANDLE。
    ~UniqueHandle() {
        reset();
    }

    // 返回底层 HANDLE。
    [[nodiscard]] HANDLE get() const noexcept {
        return value_;
    }

    // 返回 HANDLE 是否有效。
    [[nodiscard]] explicit operator bool() const noexcept {
        return value_ != nullptr && value_ != INVALID_HANDLE_VALUE;
    }

    // 放弃 HANDLE 所有权。
    [[nodiscard]] HANDLE release() noexcept {
        const HANDLE value = value_;
        value_ = nullptr;
        return value;
    }

    // 关闭旧 HANDLE 并保存新值。
    void reset(HANDLE value = nullptr) noexcept {
        if (value_ != nullptr && value_ != INVALID_HANDLE_VALUE) {
            CloseHandle(value_);
        }
        value_ = value;
    }

private:
    HANDLE value_ = nullptr;  // 被管理的 HANDLE
};

// 自动销毁 CreateEnvironmentBlock 返回的环境。
class UniqueEnvironment {
public:
    UniqueEnvironment() = default;
    UniqueEnvironment(const UniqueEnvironment&) = delete;
    UniqueEnvironment& operator=(const UniqueEnvironment&) = delete;

    // 转移环境块所有权。
    UniqueEnvironment(UniqueEnvironment&& other) noexcept : value_(other.release()) {}

    // 转移赋值环境块所有权。
    UniqueEnvironment& operator=(UniqueEnvironment&& other) noexcept {
        if (this != &other) {
            reset(other.release());
        }
        return *this;
    }

    // 销毁有效环境块。
    ~UniqueEnvironment() {
        reset();
    }

    // 返回底层环境块。
    [[nodiscard]] void* get() const noexcept {
        return value_;
    }

    // 返回接收 CreateEnvironmentBlock 结果的地址。
    [[nodiscard]] void** put() noexcept {
        reset();
        return &value_;
    }

private:
    // 放弃环境块所有权。
    [[nodiscard]] void* release() noexcept {
        void* value = value_;
        value_ = nullptr;
        return value;
    }

    // 销毁旧环境块并保存新值。
    void reset(void* value = nullptr) noexcept {
        if (value_ != nullptr) {
            DestroyEnvironmentBlock(value_);
        }
        value_ = value;
    }

    void* value_ = nullptr;  // 双 NUL 结尾的 Unicode 环境块
};

// 保存一次用户 Token 选择及仅测试可用的当前进程回退。
struct SelectedUserToken {
    UniqueHandle token;  // 活动控制台用户 Token
#ifdef SATSUMA_INTERACTIVE_TESTS
    bool use_current_process{false};  // 非 SYSTEM 本机测试回退
#endif
};

// 标识一次登录身份，避免同 SID 重新登录时复用旧 Token。
struct TokenIdentity {
    std::string sid;              // 用户 SID
    LUID authentication_id{};     // 当前登录会话标识
    DWORD session_id{};           // Token 绑定的 Windows Session
};

// 检查 Win32 BOOL 并生成稳定错误。
void ensure_win32(const BOOL success, const char* operation) {
    if (!success) {
        throw Error(
            std::string(operation) + " failed with Win32 error " +
            std::to_string(GetLastError()));
    }
}

// 判断交互执行的 stdout 与 stderr 当前总大小是否超过上限。
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

// 拒绝 WTS 无活动控制台 Session 的哨兵值。
void validate_interactive_session_id(const DWORD session_id) {
    if (session_id == kInvalidSessionId) {
        throw Error(kNoInteractiveUserSessionError);
    }
}

// 返回当前进程所在 Windows Session。
[[nodiscard]] DWORD current_process_session_id() {
    DWORD session_id = kInvalidSessionId;
    ensure_win32(
        ProcessIdToSessionId(GetCurrentProcessId(), &session_id),
        "ProcessIdToSessionId");
    return session_id;
}

// 返回 Token 对应的稳定 SID 字符串。
[[nodiscard]] std::string token_sid(const HANDLE token) {
    DWORD size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || size == 0) {
        throw Error(
            "GetTokenInformation(TokenUser) failed with Win32 error " +
            std::to_string(GetLastError()));
    }
    std::vector<BYTE> buffer(size);
    ensure_win32(
        GetTokenInformation(
            token,
            TokenUser,
            buffer.data(),
            static_cast<DWORD>(buffer.size()),
            &size),
        "GetTokenInformation(TokenUser)");
    const auto* user = reinterpret_cast<const TOKEN_USER*>(buffer.data());

    wchar_t* sid_text = nullptr;
    if (!ConvertSidToStringSidW(user->User.Sid, &sid_text)) {
        throw Error(
            "ConvertSidToStringSidW failed with Win32 error " +
            std::to_string(GetLastError()));
    }
    std::string result;
    for (const wchar_t* cursor = sid_text; *cursor != L'\0'; ++cursor) {
        result.push_back(static_cast<char>(*cursor));
    }
    LocalFree(sid_text);
    return result;
}

// 判断指定 Token 是否属于 LocalSystem。
[[nodiscard]] bool token_is_local_system(const HANDLE token) {
    DWORD size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || size == 0) {
        throw Error(
            "GetTokenInformation(TokenUser) failed with Win32 error " +
            std::to_string(GetLastError()));
    }
    std::vector<BYTE> buffer(size);
    ensure_win32(
        GetTokenInformation(
            token,
            TokenUser,
            buffer.data(),
            static_cast<DWORD>(buffer.size()),
            &size),
        "GetTokenInformation(TokenUser)");

    DWORD system_sid_size = SECURITY_MAX_SID_SIZE;
    std::vector<BYTE> system_sid(system_sid_size);
    ensure_win32(
        CreateWellKnownSid(
            WinLocalSystemSid,
            nullptr,
            system_sid.data(),
            &system_sid_size),
        "CreateWellKnownSid(WinLocalSystemSid)");
    const auto* user = reinterpret_cast<const TOKEN_USER*>(buffer.data());
    return EqualSid(user->User.Sid, system_sid.data()) != FALSE;
}

// 打开当前进程 Token。
[[nodiscard]] UniqueHandle open_current_process_token() {
    HANDLE token = nullptr;
    ensure_win32(
        OpenProcessToken(
            GetCurrentProcess(),
            TOKEN_QUERY | TOKEN_DUPLICATE | TOKEN_IMPERSONATE,
            &token),
        "OpenProcessToken");
    return UniqueHandle(token);
}

// 生产交互执行必须从 LocalSystem 服务进入。
void validate_interactive_caller(const bool is_local_system) {
    if (!is_local_system) {
        throw Error("Interactive user execution requires LocalSystem");
    }
}

// 获取活动控制台用户 Token；当前用户回退只编译进本机测试目标。
[[nodiscard]] SelectedUserToken select_active_user_token(const DWORD session_id) {
#ifndef SATSUMA_INTERACTIVE_TESTS
    UniqueHandle caller = open_current_process_token();
    validate_interactive_caller(token_is_local_system(caller.get()));
#endif

    HANDLE token = nullptr;
    if (WTSQueryUserToken(session_id, &token)) {
        return SelectedUserToken{UniqueHandle(token)};
    }
    const DWORD error = GetLastError();
#ifdef SATSUMA_INTERACTIVE_TESTS
    if ((error == ERROR_PRIVILEGE_NOT_HELD || error == ERROR_ACCESS_DENIED) &&
        current_process_session_id() == session_id) {
        SelectedUserToken selected;
        selected.token = open_current_process_token();
        selected.use_current_process = true;
        return selected;
    }
#endif
    if (error == ERROR_NO_TOKEN) {
        throw Error(kNoInteractiveUserSessionError);
    }
    throw Error(
        "WTSQueryUserToken failed with Win32 error " +
        std::to_string(error));
}

// 返回用于检测注销重登和 Session 复用的完整 Token 身份。
[[nodiscard]] TokenIdentity token_identity(const HANDLE token) {
    TOKEN_STATISTICS statistics{};
    DWORD statistics_size = sizeof(statistics);
    ensure_win32(
        GetTokenInformation(
            token,
            TokenStatistics,
            &statistics,
            sizeof(statistics),
            &statistics_size),
        "GetTokenInformation(TokenStatistics)");
    DWORD session_id = 0;
    DWORD session_size = sizeof(session_id);
    ensure_win32(
        GetTokenInformation(
            token,
            TokenSessionId,
            &session_id,
            sizeof(session_id),
            &session_size),
        "GetTokenInformation(TokenSessionId)");
    return TokenIdentity{token_sid(token), statistics.AuthenticationId, session_id};
}

// 比较两次查询是否仍属于同一次登录。
[[nodiscard]] bool same_token_identity(
    const TokenIdentity& left,
    const TokenIdentity& right) noexcept {
    return left.sid == right.sid &&
           left.authentication_id.LowPart == right.authentication_id.LowPart &&
           left.authentication_id.HighPart == right.authentication_id.HighPart &&
           left.session_id == right.session_id;
}

// 从 Unicode 环境块读取指定变量。
[[nodiscard]] std::filesystem::path environment_path(
    const void* environment,
    const std::wstring_view name) {
    const auto* cursor = static_cast<const wchar_t*>(environment);
    const std::wstring prefix = std::wstring(name) + L"=";
    while (cursor != nullptr && *cursor != L'\0') {
        const std::wstring_view entry(cursor);
        if (entry.size() > prefix.size() &&
            _wcsnicmp(entry.data(), prefix.c_str(), prefix.size()) == 0) {
            return std::filesystem::path(entry.substr(prefix.size()));
        }
        cursor += entry.size() + 1;
    }
    throw Error("Interactive user environment does not define LOCALAPPDATA");
}

// RevertToSelf 失败后不能继续执行任何 SYSTEM 工作。
[[noreturn]] void fail_fast_after_revert_failure(const DWORD error) noexcept {
    const UINT exit_code = error == ERROR_SUCCESS
        ? static_cast<UINT>(ERROR_CANNOT_IMPERSONATE)
        : static_cast<UINT>(error);
    TerminateProcess(GetCurrentProcess(), exit_code);
    std::terminate();
}

// 恢复进程身份，失败时立即终止 Agent。
void revert_to_self_or_fail_fast() noexcept {
    if (!RevertToSelf()) {
        fail_fast_after_revert_failure(GetLastError());
    }
}

// 在绑定用户身份下执行一个有限文件操作。
void run_impersonated(
    const HANDLE token,
    const std::function<void()>& operation) {
    ensure_win32(ImpersonateLoggedOnUser(token), "ImpersonateLoggedOnUser");
    try {
        operation();
    } catch (...) {
        const std::exception_ptr failure = std::current_exception();
        revert_to_self_or_fail_fast();
        std::rethrow_exception(failure);
    }
    revert_to_self_or_fail_fast();
}

// 只给当前已验证用户 SID 增加本次运行目录的可继承权限。
void grant_run_directory_access(
    const std::filesystem::path& directory,
    const HANDLE token) {
    DWORD size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || size == 0) {
        throw Error("Cannot query interactive user SID");
    }
    std::vector<BYTE> token_buffer(size);
    ensure_win32(
        GetTokenInformation(token, TokenUser, token_buffer.data(), size, &size),
        "GetTokenInformation(TokenUser)");
    const auto* user = reinterpret_cast<const TOKEN_USER*>(token_buffer.data());

    PACL current_acl = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    const DWORD read_status = GetNamedSecurityInfoW(
        windows_file_path(directory).data(),
        SE_FILE_OBJECT,
        DACL_SECURITY_INFORMATION,
        nullptr,
        nullptr,
        &current_acl,
        nullptr,
        &descriptor);
    if (read_status != ERROR_SUCCESS) {
        throw Error("GetNamedSecurityInfoW failed with Win32 error " + std::to_string(read_status));
    }

    EXPLICIT_ACCESSW access{};
    access.grfAccessPermissions = GENERIC_READ | GENERIC_WRITE | GENERIC_EXECUTE | DELETE;
    access.grfAccessMode = GRANT_ACCESS;
    access.grfInheritance = SUB_CONTAINERS_AND_OBJECTS_INHERIT;
    BuildTrusteeWithSidW(&access.Trustee, user->User.Sid);
    PACL updated_acl = nullptr;
    const DWORD merge_status = SetEntriesInAclW(1, &access, current_acl, &updated_acl);
    if (merge_status != ERROR_SUCCESS) {
        LocalFree(descriptor);
        throw Error("SetEntriesInAclW failed with Win32 error " + std::to_string(merge_status));
    }
    const DWORD write_status = SetNamedSecurityInfoW(
        windows_file_path(directory).data(),
        SE_FILE_OBJECT,
        DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
        nullptr,
        nullptr,
        updated_acl,
        nullptr);
    LocalFree(updated_acl);
    LocalFree(descriptor);
    if (write_status != ERROR_SUCCESS) {
        throw Error("SetNamedSecurityInfoW failed with Win32 error " + std::to_string(write_status));
    }
}

// 打开可由 Host 实时读取的继承型日志句柄。
[[nodiscard]] UniqueHandle open_log(const std::filesystem::path& path) {
    std::filesystem::create_directories(windows_file_path(path.parent_path()));
    SECURITY_ATTRIBUTES security{};
    security.nLength = sizeof(security);
    security.bInheritHandle = TRUE;
    UniqueHandle handle(CreateFileW(
        windows_file_path(path).c_str(),
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

// 打开可继承的 NUL 输入。
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
        throw Error(
            "Cannot open NUL input (Win32 error " +
            std::to_string(GetLastError()) + ")");
    }
    return handle;
}

// 把一个本进程句柄复制进已挂起的用户 helper。
[[nodiscard]] std::uint64_t duplicate_remote_handle(
    const HANDLE process,
    const HANDLE source) {
    HANDLE remote = nullptr;
    ensure_win32(
        DuplicateHandle(
            GetCurrentProcess(),
            source,
            process,
            &remote,
            0,
            TRUE,
            DUPLICATE_SAME_ACCESS),
        "DuplicateHandle");
    return static_cast<std::uint64_t>(
        reinterpret_cast<std::uintptr_t>(remote));
}

// 记录一次 helper 终止和等待结果。
struct HelperTermination {
    bool exited{false};  // helper 已确认进入终态
    std::string error;   // 终止或等待失败详情
};

// 终止 helper 所属 Job，并在关闭 Job 后再次确认 helper 已退出。
[[nodiscard]] HelperTermination terminate_helper_and_wait(
    UniqueHandle& job,
    const HANDLE process,
    const bool assigned_to_job,
    const DWORD exit_code) noexcept {
    HelperTermination result;
    if (assigned_to_job) {
        if (!TerminateJobObject(job.get(), exit_code)) {
            result.error =
                "TerminateJobObject failed with Win32 error " +
                std::to_string(GetLastError());
        } else {
            const DWORD job_wait_result = WaitForSingleObject(job.get(), 5'000);
            if (job_wait_result == WAIT_TIMEOUT) {
                result.error = "interactive process tree did not exit within 5 seconds";
            } else if (job_wait_result != WAIT_OBJECT_0) {
                result.error =
                    "WaitForSingleObject(interactive Job) failed with Win32 error " +
                    std::to_string(GetLastError());
            }
        }
    } else if (!TerminateProcess(process, exit_code)) {
        result.error =
            "TerminateProcess failed with Win32 error " +
            std::to_string(GetLastError());
    }

    DWORD wait_result = WaitForSingleObject(process, 5'000);
    if (wait_result == WAIT_TIMEOUT && assigned_to_job) {
        job.reset();
        wait_result = WaitForSingleObject(process, 5'000);
    }
    if (wait_result == WAIT_OBJECT_0) {
        result.exited = true;
    } else if (wait_result == WAIT_TIMEOUT) {
        if (!result.error.empty()) {
            result.error += "; ";
        }
        result.error += "interactive process helper did not exit within 10 seconds";
    } else {
        if (!result.error.empty()) {
            result.error += "; ";
        }
        result.error +=
            "WaitForSingleObject(interactive helper) failed with Win32 error " +
            std::to_string(GetLastError());
    }
    return result;
}

// 尽力删除一次 helper 文件及其同名原子写临时文件。
void remove_helper_file_group(const std::filesystem::path& path) noexcept {
    std::error_code error;
    std::filesystem::remove(windows_file_path(path), error);
    error.clear();
    const std::wstring temporary_prefix = path.filename().native() + L".tmp-";
    std::filesystem::directory_iterator iterator(path.parent_path(), error);
    const std::filesystem::directory_iterator end;
    while (!error && iterator != end) {
        if (iterator->path().filename().native().starts_with(temporary_prefix)) {
            std::error_code remove_error;
            std::filesystem::remove(windows_file_path(iterator->path()), remove_error);
        }
        iterator.increment(error);
    }
}

// 清理本次唯一 helper ID 对应的请求、结果和临时文件。
void remove_helper_protocol_files(
    const std::filesystem::path& request,
    const std::filesystem::path& result) noexcept {
    remove_helper_file_group(request);
    remove_helper_file_group(result);
}

}  // namespace

// 保存一次已绑定的交互用户上下文。
struct InteractiveUserSession::State {
    SelectedUserToken selected_user;          // Artifact 部署使用的用户 Token
    std::filesystem::path working_directory;  // 用户本地运行目录
    TokenIdentity identity;                   // 准备阶段绑定的登录身份
};

InteractiveUserSession::InteractiveUserSession(std::unique_ptr<State> state)
    : state_(std::move(state)) {}

InteractiveUserSession::~InteractiveUserSession() = default;

InteractiveUserSession::InteractiveUserSession(
    InteractiveUserSession&& other) noexcept = default;

InteractiveUserSession& InteractiveUserSession::operator=(
    InteractiveUserSession&& other) noexcept = default;

InteractiveUserSession InteractiveUserSession::acquire(
    const std::string& lab_id,
    const std::string& run_id,
    const std::filesystem::path& local_work_root,
    const std::string& vm_id) {
    validate_identifier(lab_id, "interactive lab_id");
    validate_identifier(run_id, "interactive run_id");
    if (!local_work_root.empty()) {
        validate_identifier(vm_id, "interactive vm_id");
    }
#ifdef SATSUMA_INTERACTIVE_TESTS
    if (g_interactive_session_unavailable) {
        throw Error(kNoInteractiveUserSessionError);
    }
#endif
    const DWORD session_id = WTSGetActiveConsoleSessionId();
    validate_interactive_session_id(session_id);

    auto state = std::make_unique<State>();
    state->selected_user = select_active_user_token(session_id);
    state->identity = token_identity(state->selected_user.token.get());
    if (state->identity.session_id != session_id) {
        throw Error("Interactive user Token belongs to the wrong Session");
    }
    UniqueEnvironment environment;
    ensure_win32(
        CreateEnvironmentBlock(
            environment.put(),
            state->selected_user.token.get(),
            FALSE),
        "CreateEnvironmentBlock");
    state->working_directory = local_work_root.empty()
        ? environment_path(environment.get(), L"LOCALAPPDATA") /
            L"SatsumaTestLab" / path_from_utf8(lab_id) / L"runs" / path_from_utf8(run_id)
        : local_work_root / path_from_utf8(lab_id) / path_from_utf8(run_id) /
            path_from_utf8(vm_id);
    std::filesystem::create_directories(windows_file_path(state->working_directory));
    grant_run_directory_access(state->working_directory, state->selected_user.token.get());
    return InteractiveUserSession(std::move(state));
}

const std::filesystem::path& InteractiveUserSession::working_directory() const noexcept {
    return state_->working_directory;
}

std::uint32_t InteractiveUserSession::session_id() const noexcept {
    return state_->identity.session_id;
}

const std::string& InteractiveUserSession::user_sid() const noexcept {
    return state_->identity.sid;
}

std::filesystem::path InteractiveUserSession::deploy_file(
    const std::filesystem::path& source,
    const std::filesystem::path& relative_destination) const {
    if (!std::filesystem::is_regular_file(windows_file_path(source))) {
        throw Error("Artifact source is not a regular file: " + path_to_utf8(source));
    }
    const std::filesystem::path destination = resolve_under_root(
        state_->working_directory,
        relative_destination);
    run_impersonated(state_->selected_user.token.get(), [&source, &destination] {
        std::filesystem::create_directories(windows_file_path(destination.parent_path()));
        std::filesystem::copy_file(
            windows_file_path(source),
            windows_file_path(destination),
            std::filesystem::copy_options::overwrite_existing);
    });
    return destination;
}

ProcessResult InteractiveUserSession::run(
    const std::filesystem::path& helper_executable,
    const ProcessRequest& request) const {
    if (!std::filesystem::is_regular_file(windows_file_path(helper_executable))) {
        throw Error(
            "Interactive process helper is not a regular file: " +
            path_to_utf8(helper_executable));
    }
    if (!std::filesystem::is_regular_file(windows_file_path(request.program))) {
        throw Error("Program is not a regular file: " + path_to_utf8(request.program));
    }
    if (!std::filesystem::is_directory(request.working_directory)) {
        throw Error(
            "Working directory does not exist: " +
            path_to_utf8(request.working_directory));
    }
    if (request.timeout.count() <= 0 ||
        request.timeout.count() >
            static_cast<std::int64_t>(std::numeric_limits<DWORD>::max())) {
        throw Error("Process timeout is outside the supported range");
    }
    if (request.max_output_bytes == 0) {
        throw Error("Process output limit must be greater than zero");
    }
    if (request.stop_token.stop_requested()) {
        throw Error("Agent stop requested");
    }

    const DWORD active_session = WTSGetActiveConsoleSessionId();
    validate_interactive_session_id(active_session);
    if (active_session != state_->identity.session_id) {
        throw Error("Interactive user session changed before process launch");
    }
#ifdef SATSUMA_INTERACTIVE_TESTS
    if (g_interactive_identity_changed) {
        throw Error("Interactive user identity changed before process launch");
    }
#endif
    SelectedUserToken launch_user = select_active_user_token(active_session);
    const TokenIdentity launch_identity = token_identity(launch_user.token.get());
    if (!same_token_identity(launch_identity, state_->identity)) {
        throw Error("Interactive user identity changed before process launch");
    }
    UniqueEnvironment launch_environment;
    ensure_win32(
        CreateEnvironmentBlock(
            launch_environment.put(),
            launch_user.token.get(),
            FALSE),
        "CreateEnvironmentBlock");

    UniqueHandle standard_output = open_log(request.stdout_path);
    UniqueHandle standard_error = open_log(request.stderr_path);
    UniqueHandle standard_input = open_null_input();
    UniqueHandle job(CreateJobObjectW(nullptr, nullptr));
    if (!job) {
        throw Error(
            "CreateJobObjectW failed with Win32 error " +
            std::to_string(GetLastError()));
    }
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    ensure_win32(
        SetInformationJobObject(
            job.get(),
            JobObjectExtendedLimitInformation,
            &limits,
            sizeof(limits)),
        "SetInformationJobObject");

    UniqueHandle cancel_event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!cancel_event) {
        throw Error(
            "CreateEventW failed with Win32 error " +
            std::to_string(GetLastError()));
    }
    std::stop_callback stop_callback(
        request.stop_token,
        [event = cancel_event.get()] { SetEvent(event); });

    const std::string helper_id = make_id("process-helper");
    const std::filesystem::path helper_request =
        state_->working_directory /
        path_from_utf8(".satsuma-" + helper_id + ".json");
    const std::filesystem::path helper_result =
        state_->working_directory /
        path_from_utf8(".satsuma-" + helper_id + ".result.json");
    remove_helper_protocol_files(helper_request, helper_result);

    std::vector<wchar_t> helper_command = build_windows_command_line(
        helper_executable,
        {"--process-helper", path_to_utf8(helper_request)});
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    std::wstring desktop = L"winsta0\\default";
    startup.lpDesktop = desktop.data();
    PROCESS_INFORMATION helper_info{};
    // Helper 保持挂起，直到加入 Job、发布请求并再次验证 Session 与 SID。
    const DWORD creation_flags =
        CREATE_SUSPENDED | CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT;
    BOOL launched = FALSE;
#ifdef SATSUMA_INTERACTIVE_TESTS
    if (launch_user.use_current_process) {
        launched = CreateProcessW(
            helper_executable.c_str(),
            helper_command.data(),
            nullptr,
            nullptr,
            FALSE,
            creation_flags,
            launch_environment.get(),
            state_->working_directory.c_str(),
            &startup,
            &helper_info);
    } else
#endif
    {
        launched = CreateProcessAsUserW(
            launch_user.token.get(),
            helper_executable.c_str(),
            helper_command.data(),
            nullptr,
            nullptr,
            FALSE,
            creation_flags,
            launch_environment.get(),
            state_->working_directory.c_str(),
            &startup,
            &helper_info);
    }
    if (!launched) {
        throw Error(
            "Cannot launch interactive process helper (Win32 error " +
            std::to_string(GetLastError()) + ")");
    }

    UniqueHandle helper_process(helper_info.hProcess);
    UniqueHandle helper_thread(helper_info.hThread);
#ifdef SATSUMA_INTERACTIVE_TESTS
    g_last_helper_pid = helper_info.dwProcessId;
#endif
    const auto start_time = std::chrono::steady_clock::now();
    bool assigned_to_job = false;
    bool helper_exit_confirmed = false;
    try {
        ensure_win32(
            AssignProcessToJobObject(job.get(), helper_process.get()),
            "AssignProcessToJobObject");
        assigned_to_job = true;
        const nlohmann::json helper_payload = {
            {"schema_version", 1},
            {"session_id", state_->identity.session_id},
            {"program", path_to_utf8(request.program)},
            {"arguments", request.arguments},
            {"verbatim_arguments", request.verbatim_arguments},
            {"environment_overrides", request.environment_overrides},
            {"working_directory", path_to_utf8(request.working_directory)},
            {"stdin_handle", duplicate_remote_handle(
                helper_process.get(), standard_input.get())},
            {"stdout_handle", duplicate_remote_handle(
                helper_process.get(), standard_output.get())},
            {"stderr_handle", duplicate_remote_handle(
                helper_process.get(), standard_error.get())},
            {"result_path", path_to_utf8(helper_result)},
        };
        run_impersonated(launch_user.token.get(), [&helper_request, &helper_payload] {
            write_json_atomic(helper_request, helper_payload);
        });
        if (WaitForSingleObject(cancel_event.get(), 0) == WAIT_OBJECT_0) {
            const HelperTermination termination = terminate_helper_and_wait(
                job,
                helper_process.get(),
                assigned_to_job,
                ERROR_OPERATION_ABORTED);
            helper_exit_confirmed = termination.exited;
            if (termination.exited) {
                remove_helper_protocol_files(helper_request, helper_result);
            }
            if (!termination.error.empty()) {
                throw Error("Agent stop requested; " + termination.error);
            }
            throw Error("Agent stop requested");
        }
        const DWORD resume_session = WTSGetActiveConsoleSessionId();
        validate_interactive_session_id(resume_session);
        if (resume_session != state_->identity.session_id) {
            throw Error("Interactive user session changed before process launch");
        }
#ifdef SATSUMA_INTERACTIVE_TESTS
        if (g_resume_identity_changed) {
            throw Error("Interactive user identity changed before process launch");
        }
#endif
        SelectedUserToken resume_user = select_active_user_token(resume_session);
        if (!same_token_identity(
                token_identity(resume_user.token.get()),
                launch_identity)) {
            throw Error("Interactive user identity changed before process launch");
        }
        if (ResumeThread(helper_thread.get()) == static_cast<DWORD>(-1)) {
            throw Error(
                "ResumeThread failed with Win32 error " +
                std::to_string(GetLastError()));
        }

        ProcessResult result;
        const HANDLE wait_handles[] = {helper_process.get(), cancel_event.get()};
        const auto deadline = start_time + request.timeout;
        for (;;) {
            const auto now = std::chrono::steady_clock::now();
            const auto remaining = now < deadline
                ? std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now)
                : std::chrono::milliseconds::zero();
            const DWORD wait_ms = static_cast<DWORD>(
                std::min(remaining, std::chrono::milliseconds(100)).count());
            const DWORD wait_result = WaitForMultipleObjects(2, wait_handles, FALSE, wait_ms);
            if (wait_result == WAIT_OBJECT_0) {
                helper_exit_confirmed = true;
                result.output_limit_exceeded = output_limit_exceeded(
                    standard_output.get(), standard_error.get(), request.max_output_bytes);
                break;
            }
            if (wait_result == WAIT_OBJECT_0 + 1) {
                const HelperTermination termination = terminate_helper_and_wait(
                    job, helper_process.get(), assigned_to_job, ERROR_OPERATION_ABORTED);
                helper_exit_confirmed = termination.exited;
                if (termination.exited) {
                    remove_helper_protocol_files(helper_request, helper_result);
                }
                if (!termination.error.empty()) {
                    throw Error("Agent stop requested; " + termination.error);
                }
                throw Error("Agent stop requested");
            }
            if (wait_result != WAIT_TIMEOUT) {
                throw Error(
                    "WaitForMultipleObjects failed with Win32 error " +
                    std::to_string(GetLastError()));
            }
            DWORD termination_code = ERROR_SUCCESS;
            if (output_limit_exceeded(
                    standard_output.get(),
                    standard_error.get(),
                    request.max_output_bytes)) {
                result.output_limit_exceeded = true;
                termination_code = ERROR_FILE_TOO_LARGE;
            } else if (std::chrono::steady_clock::now() >= deadline) {
                result.timed_out = true;
                termination_code = ERROR_TIMEOUT;
            } else {
                continue;
            }
            const HelperTermination termination = terminate_helper_and_wait(
                job, helper_process.get(), assigned_to_job, termination_code);
            helper_exit_confirmed = termination.exited;
            if (!termination.exited || !termination.error.empty()) {
                throw Error(
                    "Interactive process cleanup failed" +
                    (termination.error.empty()
                        ? std::string{}
                        : std::string(": ") + termination.error));
            }
            break;
        }

        if (!result.timed_out && !result.output_limit_exceeded) {
            if (!std::filesystem::is_regular_file(windows_file_path(helper_result))) {
                throw Error("Interactive process helper exited without a result");
            }
            const nlohmann::json helper_response = load_json(helper_result);
            if (helper_response.value("schema_version", 0) != 1 ||
                helper_response.value("session_id", kInvalidSessionId) !=
                    state_->identity.session_id) {
                throw Error("Interactive process helper returned an invalid result");
            }
            const std::string status =
                helper_response.value("status", std::string{});
            if (status == "failed") {
                throw Error(helper_response.value(
                    "error",
                    std::string("Interactive process helper failed")));
            }
            if (status != "exited" ||
                !helper_response.contains("exit_code") ||
                !helper_response.at("exit_code").is_number_unsigned()) {
                throw Error("Interactive process helper returned an invalid result");
            }
            result.exit_code =
                helper_response.at("exit_code").get<std::uint32_t>();
        }
        result.duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start_time).count();
        remove_helper_protocol_files(helper_request, helper_result);
        return result;
    } catch (const std::exception& error) {
        const std::string original_error = error.what();
        HelperTermination termination{helper_exit_confirmed, {}};
        if (!helper_exit_confirmed) {
            termination = terminate_helper_and_wait(
                job,
                helper_process.get(),
                assigned_to_job,
                ERROR_PROCESS_ABORTED);
        }
        if (termination.exited) {
            remove_helper_protocol_files(helper_request, helper_result);
        }
        if (!termination.error.empty()) {
            throw Error(original_error + "; cleanup failed: " + termination.error);
        }
        throw;
    }
}

int run_interactive_process_helper(
    const std::filesystem::path& request_path) {
    std::filesystem::path result_path;
    try {
        const nlohmann::json request = load_json(request_path);
        if (request.value("schema_version", 0) != 1) {
            throw Error("Interactive process helper requires schema_version 1");
        }
        const DWORD expected_session = request.at("session_id").get<DWORD>();
        if (current_process_session_id() != expected_session) {
            throw Error("Interactive process helper started in the wrong Session");
        }
        result_path = path_from_utf8(request.at("result_path").get<std::string>());
        const std::filesystem::path program =
            path_from_utf8(request.at("program").get<std::string>());
        const std::filesystem::path working_directory =
            path_from_utf8(request.at("working_directory").get<std::string>());
        const std::vector<std::string> arguments =
            request.at("arguments").get<std::vector<std::string>>();
        const bool verbatim_arguments = request.value("verbatim_arguments", false);
        const std::map<std::string, std::string> environment_overrides =
            request.value("environment_overrides", std::map<std::string, std::string>{});

        UniqueHandle standard_input(reinterpret_cast<HANDLE>(
            static_cast<std::uintptr_t>(
                request.at("stdin_handle").get<std::uint64_t>())));
        UniqueHandle standard_output(reinterpret_cast<HANDLE>(
            static_cast<std::uintptr_t>(
                request.at("stdout_handle").get<std::uint64_t>())));
        UniqueHandle standard_error(reinterpret_cast<HANDLE>(
            static_cast<std::uintptr_t>(
                request.at("stderr_handle").get<std::uint64_t>())));
        if (!standard_input || !standard_output || !standard_error) {
            throw Error("Interactive process helper received an invalid standard handle");
        }

        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdInput = standard_input.get();
        startup.hStdOutput = standard_output.get();
        startup.hStdError = standard_error.get();
        PROCESS_INFORMATION process_info{};
        std::vector<wchar_t> command = verbatim_arguments
            ? build_windows_command_line_verbatim(program, arguments)
            : build_windows_command_line(program, arguments);
        std::vector<wchar_t> target_environment = environment_overrides.empty()
            ? std::vector<wchar_t>{}
            : build_process_environment(environment_overrides);
        ensure_win32(
            CreateProcessW(
                program.c_str(),
                command.data(),
                nullptr,
                nullptr,
                TRUE,
                CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT,
                target_environment.empty() ? nullptr : target_environment.data(),
                working_directory.c_str(),
                &startup,
                &process_info),
            "CreateProcessW(interactive target)");
        UniqueHandle process(process_info.hProcess);
        UniqueHandle thread(process_info.hThread);
        ensure_win32(
            WaitForSingleObject(process.get(), INFINITE) == WAIT_OBJECT_0,
            "WaitForSingleObject(interactive target)");
        DWORD exit_code = 0;
        ensure_win32(
            GetExitCodeProcess(process.get(), &exit_code),
            "GetExitCodeProcess(interactive target)");
        write_json_atomic(result_path, {
            {"schema_version", 1},
            {"session_id", expected_session},
            {"status", "exited"},
            {"exit_code", exit_code},
            {"error", ""},
        });
        return 0;
    } catch (const std::exception& error) {
        if (!result_path.empty()) {
            try {
                write_json_atomic(result_path, {
                    {"schema_version", 1},
                    {"session_id", current_process_session_id()},
                    {"status", "failed"},
                    {"exit_code", nullptr},
                    {"error", error.what()},
                });
            } catch (...) {
            }
        }
        return 1;
    }
}

#ifdef SATSUMA_INTERACTIVE_TESTS
void validate_interactive_session_id_for_test(const std::uint32_t session_id) {
    validate_interactive_session_id(session_id);
}

void validate_interactive_caller_for_test(const bool is_local_system) {
    validate_interactive_caller(is_local_system);
}

void set_interactive_session_unavailable_for_test(const bool unavailable) {
    g_interactive_session_unavailable = unavailable;
}

void set_interactive_identity_changed_for_test(const bool changed) {
    g_interactive_identity_changed = changed;
}

void set_interactive_resume_identity_changed_for_test(const bool changed) {
    g_resume_identity_changed = changed;
}

std::uint32_t last_interactive_helper_pid_for_test() noexcept {
    return g_last_helper_pid;
}
#endif

}  // namespace satsuma::vm
