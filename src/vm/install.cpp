// 单文件 Guest 安装：原生 UAC、受保护目录、SCM 和旧计划任务迁移。
#include "install.hpp"

#include <iostream>
#include <array>
#include <charconv>
#include <vector>
#include <windows.h>
#include <aclapi.h>
#include <sddl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <imagehlp.h>
#include <taskschd.h>
#include <wrl/client.h>

#include "service.hpp"
#include "satsuma/core/config.hpp"
#include "satsuma/core/errors.hpp"
#include "satsuma/core/json_io.hpp"
#include "satsuma/core/path.hpp"
#include "satsuma/core/sha256.hpp"
#include "satsuma/core/version.hpp"
#include "satsuma/core/windows_command_line.hpp"

namespace satsuma::vm {
namespace {
using Microsoft::WRL::ComPtr;
using Path = std::filesystem::path;
#ifdef SATSUMA_INSTALL_TESTS
bool fail_legacy_task_delete_for_test = false;
bool fail_local_update_for_test = false;
Path update_source_for_test;
#endif

void check(bool ok, const std::string& action) {
    if (!ok) throw Error(action + "，Windows 错误码 " + std::to_string(GetLastError()));
}
void com_check(HRESULT result, const char* action) {
    if (FAILED(result)) throw Error(std::string(action) + "，HRESULT " + std::to_string(result));
}
struct Handle {
    HANDLE value{};
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};
struct ServiceHandle {
    SC_HANDLE value{};
    ~ServiceHandle() { if (value) CloseServiceHandle(value); }
};
struct LocalMemory {
    void* value{};
    ~LocalMemory() { if (value) LocalFree(value); }
};
struct Bstr {
    BSTR value{};
    explicit Bstr(const wchar_t* text = nullptr) : value(text ? SysAllocString(text) : nullptr) {}
    ~Bstr() { SysFreeString(value); }
};
struct ComScope {
    ComScope() { com_check(CoInitializeEx(nullptr, COINIT_MULTITHREADED), "初始化计划任务接口失败"); }
    ~ComScope() { CoUninitialize(); }
};

Path current_executable() {
    std::wstring buffer(32768, L'\0');
    const DWORD size = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    check(size != 0 && size < buffer.size(), "无法确定当前 EXE 路径");
    buffer.resize(size);
    return Path(buffer);
}
bool same_path(const Path& left, const Path& right) {
    return _wcsicmp(left.lexically_normal().c_str(), right.lexically_normal().c_str()) == 0;
}
void reject_reparse(const Path& path) {
    const auto absolute = std::filesystem::absolute(path).lexically_normal();
    Path part;
    for (const auto& component : absolute) {
        part /= component;
        if (part == absolute.root_name()) continue;
        const DWORD attributes = GetFileAttributesW(part.c_str());
        check(attributes != INVALID_FILE_ATTRIBUTES, "无法检查路径 " + path_to_utf8(part));
        if (attributes & FILE_ATTRIBUTE_REPARSE_POINT)
            throw Error("拒绝符号链接或重解析点：" + path_to_utf8(part));
    }
}
bool trusted_sid(PSID sid) {
    return sid && (IsWellKnownSid(sid, WinLocalSystemSid) ||
                   IsWellKnownSid(sid, WinBuiltinAdministratorsSid));
}
bool validate_acl(const Path& path, bool allow_legacy_root = false) {
    PSID owner{};
    PACL acl{};
    LocalMemory descriptor;
    const DWORD result = GetNamedSecurityInfoW(path.c_str(), SE_FILE_OBJECT,
        OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION, &owner, nullptr,
        &acl, nullptr, reinterpret_cast<PSECURITY_DESCRIPTOR*>(&descriptor.value));
    if (result != ERROR_SUCCESS || !trusted_sid(owner) || !acl)
        throw Error("目录或文件所有者/权限不可信，请由管理员人工检查：" + path_to_utf8(path));
    constexpr DWORD write_mask = FILE_WRITE_DATA | FILE_APPEND_DATA | FILE_WRITE_EA |
        FILE_WRITE_ATTRIBUTES | FILE_DELETE_CHILD | DELETE | WRITE_DAC | WRITE_OWNER |
        GENERIC_WRITE | GENERIC_ALL | MAXIMUM_ALLOWED;
    bool needs_repair = false;
    for (DWORD i = 0; i < acl->AceCount; ++i) {
        void* raw{};
        check(GetAce(acl, i, &raw) != FALSE, "读取 ACL 失败");
        const auto* header = static_cast<ACE_HEADER*>(raw);
        if (header->AceFlags & INHERIT_ONLY_ACE) continue;
        if (header->AceType == ACCESS_DENIED_ACE_TYPE) continue;
        if (header->AceType != ACCESS_ALLOWED_ACE_TYPE)
            throw Error("无法确认特殊 ACL 的安全性：" + path_to_utf8(path));
        auto* ace = static_cast<ACCESS_ALLOWED_ACE*>(raw);
        if ((ace->Mask & write_mask) && !trusted_sid(&ace->SidStart)) {
            if (!allow_legacy_root)
                throw Error("普通账户可能写入安装目录，请由管理员修复权限：" + path_to_utf8(path));
            needs_repair = true;
        }
    }
    return needs_repair;
}
bool validate_install_permissions(const Path& root, bool allow_legacy_roots);
void create_private_directory(const Path& path) {
    reject_reparse(path.parent_path());
    LocalMemory descriptor;
    check(ConvertStringSecurityDescriptorToSecurityDescriptorW(
        L"O:BAG:BAD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)", SDDL_REVISION_1,
        reinterpret_cast<PSECURITY_DESCRIPTOR*>(&descriptor.value), nullptr) != FALSE,
        "创建安全描述符失败");
    SECURITY_ATTRIBUTES attributes{sizeof(attributes), descriptor.value, FALSE};
    check(CreateDirectoryW(path.c_str(), &attributes) != FALSE,
        "无法创建专属安装目录（已有目录不会被接管） " + path_to_utf8(path));
}
void protect_copied_file(const Path& path) {
    LocalMemory descriptor;
    check(ConvertStringSecurityDescriptorToSecurityDescriptorW(
        L"O:BAG:BAD:P(A;;FA;;;SY)(A;;FA;;;BA)", SDDL_REVISION_1,
        reinterpret_cast<PSECURITY_DESCRIPTOR*>(&descriptor.value), nullptr) != FALSE,
        "创建 EXE 权限失败");
    PSID owner{};
    BOOL defaulted{};
    PACL acl{};
    BOOL present{};
    check(GetSecurityDescriptorOwner(descriptor.value, &owner, &defaulted) != FALSE &&
        GetSecurityDescriptorDacl(descriptor.value, &present, &acl, &defaulted) != FALSE && present,
        "读取 EXE 权限失败");
    auto name = path.native();
    const DWORD result = SetNamedSecurityInfoW(name.data(), SE_FILE_OBJECT,
        OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
        owner, nullptr, acl, nullptr);
    if (result != ERROR_SUCCESS) throw Error("保护 EXE 权限失败，Windows 错误码 " + std::to_string(result));
    // 只读 ISO 上的文件也必须能由后续 SYSTEM 更新事务替换。
    check(SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL) != FALSE, "设置已安装 EXE 属性失败");
}
std::vector<std::wstring> arguments(const std::wstring& command) {
    int count{};
    LocalMemory memory;
    memory.value = CommandLineToArgvW(command.c_str(), &count);
    check(memory.value != nullptr, "解析服务命令失败");
    const auto argv = static_cast<wchar_t**>(memory.value);
    return {argv, argv + count};
}

struct Installation {
    Path executable;
    Path config;
    Path root;
    std::string version;
    DWORD state{};
    bool automatic{};
    bool repair_permissions{};
};
Installation inspect_layout(const Path& executable, const Path& config) {
    if (!executable.is_absolute() || !config.is_absolute() ||
        _wcsicmp(executable.filename().c_str(), L"SatsumaVM.exe") != 0 ||
        _wcsicmp(executable.parent_path().filename().c_str(), L"bin") != 0 ||
        !same_path(config, executable.parent_path().parent_path() / L"agent.json"))
        throw Error("同名服务或计划任务指向异常路径；已停止，请管理员核实其归属。");
    const auto agent = executable.parent_path().parent_path();
    if (_wcsicmp(agent.filename().c_str(), L"agent") != 0)
        throw Error("安装路径不符合 Satsuma 布局；未修改任何现有文件。");
    Installation installation{executable, config, agent.parent_path(), {}, 0, false};
    installation.repair_permissions = validate_install_permissions(installation.root, true);
    installation.version = verify_agent_image(executable, false);
    const auto parsed = load_agent_config(config);
    if (!same_path(parsed.storage_root, installation.root) ||
        !same_path(parsed.mirror_root, installation.root / L"mirror"))
        throw Error("本机配置指向安装范围以外，请管理员核实配置；不会自动覆盖。");
    for (const auto* name : {L"install-pending.json", L"install-acl-repair.json", L"agent.json.bak", L"agent.json.new", L"agent.json.update.bak", L"agent.json.install.bak"}) {
        if (std::filesystem::exists(agent / name))
            throw Error("发现未完成安装/升级文件，请管理员检查并恢复备份：" + path_to_utf8(agent / name));
    }
    for (const auto* name : {L"SatsumaVM.new.exe", L"SatsumaVM.new.exe.partial", L"SatsumaVM.bak.exe",
                             L"SatsumaVM.install.exe", L"SatsumaVM.install.bak.exe"}) {
        if (std::filesystem::exists(executable.parent_path() / name))
            throw Error("发现升级暂存/备份文件，请先检查 Host 更新结果：" + path_to_utf8(executable.parent_path() / name));
    }
    const Path state = agent / L"update-state.json";
    if (std::filesystem::exists(state)) {
        const auto update = load_json(state);
        throw Error("发现更新事务状态，阶段：" + update.value("phase", std::string("unknown")) +
            "；原因：" + update.value("error", std::string{}) +
            "。请检查 Host 更新结果及 " + path_to_utf8(state));
    }
    return installation;
}
bool inspect_service(Installation& installation) {
    const ServiceHandle manager{OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT)};
    check(manager.value != nullptr, "无法连接 Windows 服务管理器");
    const ServiceHandle service{OpenServiceW(manager.value, L"SatsumaVM", SERVICE_QUERY_CONFIG | SERVICE_QUERY_STATUS)};
    if (!service.value) {
        if (GetLastError() == ERROR_SERVICE_DOES_NOT_EXIST) return false;
        check(false, "无法检查 SatsumaVM 服务");
    }
    DWORD bytes{};
    QueryServiceConfigW(service.value, nullptr, 0, &bytes);
    check(GetLastError() == ERROR_INSUFFICIENT_BUFFER && bytes != 0, "读取服务配置失败");
    std::vector<BYTE> buffer(bytes);
    auto* config = reinterpret_cast<QUERY_SERVICE_CONFIGW*>(buffer.data());
    check(QueryServiceConfigW(service.value, config, bytes, &bytes) != FALSE, "读取服务配置失败");
    const auto argv = arguments(config->lpBinaryPathName);
    if (argv.size() != 4 || argv[1] != L"--config" || argv[3] != L"--service" ||
        config->dwServiceType != SERVICE_WIN32_OWN_PROCESS ||
        _wcsicmp(config->lpServiceStartName, L"LocalSystem") != 0 ||
        _wcsicmp(config->lpDisplayName, kAgentServiceDisplayName.data()) != 0)
        throw Error("发现不属于当前 Satsuma 安装的同名服务；未覆盖、停止或删除该服务。");
    installation = inspect_layout(Path(argv[0]), Path(argv[2]));
    const auto spec = make_agent_service_spec(installation.executable, installation.config);
    if (_wcsicmp(config->lpBinaryPathName, spec.binary_path.c_str()) != 0)
        throw Error("服务启动命令异常；请管理员检查 SatsumaVM 的 ImagePath。");
    SERVICE_STATUS_PROCESS status{};
    check(QueryServiceStatusEx(service.value, SC_STATUS_PROCESS_INFO,
        reinterpret_cast<BYTE*>(&status), sizeof(status), &bytes) != FALSE, "读取服务状态失败");
    installation.state = status.dwCurrentState;
    installation.automatic = config->dwStartType == SERVICE_AUTO_START;
    return true;
}

