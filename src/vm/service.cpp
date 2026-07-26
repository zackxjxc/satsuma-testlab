// SatsumaVM Windows Service 生命周期和安装实现。
#include "service.hpp"

#include <atomic>
#include <chrono>
#include <fstream>
#include <functional>
#include <mutex>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <windows.h>

#include "agent.hpp"
#include "satsuma/core/config.hpp"
#include "satsuma/core/errors.hpp"
#include "satsuma/core/path.hpp"
#include "satsuma/core/windows_command_line.hpp"

namespace satsuma::vm {
namespace {

constexpr std::wstring_view kServiceDescription =
    L"Runs the Satsuma TestLab file-channel Agent inside the guest.";
constexpr DWORD kServiceStartWaitMs = 30'000;
constexpr DWORD kServiceStopWaitMs = 20'000;
constexpr DWORD kServiceDeleteWaitMs = 15'000;

// 自动释放 SCM handle。
class UniqueServiceHandle {
public:
    // 接管一个可空 SC_HANDLE。
    explicit UniqueServiceHandle(SC_HANDLE value = nullptr) : value_(value) {}

    UniqueServiceHandle(const UniqueServiceHandle&) = delete;
    UniqueServiceHandle& operator=(const UniqueServiceHandle&) = delete;

    // 允许工厂函数转移 SCM handle。
    UniqueServiceHandle(UniqueServiceHandle&& other) noexcept : value_(other.release()) {}

    // 关闭旧值后接管新 SCM handle。
    UniqueServiceHandle& operator=(UniqueServiceHandle&& other) noexcept {
        if (this != &other) {
            reset(other.release());
        }
        return *this;
    }

    // 关闭有效 SCM handle。
    ~UniqueServiceHandle() {
        reset();
    }

    // 返回底层 SCM handle。
    [[nodiscard]] SC_HANDLE get() const noexcept {
        return value_;
    }

    // 返回 SCM handle 是否有效。
    [[nodiscard]] explicit operator bool() const noexcept {
        return value_ != nullptr;
    }

    // 放弃所有权但不关闭 SCM handle。
    [[nodiscard]] SC_HANDLE release() noexcept {
        const SC_HANDLE value = value_;
        value_ = nullptr;
        return value;
    }

