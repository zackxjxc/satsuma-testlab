// JSON 文件读取和原子写入接口。
#pragma once

#include <filesystem>

#include <nlohmann/json.hpp>

namespace satsuma {

// 读取并解析 UTF-8 JSON 文件。
[[nodiscard]] nlohmann::json load_json(const std::filesystem::path& path);

// 以临时文件加原子替换方式写入 UTF-8 JSON。
void write_json_atomic(const std::filesystem::path& path, const nlohmann::json& value);

}  // namespace satsuma
