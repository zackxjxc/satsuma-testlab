// Host 任务物化和报告汇总接口。
#pragma once

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

#include "satsuma/core/config.hpp"
#include "satsuma/core/task.hpp"
#include "satsuma/core/update.hpp"

namespace satsuma::host {

// 管理 Host 侧传输状态根中的运行生命周期。
class Controller {
public:
    // 使用已验证的实验室配置创建 Controller。
    explicit Controller(LabConfig config);

    // 将任务计划和 Artifact 原子物化到新的运行目录。
    [[nodiscard]] RunManifest create_run(const std::filesystem::path& plan_path) const;

    // 将内存中的可信任务计划原子物化到新的运行目录。
    [[nodiscard]] RunManifest create_run(const TaskPlan& plan) const;

    // 汇总指定运行当前已落盘的执行结果。
    [[nodiscard]] nlohmann::json build_report(const std::string& run_id) const;

    // 列出传输状态根中的运行及其当前状态。
    [[nodiscard]] nlohmann::json list_runs() const;

    // 原子发布单个运行的取消请求。
    [[nodiscard]] nlohmann::json cancel_run(
        const std::string& run_id,
        const std::string& reason) const;

    // 删除超过保留数量且已经终态完成的共享运行。
    [[nodiscard]] nlohmann::json prune_runs(std::size_t keep) const;

    // 原子发布单个 VM 的 Agent 更新候选和清单。
    [[nodiscard]] AgentUpdateManifest publish_agent_update(
        const std::string& vm_id,
        const std::filesystem::path& binary,
        const std::string& version,
        std::optional<std::string> next_vm_id = std::nullopt) const;

    // 有限等待 Agent 更新终态，成功后删除共享更新目录。
    [[nodiscard]] AgentUpdateResult wait_agent_update(
        const std::string& vm_id,
        const std::string& update_id,
        std::chrono::seconds timeout) const;

private:
    LabConfig config_;  // 当前实验室配置
};

}  // namespace satsuma::host
