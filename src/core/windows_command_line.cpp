// CreateProcessW 参数编码实现。
#include "satsuma/core/windows_command_line.hpp"

#include "satsuma/core/path.hpp"

namespace satsuma {

std::wstring quote_windows_argument(const std::wstring_view argument) {
    if (!argument.empty() && argument.find_first_of(L" \t\n\v\"") == std::wstring_view::npos) {
        return std::wstring(argument);
    }

    std::wstring quoted = L"\"";
    std::size_t backslashes = 0;
    for (const wchar_t value : argument) {
        if (value == L'\\') {
            ++backslashes;
            continue;
        }
        if (value == L'\"') {
            quoted.append(backslashes * 2 + 1, L'\\');
            quoted.push_back(L'\"');
            backslashes = 0;
            continue;
        }
        quoted.append(backslashes, L'\\');
        backslashes = 0;
        quoted.push_back(value);
    }
    quoted.append(backslashes * 2, L'\\');
    quoted.push_back(L'\"');
    return quoted;
}

std::vector<wchar_t> build_windows_command_line(
    const std::filesystem::path& program,
    const std::vector<std::string>& arguments) {
    std::wstring command = quote_windows_argument(program.native());
    for (const auto& argument : arguments) {
        command.push_back(L' ');
        command += quote_windows_argument(path_from_utf8(argument).native());
    }
    std::vector<wchar_t> buffer(command.begin(), command.end());
    buffer.push_back(L'\0');
    return buffer;
}

}  // namespace satsuma
