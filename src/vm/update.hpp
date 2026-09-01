// SatsumaVM 独立更新通道和助手模式接口。
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <stop_token>
#include <string>

#include "satsuma/core/config.hpp"
#include "satsuma/core/update.hpp"

namespace satsuma::vm {

// 扫描并处理一个当前 VM 的待更新目录。
[[nodiscard]] bool process_pending_agent_update(
    const AgentConfig& config,
    std::stop_token stop_token);

// 在独立候选进程中停止、切换并验证 Agent Service。
[[nodiscard]] int apply_agent_update_helper(
    const std::filesystem::path& config_path,
    const std::filesystem::path& manifest_path);

#ifdef SATSUMA_UPDATE_TESTS
// 更新事务使用的全部本机镜像和安装路径。
struct AgentUpdatePaths {
    std::filesystem::path update_directory; // 共享更新目录
    std::filesystem::path manifest;         // 本机清单副本
    std::filesystem::path result;           // 共享终态结果
    std::filesystem::path config;           // 正式 Agent 配置
    std::filesystem::path config_backup;    // 本次配置备份
    std::filesystem::path formal_binary;    // 正式 SatsumaVM.exe
    std::filesystem::path new_binary;       // SatsumaVM.new.exe
    std::filesystem::path backup_binary;    // SatsumaVM.bak.exe
    std::filesystem::path state;            // 本机更新阶段状态
};

// 注入更新事务的 Service 和 presence 操作。
struct AgentUpdateOperations {
    std::function<void()> stop_service;  // 停止当前 Service
    std::function<std::uint32_t()> start_service; // 启动正式 Service
    std::function<void(
        std::uint32_t,
        const std::string&,
        const std::string&)> wait_presence; // 等待指定版本上线
};

// 使用临时文件和注入操作运行完整更新事务。
[[nodiscard]] AgentUpdateResult apply_agent_update_for_test(
    const AgentUpdatePaths& paths,
    const AgentUpdateManifest& manifest,
    const std::string& candidate_version,
    const AgentUpdateOperations& operations);

// 验证 presence 绑定 PID、版本和更新 ID。
[[nodiscard]] bool agent_update_presence_matches_for_test(
    const std::filesystem::path& presence_path,
    const AgentConfig& config,
    std::uint32_t process_id,
    const std::string& version,
    const std::string& update_id);

// 验证成功补偿受完整本机清理门禁约束。
[[nodiscard]] bool recover_committed_update_success_for_test(
    const AgentUpdatePaths& paths,
    const AgentUpdateManifest& manifest);

// 验证身份切换后的 Agent 能从来源目录补发已提交结果。
[[nodiscard]] bool recover_rebound_update_success_for_test(
    const AgentConfig& config);

// 验证更新 Helper 不会随 Agent Service 的 Job 一起终止。
[[nodiscard]] std::uint32_t agent_update_helper_creation_flags_for_test();

// 验证 Agent 重启后可清理已确认回滚的本机残留。
[[nodiscard]] bool recover_verified_failed_rollback_for_test(
    const AgentUpdatePaths& paths,
    const AgentUpdateManifest& manifest,
    const std::filesystem::path& running_executable,
    const std::string& running_version);
#endif

}  // namespace satsuma::vm
