// UTF-8 路径转换和共享根目录边界校验接口。
#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace satsuma {

// 将 UTF-8 字符串转换为 Windows 文件系统路径。
[[nodiscard]] std::filesystem::path path_from_utf8(std::string_view value);

// 将文件系统路径转换为 UTF-8 字符串。
[[nodiscard]] std::string path_to_utf8(const std::filesystem::path& value);

// 在有限时间内重试 Windows 瞬时占用导致的同卷路径改名。
void rename_path_with_retry(
    const std::filesystem::path& source,
    const std::filesystem::path& destination);

// 验证任务路径是非空、安全的相对路径。
void validate_relative_path(const std::filesystem::path& relative);

// 解析共享根目录内的相对路径，并拒绝越界和重解析点。
[[nodiscard]] std::filesystem::path resolve_under_root(
    const std::filesystem::path& root,
    const std::filesystem::path& relative);

}  // namespace satsuma
