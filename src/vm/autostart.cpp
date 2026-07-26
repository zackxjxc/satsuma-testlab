// Satsuma VM Agent 开机自启动实现。
#include "autostart.hpp"

#include <chrono>
#include <array>
#include <cstdint>
#include <limits>
#include <new>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>

#include <windows.h>
#include <aclapi.h>
#include <taskschd.h>
#include <wrl/client.h>

#include "satsuma/core/errors.hpp"
#include "satsuma/core/path.hpp"
#include "satsuma/core/windows_command_line.hpp"

namespace satsuma::vm {
namespace {

using Microsoft::WRL::ComPtr;

constexpr std::wstring_view kTaskName = L"SatsumaVM Agent";
constexpr std::string_view kTaskPath = "\\Satsuma\\SatsumaVM Agent";
constexpr std::wstring_view kInstallMutexName = L"Global\\SatsumaVM-Autostart";
using SidStorage = std::array<std::uint32_t, (SECURITY_MAX_SID_SIZE + sizeof(std::uint32_t) - 1) /
                                      sizeof(std::uint32_t)>;

// 自动释放 Win32 HANDLE。
class UniqueHandle {
public:
    // 接管一个可空 HANDLE。
    explicit UniqueHandle(HANDLE value = nullptr) : value_(value) {}

    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;

    // 关闭有效 HANDLE。
    ~UniqueHandle() {
        if (value_ != nullptr && value_ != INVALID_HANDLE_VALUE) {
            CloseHandle(value_);
        }
    }

    // 返回底层 HANDLE。
    [[nodiscard]] HANDLE get() const noexcept {
        return value_;
    }

    // 返回 HANDLE 是否有效。
    [[nodiscard]] explicit operator bool() const noexcept {
        return value_ != nullptr && value_ != INVALID_HANDLE_VALUE;
    }

private:
    HANDLE value_ = nullptr;  // 被管理的 HANDLE
};

// 标识一个固定卷中的文件或目录对象。
struct FileIdentity {
    DWORD volume_serial_number{};  // 所在卷序列号
    DWORD file_index_high{};       // 文件 ID 高位
    DWORD file_index_low{};        // 文件 ID 低位

    [[nodiscard]] bool operator==(const FileIdentity&) const = default;
};

// 读取路径指向对象的稳定 Win32 身份。
[[nodiscard]] FileIdentity read_file_identity(const std::filesystem::path& path) {
    const UniqueHandle handle(CreateFileW(
        path.c_str(),
        FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS,
        nullptr));
    if (!handle) {
        throw Error(
            "Cannot open SatsumaVM install path for identity check: " + path_to_utf8(path) +
            " (Win32 error " + std::to_string(GetLastError()) + ")");
    }

    BY_HANDLE_FILE_INFORMATION information{};
    if (!GetFileInformationByHandle(handle.get(), &information)) {
        throw Error(
            "Cannot read SatsumaVM install path identity: " + path_to_utf8(path) +
            " (Win32 error " + std::to_string(GetLastError()) + ")");
    }
    return {
        information.dwVolumeSerialNumber,
        information.nFileIndexHigh,
        information.nFileIndexLow,
    };
}

// 比较两个路径是否引用同一个文件系统对象。
[[nodiscard]] bool same_file_identity(
    const std::filesystem::path& left,
    const std::filesystem::path& right) {
    return read_file_identity(left) == read_file_identity(right);
}

// 串行化同一 Guest 内的计划任务更新。
class AutostartMutex {
public:
    // 获取全局命名互斥锁，避免两个安装或启动进程交叉覆盖。
    AutostartMutex() : handle_(CreateMutexW(nullptr, FALSE, kInstallMutexName.data())) {
        if (!handle_) {
            throw Error("Cannot create SatsumaVM autostart mutex (Win32 error " +
                        std::to_string(GetLastError()) + ")");
        }
        const DWORD wait_result = WaitForSingleObject(handle_.get(), 30000);
        if (wait_result != WAIT_OBJECT_0 && wait_result != WAIT_ABANDONED) {
            throw Error("Timed out while waiting for another SatsumaVM autostart update");
        }
        acquired_ = true;
    }

    AutostartMutex(const AutostartMutex&) = delete;
    AutostartMutex& operator=(const AutostartMutex&) = delete;

    // 释放已获取的命名互斥锁。
    ~AutostartMutex() {
        if (acquired_) {
            ReleaseMutex(handle_.get());
        }
    }

private:
    UniqueHandle handle_;     // 全局命名互斥锁句柄
    bool acquired_ = false;   // 当前线程是否拥有互斥锁
};

// 自动释放 GetNamedSecurityInfoW 分配的安全描述符。
class LocalSecurityDescriptor {
public:
    LocalSecurityDescriptor() = default;

