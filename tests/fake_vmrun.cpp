// VmrunProvider 测试使用的无害 vmrun 替身。
#include <filesystem>
#include <iostream>
#include <string>

// 模拟 vmrun list/start/stop/revertToSnapshot 的稳定行为。
int wmain(const int argc, wchar_t* argv[]) {
    if (argc == 2 && std::wstring(argv[1]) == L"list") {
        std::cout
            << "Total running VMs: 2\n"
            << "C:\\VM Space\\Client.vmx\n"
            << "D:\\Gateway\\Gateway.vmx\n";
        return 0;
    }
    if (argc == 4 &&
        std::wstring(argv[1]) == L"start" &&
        std::filesystem::path(argv[2]).filename() == L"Client VM.vmx" &&
        std::wstring(argv[3]) == L"nogui") {
        return 0;
    }
    if (argc == 4 &&
        std::wstring(argv[1]) == L"stop" &&
        ((std::filesystem::path(argv[2]).filename() == L"Soft VM.vmx" &&
          std::wstring(argv[3]) == L"soft") ||
         (std::filesystem::path(argv[2]).filename() == L"Hard VM.vmx" &&
          std::wstring(argv[3]) == L"hard"))) {
        return 0;
    }
    if (argc == 4 &&
        std::wstring(argv[1]) == L"revertToSnapshot" &&
        std::filesystem::path(argv[2]).filename() == L"Snapshot VM.vmx" &&
        std::wstring(argv[3]) == L"Clean Base") {
        return 0;
    }
    std::cerr << "unsupported fake vmrun command\n";
    return 2;
}
