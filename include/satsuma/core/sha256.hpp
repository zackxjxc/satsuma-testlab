// 文件 SHA-256 计算接口。
#pragma once

#include <filesystem>
#include <string>

namespace satsuma {

// 使用 Windows CNG 计算文件的十六进制 SHA-256。
[[nodiscard]] std::string sha256_file(const std::filesystem::path& path);

}  // namespace satsuma
