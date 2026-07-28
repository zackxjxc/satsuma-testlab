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
        std::cout << "Total running VMs: 0\n";
        return 0;
    }
    if (argc == 3 && command == L"listSnapshots" && is_test_vmx(argv[2])) {
        std::cout << "Total snapshots: 1\nclean\n";
        return 0;
    }
    if (argc == 3 && command == L"checkToolsState" && is_test_vmx(argv[2])) {
        std::cout << "running\n";
        return 0;
    }
    if (argc == 4 && command == L"start" && is_test_vmx(argv[2]) &&
        std::wstring(argv[3]) == L"nogui") {
        return 0;
    }
    if (argc == 4 && command == L"stop" && is_test_vmx(argv[2]) &&
        (std::wstring(argv[3]) == L"soft" || std::wstring(argv[3]) == L"hard")) {
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
