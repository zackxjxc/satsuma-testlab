// VM Agent 硬件身份初始化、绑定和迁移记录接口。
#pragma once

#include <filesystem>
#include <string>

#include "satsuma/core/config.hpp"

namespace satsuma::vm {

// 加载配置并应用本机 SMBIOS 身份与可选 Host 绑定。
[[nodiscard]] AgentConfig load_runtime_agent_config(
    const std::filesystem::path& config_path);

// 使用指定硬件 ID 初始化运行时配置，供测试和生产探测复用。
void prepare_agent_hardware_identity(
    AgentConfig& config,
    const std::string& hardware_id = {});

// 检查 Host 是否为未绑定 Agent 发布了新的 VM 标识。
[[nodiscard]] bool refresh_agent_binding(AgentConfig& config);

// 返回硬件维度的规范 presence 路径。
[[nodiscard]] std::filesystem::path hardware_presence_path(const AgentConfig& config);

// 在传输通道可用后发布一次硬件迁移诊断记录。
void write_hardware_migration_marker(const AgentConfig& config);

}  // namespace satsuma::vm
