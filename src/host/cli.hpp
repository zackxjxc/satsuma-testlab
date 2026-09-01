// SatsumaHost 命令行应用入口。
#pragma once

namespace satsuma::host {

// 解析并执行一个 Host 命令，返回稳定进程退出码。
[[nodiscard]] int run_cli(int argc, wchar_t* argv[]);

}  // namespace satsuma::host
