// Windows 子进程环境块构造实现。
#include "process_environment.hpp"

#include <algorithm>
#include <cwchar>
#include <filesystem>
#include <string>
#include <vector>

#include <windows.h>

#include "satsuma/core/errors.hpp"
#include "satsuma/core/path.hpp"

namespace satsuma::vm {

std::vector<wchar_t> build_process_environment(
    const std::map<std::string, std::string>& overrides) {
    std::vector<std::wstring> entries;
    wchar_t* environment = GetEnvironmentStringsW();
    if (environment == nullptr) {
        throw Error("GetEnvironmentStringsW failed with Win32 error " +
            std::to_string(GetLastError()));
    }
    for (const wchar_t* cursor = environment; *cursor != L'\0'; cursor += std::wcslen(cursor) + 1) {
        entries.emplace_back(cursor);
    }
    FreeEnvironmentStringsW(environment);

    for (const auto& [name_utf8, value_utf8] : overrides) {
        if (name_utf8.empty() || name_utf8.find('=') != std::string::npos ||
            name_utf8.find('\0') != std::string::npos || value_utf8.find('\0') != std::string::npos) {
            throw Error("Process environment override is invalid");
        }
        const std::wstring name = path_from_utf8(name_utf8).native();
        const std::wstring prefix = name + L"=";
        std::erase_if(entries, [&prefix](const std::wstring& entry) {
            return entry.size() >= prefix.size() &&
                _wcsnicmp(entry.c_str(), prefix.c_str(), prefix.size()) == 0;
        });
        entries.push_back(prefix + path_from_utf8(value_utf8).native());
    }
    std::sort(entries.begin(), entries.end(), [](const std::wstring& left, const std::wstring& right) {
        return _wcsicmp(left.c_str(), right.c_str()) < 0;
    });

    std::vector<wchar_t> block;
    for (const std::wstring& entry : entries) {
        block.insert(block.end(), entry.begin(), entry.end());
        block.push_back(L'\0');
    }
    block.push_back(L'\0');
    return block;
}

}  // namespace satsuma::vm