// 只识别历史安装器的固定任务；命令、布局和 SYSTEM 身份必须全部一致。
struct LegacyTask {
    ComPtr<ITaskFolder> folder;
    ComPtr<IRegisteredTask> task;
    Installation installation;
    bool enabled{};
    bool running{};
    LegacyTask() {
        ComPtr<ITaskService> scheduler;
        com_check(CoCreateInstance(CLSID_TaskScheduler, nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&scheduler)), "连接计划任务失败");
        VARIANT empty{};
        com_check(scheduler->Connect(empty, empty, empty, empty), "连接计划任务失败");
        Bstr folder_name(L"\\Satsuma");
        HRESULT result = scheduler->GetFolder(folder_name.value, &folder);
        if (result == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) ||
            result == HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND)) return;
        com_check(result, "查询历史任务目录失败");
        Bstr name(L"SatsumaVM Agent");
        result = folder->GetTask(name.value, &task);
        if (result == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)) return;
        com_check(result, "查询历史任务失败");
        ComPtr<ITaskDefinition> definition;
        ComPtr<IActionCollection> actions;
        ComPtr<IAction> action;
        ComPtr<IExecAction> exec;
        ComPtr<IPrincipal> principal;
        com_check(task->get_Definition(&definition), "读取历史任务失败");
        com_check(definition->get_Actions(&actions), "读取历史任务动作失败");
        LONG count{};
        com_check(actions->get_Count(&count), "读取历史任务动作数量失败");
        if (count != 1) throw Error("同名计划任务有未知动作，未修改；请管理员核实。");
        com_check(actions->get_Item(1, &action), "读取历史任务动作失败");
        com_check(action.As(&exec), "同名任务不是 Satsuma EXE 启动动作");
        Bstr exe, args, user;
        com_check(exec->get_Path(&exe.value), "读取任务 EXE 失败");
        com_check(exec->get_Arguments(&args.value), "读取任务参数失败");
        com_check(definition->get_Principal(&principal), "读取任务身份失败");
        com_check(principal->get_UserId(&user.value), "读取任务身份失败");
        TASK_LOGON_TYPE logon{};
        com_check(principal->get_LogonType(&logon), "读取任务登录方式失败");
        if (!user.value || ( _wcsicmp(user.value, L"SYSTEM") != 0 &&
            _wcsicmp(user.value, L"S-1-5-18") != 0 &&
            _wcsicmp(user.value, L"NT AUTHORITY\\SYSTEM") != 0) || logon != TASK_LOGON_SERVICE_ACCOUNT)
            throw Error("同名计划任务不是 SYSTEM Satsuma 任务，未修改。");
        if (!exe.value || !args.value) throw Error("同名计划任务缺少启动路径或参数，未修改。");
        const auto argv = arguments(quote_windows_argument(exe.value) + L" " + args.value);
        if (argv.size() != 4 || argv[1] != L"--config" || argv[3] != L"--watch")
            throw Error("同名计划任务参数不属于历史 Satsuma 安装，未修改。");
        installation = inspect_layout(Path(argv[0]), Path(argv[2]));
        VARIANT_BOOL active{};
        com_check(task->get_Enabled(&active), "读取任务启用状态失败");
        enabled = active != VARIANT_FALSE;
        TASK_STATE state{};
        com_check(task->get_State(&state), "读取任务运行状态失败");
        running = state == TASK_STATE_RUNNING || state == TASK_STATE_QUEUED;
    }
    void stop() {
        com_check(task->put_Enabled(VARIANT_FALSE), "禁用历史任务失败");
        com_check(task->Stop(0), "停止历史任务失败");
        const ULONGLONG deadline = GetTickCount64() + 15000;
        TASK_STATE state{};
        do {
            com_check(task->get_State(&state), "等待历史任务停止失败");
            if (state != TASK_STATE_RUNNING && state != TASK_STATE_QUEUED) return;
            Sleep(100);
        } while (GetTickCount64() < deadline);
        throw Error("历史任务在 15 秒内未停止，请管理员检查。");
    }
    void restore() {
        com_check(task->put_Enabled(enabled ? VARIANT_TRUE : VARIANT_FALSE), "恢复历史任务启用状态失败");
        if (running) {
            VARIANT empty{};
            ComPtr<IRunningTask> instance;
            com_check(task->Run(empty, &instance), "恢复历史任务运行失败");
        }
    }
    void erase() {
#ifdef SATSUMA_INSTALL_TESTS
        if (fail_legacy_task_delete_for_test) throw Error("删除已确认的历史任务失败（测试故障注入）");
#endif
        Bstr name(L"SatsumaVM Agent");
        com_check(folder->DeleteTask(name.value, 0), "删除已确认的历史任务失败");
    }
};

