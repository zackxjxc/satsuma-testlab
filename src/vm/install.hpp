// Guest 单文件安装入口；服务和更新共用机器级互斥锁。
#pragma once

#include <filesystem>
#include <string>

namespace satsuma::vm {

class AgentInstallLock {
public:
    AgentInstallLock();
    ~AgentInstallLock();
    AgentInstallLock(const AgentInstallLock&) = delete;
    AgentInstallLock& operator=(const AgentInstallLock&) = delete;
private:
    void* handle_{};
};

[[nodiscard]] int run_agent_installer(bool elevated_child = false);
// 只读验证，不执行候选文件。供安装入口及回归测试共用。
[[nodiscard]] std::string verify_agent_image(const std::filesystem::path& path, bool checksum);
void validate_install_tree(const std::filesystem::path& root);
// 创建或只读验证 SYSTEM/管理员专用的证据暂存根，不修复或接管未知目录。
[[nodiscard]] std::filesystem::path prepare_agent_staging_root(const std::filesystem::path& root);
// 独占创建证据尝试目录并设置显式私有 DACL，拒绝覆盖已有目录。
void create_agent_attempt_directory(const std::filesystem::path& path);
// 数字版本比较：返回 -1/0/1；不认识的版本格式拒绝自动更新。
[[nodiscard]] int compare_agent_versions(const std::string& left, const std::string& right);

#ifdef SATSUMA_INSTALL_TESTS
// 仅测试库：在服务成功启动后模拟删除旧任务失败，验证真实 SCM 回滚。
[[nodiscard]] int run_agent_installer_with_task_delete_failure_for_test();
[[nodiscard]] int run_agent_installer_with_update_failure_for_test(const std::filesystem::path& source);
#endif

}  // namespace satsuma::vm
