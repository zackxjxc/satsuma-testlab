// Host 自动化环境检测接口。
#pragma once

#include <chrono>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

#include "satsuma/core/config.hpp"

namespace satsuma::host {

// 检查 Host 路径、VMware 控制通道和 VM 配置。
class Diagnostics {
public:
    // 保存一份已验证的实验室配置。
    explicit Diagnostics(LabConfig config);

    // 检查全部 VM 或调用方指定的单台 VM。
    [[nodiscard]] nlohmann::json inspect_environment(
        const std::optional<std::string>& vm_id = std::nullopt) const;

    // 发布无害 echo 任务并等待 Agent 返回完整结果。
    [[nodiscard]] nlohmann::json run_probe(
        const std::optional<std::string>& vm_id,
        std::chrono::seconds timeout) const;

private:
    LabConfig config_;  // 当前实验室配置
};

}  // namespace satsuma::host
