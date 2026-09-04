// Host Artifact 物理存储与 Guest 逻辑目标路径的映射。
#pragma once

#include <cstddef>
#include <filesystem>
#include <string>

#include "satsuma/core/path.hpp"
#include "satsuma/core/task.hpp"

namespace satsuma::host {

// 清单在运行发布后不可变，因此序号可以安全隔离不同 VM 的同名目标。
[[nodiscard]] inline std::filesystem::path artifact_storage_path(
    const std::filesystem::path& run_directory,
    const RunManifest& manifest,
    const std::size_t index) {
    const auto store = resolve_under_root(run_directory, L".artifacts");
    if (std::filesystem::exists(windows_file_path(store))) {
        return resolve_under_root(
            store, path_from_utf8(std::to_string(index) + ".bin"));
    }
    // 已发布的旧运行仍从原始逻辑路径读取；新布局缺件时禁止逐文件回退。
    return resolve_under_root(run_directory, manifest.artifacts.at(index).path);
}

}  // namespace satsuma::host