    LocalSecurityDescriptor(const LocalSecurityDescriptor&) = delete;
    LocalSecurityDescriptor& operator=(const LocalSecurityDescriptor&) = delete;

    // 释放 LocalAlloc 内存。
    ~LocalSecurityDescriptor() {
        if (value_ != nullptr) {
            LocalFree(value_);
        }
    }

    // 返回接收安全描述符的地址。
    [[nodiscard]] PSECURITY_DESCRIPTOR* address() noexcept {
        return &value_;
    }

private:
    PSECURITY_DESCRIPTOR value_ = nullptr;  // 被管理的安全描述符
};

// 自动释放 Task Scheduler 使用的 BSTR。
class UniqueBstr {
public:
    // 从 UTF-16 文本分配 BSTR。
    explicit UniqueBstr(const std::wstring_view value) {
        if (value.size() > std::numeric_limits<UINT>::max()) {
            throw Error("Task Scheduler text is too long");
        }
        value_ = SysAllocStringLen(value.data(), static_cast<UINT>(value.size()));
        if (value_ == nullptr) {
            throw std::bad_alloc();
        }
    }

    UniqueBstr(const UniqueBstr&) = delete;
    UniqueBstr& operator=(const UniqueBstr&) = delete;

    // 释放 BSTR。
    ~UniqueBstr() {
        SysFreeString(value_);
    }

    // 返回 COM API 使用的字符串指针。
    [[nodiscard]] BSTR get() const noexcept {
        return value_;
    }

private:
    BSTR value_ = nullptr;  // 被管理的 BSTR
};

// 自动释放 COM 属性读取返回的 BSTR。
class ReturnedBstr {
public:
    ReturnedBstr() = default;

    ReturnedBstr(const ReturnedBstr&) = delete;
    ReturnedBstr& operator=(const ReturnedBstr&) = delete;

    // 释放 COM 分配的 BSTR。
    ~ReturnedBstr() {
        SysFreeString(value_);
    }

    // 返回接收 BSTR 的地址。
    [[nodiscard]] BSTR* address() noexcept {
        return &value_;
    }

    // 返回可比较的 UTF-16 文本。
    [[nodiscard]] std::wstring_view view() const noexcept {
        return value_ == nullptr
            ? std::wstring_view{}
            : std::wstring_view(value_, SysStringLen(value_));
    }

private:
    BSTR value_ = nullptr;  // 被管理的返回值
};

// 自动初始化并清理 VARIANT。
class ScopedVariant {
public:
    // 创建空 VARIANT。
    ScopedVariant() {
        VariantInit(&value_);
    }

    ScopedVariant(const ScopedVariant&) = delete;
    ScopedVariant& operator=(const ScopedVariant&) = delete;

    // 清理可能持有的 BSTR。
    ~ScopedVariant() {
        VariantClear(&value_);
    }

    // 将当前值设置为 BSTR。
    void set_string(const std::wstring_view value) {
        UniqueBstr text(value);
        value_.vt = VT_BSTR;
        value_.bstrVal = SysAllocString(text.get());
        if (value_.bstrVal == nullptr) {
            value_.vt = VT_EMPTY;
            throw std::bad_alloc();
        }
    }

    // 返回 COM API 使用的值。
    [[nodiscard]] VARIANT get() const noexcept {
        return value_;
    }

private:
    VARIANT value_{};  // 被管理的 VARIANT
};

// 在当前线程建立 COM 生命周期。
class ScopedCom {
public:
    // 初始化多线程 COM；线程已有其他模型时复用现有初始化。
    ScopedCom() {
        const HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (result != RPC_E_CHANGED_MODE) {
            check(result, "CoInitializeEx");
            uninitialize_ = true;
        }

        const HRESULT security_result = CoInitializeSecurity(
            nullptr,
            -1,
            nullptr,
            nullptr,
            RPC_C_AUTHN_LEVEL_PKT_PRIVACY,
            RPC_C_IMP_LEVEL_IMPERSONATE,
            nullptr,
            EOAC_NONE,
            nullptr);
        if (security_result != RPC_E_TOO_LATE) {
            if (FAILED(security_result) && uninitialize_) {
                CoUninitialize();
                uninitialize_ = false;
            }
            check(security_result, "CoInitializeSecurity");
        }
    }

    ScopedCom(const ScopedCom&) = delete;
    ScopedCom& operator=(const ScopedCom&) = delete;

    // 仅清理由本对象完成的 COM 初始化。
    ~ScopedCom() {
        if (uninitialize_) {
            CoUninitialize();
        }
    }