    // 关闭旧值并保存新 SCM handle。
    void reset(SC_HANDLE value = nullptr) noexcept {
        if (value_ != nullptr) {
            CloseServiceHandle(value_);
        }
        value_ = value;
    }

private:
    SC_HANDLE value_ = nullptr;  // 被管理的 SCM handle
};

// 检查 Win32 BOOL 返回值并附带管理员提示。
void ensure_win32(const BOOL success, const char* operation) {
    if (success) {
        return;
    }
    const DWORD error = GetLastError();
    std::string message =
        std::string(operation) + " failed with Win32 error " + std::to_string(error);
    if (error == ERROR_ACCESS_DENIED) {
        message += "; run SatsumaVM as administrator";
    }
    throw Error(message);
}

// 返回当前进程对应的可执行文件绝对路径。
[[nodiscard]] std::filesystem::path current_executable_path() {
    std::wstring buffer(32'768, L'\0');
    const DWORD length = GetModuleFileNameW(
        nullptr,
        buffer.data(),
        static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size()) {
        throw Error(
            "Cannot resolve SatsumaVM executable path (Win32 error " +
            std::to_string(GetLastError()) + ")");
    }
    buffer.resize(length);
    return std::filesystem::path(std::move(buffer));
}

// 拒绝不存在的 Service 输入文件并返回规范绝对路径。
[[nodiscard]] std::filesystem::path normalize_input_file(
    const std::filesystem::path& value,
    const char* label) {
    const std::filesystem::path normalized = std::filesystem::canonical(value);
    if (!std::filesystem::is_regular_file(normalized)) {
        throw Error(std::string(label) + " is not a regular file: " + path_to_utf8(normalized));
    }
    return normalized;
}

// 构造允许配置文件已缺失的卸载规范。
[[nodiscard]] AgentServiceSpec make_agent_service_removal_spec(
    const std::filesystem::path& executable,
    const std::filesystem::path& config) {
    AgentServiceSpec spec;
    spec.executable = normalize_input_file(executable, "SatsumaVM executable");
    spec.config = std::filesystem::absolute(config).lexically_normal();
    spec.working_directory = spec.executable.parent_path();
    spec.binary_path =
        L"\"" + spec.executable.native() + L"\" --config " +
        quote_windows_argument(spec.config.native()) + L" --service";
    return spec;
}

// 使用 Windows 路径语义比较 SCM 文本。
[[nodiscard]] bool text_equal_case_insensitive(
    const wchar_t* left,
    const std::wstring_view right) {
    return left != nullptr && _wcsicmp(left, std::wstring(right).c_str()) == 0;
}

// 判断 SCM 可选字符串是否为空。
[[nodiscard]] bool empty_text(const wchar_t* value) {
    return value == nullptr || *value == L'\0';
}

// 判断 Service 账户是否为默认 LocalSystem。
[[nodiscard]] bool is_local_system_account(const wchar_t* value) {
    return empty_text(value) ||
           _wcsicmp(value, L"LocalSystem") == 0 ||
           _wcsicmp(value, L"NT AUTHORITY\\LocalSystem") == 0;
}

// 查询可变长度的 QueryServiceConfigW 数据。
[[nodiscard]] std::vector<BYTE> query_service_config(const SC_HANDLE service) {
    DWORD bytes_needed = 0;
    QueryServiceConfigW(service, nullptr, 0, &bytes_needed);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || bytes_needed == 0) {
        ensure_win32(FALSE, "QueryServiceConfigW(size)");
    }
    std::vector<BYTE> buffer(bytes_needed);
    ensure_win32(
        QueryServiceConfigW(
            service,
            reinterpret_cast<QUERY_SERVICE_CONFIGW*>(buffer.data()),
            bytes_needed,
            &bytes_needed),
        "QueryServiceConfigW");
    return buffer;
}

// 查询可变长度的 QueryServiceConfig2W 数据。
[[nodiscard]] std::vector<BYTE> query_service_config2(
    const SC_HANDLE service,
    const DWORD level) {
    DWORD bytes_needed = 0;
    QueryServiceConfig2W(service, level, nullptr, 0, &bytes_needed);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || bytes_needed == 0) {
        ensure_win32(FALSE, "QueryServiceConfig2W(size)");
    }
    std::vector<BYTE> buffer(bytes_needed);
    ensure_win32(
        QueryServiceConfig2W(
            service,
            level,
            buffer.data(),
            bytes_needed,
            &bytes_needed),
        "QueryServiceConfig2W");
    return buffer;
}

// 查询一个固定大小的 Service 配置值。
template <typename Value>
[[nodiscard]] Value query_service_config2_value(
    const SC_HANDLE service,
    const DWORD level) {
    Value value{};
    DWORD bytes_needed = 0;
    ensure_win32(
        QueryServiceConfig2W(
            service,
            level,
            reinterpret_cast<BYTE*>(&value),
            sizeof(value),
            &bytes_needed),
        "QueryServiceConfig2W");
    return value;
}

// 精确确认同名 Service 使用预期 Agent 命令。
[[nodiscard]] bool service_belongs_to_agent(
    const QUERY_SERVICE_CONFIGW& config,
    const AgentServiceSpec& spec) {
    return text_equal_case_insensitive(config.lpBinaryPathName, spec.binary_path);
}

// 检查 Service 基础配置和恢复策略。
[[nodiscard]] bool service_definition_matches(
    const SC_HANDLE service,
    const AgentServiceSpec& spec) {
    const std::vector<BYTE> config_buffer = query_service_config(service);
    const auto* config = reinterpret_cast<const QUERY_SERVICE_CONFIGW*>(config_buffer.data());
    if (config->dwServiceType != SERVICE_WIN32_OWN_PROCESS ||
        config->dwStartType != SERVICE_AUTO_START ||
        config->dwErrorControl != SERVICE_ERROR_NORMAL ||
        !text_equal_case_insensitive(config->lpBinaryPathName, spec.binary_path) ||
        !text_equal_case_insensitive(config->lpDisplayName, kAgentServiceDisplayName) ||
        !is_local_system_account(config->lpServiceStartName)) {
        return false;
    }

    const auto delayed = query_service_config2_value<SERVICE_DELAYED_AUTO_START_INFO>(
        service,
        SERVICE_CONFIG_DELAYED_AUTO_START_INFO);
    const auto failure_flag = query_service_config2_value<SERVICE_FAILURE_ACTIONS_FLAG>(
        service,
        SERVICE_CONFIG_FAILURE_ACTIONS_FLAG);
    if ((delayed.fDelayedAutostart != FALSE) != spec.delayed_auto_start ||
        (failure_flag.fFailureActionsOnNonCrashFailures != FALSE) !=
            spec.restart_on_non_crash) {
        return false;
    }

    const std::vector<BYTE> description_buffer = query_service_config2(
        service,
        SERVICE_CONFIG_DESCRIPTION);
    const auto* description =
        reinterpret_cast<const SERVICE_DESCRIPTIONW*>(description_buffer.data());
    if (!text_equal_case_insensitive(description->lpDescription, kServiceDescription)) {
        return false;
    }

    const std::vector<BYTE> failure_buffer = query_service_config2(
        service,
        SERVICE_CONFIG_FAILURE_ACTIONS);
    const auto* failure =
        reinterpret_cast<const SERVICE_FAILURE_ACTIONSW*>(failure_buffer.data());
    if (failure->dwResetPeriod != spec.failure_reset_seconds ||
        !empty_text(failure->lpRebootMsg) ||
        !empty_text(failure->lpCommand) ||
        failure->cActions != spec.restart_delays_ms.size() ||
        failure->lpsaActions == nullptr) {
        return false;
    }
    for (std::size_t index = 0; index < spec.restart_delays_ms.size(); ++index) {
        if (failure->lpsaActions[index].Type != SC_ACTION_RESTART ||
            failure->lpsaActions[index].Delay != spec.restart_delays_ms[index]) {
            return false;
        }
    }
    return true;
}

// 写入 Service 基础配置和恢复策略。
void configure_service(const SC_HANDLE service, const AgentServiceSpec& spec) {
    ensure_win32(
        ChangeServiceConfigW(
            service,
            SERVICE_WIN32_OWN_PROCESS,
            SERVICE_AUTO_START,
            SERVICE_ERROR_NORMAL,
            spec.binary_path.c_str(),
            nullptr,
            nullptr,
            nullptr,
            L"LocalSystem",
            nullptr,
            kAgentServiceDisplayName.data()),
        "ChangeServiceConfigW");

    SERVICE_DESCRIPTIONW description{
        const_cast<wchar_t*>(kServiceDescription.data()),
    };
    ensure_win32(
        ChangeServiceConfig2W(service, SERVICE_CONFIG_DESCRIPTION, &description),
        "ChangeServiceConfig2W(description)");

    SERVICE_DELAYED_AUTO_START_INFO delayed{
        spec.delayed_auto_start ? TRUE : FALSE,
    };
    ensure_win32(
        ChangeServiceConfig2W(
            service,
            SERVICE_CONFIG_DELAYED_AUTO_START_INFO,
            &delayed),
        "ChangeServiceConfig2W(delayed start)");

    std::array<SC_ACTION, 3> actions{};
    for (std::size_t index = 0; index < actions.size(); ++index) {
        actions[index].Type = SC_ACTION_RESTART;
        actions[index].Delay = spec.restart_delays_ms[index];
    }
    SERVICE_FAILURE_ACTIONSW failure{};
    failure.dwResetPeriod = spec.failure_reset_seconds;
    failure.cActions = static_cast<DWORD>(actions.size());
    failure.lpsaActions = actions.data();
    ensure_win32(
        ChangeServiceConfig2W(service, SERVICE_CONFIG_FAILURE_ACTIONS, &failure),
        "ChangeServiceConfig2W(failure actions)");

    SERVICE_FAILURE_ACTIONS_FLAG failure_flag{
        spec.restart_on_non_crash ? TRUE : FALSE,
    };
    ensure_win32(
        ChangeServiceConfig2W(
            service,
            SERVICE_CONFIG_FAILURE_ACTIONS_FLAG,
            &failure_flag),
        "ChangeServiceConfig2W(failure flag)");
}

// 查询当前 Service 状态和进程 ID。
[[nodiscard]] SERVICE_STATUS_PROCESS query_service_status(const SC_HANDLE service) {
    SERVICE_STATUS_PROCESS status{};
    DWORD bytes_needed = 0;
    ensure_win32(
        QueryServiceStatusEx(
            service,
            SC_STATUS_PROCESS_INFO,
            reinterpret_cast<BYTE*>(&status),
            sizeof(status),
            &bytes_needed),
        "QueryServiceStatusEx");
    return status;
}

// 等待 Service 到达指定状态。
[[nodiscard]] SERVICE_STATUS_PROCESS wait_for_service_state(
    const SC_HANDLE service,
    const DWORD desired_state,
    const DWORD timeout_ms) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    for (;;) {
        const SERVICE_STATUS_PROCESS status = query_service_status(service);
        if (status.dwCurrentState == desired_state) {
            return status;
        }
        if (desired_state == SERVICE_RUNNING && status.dwCurrentState == SERVICE_STOPPED) {
            throw Error(
                "SatsumaVM service stopped during startup (Win32 exit " +
                std::to_string(status.dwWin32ExitCode) + ", service exit " +
                std::to_string(status.dwServiceSpecificExitCode) + ")");
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            throw Error("Timed out while waiting for the SatsumaVM service state");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

// 停止正在运行或启动中的 Service。
void stop_service(const SC_HANDLE service) {
    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::milliseconds(kServiceStartWaitMs + kServiceStopWaitMs);
    bool stop_sent = false;
    for (;;) {
        const SERVICE_STATUS_PROCESS status = query_service_status(service);
        if (status.dwCurrentState == SERVICE_STOPPED) {
            return;
        }

        // START_PENDING 不接受 STOP，先等待它进入可停止状态。
        if (!stop_sent &&
            status.dwCurrentState != SERVICE_START_PENDING &&
            status.dwCurrentState != SERVICE_STOP_PENDING) {
            SERVICE_STATUS control_status{};
            if (ControlService(service, SERVICE_CONTROL_STOP, &control_status)) {
                stop_sent = true;
            } else {
                const DWORD error = GetLastError();
                if (error != ERROR_SERVICE_NOT_ACTIVE &&
                    error != ERROR_SERVICE_CANNOT_ACCEPT_CTRL) {
                    SetLastError(error);
                    ensure_win32(FALSE, "ControlService(STOP)");
                }
            }
        }

        if (std::chrono::steady_clock::now() >= deadline) {
            throw Error(
                "Timed out while stopping the SatsumaVM service (state " +
                std::to_string(status.dwCurrentState) + ")");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

// 启动 Service 并等待 SCM 返回有效进程 ID。
[[nodiscard]] std::uint32_t start_service(const SC_HANDLE service) {
    SERVICE_STATUS_PROCESS status = query_service_status(service);
    if (status.dwCurrentState == SERVICE_RUNNING) {
        return status.dwProcessId;
    }
    if (status.dwCurrentState == SERVICE_STOP_PENDING) {
        status = wait_for_service_state(service, SERVICE_STOPPED, kServiceStopWaitMs);
    }
    if (status.dwCurrentState != SERVICE_START_PENDING) {
        if (!StartServiceW(service, 0, nullptr)) {
            const DWORD error = GetLastError();
            if (error != ERROR_SERVICE_ALREADY_RUNNING) {
                SetLastError(error);
                ensure_win32(FALSE, "StartServiceW");
            }
        }
    }
    status = wait_for_service_state(service, SERVICE_RUNNING, kServiceStartWaitMs);
    if (status.dwProcessId == 0) {
        throw Error("SCM returned an invalid SatsumaVM service process ID");
    }
    return status.dwProcessId;
}

// 等待已标记删除的 Service 真正从 SCM 消失。
void wait_for_service_absent(const SC_HANDLE manager) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(kServiceDeleteWaitMs);
    for (;;) {
        UniqueServiceHandle probe(OpenServiceW(
            manager,
            kAgentServiceName.data(),
            SERVICE_QUERY_STATUS));
        if (!probe) {
            const DWORD error = GetLastError();
            if (error == ERROR_SERVICE_DOES_NOT_EXIST) {
                return;
            }
            if (error != ERROR_SERVICE_MARKED_FOR_DELETE) {
                throw Error(
                    "Cannot confirm SatsumaVM service deletion (Win32 error " +
                    std::to_string(error) + ")");
            }
        }
        probe.reset();
        if (std::chrono::steady_clock::now() >= deadline) {
            throw Error("Timed out while confirming SatsumaVM service deletion");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

// 打开本机 SCM。
[[nodiscard]] UniqueServiceHandle open_service_manager(const DWORD access) {
    UniqueServiceHandle manager(OpenSCManagerW(nullptr, nullptr, access));
    if (!manager) {
        ensure_win32(FALSE, "OpenSCManagerW");
    }
    return manager;
}

// 验证配置中的 work 路径使用同一固定安装根。
void validate_local_work_root(
    const AgentServiceSpec& spec,
    const std::filesystem::path& local_work_root) {
    const std::filesystem::path expected =
        std::filesystem::absolute(spec.config.parent_path() / L"work").lexically_normal();
    const std::filesystem::path actual =
        std::filesystem::absolute(local_work_root).lexically_normal();
    if (_wcsicmp(expected.c_str(), actual.c_str()) != 0 ||
        !std::filesystem::is_directory(actual)) {
        throw Error("Agent local_work_root must use the fixed install work directory");
    }
}

// 返回指定配置安装根中的 Service 启动错误日志。
[[nodiscard]] std::filesystem::path service_startup_log_path(
    const std::filesystem::path& config_path) {
    return std::filesystem::absolute(config_path).parent_path() /
        L"agent-startup-error.log";
}

// 尽力追加 Service 启动错误。
void append_service_startup_error(
    const std::filesystem::path& config_path,
    const std::string& message) noexcept {
    try {
        std::ofstream output(
            service_startup_log_path(config_path),
            std::ios::binary | std::ios::app);
        if (output) {
            output << message << '\n';
        }
    } catch (...) {
    }
}

// 构造 SCM 状态字段。
[[nodiscard]] SERVICE_STATUS make_service_status(
    const DWORD state,
    const DWORD win32_exit_code = NO_ERROR,
    const DWORD service_exit_code = 0,
    const DWORD checkpoint = 0,
    const DWORD wait_hint_ms = 0) {
    SERVICE_STATUS status{};
    status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    status.dwCurrentState = state;
    status.dwControlsAccepted = state == SERVICE_RUNNING
        ? SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN
        : 0;
    status.dwWin32ExitCode = win32_exit_code;
    status.dwServiceSpecificExitCode = service_exit_code;
    status.dwCheckPoint = checkpoint;
    status.dwWaitHint = wait_hint_ms;
    return status;
}

// ServiceMain 与控制处理器共享的状态。
struct ServiceContext {
    std::filesystem::path config_path;                 // SCM 命令行提供的配置
    SERVICE_STATUS_HANDLE status_handle{};             // SCM 状态上报句柄
    SERVICE_STATUS status = make_service_status(       // 注册 Handler 前的可观察状态
        SERVICE_START_PENDING,
        NO_ERROR,
        0,
        1,
        20'000);
    std::mutex status_mutex;                           // 串行化状态迁移
    std::stop_source stop_source;                      // Agent 生命周期停止源
    std::function<void(const SERVICE_STATUS&)> sink =  // 可替换的状态上报端
        [](const SERVICE_STATUS&) {};
    int process_exit_code{1};                          // dispatcher 最终退出码
    bool final_status_latched{false};                  // SERVICE_STOPPED 是否已门闩
};

// 串行上报非终态 SCM 状态。
[[nodiscard]] bool report_service_status(
    ServiceContext& context,
    const DWORD state,
    const DWORD win32_exit_code = NO_ERROR,
    const DWORD service_exit_code = 0,
    const DWORD checkpoint = 0,
    const DWORD wait_hint_ms = 0) {
    std::scoped_lock lock(context.status_mutex);
    if (context.final_status_latched) {
        return false;
    }
    if (state == SERVICE_STOPPED) {
        throw Error("SERVICE_STOPPED must use the final status latch");
    }
    if (state == SERVICE_RUNNING && context.stop_source.stop_requested()) {
        return false;
    }
    context.status = make_service_status(
        state,
        win32_exit_code,
        service_exit_code,
        checkpoint,
        wait_hint_ms);
    context.sink(context.status);
    return true;
}

// ServiceMain 清理后独立上报的终态副本。
struct FinalServiceStatus {
    SERVICE_STATUS_HANDLE handle{};  // 已安全发布的 SCM 句柄
    SERVICE_STATUS status{};         // 独立于 ServiceContext 的终态副本
    bool ready{false};               // 本次是否获得唯一终态上报权
};

// 在锁内先门闩终态，再返回可独立上报的拷贝。
[[nodiscard]] FinalServiceStatus latch_final_service_status(
    ServiceContext& context,
    const DWORD win32_exit_code,
    const DWORD service_exit_code) {
    FinalServiceStatus final_status;
    std::scoped_lock lock(context.status_mutex);
    if (context.final_status_latched) {
        return final_status;
    }
    context.status = make_service_status(
        SERVICE_STOPPED,
        win32_exit_code,
        service_exit_code);
    context.final_status_latched = true;
    final_status.handle = context.status_handle;
    final_status.status = context.status;
    final_status.ready = true;
    return final_status;
}

// 不改变字段地重报当前状态。
void report_current_service_status(ServiceContext& context) {
    std::scoped_lock lock(context.status_mutex);
    if (!context.final_status_latched) {
        context.sink(context.status);
    }
}

// 处理 STOP、SHUTDOWN 和 INTERROGATE 控制码。
[[nodiscard]] DWORD handle_service_control(
    const DWORD control,
    ServiceContext& context) {
    switch (control) {
        case SERVICE_CONTROL_STOP:
        case SERVICE_CONTROL_SHUTDOWN:
            {
                std::scoped_lock lock(context.status_mutex);
                if (context.final_status_latched) {
                    return NO_ERROR;
                }
                context.stop_source.request_stop();
                if (context.status.dwCurrentState != SERVICE_STOP_PENDING) {
                    context.status = make_service_status(
                        SERVICE_STOP_PENDING,
                        NO_ERROR,
                        0,
                        1,
                        10'000);
                    context.sink(context.status);
                }
            }
            return NO_ERROR;
        case SERVICE_CONTROL_INTERROGATE:
            report_current_service_status(context);
            return NO_ERROR;
        default:
            return ERROR_CALL_NOT_IMPLEMENTED;
    }
}

// 将 SCM 控制回调转发到当前 ServiceContext。
DWORD WINAPI service_control_handler(
    const DWORD control,
    const DWORD,
    void*,
    void* raw_context) noexcept {
    try {
        return handle_service_control(control, *static_cast<ServiceContext*>(raw_context));
    } catch (...) {
        return ERROR_GEN_FAILURE;
    }
}

// ServiceMain 清理完成后的终态。
struct ServiceRunResult {
    DWORD win32_exit_code{NO_ERROR};  // SCM 通用退出码
    DWORD service_exit_code{};        // Service 专用退出码
};

// 运行实际 ServiceMain 生命周期，并在返回前完成 Agent 清理。
[[nodiscard]] ServiceRunResult run_service_main(ServiceContext& context) noexcept {
    std::string failure_message;
    DWORD final_win32_exit = NO_ERROR;
    DWORD final_service_exit = 0;
    try {
        const SERVICE_STATUS_HANDLE status_handle = RegisterServiceCtrlHandlerExW(
            kAgentServiceName.data(),
            service_control_handler,
            &context);
        if (status_handle == nullptr) {
            throw Error(
                "RegisterServiceCtrlHandlerExW failed with Win32 error " +
                std::to_string(GetLastError()));
        }
        {
            std::scoped_lock lock(context.status_mutex);
            context.status_handle = status_handle;
            context.sink = [status_handle](const SERVICE_STATUS& status) {
                SERVICE_STATUS mutable_status = status;
                ensure_win32(
                    SetServiceStatus(status_handle, &mutable_status),
                    "SetServiceStatus");
            };
            // Handler 可在注册返回前请求 STOP，此处发布它留下的当前状态。
            context.sink(context.status);
        }

        if (!context.stop_source.stop_requested()) {
            const std::filesystem::path executable = current_executable_path();
            ensure_win32(
                SetCurrentDirectoryW(executable.parent_path().c_str()),
                "SetCurrentDirectoryW");
            AgentConfig config = load_agent_config(context.config_path);
            Agent agent(std::move(config));
            static_cast<void>(report_service_status(context, SERVICE_RUNNING));
            agent.run_watch(context.stop_source.get_token());
        }
        context.process_exit_code = 0;
    } catch (const std::exception& error) {
        failure_message = error.what();
        final_win32_exit = ERROR_SERVICE_SPECIFIC_ERROR;
        final_service_exit = 1;
        context.process_exit_code = 1;
    } catch (...) {
        failure_message = "unknown Service exception";
        final_win32_exit = ERROR_SERVICE_SPECIFIC_ERROR;
        final_service_exit = 1;
        context.process_exit_code = 1;
    }

    if (!failure_message.empty()) {
        append_service_startup_error(context.config_path, failure_message);
    }
    return {final_win32_exit, final_service_exit};
}

std::mutex dispatcher_mutex;               // 防止同进程重复进入 dispatcher
ServiceContext* dispatcher_context{};      // ServiceMain 当前上下文
std::atomic<int> dispatcher_exit_code{1};  // STOPPED 前确定的进程退出码

// StartServiceCtrlDispatcherW 使用的无异常回调。
void WINAPI service_main_callback(DWORD, wchar_t**) noexcept {
    if (dispatcher_context == nullptr) {
        return;
    }
    const ServiceRunResult result = run_service_main(*dispatcher_context);
    dispatcher_exit_code.store(
        dispatcher_context->process_exit_code,
        std::memory_order_release);
    const FinalServiceStatus final_status = latch_final_service_status(
        *dispatcher_context,
        result.win32_exit_code,
        result.service_exit_code);
    if (!final_status.ready || final_status.handle == nullptr) {
        return;
    }
    SERVICE_STATUS status = final_status.status;
    SetServiceStatus(final_status.handle, &status);
}

#ifdef SATSUMA_SERVICE_TESTS
// 将 Win32 状态复制为测试可观察值。
[[nodiscard]] AgentServiceStatusSnapshot snapshot_service_status(
    const SERVICE_STATUS& status) {
    return {
        status.dwCurrentState,
        status.dwControlsAccepted,
        status.dwWin32ExitCode,
        status.dwServiceSpecificExitCode,
        status.dwCheckPoint,
        status.dwWaitHint,
    };
}
#endif

}  // namespace

AgentServiceSpec make_agent_service_spec(
    const std::filesystem::path& executable,
    const std::filesystem::path& config) {
    AgentServiceSpec spec;
    spec.executable = normalize_input_file(executable, "SatsumaVM executable");
    spec.config = normalize_input_file(config, "Agent config");
    spec.working_directory = spec.executable.parent_path();
    spec.binary_path =
        L"\"" + spec.executable.native() + L"\" --config " +
        quote_windows_argument(spec.config.native()) + L" --service";
    return spec;
}

int run_agent_service_dispatcher(const std::filesystem::path& config) {
    std::scoped_lock lock(dispatcher_mutex);
    auto* const context = new ServiceContext;
    context->config_path = config;
    dispatcher_context = context;
    dispatcher_exit_code.store(1, std::memory_order_relaxed);
    SERVICE_TABLE_ENTRYW table[] = {
        {const_cast<wchar_t*>(kAgentServiceName.data()), service_main_callback},
        {nullptr, nullptr},
    };
    if (!StartServiceCtrlDispatcherW(table)) {
        const DWORD error = GetLastError();
        dispatcher_context = nullptr;
        delete context;
        return static_cast<int>(error);
    }
    // 成功上报 STOPPED 后 SCM 可立即终止进程，故意不再访问或析构 context。
    return dispatcher_exit_code.load(std::memory_order_acquire);
}

AgentServiceResult ensure_agent_service(
    const std::filesystem::path& config,
    const std::filesystem::path& local_work_root,
    const bool start_now) {
    const AgentServiceSpec spec = make_agent_service_spec(
        current_executable_path(),
        config);
    validate_local_work_root(spec, local_work_root);
    const UniqueServiceHandle manager = open_service_manager(
        SC_MANAGER_CONNECT | SC_MANAGER_CREATE_SERVICE);

    constexpr DWORD access =
        SERVICE_QUERY_CONFIG | SERVICE_CHANGE_CONFIG | SERVICE_QUERY_STATUS |
        SERVICE_START | SERVICE_STOP;
    UniqueServiceHandle service(OpenServiceW(
        manager.get(),
        kAgentServiceName.data(),
        access));
    bool existed = static_cast<bool>(service);
    if (!existed && GetLastError() != ERROR_SERVICE_DOES_NOT_EXIST) {
        ensure_win32(FALSE, "OpenServiceW");
    }

    bool definition_matches = false;
    bool was_running = false;
    if (existed) {
        const std::vector<BYTE> existing_buffer = query_service_config(service.get());
        const auto* existing =
            reinterpret_cast<const QUERY_SERVICE_CONFIGW*>(existing_buffer.data());
        if (!service_belongs_to_agent(*existing, spec)) {
            throw Error("Existing SatsumaVM service points to a different command");
        }
        definition_matches = service_definition_matches(service.get(), spec);
        was_running = query_service_status(service.get()).dwCurrentState != SERVICE_STOPPED;
    } else {
        service.reset(CreateServiceW(
            manager.get(),
            kAgentServiceName.data(),
            kAgentServiceDisplayName.data(),
            access | DELETE,
            SERVICE_WIN32_OWN_PROCESS,
            SERVICE_AUTO_START,
            SERVICE_ERROR_NORMAL,
            spec.binary_path.c_str(),
            nullptr,
            nullptr,
            nullptr,
            L"LocalSystem",
            nullptr));
        if (!service) {
            ensure_win32(FALSE, "CreateServiceW");
        }
    }

    try {
        if (existed && !definition_matches && was_running) {
            stop_service(service.get());
        }
        if (!definition_matches) {
            configure_service(service.get(), spec);
        }

        const bool should_run = start_now || was_running;
        std::uint32_t process_id = 0;
        if (should_run) {
            process_id = start_service(service.get());
        } else {
            const SERVICE_STATUS_PROCESS status = query_service_status(service.get());
            if (status.dwCurrentState == SERVICE_RUNNING) {
                process_id = status.dwProcessId;
            }
        }
        return {
            existed
                ? (definition_matches ? ServiceChange::Unchanged : ServiceChange::Updated)
                : ServiceChange::Created,
            "SatsumaVM",
            should_run,
            process_id,
        };
    } catch (...) {
        if (!existed) {
            try {
                stop_service(service.get());
            } catch (...) {
            }
            DeleteService(service.get());
        }
        throw;
    }
}

AgentServiceStopResult stop_owned_agent_service(
    const std::filesystem::path& executable,
    const std::filesystem::path& config) {
    const UniqueServiceHandle manager = open_service_manager(SC_MANAGER_CONNECT);
    constexpr DWORD access =
        SERVICE_QUERY_CONFIG | SERVICE_QUERY_STATUS | SERVICE_STOP;
    const UniqueServiceHandle service(OpenServiceW(
        manager.get(),
        kAgentServiceName.data(),
        access));
    if (!service) {
        if (GetLastError() == ERROR_SERVICE_DOES_NOT_EXIST) {
            return {};
        }
        ensure_win32(FALSE, "OpenServiceW");
    }

    const AgentServiceSpec spec = make_agent_service_spec(executable, config);
    const std::vector<BYTE> existing_buffer = query_service_config(service.get());
    const auto* existing =
        reinterpret_cast<const QUERY_SERVICE_CONFIGW*>(existing_buffer.data());
    if (!service_belongs_to_agent(*existing, spec)) {
        throw Error("Existing SatsumaVM service points to a different command");
    }
    const bool was_active =
        query_service_status(service.get()).dwCurrentState != SERVICE_STOPPED;
    stop_service(service.get());
    return {true, was_active};
}

std::uint32_t start_owned_agent_service(
    const std::filesystem::path& executable,
    const std::filesystem::path& config) {
    const UniqueServiceHandle manager = open_service_manager(SC_MANAGER_CONNECT);
    constexpr DWORD access =
        SERVICE_QUERY_CONFIG | SERVICE_QUERY_STATUS | SERVICE_START;
    const UniqueServiceHandle service(OpenServiceW(
        manager.get(),
        kAgentServiceName.data(),
        access));
    if (!service) {
        if (GetLastError() == ERROR_SERVICE_DOES_NOT_EXIST) {
            throw Error("SatsumaVM service does not exist");
        }
        ensure_win32(FALSE, "OpenServiceW");
    }

    const AgentServiceSpec spec = make_agent_service_spec(executable, config);
    const std::vector<BYTE> existing_buffer = query_service_config(service.get());
    const auto* existing =
        reinterpret_cast<const QUERY_SERVICE_CONFIGW*>(existing_buffer.data());
    if (!service_belongs_to_agent(*existing, spec)) {
        throw Error("Existing SatsumaVM service points to a different command");
    }
    return start_service(service.get());
}

bool remove_agent_service(const std::filesystem::path& config) {
    const AgentServiceSpec spec = make_agent_service_removal_spec(
        current_executable_path(),
        config);
    const UniqueServiceHandle manager = open_service_manager(SC_MANAGER_CONNECT);
    constexpr DWORD access =
        SERVICE_QUERY_CONFIG | SERVICE_QUERY_STATUS | SERVICE_STOP | DELETE;
    UniqueServiceHandle service(OpenServiceW(
        manager.get(),
        kAgentServiceName.data(),
        access));
    if (!service) {
        if (GetLastError() == ERROR_SERVICE_DOES_NOT_EXIST) {
            return false;
        }
        ensure_win32(FALSE, "OpenServiceW");
    }

    const std::vector<BYTE> existing_buffer = query_service_config(service.get());
    const auto* existing =
        reinterpret_cast<const QUERY_SERVICE_CONFIGW*>(existing_buffer.data());
    if (!service_belongs_to_agent(*existing, spec)) {
        throw Error("Existing SatsumaVM service points to a different command");
    }
    stop_service(service.get());
    ensure_win32(DeleteService(service.get()), "DeleteService");
    service.reset();
    wait_for_service_absent(manager.get());
    return true;
}

#ifdef SATSUMA_SERVICE_TESTS
std::vector<AgentServiceStatusSnapshot>
agent_service_status_sequence_for_test(const bool fail) {
    ServiceContext context;
    std::vector<AgentServiceStatusSnapshot> states;
    context.sink = [&states](const SERVICE_STATUS& status) {
        states.push_back(snapshot_service_status(status));
    };
    static_cast<void>(
        report_service_status(context, SERVICE_START_PENDING, NO_ERROR, 0, 1, 20'000));
    static_cast<void>(report_service_status(context, SERVICE_RUNNING));
    const auto final_sink = context.sink;
    FinalServiceStatus final_status;
    if (fail) {
        final_status = latch_final_service_status(
            context,
            ERROR_SERVICE_SPECIFIC_ERROR,
            1);
    } else {
        static_cast<void>(report_service_status(
            context,
            SERVICE_STOP_PENDING,
            NO_ERROR,
            0,
            1,
            10'000));
        final_status = latch_final_service_status(context, NO_ERROR, 0);
    }
    if (!final_status.ready) {
        throw Error("Service test could not latch SERVICE_STOPPED");
    }
    final_sink(final_status.status);
    static_cast<void>(handle_service_control(SERVICE_CONTROL_INTERROGATE, context));
    static_cast<void>(handle_service_control(SERVICE_CONTROL_STOP, context));
    if (context.stop_source.stop_requested()) {
        throw Error("Service control changed state after SERVICE_STOPPED");
    }
    return states;
}

AgentServiceControlTestResult agent_service_control_for_test(
    const std::uint32_t control) {
    ServiceContext context;
    AgentServiceControlTestResult result;
    context.sink = [&result](const SERVICE_STATUS& status) {
        result.states.push_back(snapshot_service_status(status));
    };
    static_cast<void>(report_service_status(context, SERVICE_RUNNING));
    result.handler_result = handle_service_control(control, context);
    result.stop_requested = context.stop_source.stop_requested();
    return result;
}

bool agent_service_binary_belongs_for_test(
    const std::wstring& binary_path,
    const AgentServiceSpec& spec) {
    QUERY_SERVICE_CONFIGW config{};
    config.lpBinaryPathName = const_cast<wchar_t*>(binary_path.c_str());
    return service_belongs_to_agent(config, spec);
}

bool agent_service_stop_survives_report_failure_for_test() {
    ServiceContext context;
    context.sink = [](const SERVICE_STATUS&) {
        throw Error("injected status failure");
    };
    const DWORD result = service_control_handler(
        SERVICE_CONTROL_STOP,
        0,
        nullptr,
        &context);
    return result == ERROR_GEN_FAILURE && context.stop_source.stop_requested();
}

std::vector<AgentServiceStatusSnapshot>
agent_service_registration_window_sequence_for_test() {
    ServiceContext context;
    std::vector<AgentServiceStatusSnapshot> states;
    context.sink = [&states](const SERVICE_STATUS& status) {
        states.push_back(snapshot_service_status(status));
    };
    static_cast<void>(handle_service_control(SERVICE_CONTROL_INTERROGATE, context));
    static_cast<void>(handle_service_control(SERVICE_CONTROL_STOP, context));
    if (report_service_status(context, SERVICE_RUNNING)) {
        throw Error("Service entered RUNNING after a registration-window STOP");
    }
    const auto final_sink = context.sink;
    const FinalServiceStatus final_status = latch_final_service_status(context, NO_ERROR, 0);
    if (!final_status.ready) {
        throw Error("Service registration-window test could not latch STOPPED");
    }
    final_sink(final_status.status);
    return states;
}

bool agent_service_final_status_order_for_test() {
    ServiceContext context;
    bool latch_observed = false;
    bool mutex_was_unlocked = false;
    context.sink = [&context, &latch_observed, &mutex_was_unlocked](
                       const SERVICE_STATUS& status) {
        std::unique_lock lock(context.status_mutex, std::try_to_lock);
        mutex_was_unlocked = lock.owns_lock();
        latch_observed = mutex_was_unlocked && context.final_status_latched &&
            context.status.dwCurrentState == SERVICE_STOPPED &&
            status.dwCurrentState == SERVICE_STOPPED;
    };
    const auto final_sink = context.sink;
    const FinalServiceStatus final_status = latch_final_service_status(context, NO_ERROR, 0);
    if (!final_status.ready) {
        return false;
    }
    final_sink(final_status.status);
    const FinalServiceStatus duplicate = latch_final_service_status(context, NO_ERROR, 0);
    return latch_observed && mutex_was_unlocked && !duplicate.ready;
}

std::filesystem::path agent_service_startup_log_path_for_test(
    const std::filesystem::path& config_path) {
    return service_startup_log_path(config_path);
}
#endif

}  // namespace satsuma::vm
