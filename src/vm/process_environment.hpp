// Windows 子进程环境块构造接口。
#pragma once

#include <map>
#include <string>
#include <vector>

namespace satsuma::vm {

// 在当前进程环境上应用 UTF-8 覆盖并生成双 NUL 结尾的 Unicode 环境块。
[[nodiscard]] std::vector<wchar_t> build_process_environment(
    const std::map<std::string, std::string>& overrides);

}  // namespace satsuma::vm