    // 将失败 HRESULT 转换为稳定异常。
    static void check(const HRESULT result, const char* operation) {
        if (SUCCEEDED(result)) {
            return;
        }
        std::ostringstream message;
        message << operation << " failed (HRESULT 0x" << std::hex
                << static_cast<unsigned long>(result) << ')';
        if (result == E_ACCESSDENIED) {
            message << "; run SatsumaVM as administrator";
        }
        throw Error(message.str());
    }

private:
    bool uninitialize_ = false;  // 当前对象是否拥有 COM 初始化计数
};

// 返回当前进程对应的可执行文件绝对路径。
[[nodiscard]] std::filesystem::path current_executable_path() {
    std::wstring buffer(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size()) {
        throw Error("Cannot resolve SatsumaVM executable path (Win32 error " +
                    std::to_string(GetLastError()) + ")");
    }
    buffer.resize(length);
    return std::filesystem::path(std::move(buffer));
}

// 拒绝不存在的计划任务输入文件并返回规范绝对路径。
[[nodiscard]] std::filesystem::path normalize_input_file(
    const std::filesystem::path& value,
    const char* label) {
    const std::filesystem::path normalized = std::filesystem::canonical(value);
    if (!std::filesystem::is_regular_file(normalized)) {
        throw Error(std::string(label) + " is not a regular file: " + path_to_utf8(normalized));
    }
    return normalized;
}

// 比较规范 Windows 路径时忽略大小写。
[[nodiscard]] bool path_equal(
    const std::filesystem::path& left,
    const std::filesystem::path& right) {
    return _wcsicmp(left.c_str(), right.c_str()) == 0;
}

// 构造用于 ACL 检查的系统或管理员 SID。
[[nodiscard]] SidStorage make_well_known_sid(
    const WELL_KNOWN_SID_TYPE type) {
    SidStorage storage{};
    DWORD size = static_cast<DWORD>(storage.size());
    if (!CreateWellKnownSid(type, nullptr, storage.data(), &size)) {
        throw Error("CreateWellKnownSid failed (Win32 error " + std::to_string(GetLastError()) + ")");
    }
    return storage;
}

// 判断 ACE 或所有者是否属于可修改机器级安装的受信主体。
[[nodiscard]] bool is_trusted_sid(
    PSID sid,
    const SidStorage& system_sid,
    const SidStorage& administrators_sid) {
    return EqualSid(sid, const_cast<std::uint32_t*>(system_sid.data())) != FALSE ||
           EqualSid(sid, const_cast<std::uint32_t*>(administrators_sid.data())) != FALSE;
}

// 拒绝可被普通用户替换的 SYSTEM 任务文件或目录。
void validate_machine_acl(
    const std::filesystem::path& path,
    const bool require_trusted_owner = true,
    const bool allow_child_creation = false) {
    PSID owner = nullptr;
    PACL dacl = nullptr;
    LocalSecurityDescriptor descriptor;
    const DWORD result = GetNamedSecurityInfoW(
        const_cast<wchar_t*>(path.c_str()),
        SE_FILE_OBJECT,
        OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
        &owner,
        nullptr,
        &dacl,
        nullptr,
        descriptor.address());
    if (result != ERROR_SUCCESS) {
        throw Error("Cannot read SatsumaVM install ACL (Win32 error " + std::to_string(result) + "): " +
                    path_to_utf8(path));
    }

    const auto system_sid = make_well_known_sid(WinLocalSystemSid);
    const auto administrators_sid = make_well_known_sid(WinBuiltinAdministratorsSid);
    if (require_trusted_owner &&
        (owner == nullptr || !is_trusted_sid(owner, system_sid, administrators_sid))) {
        throw Error("SatsumaVM install path must be owned by SYSTEM or Administrators: " + path_to_utf8(path));
    }
    if (dacl == nullptr) {
        throw Error("SatsumaVM install path must not use a null DACL: " + path_to_utf8(path));
    }

    ACCESS_MASK write_rights =
        GENERIC_ALL | GENERIC_WRITE | WRITE_DAC | WRITE_OWNER | DELETE |
        FILE_WRITE_DATA | FILE_APPEND_DATA | FILE_WRITE_EA | FILE_WRITE_ATTRIBUTES |
        FILE_ADD_FILE | FILE_ADD_SUBDIRECTORY | FILE_DELETE_CHILD;
    if (allow_child_creation) {
        write_rights &= ~(FILE_ADD_FILE | FILE_ADD_SUBDIRECTORY);
    }
    for (DWORD index = 0; index < dacl->AceCount; ++index) {
        void* raw_ace = nullptr;
        if (!GetAce(dacl, index, &raw_ace)) {
            throw Error("Cannot inspect SatsumaVM install ACL (Win32 error " +
                        std::to_string(GetLastError()) + ")");
        }
        const auto* header = static_cast<const ACE_HEADER*>(raw_ace);
        if ((header->AceFlags & INHERIT_ONLY_ACE) != 0) {
            continue;
        }
        if (header->AceType != ACCESS_ALLOWED_ACE_TYPE &&
            header->AceType != ACCESS_ALLOWED_OBJECT_ACE_TYPE &&
            header->AceType != ACCESS_ALLOWED_CALLBACK_ACE_TYPE &&
            header->AceType != ACCESS_ALLOWED_CALLBACK_OBJECT_ACE_TYPE) {
            continue;
        }

        const auto* mask = reinterpret_cast<const ACCESS_MASK*>(
            static_cast<const unsigned char*>(raw_ace) + sizeof(ACE_HEADER));
        if ((*mask & write_rights) == 0) {
            continue;
        }
        if (header->AceType != ACCESS_ALLOWED_ACE_TYPE) {
            throw Error("SatsumaVM install ACL contains an unsupported writable ACE: " + path_to_utf8(path));
        }

        const auto* allowed = static_cast<const ACCESS_ALLOWED_ACE*>(raw_ace);
        PSID sid = const_cast<DWORD*>(&allowed->SidStart);
        if (!is_trusted_sid(sid, system_sid, administrators_sid)) {
            throw Error("SatsumaVM install path is writable by an untrusted principal: " + path_to_utf8(path));
        }
    }
}

// 验证计划任务只能引用受保护的本地机器级安装布局。
void validate_installed_layout(
    const AgentAutostartSpec& spec,
    const std::filesystem::path& local_work_root) {
    if (local_work_root.empty()) {
        throw Error("Agent local_work_root is empty during autostart validation");
    }
    const std::filesystem::path install_root = spec.executable.parent_path().parent_path();
    const std::filesystem::path expected_executable = install_root / L"bin" / L"SatsumaVM.exe";
    const std::filesystem::path expected_config = install_root / L"agent.json";
    const std::filesystem::path expected_work_root = install_root / L"work";
    const std::filesystem::path normalized_work_root = std::filesystem::canonical(local_work_root);

    if (!same_file_identity(spec.executable, expected_executable) ||
        !same_file_identity(spec.config, expected_config) ||
        !same_file_identity(normalized_work_root, expected_work_root)) {
        throw Error(
            "Autostart layout mismatch: executable=" + path_to_utf8(spec.executable) +
            ", config=" + path_to_utf8(spec.config) +
            ", work=" + path_to_utf8(normalized_work_root) +
            ", expected_root=" + path_to_utf8(install_root));
    }

    const std::filesystem::path drive_root = install_root.root_path();
    if (drive_root.empty() || GetDriveTypeW(drive_root.c_str()) != DRIVE_FIXED) {
        throw Error("SatsumaVM autostart requires a fixed local drive");
    }

    const std::array paths = {
        install_root,
        spec.executable.parent_path(),
        spec.executable,
        spec.config,
        normalized_work_root,
    };
    const auto validate_path = [](
        const std::filesystem::path& path,
        const bool require_trusted_owner = true,
        const bool allow_child_creation = false) {
        const DWORD attributes = GetFileAttributesW(path.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES) {
            throw Error("SatsumaVM install path is unavailable: " + path_to_utf8(path));
        }
        if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
            throw Error("SatsumaVM install path must not be a reparse point: " + path_to_utf8(path));
        }
        validate_machine_acl(path, require_trusted_owner, allow_child_creation);
    };
    for (const auto& path : paths) {
        validate_path(path);
    }

