// CreateProcessW 参数编码接口。
#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace satsuma {

// 按 CommandLineToArgvW 规则引用单个 UTF-16 参数。
[[nodiscard]] std::wstring quote_windows_argument(std::wstring_view argument);

// 生成 CreateProcessW 所需的可修改 UTF-16 命令行缓冲区。
[[nodiscard]] std::vector<wchar_t> build_windows_command_line(
    const std::filesystem::path& program,
    const std::vector<std::string>& arguments);

// 拼接已经由内部适配器完成解释器转义的参数，不接受用户直接控制。
[[nodiscard]] std::vector<wchar_t> build_windows_command_line_verbatim(
    const std::filesystem::path& program,
    const std::vector<std::string>& arguments);

}  // namespace satsuma
