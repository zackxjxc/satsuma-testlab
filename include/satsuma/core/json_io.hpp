// JSON 文件读取和原子写入接口。
#pragma once

#include <filesystem>

#include <nlohmann/json.hpp>

#include "satsuma/core/errors.hpp"

namespace satsuma {

// 表示打开或读取 JSON 文件时发生的环境 I/O 错误。
class JsonIoError : public Error {
public:
    using Error::Error;
};

// 读取并解析 UTF-8 JSON 文件。
[[nodiscard]] nlohmann::json load_json(const std::filesystem::path& path);

// 判断路径是否为原子 JSON 写入过程产生的临时文件。
[[nodiscard]] bool is_json_atomic_temporary_file(const std::filesystem::path& path);

// 以临时文件加原子替换方式写入 UTF-8 JSON。
void write_json_atomic(const std::filesystem::path& path, const nlohmann::json& value);

// 只在父目录仍存在时原子写入，不得重建已被其他参与方清理的目录。
void write_json_atomic_existing_parent(
    const std::filesystem::path& path,
    const nlohmann::json& value);

}  // namespace satsuma
