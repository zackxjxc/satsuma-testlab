// VmrunProvider 测试使用的无害 vmrun 替身。
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

// 验证 Host 生成的 AI 快照名包含固定前缀和 14 位时间戳。
[[nodiscard]] bool is_generated_ai_snapshot(const std::wstring_view name) {
    constexpr std::wstring_view prefix = L"satsuma-ai-network-ready-";
    if (!name.starts_with(prefix) || name.size() != prefix.size() + 14) {
        return false;
    }
    return std::all_of(name.begin() + prefix.size(), name.end(), [](const wchar_t character) {
        return character >= L'0' && character <= L'9';
    });
}

// 返回快照晚到成功场景使用的跨进程状态文件。
[[nodiscard]] std::filesystem::path reconciliation_state_path(
    const std::filesystem::path& vmx) {
    return vmx.parent_path() / L"snapshot-reconciliation.state";
}

// 返回快照恢复瞬时失败场景使用的跨进程状态文件。
[[nodiscard]] std::filesystem::path snapshot_restore_retry_state_path(
    const std::filesystem::path& vmx) {
    return vmx.parent_path() / L"snapshot-restore-retry.state";
}

// 返回电源关闭失败场景使用的跨进程运行状态文件。
[[nodiscard]] std::filesystem::path running_vm_state_path() {
    return std::filesystem::temp_directory_path() / L"satsuma-fake-vmrun-running-vm.txt";
}

// 读取测试替身维护的额外运行 VM 状态。
[[nodiscard]] std::pair<std::string, std::string> read_running_vm_state() {
    std::ifstream state(running_vm_state_path(), std::ios::binary);
    std::string mode;
    std::string path;
    std::getline(state, mode);
    std::getline(state, path);
    return {mode, path};
}

// 写入测试替身维护的额外运行 VM 状态。
void write_running_vm_state(const std::string& mode, const std::string& path) {
    std::ofstream state(running_vm_state_path(), std::ios::binary | std::ios::trunc);
    state << mode << '\n' << path << '\n';
}

// 读取 Host 集成测试显式声明的基础运行 VM。
[[nodiscard]] std::wstring read_environment(const wchar_t* name) {
    std::size_t required = 0;
    if (_wgetenv_s(&required, nullptr, 0, name) != 0 || required <= 1) {
        return {};
    }
    std::vector<wchar_t> buffer(required);
    if (_wgetenv_s(&required, buffer.data(), buffer.size(), name) != 0) {
        return {};
    }
    return std::wstring(buffer.data());
}

}  // namespace

