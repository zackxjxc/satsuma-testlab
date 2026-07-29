// 交互用户 Session 准备、Artifact 部署和进程启动接口。
#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

#include "process_runner.hpp"

namespace satsuma::vm {

// 无可用控制台用户时写入 execution.json 的稳定错误。
inline constexpr char kNoInteractiveUserSessionError[] =
    "No active interactive user session is available";

// 绑定一次活动控制台用户身份，启动前会重新验证 Session 和 SID。
class InteractiveUserSession {
public:
    // 释放用户 Token 和环境块。
    ~InteractiveUserSession();

    InteractiveUserSession(const InteractiveUserSession&) = delete;
    InteractiveUserSession& operator=(const InteractiveUserSession&) = delete;

    // 转移已准备的用户身份。
    InteractiveUserSession(InteractiveUserSession&& other) noexcept;

    // 转移赋值已准备的用户身份。
    InteractiveUserSession& operator=(InteractiveUserSession&& other) noexcept;

    // 获取当前活动控制台用户并创建本次运行工作目录。
    [[nodiscard]] static InteractiveUserSession acquire(
        const std::string& lab_id,
        const std::string& run_id,
        const std::filesystem::path& local_work_root = {},
        const std::string& vm_id = {});

    // 返回交互用户本地工作目录。
    [[nodiscard]] const std::filesystem::path& working_directory() const noexcept;

    // 返回准备阶段绑定的 Windows Session ID。
    [[nodiscard]] std::uint32_t session_id() const noexcept;

    // 返回准备阶段绑定的用户 SID。
    [[nodiscard]] const std::string& user_sid() const noexcept;

    // 以交互用户身份把共享 Artifact 部署到工作目录。
    [[nodiscard]] std::filesystem::path deploy_file(
        const std::filesystem::path& source,
        const std::filesystem::path& relative_destination) const;

    // 通过同一 SatsumaVM helper 启动进程并维持 Job/超时/取消语义。
    [[nodiscard]] ProcessResult run(
        const std::filesystem::path& helper_executable,
        const ProcessRequest& request) const;

private:
    struct State;

    // 接管已经验证的用户身份状态。
    explicit InteractiveUserSession(std::unique_ptr<State> state);

    std::unique_ptr<State> state_;  // Token、环境、Session 和工作目录
};

// 用户 Session 内的隐藏 helper 入口。
[[nodiscard]] int run_interactive_process_helper(
    const std::filesystem::path& request_path);

#ifdef SATSUMA_INTERACTIVE_TESTS
// 验证无活动 Session 时使用稳定错误且不进入启动流程。
void validate_interactive_session_id_for_test(std::uint32_t session_id);

// 验证生产交互执行只接受 LocalSystem 调用方。
void validate_interactive_caller_for_test(bool is_local_system);

// 注入无活动 Session，供 Agent 终态失败测试使用。
void set_interactive_session_unavailable_for_test(bool unavailable);

// 注入准备后身份变化，验证启动门禁不会回落 SYSTEM。
void set_interactive_identity_changed_for_test(bool changed);

// 注入 helper 挂起后的身份变化，验证恢复前门禁和清理。
void set_interactive_resume_identity_changed_for_test(bool changed);

// 返回最近一次测试启动的 helper PID。
[[nodiscard]] std::uint32_t last_interactive_helper_pid_for_test() noexcept;
#endif

}  // namespace satsuma::vm