void show_status(const Installation& installation) {
    const char* state = "正在启动或停止";
    if (installation.state == SERVICE_RUNNING) state = "运行中";
    if (installation.state == SERVICE_STOPPED) state = "已停止（请管理员检查服务启动日志）";
    if (installation.state == SERVICE_PAUSED) state = "已暂停";
    std::cout << "已安装 Satsuma Agent\nAgent 版本：" << installation.version
              << "\n服务名称：SatsumaVM\n服务状态：" << state
              << "\n启动方式：" << (installation.automatic ? "自动" : "异常：不是自动启动")
              << "\n安装位置：" << path_to_utf8(installation.root) << '\n';
    if (!installation.automatic || installation.state != SERVICE_RUNNING)
        throw Error("安装文件已保留，但服务状态需要人工处理。请检查 Windows 服务管理器及 agent/agent-startup-error.log。");
}

// 服务在配置加载、Agent 构造完成后才报告 RUNNING。presence 依赖 VMCI，不能用于离线安装验收。
void verify_local_service(const Installation& installation, DWORD pid, const std::string& hash) {
    const Handle process{OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)};
    check(process.value != nullptr, "无法检查新服务进程");
    std::wstring image(32768, L'\0');
    DWORD length = static_cast<DWORD>(image.size());
    check(QueryFullProcessImageNameW(process.value, 0, image.data(), &length) != FALSE,
        "读取服务进程映像失败");
    image.resize(length);
    if (!same_path(Path(image), installation.executable)) throw Error("服务进程映像不是目标 EXE。");
    if (WaitForSingleObject(process.value, 3000) != WAIT_TIMEOUT)
        throw Error("Agent 启动后退出，请检查 agent/agent-startup-error.log。");
    const ServiceHandle manager{OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT)};
    check(manager.value != nullptr, "无法复查服务管理器");
    const ServiceHandle service{OpenServiceW(manager.value, L"SatsumaVM", SERVICE_QUERY_STATUS)};
    check(service.value != nullptr, "无法复查服务状态");
    SERVICE_STATUS_PROCESS status{};
    DWORD bytes{};
    check(QueryServiceStatusEx(service.value, SC_STATUS_PROCESS_INFO,
        reinterpret_cast<BYTE*>(&status), sizeof(status), &bytes) != FALSE, "复查服务状态失败");
    if (status.dwCurrentState != SERVICE_RUNNING || status.dwProcessId != pid ||
        sha256_file(installation.executable) != hash)
        throw Error("Agent 启动验证失败：服务状态、进程或 EXE 哈希发生变化。");
}

