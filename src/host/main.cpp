// SatsumaHost Windows 进程入口。
#include "cli.hpp"

int wmain(const int argc, wchar_t* argv[]) {
    return satsuma::host::run_cli(argc, argv);
}