    // 父目录的所有者可以是 TrustedInstaller，但不能允许普通用户替换安装根。
    for (std::filesystem::path parent = install_root.parent_path();
         !parent.empty() && parent != install_root.root_path();
         parent = parent.parent_path()) {
        validate_path(parent, false, true);
    }
    if (!install_root.root_path().empty()) {
        validate_path(install_root.root_path(), false, true);
    }

    // 既有 work 文件也必须继承同一 ACL，防止旧文件成为可写替身。
    std::error_code iterator_error;
    for (std::filesystem::recursive_directory_iterator iterator(normalized_work_root, iterator_error);
         iterator != std::filesystem::recursive_directory_iterator();
         iterator.increment(iterator_error)) {
        if (iterator_error) {
            throw Error("Cannot enumerate SatsumaVM work ACL: " + iterator_error.message());
        }
        validate_path(iterator->path());
    }
    if (iterator_error) {
        throw Error("Cannot enumerate SatsumaVM work ACL: " + iterator_error.message());
    }
}

// 判断 Task Scheduler 查询结果是否表示对象不存在。
[[nodiscard]] bool is_missing_object(const HRESULT result) {
    return result == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
}

// 连接本机 Task Scheduler 服务。
[[nodiscard]] ComPtr<ITaskService> connect_task_service() {
    ComPtr<ITaskService> service;
    ScopedCom::check(
        CoCreateInstance(
            CLSID_TaskScheduler,
            nullptr,
            CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&service)),
        "CoCreateInstance(TaskScheduler)");
    ScopedVariant empty;
    ScopedCom::check(
        service->Connect(empty.get(), empty.get(), empty.get(), empty.get()),
        "ITaskService::Connect");
    return service;
}

