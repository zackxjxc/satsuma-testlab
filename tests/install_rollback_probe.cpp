// 仅在隔离 Guest 中显式执行；生产 EXE 不包含此故障入口。
#include <string_view>
#include "install.hpp"

int wmain(int argc, wchar_t* argv[]) {
    if (argc == 3 && std::wstring_view(argv[1]) == L"--simulate-local-update-failure")
        return satsuma::vm::run_agent_installer_with_update_failure_for_test(argv[2]);
    if (argc != 2 || std::wstring_view(argv[1]) != L"--simulate-task-delete-failure") return 2;
    return satsuma::vm::run_agent_installer_with_task_delete_failure_for_test();
}
