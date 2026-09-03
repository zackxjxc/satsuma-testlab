// 根据用户选定的 VM 和发行包生成可交付的 Host 配置。
#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace satsuma::host {

// 只生成新配置并查询快照，不启动 Host 服务、不改变 VM 电源或协议状态。
[[nodiscard]] nlohmann::json initialize_lab(
    const std::vector<std::wstring>& arguments,
    const std::filesystem::path& executable_directory);

}  // namespace satsuma::host
