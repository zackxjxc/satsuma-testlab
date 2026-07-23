// VmrunProvider 测试使用的无害 vmrun 替身。
#include <algorithm>
#include <filesystem>
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

}  // namespace

// 模拟 VmrunProvider 当前支持的结构化命令。
int wmain(const int argc, wchar_t* argv[]) {
    if (argc == 2 && std::wstring(argv[1]) == L"list") {
        std::cout
            << "Total running VMs: 2\n"
            << "C:\\VM Space\\Client.vmx\n"
            << "D:\\Gateway\\Gateway.vmx\n";
        return 0;
    }
    if (argc == 3 &&
        std::wstring(argv[1]) == L"listSnapshots" &&
        std::filesystem::path(argv[2]).filename() == L"List Snapshots VM.vmx") {
        std::cout
            << "Total snapshots: 3\n"
            << "clean\n"
            << "satsuma-ai-network-ready\n"
            << "satsuma-ai-client-ready\n";
        return 0;
    }
    if (argc == 3 &&
        std::wstring(argv[1]) == L"listSnapshots" &&
        std::filesystem::path(argv[2]).filename() == L"Client VM.vmx") {
        std::cout << "Total snapshots: 2\nclean\nsatsuma-ai-obsolete\n";
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
          std::wstring(argv[3]) == L"hard") ||
         (std::filesystem::path(argv[2]).filename() == L"Client VM.vmx" &&
          std::wstring(argv[3]) == L"soft"))) {
        return 0;
    }
    if (argc == 4 &&
        std::wstring(argv[1]) == L"revertToSnapshot" &&
        ((std::filesystem::path(argv[2]).filename() == L"Snapshot VM.vmx" &&
          std::wstring(argv[3]) == L"Clean Base") ||
         (std::filesystem::path(argv[2]).filename() == L"Client VM.vmx" &&
          std::wstring(argv[3]) == L"clean"))) {
        return 0;
    }
    if (argc == 4 &&
        std::wstring(argv[1]) == L"snapshot" &&
        ((std::filesystem::path(argv[2]).filename() == L"Create VM.vmx" &&
          std::wstring(argv[3]) == L"satsuma-ai-network-ready") ||
         (std::filesystem::path(argv[2]).filename() == L"Client VM.vmx" &&
          is_generated_ai_snapshot(argv[3])))) {
        return 0;
    }
    if (argc == 4 &&
        std::wstring(argv[1]) == L"deleteSnapshot" &&
        ((std::filesystem::path(argv[2]).filename() == L"Delete VM.vmx" &&
          std::wstring(argv[3]) == L"satsuma-ai-obsolete") ||
         (std::filesystem::path(argv[2]).filename() == L"Client VM.vmx" &&
          std::wstring(argv[3]) == L"satsuma-ai-obsolete"))) {
        return 0;
    }
    std::cerr << "unsupported fake vmrun command\n";
    return 2;
}