// 获取或创建 Satsuma 专用任务目录。
[[nodiscard]] ComPtr<ITaskFolder> get_or_create_task_folder(ITaskService* service) {
    const UniqueBstr root_path(L"\\");
    ComPtr<ITaskFolder> root;
    ScopedCom::check(service->GetFolder(root_path.get(), &root), "ITaskService::GetFolder");

    const UniqueBstr folder_path(L"Satsuma");
    ComPtr<ITaskFolder> folder;
    const HRESULT lookup = root->GetFolder(folder_path.get(), &folder);
    if (SUCCEEDED(lookup)) {
        return folder;
    }
    if (!is_missing_object(lookup)) {
        ScopedCom::check(lookup, "ITaskFolder::GetFolder");
    }

    ScopedVariant security_descriptor;
    const HRESULT create_result =
        root->CreateFolder(folder_path.get(), security_descriptor.get(), &folder);
    if (create_result == HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS)) {
        ScopedCom::check(root->GetFolder(folder_path.get(), &folder), "ITaskFolder::GetFolder");
    } else {
        ScopedCom::check(create_result, "ITaskFolder::CreateFolder");
    }
    return folder;
}

// 停止旧任务实例并等待 Task Scheduler 确认退出。
void stop_running_task(IRegisteredTask* task) {
    TASK_STATE state = TASK_STATE_UNKNOWN;
    ScopedCom::check(task->get_State(&state), "IRegisteredTask::get_State");
    if (state != TASK_STATE_RUNNING && state != TASK_STATE_QUEUED) {
        return;
    }

    ScopedCom::check(task->Stop(0), "IRegisteredTask::Stop");
    constexpr int attempts = 100;
    for (int attempt = 0; attempt < attempts; ++attempt) {
        ScopedCom::check(task->get_State(&state), "IRegisteredTask::get_State");
        if (state != TASK_STATE_RUNNING && state != TASK_STATE_QUEUED) {
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    throw Error("Timed out while stopping the existing SatsumaVM task instance");
}

// 比较 Task Scheduler 规范化后的 UTF-16 文本。
[[nodiscard]] bool text_equal_case_insensitive(
    const std::wstring_view left,
    const std::wstring_view right) {
    if (left.size() != right.size()) {
        return false;
    }
    return left.empty() || _wcsnicmp(left.data(), right.data(), left.size()) == 0;
}

// 检查现有任务的 SYSTEM 身份和最高权限。
[[nodiscard]] bool principal_matches(ITaskDefinition* definition) {
    ComPtr<IPrincipal> principal;
    ScopedCom::check(definition->get_Principal(&principal), "get_Principal");

    ReturnedBstr user_id;
    TASK_LOGON_TYPE logon_type = TASK_LOGON_NONE;
    TASK_RUNLEVEL_TYPE run_level = TASK_RUNLEVEL_LUA;
    ScopedCom::check(principal->get_UserId(user_id.address()), "IPrincipal::get_UserId");
    ScopedCom::check(principal->get_LogonType(&logon_type), "IPrincipal::get_LogonType");
    ScopedCom::check(principal->get_RunLevel(&run_level), "IPrincipal::get_RunLevel");
    const bool system_user =
        text_equal_case_insensitive(user_id.view(), L"S-1-5-18") ||
        text_equal_case_insensitive(user_id.view(), L"SYSTEM");
    return system_user &&
           logon_type == TASK_LOGON_SERVICE_ACCOUNT &&
           run_level == TASK_RUNLEVEL_HIGHEST;
}

// 检查现有任务只执行当前受保护的 Agent。
[[nodiscard]] bool action_matches(
    ITaskDefinition* definition,
    const AgentAutostartSpec& spec) {
    ComPtr<IActionCollection> actions;
    ScopedCom::check(definition->get_Actions(&actions), "get_Actions");
    LONG count = 0;
    ScopedCom::check(actions->get_Count(&count), "IActionCollection::get_Count");
    if (count != 1) {
        return false;
    }

    ComPtr<IAction> action;
    ScopedCom::check(actions->get_Item(1, &action), "IActionCollection::get_Item");
    TASK_ACTION_TYPE action_type = TASK_ACTION_EXEC;
    ScopedCom::check(action->get_Type(&action_type), "IAction::get_Type");
    if (action_type != TASK_ACTION_EXEC) {
        return false;
    }

    ComPtr<IExecAction> execute;
    ScopedCom::check(action.As(&execute), "QueryInterface(IExecAction)");
    ReturnedBstr executable;
    ReturnedBstr arguments;
    ReturnedBstr working_directory;
    ScopedCom::check(execute->get_Path(executable.address()), "IExecAction::get_Path");
    ScopedCom::check(execute->get_Arguments(arguments.address()), "IExecAction::get_Arguments");
    ScopedCom::check(
        execute->get_WorkingDirectory(working_directory.address()),
        "IExecAction::get_WorkingDirectory");
    return text_equal_case_insensitive(executable.view(), spec.executable.native()) &&
           arguments.view() == spec.arguments &&
           text_equal_case_insensitive(working_directory.view(), spec.working_directory.native());
}

// 检查现有任务只有一个带固定延迟的开机触发器。
[[nodiscard]] bool trigger_matches(ITaskDefinition* definition) {
    ComPtr<ITriggerCollection> triggers;
    ScopedCom::check(definition->get_Triggers(&triggers), "get_Triggers");
    LONG count = 0;
    ScopedCom::check(triggers->get_Count(&count), "ITriggerCollection::get_Count");
    if (count != 1) {
        return false;
    }

    ComPtr<ITrigger> trigger;
    ScopedCom::check(triggers->get_Item(1, &trigger), "ITriggerCollection::get_Item");
    TASK_TRIGGER_TYPE2 trigger_type = TASK_TRIGGER_EVENT;
    ScopedCom::check(trigger->get_Type(&trigger_type), "ITrigger::get_Type");
    if (trigger_type != TASK_TRIGGER_BOOT) {
        return false;
    }

    ComPtr<IBootTrigger> boot_trigger;
    ScopedCom::check(trigger.As(&boot_trigger), "QueryInterface(IBootTrigger)");
    ReturnedBstr delay;
    VARIANT_BOOL enabled = VARIANT_FALSE;
    ScopedCom::check(boot_trigger->get_Delay(delay.address()), "IBootTrigger::get_Delay");
    ScopedCom::check(boot_trigger->get_Enabled(&enabled), "IBootTrigger::get_Enabled");
    return delay.view() == L"PT15S" && enabled == VARIANT_TRUE;
}

// 检查现有任务的无限运行、单实例和失败重启策略。
[[nodiscard]] bool settings_match(ITaskDefinition* definition) {
    ComPtr<ITaskSettings> settings;
    ScopedCom::check(definition->get_Settings(&settings), "get_Settings");
    VARIANT_BOOL enabled = VARIANT_FALSE;
    VARIANT_BOOL start_when_available = VARIANT_FALSE;
    VARIANT_BOOL disallow_on_batteries = VARIANT_TRUE;
    VARIANT_BOOL stop_on_batteries = VARIANT_TRUE;
    ReturnedBstr execution_time_limit;
    ReturnedBstr restart_interval;
    int restart_count = 0;
    TASK_INSTANCES_POLICY instances = TASK_INSTANCES_PARALLEL;
    ScopedCom::check(settings->get_Enabled(&enabled), "ITaskSettings::get_Enabled");
    ScopedCom::check(
        settings->get_StartWhenAvailable(&start_when_available),
        "ITaskSettings::get_StartWhenAvailable");
    ScopedCom::check(
        settings->get_ExecutionTimeLimit(execution_time_limit.address()),
        "ITaskSettings::get_ExecutionTimeLimit");
    ScopedCom::check(
        settings->get_RestartInterval(restart_interval.address()),
        "ITaskSettings::get_RestartInterval");
    ScopedCom::check(settings->get_RestartCount(&restart_count), "ITaskSettings::get_RestartCount");
    ScopedCom::check(
        settings->get_MultipleInstances(&instances),
        "ITaskSettings::get_MultipleInstances");
    ScopedCom::check(
        settings->get_DisallowStartIfOnBatteries(&disallow_on_batteries),
        "ITaskSettings::get_DisallowStartIfOnBatteries");
    ScopedCom::check(
        settings->get_StopIfGoingOnBatteries(&stop_on_batteries),
        "ITaskSettings::get_StopIfGoingOnBatteries");
    return enabled == VARIANT_TRUE &&
           start_when_available == VARIANT_TRUE &&
           execution_time_limit.view() == L"PT0S" &&
           restart_interval.view() == L"PT1M" &&
           restart_count == 999 &&
           instances == TASK_INSTANCES_IGNORE_NEW &&
           disallow_on_batteries == VARIANT_FALSE &&
           stop_on_batteries == VARIANT_FALSE;
}

// 判断现有任务是否已经完全符合当前 Agent 规范。
[[nodiscard]] bool task_definition_matches(
    IRegisteredTask* task,
    const AgentAutostartSpec& spec) {
    ComPtr<ITaskDefinition> definition;
    ScopedCom::check(task->get_Definition(&definition), "IRegisteredTask::get_Definition");
    return principal_matches(definition.Get()) &&
           action_matches(definition.Get(), spec) &&
           trigger_matches(definition.Get()) &&
           settings_match(definition.Get());
}

// 配置开机触发、崩溃重启和 SYSTEM 最高权限。
void configure_task_policy(ITaskDefinition* definition) {
    ComPtr<IRegistrationInfo> registration;
    ScopedCom::check(definition->get_RegistrationInfo(&registration), "get_RegistrationInfo");
    const UniqueBstr author(L"Satsuma TestLab");
    const UniqueBstr description(L"Starts SatsumaVM Agent after the guest operating system boots.");
    ScopedCom::check(registration->put_Author(author.get()), "put_Author");
    ScopedCom::check(registration->put_Description(description.get()), "put_Description");

    ComPtr<IPrincipal> principal;
    ScopedCom::check(definition->get_Principal(&principal), "get_Principal");
    const UniqueBstr principal_id(L"SatsumaAgentPrincipal");
    const UniqueBstr system_user(L"S-1-5-18");
    ScopedCom::check(principal->put_Id(principal_id.get()), "IPrincipal::put_Id");
    ScopedCom::check(principal->put_UserId(system_user.get()), "IPrincipal::put_UserId");
    ScopedCom::check(
        principal->put_LogonType(TASK_LOGON_SERVICE_ACCOUNT),
        "IPrincipal::put_LogonType");
    ScopedCom::check(principal->put_RunLevel(TASK_RUNLEVEL_HIGHEST), "IPrincipal::put_RunLevel");

    ComPtr<ITaskSettings> settings;
    ScopedCom::check(definition->get_Settings(&settings), "get_Settings");
    const UniqueBstr no_time_limit(L"PT0S");
    const UniqueBstr restart_interval(L"PT1M");
    ScopedCom::check(settings->put_Enabled(VARIANT_TRUE), "ITaskSettings::put_Enabled");
    ScopedCom::check(settings->put_StartWhenAvailable(VARIANT_TRUE), "put_StartWhenAvailable");
    ScopedCom::check(settings->put_ExecutionTimeLimit(no_time_limit.get()), "put_ExecutionTimeLimit");
    ScopedCom::check(settings->put_RestartInterval(restart_interval.get()), "put_RestartInterval");
    ScopedCom::check(settings->put_RestartCount(999), "put_RestartCount");
    ScopedCom::check(
        settings->put_MultipleInstances(TASK_INSTANCES_IGNORE_NEW),
        "put_MultipleInstances");
    ScopedCom::check(
        settings->put_DisallowStartIfOnBatteries(VARIANT_FALSE),
        "put_DisallowStartIfOnBatteries");
    ScopedCom::check(
        settings->put_StopIfGoingOnBatteries(VARIANT_FALSE),
        "put_StopIfGoingOnBatteries");

    ComPtr<ITriggerCollection> triggers;
    ScopedCom::check(definition->get_Triggers(&triggers), "get_Triggers");
    ComPtr<ITrigger> trigger;
    ScopedCom::check(triggers->Create(TASK_TRIGGER_BOOT, &trigger), "ITriggerCollection::Create");
    ComPtr<IBootTrigger> boot_trigger;
    ScopedCom::check(trigger.As(&boot_trigger), "QueryInterface(IBootTrigger)");
    const UniqueBstr trigger_id(L"GuestBoot");
    const UniqueBstr boot_delay(L"PT15S");
    ScopedCom::check(boot_trigger->put_Id(trigger_id.get()), "IBootTrigger::put_Id");
    ScopedCom::check(boot_trigger->put_Delay(boot_delay.get()), "IBootTrigger::put_Delay");
    ScopedCom::check(boot_trigger->put_Enabled(VARIANT_TRUE), "IBootTrigger::put_Enabled");
}

// 配置只包含当前 Agent 可执行文件和配置路径的执行动作。
void configure_task_action(ITaskDefinition* definition, const AgentAutostartSpec& spec) {
    ComPtr<IActionCollection> actions;
    ScopedCom::check(definition->get_Actions(&actions), "get_Actions");
    ComPtr<IAction> action;
    ScopedCom::check(actions->Create(TASK_ACTION_EXEC, &action), "IActionCollection::Create");
    ComPtr<IExecAction> execute;
    ScopedCom::check(action.As(&execute), "QueryInterface(IExecAction)");

    const UniqueBstr action_id(L"RunAgent");
    const UniqueBstr executable(spec.executable.native());
    const UniqueBstr arguments(spec.arguments);
    const UniqueBstr working_directory(spec.working_directory.native());
    ScopedCom::check(action->put_Id(action_id.get()), "IAction::put_Id");
    ScopedCom::check(execute->put_Path(executable.get()), "IExecAction::put_Path");
    ScopedCom::check(execute->put_Arguments(arguments.get()), "IExecAction::put_Arguments");
    ScopedCom::check(
        execute->put_WorkingDirectory(working_directory.get()),
        "IExecAction::put_WorkingDirectory");
}

}  // namespace

AgentAutostartSpec make_agent_autostart_spec(
    const std::filesystem::path& executable,
    const std::filesystem::path& config) {
    AgentAutostartSpec spec;
    spec.executable = normalize_input_file(executable, "SatsumaVM executable");
    spec.config = normalize_input_file(config, "Agent config");
    spec.working_directory = spec.executable.parent_path();
    spec.arguments = L"--config " + quote_windows_argument(spec.config.native()) + L" --watch";
    return spec;
}

#ifdef SATSUMA_AUTOSTART_TESTS
void validate_agent_autostart_parent_acl_for_test(const std::filesystem::path& path) {
    validate_machine_acl(path, false, true);
}

bool agent_autostart_same_file_for_test(
    const std::filesystem::path& left,
    const std::filesystem::path& right) {
    return same_file_identity(left, right);
}

bool agent_autostart_definition_round_trip_for_test(const AgentAutostartSpec& spec) {
    ScopedCom com;
    const ComPtr<ITaskService> service = connect_task_service();
    ComPtr<ITaskDefinition> definition;
    ScopedCom::check(service->NewTask(0, &definition), "ITaskService::NewTask");
    configure_task_policy(definition.Get());
    configure_task_action(definition.Get(), spec);
    return principal_matches(definition.Get()) &&
           action_matches(definition.Get(), spec) &&
           trigger_matches(definition.Get()) &&
           settings_match(definition.Get());
}
#endif

AgentAutostartResult ensure_agent_autostart(
    const std::filesystem::path& config,
    const std::filesystem::path& local_work_root,
    const bool start_now) {
    [[maybe_unused]] AutostartMutex update_lock;
    const AgentAutostartSpec spec = make_agent_autostart_spec(current_executable_path(), config);
    validate_installed_layout(spec, local_work_root);
    ScopedCom com;
    const ComPtr<ITaskService> service = connect_task_service();
    const ComPtr<ITaskFolder> folder = get_or_create_task_folder(service.Get());

    const UniqueBstr task_name(kTaskName);
    ComPtr<IRegisteredTask> existing_task;
    const HRESULT lookup = folder->GetTask(task_name.get(), &existing_task);
    const bool existed = SUCCEEDED(lookup);
    if (!existed && !is_missing_object(lookup)) {
        ScopedCom::check(lookup, "ITaskFolder::GetTask");
    }
    const bool definition_matches = existed && task_definition_matches(existing_task.Get(), spec);
    if (start_now && existed) {
        stop_running_task(existing_task.Get());
    }

    ScopedVariant empty;
    ComPtr<IRegisteredTask> registered_task;
    AutostartChange change = AutostartChange::Unchanged;
    if (definition_matches) {
        registered_task = existing_task;
    } else {
        ComPtr<ITaskDefinition> definition;
        ScopedCom::check(service->NewTask(0, &definition), "ITaskService::NewTask");
        configure_task_policy(definition.Get());
        configure_task_action(definition.Get(), spec);

        ScopedVariant system_user;
        system_user.set_string(L"S-1-5-18");
        ScopedCom::check(
            folder->RegisterTaskDefinition(
                task_name.get(),
                definition.Get(),
                TASK_CREATE_OR_UPDATE,
                system_user.get(),
                empty.get(),
                TASK_LOGON_SERVICE_ACCOUNT,
                empty.get(),
                &registered_task),
            "ITaskFolder::RegisterTaskDefinition");
        change = existed ? AutostartChange::Updated : AutostartChange::Created;
    }

    DWORD engine_process_id = 0;
    if (start_now) {
        ComPtr<IRunningTask> running_task;
        ScopedCom::check(registered_task->Run(empty.get(), &running_task), "IRegisteredTask::Run");
        ScopedCom::check(
            running_task->get_EnginePID(&engine_process_id),
            "IRunningTask::get_EnginePID");
        if (engine_process_id == 0) {
            throw Error("Task Scheduler returned an invalid SatsumaVM process ID");
        }
    }

    return {
        change,
        std::string(kTaskPath),
        start_now,
        engine_process_id,
    };
}

}  // namespace satsuma::vm
