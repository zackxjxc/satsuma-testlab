// Host 侧 Agent 硬件发现、绑定和 presence 校验接口。
#pragma once

#include <filesystem>
#include <string>

#include <nlohmann/json.hpp>

#include "satsuma/core/config.hpp"

namespace satsuma::host {

// 枚举共享目录中按硬件 UUID 发布的 Agent presence。
[[nodiscard]] nlohmann::json discover_agents(const LabConfig& config);

// 将在线硬件身份绑定到实验室中的业务 VM，并发布 Agent 绑定记录。
[[nodiscard]] nlohmann::json bind_agent_hardware(
    const std::filesystem::path& config_path,
    const LabConfig& config,
    const std::string& vm_id,
    const std::string& hardware_id);

// 返回 Host 对指定 VM 期望的 presence 路径。
[[nodiscard]] std::filesystem::path vm_presence_path(
    const LabConfig& config,
    const VmConfig& vm);

// 读取并验证 presence 的实验室、业务身份和硬件绑定。
[[nodiscard]] nlohmann::json load_vm_presence(
    const LabConfig& config,
    const VmConfig& vm);

}  // namespace satsuma::host
