// Host 单 VM 生命周期编排接口。
#pragma once

#include <chrono>
#include <filesystem>

#include <nlohmann/json.hpp>

#include "satsuma/core/config.hpp"

namespace satsuma::host {

// 按任务生命周期策略执行并归档一次单 VM 运行。
class Orchestrator {
public:
    // 保存一份已验证的实验室配置。
    explicit Orchestrator(LabConfig config);

    // 执行任务计划并返回包含业务与恢复结果的机器可读报告。
    [[nodiscard]] nlohmann::json execute(
        const std::filesystem::path& plan_path,
        std::chrono::seconds timeout) const;

private:
    LabConfig config_;  // 当前实验室配置
};

}  // namespace satsuma::host
