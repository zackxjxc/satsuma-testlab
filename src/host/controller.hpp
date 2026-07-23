// Host 任务物化和报告汇总接口。
#pragma once

#include <filesystem>
#include <string>

#include <nlohmann/json.hpp>

#include "satsuma/core/config.hpp"
#include "satsuma/core/task.hpp"

namespace satsuma::host {

// 管理 Host 侧共享目录中的运行生命周期。
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

private:
    LabConfig config_;  // 当前实验室配置
};

}  // namespace satsuma::host