void update_or_show(Installation& installation) {
    Path source = current_executable();
#ifdef SATSUMA_INSTALL_TESTS
    if (!update_source_for_test.empty()) source = update_source_for_test;
#endif
    const Handle source_lock{CreateFileW(source.c_str(), GENERIC_READ, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
    check(source_lock.value != INVALID_HANDLE_VALUE, "无法锁定安装介质");
    const auto version = verify_agent_image(source, true);
    const int order = compare_agent_versions(installation.version, version);
    if (order > 0) {
        std::cout << "已安装版本高于当前 EXE（" << version << "），保留现有版本，不降级。\n";
        show_status(installation);
        return;
    }
    const auto hash = sha256_file(source);
    const auto old_hash = sha256_file(installation.executable);
    if (order == 0 && hash == old_hash) {
        std::cout << "版本及 EXE 内容一致，无需更新。\n";
        show_status(installation);
        return;
    }
    // 不在更新事务中顺带改变现有服务定义。
    if (!installation.automatic || installation.state != SERVICE_RUNNING) {
        show_status(installation);
        return;
    }
    const auto agent = installation.config.parent_path();
    // 与历史 Host 更新器的暂存名分开，防止旧版清理逻辑碰到本机更新文件。
    const auto staged = installation.executable.parent_path() / L"SatsumaVM.install.exe";
    const auto backup = installation.executable.parent_path() / L"SatsumaVM.install.bak.exe";
    const auto config_backup = agent / L"agent.json.install.bak";
    const auto journal = agent / L"install-pending.json";
    bool staged_created = false, config_saved = false, stop_attempted = false;
    bool backed_up = false, switched = false, config_changed = false, committed = false;
    const auto record = [&](const char* stage, const std::string& error = {}) {
        write_json_atomic(journal, {{"stage", stage}, {"source_sha256", hash},
            {"previous_sha256", old_hash}, {"version", version}, {"error", error}});
    };
    std::cout << "正在更新：" << installation.version << " → " << version
              << (order == 0 ? "（同版本，EXE 内容不同）\n" : "\n") << std::flush;
    try {
        record("preparing_update");
        check(CopyFileW(source.c_str(), staged.c_str(), TRUE) != FALSE, "暂存新版 EXE 失败");
        staged_created = true;
        protect_copied_file(staged);
        if (verify_agent_image(staged, true) != version || sha256_file(staged) != hash)
            throw Error("暂存 EXE 校验失败，原服务未被替换。");
        check(CopyFileW(installation.config.c_str(), config_backup.c_str(), TRUE) != FALSE, "备份本机配置失败");
        config_saved = true;
        protect_copied_file(config_backup);
        record("stopping_service");
        stop_attempted = true;
        (void)stop_owned_agent_service(installation.executable, installation.config);
        // 旧版 Agent 可能还不认识安装锁；停服后再次排除它启动了 Host 更新助手。
        for (const auto& pending : {agent / L"update-state.json", agent / L"agent.json.update.bak",
             installation.executable.parent_path() / L"SatsumaVM.new.exe",
             installation.executable.parent_path() / L"SatsumaVM.new.exe.partial",
             installation.executable.parent_path() / L"SatsumaVM.bak.exe"}) {
            if (std::filesystem::exists(pending))
                throw Error("停服后发现 Host 更新事务，请先处理：" + path_to_utf8(pending));
        }
        if (sha256_file(installation.executable) != old_hash)
            throw Error("检查期间原 EXE 已改变，停止本机更新，请核实其他维护进程。");
        record("switching_files");
        std::filesystem::rename(installation.executable, backup);
        backed_up = true;
        std::filesystem::rename(staged, installation.executable);
        switched = true;
        staged_created = false;
#ifdef SATSUMA_INSTALL_TESTS
        if (fail_local_update_for_test) throw Error("替换后模拟更新失败（测试故障注入）");
#endif
        auto config = load_json(config_backup);
        // 保留所有本机参数、扩展字段及身份，只更新版本元数据。
        if (config.contains("agent_version") && config.value("agent_version", std::string{}) != version) {
            config["agent_version"] = version;
            config_changed = true;
            write_json_atomic(installation.config, config);
        }
        record("starting_updated_service");
        const auto pid = start_owned_agent_service(installation.executable, installation.config);
        verify_local_service(installation, pid, hash);
        record("committed");
        committed = true;
        std::filesystem::remove(backup);
        std::filesystem::remove(config_backup);
        std::filesystem::remove(journal);
    } catch (const std::exception& error) {
        std::string failure = error.what();
        if (committed)
            throw Error("更新已成功，但备份清理未完成：" + failure + "；请检查 " + path_to_utf8(journal));
        try {
            if (switched) {
                (void)stop_owned_agent_service(installation.executable, installation.config);
                std::filesystem::rename(installation.executable, staged);
                staged_created = true;
            }
            if (backed_up) std::filesystem::rename(backup, installation.executable);
            if (config_changed) {
                check(CopyFileW(config_backup.c_str(), installation.config.c_str(), FALSE) != FALSE, "恢复原配置失败");
                protect_copied_file(installation.config);
            }
            if (stop_attempted) {
                const auto pid = start_owned_agent_service(installation.executable, installation.config);
                verify_local_service(installation, pid, old_hash);
            }
            if (staged_created) std::filesystem::remove(staged);
            if (config_saved) std::filesystem::remove(config_backup);
            std::filesystem::remove(journal);
        } catch (const std::exception& rollback) {
            failure += "；恢复未完成：";
            failure += rollback.what();
            try { record("rollback_failed", failure); } catch (...) {}
            throw Error(failure + "；请保留现场并检查 " + path_to_utf8(journal));
        }
        throw Error("更新失败，已保持或恢复原有安装：" + failure);
    }
    std::cout << "更新成功，原有配置和本机硬件身份已保留。\n";
    (void)inspect_service(installation);
    show_status(installation);
}

// 历史脚本保护了 agent/work，却遗漏 storage_root 和 mirror。
// 仅修复这两个可信所有者的目录；可执行文件、配置或工作根不安全时仍拒绝接管。
void repair_legacy_permissions(const Installation& installation) {
    if (!installation.repair_permissions) return;
    (void)verify_agent_image(current_executable(), true);
    const Handle root_lock{CreateFileW(installation.root.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
    check(root_lock.value != INVALID_HANDLE_VALUE, "无法锁定旧安装根目录");
    const Handle agent_lock{CreateFileW(installation.config.parent_path().c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
    check(agent_lock.value != INVALID_HANDLE_VALUE, "无法锁定旧 Agent 目录");
    (void)validate_install_permissions(installation.root, true);
    const auto journal = installation.config.parent_path() / L"install-acl-repair.json";
    if (std::filesystem::exists(journal)) throw Error("发现未完成权限修复，请检查 " + path_to_utf8(journal));
    write_json_atomic(journal, {{"stage", "repairing_permissions"}, {"root", path_to_utf8(installation.root)}});
    try {
        LocalMemory descriptor;
        check(ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)", SDDL_REVISION_1,
            reinterpret_cast<PSECURITY_DESCRIPTOR*>(&descriptor.value), nullptr) != FALSE,
            "创建目录权限失败");
        for (const auto& path : {installation.root, installation.root / L"mirror"}) {
            if (!std::filesystem::exists(path) || !validate_acl(path, true)) continue;
            // SetFileSecurity 保留现有子对象 ACL；高层 SetSecurityInfo 会传播继承，不能用于此迁移。
            Handle directory;
            if (!same_path(path, installation.root)) {
                directory.value = CreateFileW(path.c_str(), GENERIC_READ,
                    FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                    FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
                check(directory.value != INVALID_HANDLE_VALUE, "打开待修复目录失败");
            }
            reject_reparse(path);
            check(SetFileSecurityW(path.c_str(), DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
                descriptor.value) != FALSE, "修复目录权限失败");
        }
        validate_install_tree(installation.root);
        std::filesystem::remove(journal);
        std::cout << "已修复旧版安装根目录权限；现有文件、配置及任务目录权限保持不变。\n";
    } catch (const std::exception& error) {
        throw Error(std::string("旧版目录权限修复未完成：") + error.what() +
            "；已保留诊断记录 " + path_to_utf8(journal));
    }
}

void install_or_inspect() {
    const AgentInstallLock lock;
    const ComScope com;
    Installation existing;
    const bool installed = inspect_service(existing);
    LegacyTask legacy;
    if (installed && legacy.task && !same_path(existing.root, legacy.installation.root))
        throw Error("服务与历史任务属于不同安装目录；未修改，请管理员核实。");
    if (installed) repair_legacy_permissions(existing);
    else if (legacy.task) repair_legacy_permissions(legacy.installation);
    if (legacy.task) {
        bool stopped = false;
        bool service_created = false;
        try {
            stopped = true;
            legacy.stop();
            if (!installed) {
                const auto config = load_agent_config(legacy.installation.config);
                const auto result = ensure_agent_service_at(legacy.installation.executable,
                    legacy.installation.config, config.local_work_root, true);
                service_created = result.change == ServiceChange::Created;
            } else if (existing.state != SERVICE_RUNNING || !existing.automatic) {
                throw Error("现有服务不正常，不能删除旧任务；请先修复服务。");
            }
            legacy.erase();
            stopped = false;
            std::cout << "已安全迁移历史计划任务；原有 EXE、配置及硬件身份保持不变。\n";
        } catch (const std::exception& error) {
            std::string failure = error.what();
            try {
                if (service_created) (void)remove_agent_service_at(legacy.installation.executable, legacy.installation.config);
                if (stopped) legacy.restore();
            } catch (const std::exception& rollback) { failure += "; 恢复失败："; failure += rollback.what(); }
            throw Error(failure);
        }
        (void)inspect_service(existing);
        update_or_show(existing);
        return;
    }
    if (installed) { update_or_show(existing); return; }

    PWSTR known_folder{};
    com_check(SHGetKnownFolderPath(FOLDERID_ProgramData, KF_FLAG_DEFAULT, nullptr, &known_folder), "定位本机 ProgramData 失败");
    const Path root = Path(known_folder) / L"SatsumaTestLab";
    CoTaskMemFree(known_folder);
    // 无法证明归属的旧目录或中断现场只报告，不覆盖、不清理。
    for (const Path& candidate : {root, Path(L"D:\\SatsumaTestLab")}) {
        if (std::filesystem::exists(candidate))
            throw Error("发现既有目录，但没有可确认的服务或历史任务。请管理员检查安装/升级记录：" + path_to_utf8(candidate));
    }
    if (GetDriveTypeW(root.root_path().c_str()) != DRIVE_FIXED)
        throw Error("ProgramData 必须位于本机固定磁盘。");
    ULARGE_INTEGER available{};
    check(GetDiskFreeSpaceExW(root.parent_path().c_str(), &available, nullptr, nullptr) != FALSE, "读取磁盘空间失败");
    if (available.QuadPart < 2ULL * 1024 * 1024 * 1024) throw Error("安装盘可用空间不足 2 GiB。");
    const Path source = current_executable();
    // 保持源文件只读共享，复制与校验期间禁止修改或替换。
    const Handle source_lock{CreateFileW(source.c_str(), GENERIC_READ, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
    check(source_lock.value != INVALID_HANDLE_VALUE, "无法锁定安装介质");
    (void)verify_agent_image(source, true);
    const std::string hash = sha256_file(source);
    create_private_directory(root);
    const Path agent = root / L"agent";
    const Path binary = agent / L"bin" / L"SatsumaVM.exe";
    const Path config_path = agent / L"agent.json";
    try {
        create_private_directory(agent);
        write_json_atomic(agent / L"install-pending.json", {{"stage", "preparing"}, {"source_sha256", hash}});
        create_private_directory(agent / L"bin");
        create_private_directory(root / L"work");
        create_private_directory(root / L"mirror");
        check(CopyFileW(source.c_str(), binary.c_str(), TRUE) != FALSE, "复制 EXE 失败");
        protect_copied_file(binary);
        if (sha256_file(binary) != hash) throw Error("EXE 复制后的 SHA-256 校验失败。");
        (void)verify_agent_image(binary, true);
        write_json_atomic(config_path, {{"schema_version", 1}, {"protocol_version", 4},
            {"agent_version", kVersion}, {"storage_root", path_to_utf8(root)},
            {"mirror_root", path_to_utf8(root / L"mirror")},
            {"transport", {{"host_cid", 2}, {"vmci_port", 42510}, {"request_timeout_ms", 10000}}},
            {"poll_interval_ms", 1000}, {"reconnect_interval_ms", 1000}});
        const auto config = load_agent_config(config_path);
        validate_install_tree(root);
        write_json_atomic(agent / L"install-pending.json", {{"stage", "starting_service"}});
        (void)ensure_agent_service_at(binary, config_path, config.local_work_root, true);
        std::filesystem::remove(agent / L"install-pending.json");
    } catch (const std::exception& error) {
        std::string failure = error.what();
        try { write_json_atomic(agent / L"install-pending.json", {{"stage", "failed"}, {"error", failure}}); }
        catch (...) {}
        throw Error(failure + "\n首次安装未完成，已保留诊断现场：" + path_to_utf8(root) +
            "\n请管理员检查 agent/install-pending.json 和 agent/agent-startup-error.log，确认服务停止并核实目录归属后再人工处理。");
    }
    std::cout << "安装成功。Guest 重启后服务将自动运行，并持续连接 Host VMCI。\n";
    (void)inspect_service(existing);
    show_status(existing);
}
}  // namespace

AgentInstallLock::AgentInstallLock() {
    LocalMemory descriptor;
    check(ConvertStringSecurityDescriptorToSecurityDescriptorW(
        L"D:P(A;;GA;;;SY)(A;;GA;;;BA)", SDDL_REVISION_1,
        reinterpret_cast<PSECURITY_DESCRIPTOR*>(&descriptor.value), nullptr) != FALSE, "创建安装锁权限失败");
    SECURITY_ATTRIBUTES attributes{sizeof(attributes), descriptor.value, FALSE};
    handle_ = CreateMutexW(&attributes, FALSE, L"Global\\SatsumaVM-Install");
    check(handle_ != nullptr, "无法获取安装锁，请检查是否有其他安装/更新进程");
    const DWORD result = WaitForSingleObject(handle_, 0);
    if (result != WAIT_OBJECT_0 && result != WAIT_ABANDONED) {
        CloseHandle(handle_);
        handle_ = nullptr;
        throw Error("另一个 Satsuma 安装、维护或更新正在运行，请稍后重试。");
    }
}
AgentInstallLock::~AgentInstallLock() {
    if (handle_) { ReleaseMutex(handle_); CloseHandle(handle_); }
}

int compare_agent_versions(const std::string& left, const std::string& right) {
    const auto parse = [](const std::string& version) {
        std::array<unsigned long, 4> parts{};
        std::size_t start = 0, count = 0;
        do {
            if (count == parts.size()) throw Error("无法比较版本：" + version);
            const auto end = version.find('.', start);
            const auto limit = end == std::string::npos ? version.size() : end;
            const auto result = std::from_chars(version.data() + start, version.data() + limit, parts[count++]);
            if (result.ec != std::errc{} || result.ptr != version.data() + limit)
                throw Error("无法比较版本：" + version + "；未执行自动更新。");
            if (end == std::string::npos) break;
            start = end + 1;
        } while (true);
        if (count < 3) throw Error("版本必须包含主、次、修订号：" + version);
        return parts;
    };
    const auto a = parse(left), b = parse(right);
    return a < b ? -1 : a > b ? 1 : 0;
}

std::string verify_agent_image(const Path& path, bool checksum) {
    DWORD ignored{};
    const DWORD size = GetFileVersionInfoSizeW(path.c_str(), &ignored);
    check(size != 0, "EXE 缺少有效版本资源：" + path_to_utf8(path));
    std::vector<BYTE> data(size);
    check(GetFileVersionInfoW(path.c_str(), 0, size, data.data()) != FALSE, "读取 EXE 版本资源失败");
    const auto field = [&data](const wchar_t* name) {
        void* value{};
        UINT length{};
        const std::wstring query = L"\\StringFileInfo\\040904B0\\" + std::wstring(name);
        if (!VerQueryValueW(data.data(), query.c_str(), &value, &length) || !value || length == 0)
            throw Error("EXE 版本标识不完整。");
        return std::wstring(static_cast<wchar_t*>(value));
    };
    if (field(L"OriginalFilename") != L"SatsumaVM.exe" || field(L"InternalName") != L"SatsumaVM" ||
        field(L"ProductName") != L"Satsuma TestLab" ||
        field(L"FileDescription") != L"Satsuma TestLab Guest Agent")
        throw Error("EXE 不是 Satsuma Guest Agent，拒绝安装或接管。");
    if (checksum) {
        DWORD header{}, calculated{};
        auto name = path.native();
        if (MapFileAndCheckSumW(name.data(), &header, &calculated) != CHECKSUM_SUCCESS ||
            header == 0 || header != calculated)
            throw Error("EXE 完整性校验失败，请重新获取完整发行版 SatsumaVM.exe。");
    }
    return path_to_utf8(Path(field(L"ProductVersion")));
}

namespace {
bool validate_install_permissions(const Path& root, bool allow_legacy_roots) {
    reject_reparse(root);
    bool repair = validate_acl(root, allow_legacy_roots);
    // 任务引擎可给交互用户授权单个 job；安装器只验证全局工作根，不接管 job 数据。
    for (const auto* directory : {L"agent", L"work", L"mirror"}) {
        const auto path = root / directory;
        if (!std::filesystem::exists(path)) continue;
        reject_reparse(path);
        repair = validate_acl(path, allow_legacy_roots && std::wstring_view(directory) == L"mirror") || repair;
    }
    const auto agent = root / L"agent";
    if (std::filesystem::exists(agent)) {
        for (const auto& entry : std::filesystem::recursive_directory_iterator(agent)) {
            // Agent 原子写缓存时会短暂创建并改名临时 JSON；其父目录已验证。
            if (is_json_atomic_temporary_file(entry.path())) continue;
            reject_reparse(entry.path());
            validate_acl(entry.path());
        }
    }
    return repair;
}
}  // namespace
void validate_install_tree(const Path& root) { (void)validate_install_permissions(root, false); }

int run_agent_installer(bool elevated_child) {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    int result = 0;
    try {
        std::cout << "Satsuma Agent 安装、更新与状态检查 " << kVersion << "\n" << std::flush;
        SID_IDENTIFIER_AUTHORITY authority = SECURITY_NT_AUTHORITY;
        PSID administrators{};
        check(AllocateAndInitializeSid(&authority, 2, SECURITY_BUILTIN_DOMAIN_RID,
            DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0, &administrators) != FALSE, "检查管理员身份失败");
        BOOL admin{};
        const BOOL checked = CheckTokenMembership(nullptr, administrators, &admin);
        FreeSid(administrators);
        check(checked != FALSE, "检查管理员权限失败");
        if (!admin) {
            if (elevated_child) throw Error("没有取得管理员权限，安装已停止。");
            const Path source = current_executable();
            SHELLEXECUTEINFOW launch{sizeof(launch)};
            launch.fMask = SEE_MASK_NOCLOSEPROCESS;
            launch.lpVerb = L"runas";
            launch.lpFile = source.c_str();
            launch.lpParameters = L"--install-elevated";
            launch.nShow = SW_SHOWNORMAL;
            check(ShellExecuteExW(&launch) != FALSE, "未能取得管理员权限（可能取消了 UAC），未执行安装");
            const Handle child{launch.hProcess};
            return 0; // 管理员窗口负责执行和显示结果；普通权限进程立即退出。
        }
        install_or_inspect();
    } catch (const std::exception& error) {
        std::cerr << "操作未完成：" << error.what() << '\n';
        result = 1;
    }
    std::cout << "\n按 Enter 键关闭窗口。" << std::flush;
    std::string line;
    std::getline(std::cin, line);
    return result;
}
#ifdef SATSUMA_INSTALL_TESTS
int run_agent_installer_with_task_delete_failure_for_test() {
    fail_legacy_task_delete_for_test = true;
    const int result = run_agent_installer();
    fail_legacy_task_delete_for_test = false;
    return result;
}
int run_agent_installer_with_update_failure_for_test(const Path& source) {
    update_source_for_test = source;
    fail_local_update_for_test = true;
    const int result = run_agent_installer();
    fail_local_update_for_test = false;
    update_source_for_test.clear();
    return result;
}
#endif
}  // namespace satsuma::vm
