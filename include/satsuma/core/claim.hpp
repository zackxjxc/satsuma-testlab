// Satsuma Agent 步骤 claim 租约模型和恢复判定接口。
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

#include <nlohmann/json_fwd.hpp>

namespace satsuma {

// 租约到期后的安全处理结论。
enum class ClaimRecoveryDecision {
    Wait,
    Retry,
    ManualInterventionRequired,
};

// Agent 为一次步骤领取持久化的有限租约。
struct StepClaimLease {
    int schema_version{2};               // claim schema 版本
    std::string run_id;                  // 对应运行 ID
    std::string vm_id;                   // 目标虚拟机 ID
    std::string step_id;                 // 对应步骤 ID
    std::string job_id;                  // 本次执行 Job ID
    std::string session_id;              // 领取 Agent 会话 ID
    std::string boot_id;                 // 领取 Agent 启动 ID
    std::string claimed_at;              // 便于取证的 UTC 时间
    std::int64_t claimed_unix_ms{0};     // 领取时间戳
    std::int64_t lease_expires_unix_ms{0}; // 租约截止时间戳
    bool retry_safe{false};              // 到期且身份变化后是否允许重试
    std::uint32_t attempt{1};            // 从 1 开始的执行尝试次数
};

// 返回当前 Unix 毫秒时间戳。
[[nodiscard]] std::int64_t unix_time_ms();

// 创建并验证一份新的步骤 claim 租约。
[[nodiscard]] StepClaimLease make_step_claim_lease(
    std::string run_id,
    std::string vm_id,
    std::string step_id,
    std::string job_id,
    std::string session_id,
    std::string boot_id,
    std::int64_t claimed_unix_ms,
    std::int64_t lease_duration_ms,
    bool retry_safe,
    std::uint32_t attempt = 1);

// 根据时间与当前启动身份决定等待、重试或转人工门禁。
[[nodiscard]] ClaimRecoveryDecision evaluate_claim_recovery(
    const StepClaimLease& claim,
    std::int64_t now_unix_ms,
    const std::string& current_boot_id);

// 读取并验证持久化 claim 租约。
[[nodiscard]] StepClaimLease load_step_claim_lease(const std::filesystem::path& path);

// 将 claim 租约转换为 JSON。
void to_json(nlohmann::json& value, const StepClaimLease& claim);

// 从 JSON 解析并验证 claim 租约。
void from_json(const nlohmann::json& value, StepClaimLease& claim);

}  // namespace satsuma
