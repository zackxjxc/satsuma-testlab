// VmrunProvider 测试使用的无害 vmrun 替身。
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>

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

// 返回电源关闭失败场景使用的跨进程运行状态文件。
[[nodiscard]] std::filesystem::path running_vm_state_path() {
    return std::filesystem::temp_directory_path() / L"satsuma-fake-vmrun-running-vm.txt";
}

}  // namespace

// 模拟 VmrunProvider 当前支持的结构化命令。
int wmain(const int argc, wchar_t* argv[]) {
    if (argc == 2 && std::wstring(argv[1]) == L"list") {
        std::ifstream state(running_vm_state_path());
        std::string running_path;
        std::getline(state, running_path);
        std::cout
            << "Total running VMs: " << (running_path.empty() ? 2 : 3) << "\n"
            << "C:\\VM Space\\VM-01.vmx\n"
            << "D:\\VM-02\\VM-02.vmx\n";
        if (!running_path.empty()) {
            std::cout << running_path << '\n';
        }
        state.close();
        std::error_code error;
        std::filesystem::remove(running_vm_state_path(), error);
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
        if (filename == L"VM 01.vmx" || filename == L"Tools Running VM.vmx") {
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
        std::wstring(argv[1]) == L"start" &&
        std::filesystem::path(argv[2]).filename() == L"VM 01.vmx" &&
        std::wstring(argv[3]) == L"nogui") {
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
        std::ofstream state(running_vm_state_path(), std::ios::binary | std::ios::trunc);
        state << std::filesystem::path(argv[2]).string();
        return state ? 7 : 3;
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
    std::cerr << "unsupported fake vmrun command\n";
    return 2;
}