// 模拟 VmrunProvider 当前支持的结构化命令。
int wmain(const int argc, wchar_t* argv[]) {
    if (argc == 2 && std::wstring(argv[1]) == L"list") {
        const std::string running_path = read_running_vm_state().second;
        std::vector<std::string> paths = {
            "C:\\VM Space\\VM-01.vmx",
            "D:\\VM-02\\VM-02.vmx",
        };
        const std::wstring configured = read_environment(L"SATSUMA_FAKE_VMRUN_RUNNING_VMX");
        if (!configured.empty()) {
            paths.push_back(std::filesystem::path(configured).string());
        }
        if (!running_path.empty() &&
            std::find(paths.begin(), paths.end(), running_path) == paths.end()) {
            paths.push_back(running_path);
        }
        std::cout << "Total running VMs: " << paths.size() << '\n';
        for (const std::string& path : paths) {
            std::cout << path << '\n';
        }
        return 0;
    }
    if (argc == 3 &&
        std::wstring(argv[1]) == L"listSnapshots" &&
        std::filesystem::path(argv[2]).filename() == L"List Snapshots VM.vmx") {
        std::cout
            << "Total snapshots: 3\n"
            << "clean\n"
            << "satsuma-ai-network-ready\n"
            << "satsuma-ai-vm_01-ready\n";
        return 0;
    }
    if (argc == 3 &&
        std::wstring(argv[1]) == L"listSnapshots" &&
        std::filesystem::path(argv[2]).filename() == L"VM 01.vmx") {
        std::cout
            << "Total snapshots: 3\n"
            << "clean\n"
            << "satsuma-ai-obsolete\n"
            << "satsuma-ai-recovery-fail\n";
        return 0;
    }
    if (argc == 3 &&
        std::wstring(argv[1]) == L"listSnapshots" &&
        std::filesystem::path(argv[2]).filename() == L"Reconcile VM.vmx") {
        std::ifstream state(reconciliation_state_path(argv[2]));
        std::string snapshot_name;
        std::getline(state, snapshot_name);
        if (snapshot_name.empty()) {
            std::cout << "Total snapshots: 1\nclean\n";
        } else {
            std::cout << "Total snapshots: 2\nclean\n" << snapshot_name << '\n';
        }
        return 0;
    }
    if (argc == 3 && std::wstring(argv[1]) == L"checkToolsState") {
        const std::filesystem::path filename = std::filesystem::path(argv[2]).filename();
        if (filename == L"VM 01.vmx") {
            std::cout << "installed\n";
            return 0;
        }
        if (filename == L"Tools Running VM.vmx") {
            std::cout << "running\n";
            return 0;
        }
        if (filename == L"Tools Installed VM.vmx") {
            std::cout << "installed\n";
            return 0;
        }
        if (filename == L"Tools Unknown VM.vmx") {
            std::cout << "unknown\n";
            return 0;
        }
    }
    if (argc == 4 &&
        std::wstring(argv[1]) == L"getGuestIPAddress" &&
        (std::filesystem::path(argv[2]).filename() == L"VM 01.vmx" ||
         std::filesystem::path(argv[2]).filename() == L"Tools Installed VM.vmx") &&
        std::wstring(argv[3]) == L"-wait") {
        std::cout << "192.0.2.10\n";
        return 0;
    }
    if (argc == 4 &&
        std::wstring(argv[1]) == L"start" &&
        std::filesystem::path(argv[2]).filename() == L"VM 01.vmx" &&
        std::wstring(argv[3]) == L"nogui") {
        write_running_vm_state("persistent", std::filesystem::path(argv[2]).string());
        return 0;
    }
    if (argc == 4 &&
        std::wstring(argv[1]) == L"stop" &&
        ((std::filesystem::path(argv[2]).filename() == L"Soft VM.vmx" &&
          std::wstring(argv[3]) == L"soft") ||
         (std::filesystem::path(argv[2]).filename() == L"Hard VM.vmx" &&
          std::wstring(argv[3]) == L"hard") ||
         (std::filesystem::path(argv[2]).filename() == L"VM 01.vmx" &&
          (std::wstring(argv[3]) == L"soft" || std::wstring(argv[3]) == L"hard")))) {
        const auto [mode, path] = read_running_vm_state();
        if (mode == "persistent" && path == std::filesystem::path(argv[2]).string()) {
            std::error_code error;
            std::filesystem::remove(running_vm_state_path(), error);
        }
        return 0;
    }
    if (argc == 4 &&
        std::wstring(argv[1]) == L"stop" &&
        std::filesystem::path(argv[2]).filename() == L"Stop Reconciled VM.vmx" &&
        std::wstring(argv[3]) == L"hard") {
        return 7;
    }
    if (argc == 4 &&
        std::wstring(argv[1]) == L"stop" &&
        std::filesystem::path(argv[2]).filename() == L"Stop Failed VM.vmx" &&
        std::wstring(argv[3]) == L"hard") {
        const auto [mode, path] = read_running_vm_state();
        if (mode == "persistent" && path == std::filesystem::path(argv[2]).string()) {
            std::error_code error;
            return std::filesystem::remove(running_vm_state_path(), error) && !error ? 0 : 3;
        }
        write_running_vm_state("persistent", std::filesystem::path(argv[2]).string());
        return 7;
    }
    if (argc == 4 &&
        std::wstring(argv[1]) == L"revertToSnapshot" &&
        ((std::filesystem::path(argv[2]).filename() == L"Snapshot VM.vmx" &&
          std::wstring(argv[3]) == L"Clean Base") ||
         (std::filesystem::path(argv[2]).filename() == L"VM 01.vmx" &&
          std::wstring(argv[3]) == L"clean"))) {
        return 0;
    }
    if (argc == 4 &&
        std::wstring(argv[1]) == L"revertToSnapshot" &&
        std::filesystem::path(argv[2]).filename() == L"Snapshot Retry VM.vmx" &&
        std::wstring(argv[3]) == L"Clean Base") {
        const std::filesystem::path state_path = snapshot_restore_retry_state_path(argv[2]);
        if (!std::filesystem::is_regular_file(state_path)) {
            std::ofstream state(state_path, std::ios::binary | std::ios::trunc);
            state << "retry";
            std::cerr << "Error: The virtual machine is busy.\n";
            return state ? 7 : 3;
        }
        std::error_code error;
        return std::filesystem::remove(state_path, error) && !error ? 0 : 3;
    }
    if (argc == 4 &&
        std::wstring(argv[1]) == L"snapshot" &&
        ((std::filesystem::path(argv[2]).filename() == L"Create VM.vmx" &&
          std::wstring(argv[3]) == L"satsuma-ai-network-ready") ||
         (std::filesystem::path(argv[2]).filename() == L"VM 01.vmx" &&
          is_generated_ai_snapshot(argv[3])))) {
        return 0;
    }
    if (argc == 4 &&
        std::wstring(argv[1]) == L"snapshot" &&
        std::filesystem::path(argv[2]).filename() == L"Reconcile VM.vmx") {
        std::ofstream state(reconciliation_state_path(argv[2]), std::ios::binary | std::ios::trunc);
        state << std::filesystem::path(argv[3]).string();
        return state ? 7 : 3;
    }
    if (argc == 4 &&
        std::wstring(argv[1]) == L"deleteSnapshot" &&
        ((std::filesystem::path(argv[2]).filename() == L"Delete VM.vmx" &&
          std::wstring(argv[3]) == L"satsuma-ai-obsolete") ||
         (std::filesystem::path(argv[2]).filename() == L"VM 01.vmx" &&
          std::wstring(argv[3]) == L"satsuma-ai-obsolete"))) {
        return 0;
    }
    if (argc == 4 &&
        std::wstring(argv[1]) == L"deleteSnapshot" &&
        std::filesystem::path(argv[2]).filename() == L"Reconcile VM.vmx") {
        std::error_code error;
        const bool removed = std::filesystem::remove(reconciliation_state_path(argv[2]), error);
        return removed && !error ? 7 : 3;
    }
    std::cerr << "unsupported fake vmrun command:";
    for (int index = 1; index < argc; ++index) {
        std::wcerr << L" [" << argv[index] << L"]";
    }
    std::cerr << '\n';
    return 2;
}
