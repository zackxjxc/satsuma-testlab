// Host 侧 Agent 硬件发现、绑定和 presence 校验接口。
#pragma once

#include <filesystem>
#include <chrono>
#include <string>

#include <nlohmann/json.hpp>

#include "satsuma/core/config.hpp"

namespace satsuma::host {

// 枚举 Host 状态根中按硬件 UUID 发布的 Agent presence。
[[nodiscard]] nlohmann::json discover_agents(const LabConfig& config);

// 将在线硬件身份绑定到实验室中的稳定 VM 标识，并发布 Agent 绑定记录。
[[nodiscard]] nlohmann::json bind_agent_hardware(
    const std::filesystem::path& config_path,
    const LabConfig& config,
    const std::string& vm_id,
    const std::string& hardware_id);

// 返回 Host 对指定 VM 期望的 presence 路径。
[[nodiscard]] std::filesystem::path vm_presence_path(
    const LabConfig& config,
    const VmConfig& vm);

// 读取并验证 presence 的实验室、VM 标识和硬件绑定。
[[nodiscard]] nlohmann::json load_vm_presence(
    const LabConfig& config,
    const VmConfig& vm);

// 读取并验证指定 VM 的环境清单及 presence 摘要引用。
[[nodiscard]] nlohmann::json load_vm_inventory(
    const LabConfig& config,
    const VmConfig& vm);

// 发布显式刷新请求并有限等待对应的新清单。
[[nodiscard]] nlohmann::json refresh_vm_inventory(
    const LabConfig& config,
    const VmConfig& vm,
    std::chrono::seconds timeout);

}  // namespace satsuma::host
