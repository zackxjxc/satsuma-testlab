// 双 VM 生命周期集成测试使用的可记录 vmrun 替身。
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

// 读取可选的宽字符环境变量。
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

// 记录 Host 发出的结构化命令，供 CMake 测试核对顺序。
void log_invocation(const int argc, wchar_t* argv[]) {
    const std::wstring log_value = read_environment(L"SATSUMA_MULTI_VM_VMRUN_LOG");
    if (log_value.empty()) {
        return;
    }
    std::wofstream output(
        std::filesystem::path(log_value),
        std::ios::binary | std::ios::app);
    if (!output) {
        std::wcerr << L"cannot open multi-VM vmrun log\n";
        std::exit(3);
    }
    output << argv[1];
    if (argc >= 3) {
        output << L'|' << std::filesystem::path(argv[2]).filename().wstring();
    }
    for (int index = 3; index < argc; ++index) {
        output << L'|' << argv[index];
    }
    output << L'\n';
}

// 返回 VMX 是否属于当前双 VM 测试。
[[nodiscard]] bool is_test_vmx(const std::filesystem::path& path) {
    const std::filesystem::path filename = path.filename();
    return filename == L"Client VM.vmx" || filename == L"Gateway VM.vmx";
}

// 记录隔离场景内的 Tools 查询次数，模拟 Agent 上线后 heartbeat 继续延迟一次。
[[nodiscard]] std::size_t tools_check_count(const std::filesystem::path& vmx) {
    std::filesystem::path marker = vmx;
    marker += L".tools-checked";
    std::size_t count = 0;
    if (std::filesystem::exists(marker)) {
        std::ifstream input(marker, std::ios::binary);
        input >> count;
        if (!input) {
            std::wcerr << L"cannot read Tools state marker\n";
            std::exit(3);
        }
    }
    std::ofstream output(marker, std::ios::binary | std::ios::trunc);
    if (!output) {
        std::wcerr << L"cannot create Tools state marker\n";
        std::exit(3);
    }
    output << count + 1 << '\n';
    return count;
}

// 返回软关机报错后的延迟状态标记路径。
[[nodiscard]] std::filesystem::path delayed_stop_marker(const std::filesystem::path& vmx) {
    std::filesystem::path marker = vmx;
    marker += L".stop-reconcile-pending";
    return marker;
}

// 返回启动报错后的两阶段延迟状态标记路径。
[[nodiscard]] std::filesystem::path delayed_start_marker(
    const std::filesystem::path& vmx,
    const wchar_t* suffix) {
    std::filesystem::path marker = vmx;
    marker += suffix;
    return marker;
}

// 将 Windows 路径按 vmrun 约定转换为 UTF-8 输出。
[[nodiscard]] std::string path_to_utf8(const std::filesystem::path& path) {
    const std::u8string value = path.u8string();
    return {value.begin(), value.end()};
}

}  // namespace

