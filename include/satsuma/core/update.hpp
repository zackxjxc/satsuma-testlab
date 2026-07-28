// SatsumaVM 独立自更新清单和结果模型。
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

#include <nlohmann/json_fwd.hpp>

namespace satsuma {

// Host 发布给单个 Agent 的不可变更新清单。
struct AgentUpdateManifest {
    int schema_version{1};            // JSON 结构版本
    int protocol_version{1};          // 独立更新通道协议版本
    std::string type{"update_agent"}; // 固定更新类型
    std::string lab_id;               // 目标实验室 ID
    std::string vm_id;                // 领取更新的当前 VM ID
    std::optional<std::string> next_vm_id; // 协议 v2 成功后启用的新 VM ID
    std::string update_id;            // 本次更新唯一 ID
    std::string version;              // 候选 Agent 版本
    std::filesystem::path binary;     // 更新目录内的相对候选文件
    std::uint64_t size{};             // 候选字节数
    std::string sha256;               // 候选 SHA-256
    std::string created_at;           // Host 创建时间
};

// Agent 更新助手写回共享目录的终态结果。
struct AgentUpdateResult {
    int schema_version{1};       // JSON 结构版本
    std::string update_id;       // 对应更新 ID
    std::string vm_id;           // 执行更新的 VM ID
    std::string version;         // 请求的候选版本
    std::string status;          // succeeded 或 failed
    std::string rollback_status; // none、succeeded 或 failed
    std::uint32_t process_id{};  // 成功上线的新 Service PID
    std::string error;           // 失败诊断
    std::string completed_at;    // Agent 完成时间
};

// 读取并验证更新清单。
[[nodiscard]] AgentUpdateManifest load_agent_update_manifest(
    const std::filesystem::path& path);

// 读取并验证更新结果。
[[nodiscard]] AgentUpdateResult load_agent_update_result(
    const std::filesystem::path& path);

// 将更新清单转换为 JSON。
void to_json(nlohmann::json& value, const AgentUpdateManifest& manifest);

// 从 JSON 解析更新清单。
void from_json(const nlohmann::json& value, AgentUpdateManifest& manifest);

// 将更新结果转换为 JSON。
void to_json(nlohmann::json& value, const AgentUpdateResult& result);

// 从 JSON 解析更新结果。
void from_json(const nlohmann::json& value, AgentUpdateResult& result);

}  // namespace satsuma
