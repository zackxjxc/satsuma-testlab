// SatsumaVM Windows Service 生命周期和安装接口。
#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace satsuma::vm {

inline constexpr std::wstring_view kAgentServiceName = L"SatsumaVM";
inline constexpr std::wstring_view kAgentServiceDisplayName = L"SatsumaVM Agent";

// Windows Service 中保存的 Agent 路径和基础恢复策略。
struct AgentServiceSpec {
    std::filesystem::path executable;        // 当前 SatsumaVM 可执行文件
    std::filesystem::path config;            // 当前 Agent 配置文件
    std::filesystem::path working_directory; // Agent 固定工作目录
    std::wstring binary_path;                // SCM 原生命令行
    std::array<std::uint32_t, 3> restart_delays_ms{5'000, 15'000, 60'000};
    std::uint32_t failure_reset_seconds{86'400}; // 失败动作序列重置周期
    bool delayed_auto_start{false};              // 普通自动启动，Agent 自身负责 VMCI 重连
    bool restart_on_non_crash{true};             // 非崩溃失败也执行恢复动作
};

// 注册 Windows Service 后的变更类型。
enum class ServiceChange {
    Created,
    Updated,
    Unchanged,
};

// Agent Windows Service 的注册结果。
struct AgentServiceResult {
    ServiceChange change;              // 本次创建或更新
    std::string service_name;          // SCM 中的稳定服务名
    bool start_requested = false;      // 本次是否要求服务运行
    std::uint32_t process_id{};        // 当前服务进程 ID
};

// Agent Windows Service 的停止结果。
struct AgentServiceStopResult {
    bool existed{false};     // 同名 Service 是否存在
    bool was_active{false};  // 停止前是否处于非 STOPPED 状态
};

// 构造规范路径和完整引用的 SCM 命令行。
[[nodiscard]] AgentServiceSpec make_agent_service_spec(
    const std::filesystem::path& executable,
    const std::filesystem::path& config);

// 进入 SCM dispatcher 并运行可取消的文件 Agent。
[[nodiscard]] int run_agent_service_dispatcher(const std::filesystem::path& config);

// 创建或更新 Agent Service，并按需立即启动。
[[nodiscard]] AgentServiceResult ensure_agent_service(
    const std::filesystem::path& config,
    const std::filesystem::path& local_work_root,
    bool start_now);

// 安装器从介质注册已验证的目标文件，不运行子进程。
[[nodiscard]] AgentServiceResult ensure_agent_service_at(
    const std::filesystem::path& executable,
    const std::filesystem::path& config,
    const std::filesystem::path& local_work_root,
    bool start_now);

[[nodiscard]] bool remove_agent_service_at(
    const std::filesystem::path& executable,
    const std::filesystem::path& config);

// 确认固定命令后处理 pending 状态并停止 Agent Service。
[[nodiscard]] AgentServiceStopResult stop_owned_agent_service(
    const std::filesystem::path& executable,
    const std::filesystem::path& config);

// 确认固定命令后启动既有 Agent Service，并返回 SCM PID。
[[nodiscard]] std::uint32_t start_owned_agent_service(
    const std::filesystem::path& executable,
    const std::filesystem::path& config);

// 停止并删除属于当前安装路径的 Agent Service。
[[nodiscard]] bool remove_agent_service(const std::filesystem::path& config);

#ifdef SATSUMA_SERVICE_TESTS
// 可观察的 SCM 状态快照。
struct AgentServiceStatusSnapshot {
    std::uint32_t state{};             // SERVICE_* 状态
    std::uint32_t controls_accepted{}; // 当前接受的控制码
    std::uint32_t win32_exit_code{};   // SCM 通用退出码
    std::uint32_t service_exit_code{}; // Service 专用退出码
    std::uint32_t checkpoint{};        // pending 状态进度
    std::uint32_t wait_hint_ms{};      // pending 状态等待提示
};

// 控制处理器测试结果。
struct AgentServiceControlTestResult {
    std::uint32_t handler_result{};                 // HandlerEx 返回码
    bool stop_requested{false};                     // stop_source 最终状态
    std::vector<AgentServiceStatusSnapshot> states; // 上报的状态序列
};

// 返回正常或异常路径应上报的完整状态序列。
[[nodiscard]] std::vector<AgentServiceStatusSnapshot>
agent_service_status_sequence_for_test(bool fail);

// 对指定控制码运行真实控制处理逻辑。
[[nodiscard]] AgentServiceControlTestResult
agent_service_control_for_test(std::uint32_t control);

// 验证已有 Service 命令行是否属于当前 Agent 可执行文件。
[[nodiscard]] bool agent_service_binary_belongs_for_test(
    const std::wstring& binary_path,
    const AgentServiceSpec& spec);

// 验证状态上报失败不能阻止 STOP 请求到达 Agent。
[[nodiscard]] bool agent_service_stop_survives_report_failure_for_test();

// 返回 Handler 注册窗口内 STOP 的状态序列。
[[nodiscard]] std::vector<AgentServiceStatusSnapshot>
agent_service_registration_window_sequence_for_test();

// 验证 STOPPED 在上报前已门闩，且上报时未持锁。
[[nodiscard]] bool agent_service_final_status_order_for_test();

// 返回指定配置对应的 Service 启动日志路径。
[[nodiscard]] std::filesystem::path agent_service_startup_log_path_for_test(
    const std::filesystem::path& config_path);
#endif

}  // namespace satsuma::vm