// 模拟双 VM 编排需要的最小 vmrun 命令集。
int wmain(const int argc, wchar_t* argv[]) {
    if (argc < 2) {
        std::cerr << "missing fake vmrun command\n";
        return 2;
    }
    log_invocation(argc, argv);
    const std::wstring command = argv[1];

    if (argc == 2 && command == L"list") {
        const std::wstring delayed_start_vmx =
            read_environment(L"SATSUMA_MULTI_VM_DELAYED_START_VMX");
        if (!delayed_start_vmx.empty()) {
            const std::filesystem::path vmx(delayed_start_vmx);
            const std::filesystem::path pending =
                delayed_start_marker(vmx, L".start-reconcile-pending");
            const std::filesystem::path ready =
                delayed_start_marker(vmx, L".start-reconcile-ready");
            if (std::filesystem::exists(pending)) {
                std::error_code rename_error;
                std::filesystem::rename(pending, ready, rename_error);
                if (rename_error) {
                    std::cerr << "cannot advance delayed start state\n";
                    return 3;
                }
                std::cout << "Total running VMs: 0\n";
                return 0;
            }
            if (std::filesystem::exists(ready)) {
                std::error_code remove_error;
                std::filesystem::remove(ready, remove_error);
                if (remove_error) {
                    std::cerr << "cannot complete delayed start state\n";
                    return 3;
                }
                std::cout << "Total running VMs: 1\n" << path_to_utf8(vmx) << '\n';
                return 0;
            }
        }
        const std::wstring delayed_stop_vmx =
            read_environment(L"SATSUMA_MULTI_VM_DELAYED_STOP_VMX");
        if (!delayed_stop_vmx.empty()) {
            const std::filesystem::path vmx(delayed_stop_vmx);
            const std::filesystem::path marker = delayed_stop_marker(vmx);
            if (std::filesystem::exists(marker)) {
                std::error_code remove_error;
                std::filesystem::remove(marker, remove_error);
                if (remove_error) {
                    std::cerr << "cannot advance delayed stop state\n";
                    return 3;
                }
                std::cout << "Total running VMs: 1\n" << path_to_utf8(vmx) << '\n';
                return 0;
            }
        }
        std::cout << "Total running VMs: 0\n";
        return 0;
    }
    if (argc == 3 && command == L"listSnapshots" && is_test_vmx(argv[2])) {
        std::cout << "Total snapshots: 1\nclean\n";
        return 0;
    }
    if (argc == 3 && command == L"checkToolsState" && is_test_vmx(argv[2])) {
        std::cout << (tools_check_count(argv[2]) < 2 ? "installed\n" : "running\n");
        return 0;
    }
    if (argc == 4 && command == L"start" && is_test_vmx(argv[2]) &&
        std::wstring(argv[3]) == L"nogui") {
        const bool fail_client_start =
            read_environment(L"SATSUMA_MULTI_VM_FAIL_CLIENT_START") == L"1";
        if (fail_client_start && std::filesystem::path(argv[2]).filename() == L"Client VM.vmx") {
            std::ofstream marker(
                delayed_start_marker(argv[2], L".start-reconcile-pending"),
                std::ios::binary | std::ios::trunc);
            marker << "pending\n";
            if (!marker) {
                std::cerr << "cannot create delayed start marker\n";
                return 3;
            }
            std::cerr << "injected reconciled client start failure\n";
            return 9;
        }
        return 0;
    }
    if (argc == 4 && command == L"stop" && is_test_vmx(argv[2]) &&
        (std::wstring(argv[3]) == L"soft" || std::wstring(argv[3]) == L"hard")) {
        const bool fail_gateway_soft_stop =
            read_environment(L"SATSUMA_MULTI_VM_FAIL_GATEWAY_SOFT_STOP") == L"1";
        if (fail_gateway_soft_stop && std::filesystem::path(argv[2]).filename() == L"Gateway VM.vmx" &&
            std::wstring(argv[3]) == L"soft") {
            std::ofstream marker(delayed_stop_marker(argv[2]), std::ios::binary | std::ios::trunc);
            marker << "pending\n";
            if (!marker) {
                std::cerr << "cannot create delayed stop marker\n";
                return 3;
            }
            std::cerr << "injected reconciled gateway stop failure\n";
            return 8;
        }
        return 0;
    }
    if (argc == 4 && command == L"revertToSnapshot" && is_test_vmx(argv[2]) &&
        std::wstring(argv[3]) == L"clean") {
        const bool fail_gateway =
            read_environment(L"SATSUMA_MULTI_VM_FAIL_GATEWAY_REVERT") == L"1";
        if (fail_gateway && std::filesystem::path(argv[2]).filename() == L"Gateway VM.vmx") {
            std::cerr << "injected gateway cleanup failure\n";
            return 7;
        }
        return 0;
    }

    std::cerr << "unsupported multi-VM fake vmrun command\n";
    return 2;
}
