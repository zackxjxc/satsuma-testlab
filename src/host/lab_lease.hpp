// Host 实验室进程互斥和持久写租约接口。
#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

#include "satsuma/core/config.hpp"

namespace satsuma::host {

// 在一个 Host 写会话期间维持进程互斥和持久租约续期。
class LabLease {
public:
    ~LabLease();

    LabLease(const LabLease&) = delete;
    LabLease& operator=(const LabLease&) = delete;

    // 获取实验室写租约；恢复模式只接管同一 run_id 的死亡进程租约。
    [[nodiscard]] static std::unique_ptr<LabLease> acquire(
        const LabConfig& config,
        const std::filesystem::path& config_path,
        const std::string& command,
        std::optional<std::string> run_id = std::nullopt,
        bool recovery = false);

    // 发布 run 后把生成的 ID 绑定到持久租约。
    void attach_run(const std::string& run_id);

    // 写入终态并允许后续写会话进入。
    void release(const std::string& terminal_state);

    // 返回当前租约及进程存活摘要，不改变任何状态。
    [[nodiscard]] static nlohmann::json status(const LabConfig& config);

    // 显式放弃死亡进程留下的租约。
    [[nodiscard]] static nlohmann::json force_unlock(
        const LabConfig& config,
        const std::filesystem::path& config_path);

    // 确认普通 run 已终态后释放其持久租约。
    [[nodiscard]] static nlohmann::json finalize_run(
        const LabConfig& config,
        const std::filesystem::path& config_path,
        const std::string& run_id);

private:
    struct State;

    // 接管已经创建的 Win32 互斥和续租状态。
    explicit LabLease(std::unique_ptr<State> state);

    std::unique_ptr<State> state_; // Win32 句柄、租约 JSON 和续租线程
};

}  // namespace satsuma::host
