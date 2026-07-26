// Satsuma VM Agent 开机自启动接口。
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace satsuma::vm {

// 计划任务中保存的 Agent 启动参数。
struct AgentAutostartSpec {
    std::filesystem::path executable;        // 当前 SatsumaVM 可执行文件
    std::filesystem::path config;            // 当前 Agent 配置文件
    std::filesystem::path working_directory; // Agent 本地工作目录
    std::wstring arguments;                  // Task Scheduler 原生命令行参数
};

// 注册计划任务后的变更类型。
enum class AutostartChange {
    Created,
    Updated,
    Unchanged,
};

// Agent 计划任务的注册结果。
struct AgentAutostartResult {
    AutostartChange change;             // 本次创建或更新
    std::string task_path;              // Task Scheduler 中的稳定路径
    bool start_requested = false;       // 本次是否请求立即启动
    std::uint32_t engine_process_id{};  // Task Scheduler 返回的引擎进程 ID
};

// 构造可测试的绝对路径和 Task Scheduler 参数。
[[nodiscard]] AgentAutostartSpec make_agent_autostart_spec(
    const std::filesystem::path& executable,
    const std::filesystem::path& config);

// 验证 Agent 可执行文件、配置和工作目录属于同一受保护安装布局。
void validate_agent_install_layout(
    const std::filesystem::path& executable,
    const std::filesystem::path& config,
    const std::filesystem::path& local_work_root);

#ifdef SATSUMA_AUTOSTART_TESTS
// 验证父目录 ACL，并允许只在父目录创建其他子项。
void validate_agent_autostart_parent_acl_for_test(const std::filesystem::path& path);

// 使用 Win32 文件身份判断两个路径是否引用同一对象。
[[nodiscard]] bool agent_autostart_same_file_for_test(
    const std::filesystem::path& left,
    const std::filesystem::path& right);

// 在内存任务定义中验证全部自启动策略可以往返。
[[nodiscard]] bool agent_autostart_definition_round_trip_for_test(
    const AgentAutostartSpec& spec);
#endif

// 创建或覆盖 SYSTEM 开机任务，并按需立即启动。
[[nodiscard]] AgentAutostartResult ensure_agent_autostart(
    const std::filesystem::path& config,
    const std::filesystem::path& local_work_root,
    bool start_now);

}  // namespace satsuma::vm
