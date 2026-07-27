// VM Agent claim 文件事务、续租 sidecar 和结果 fencing 接口。
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json_fwd.hpp>

#include "satsuma/core/claim.hpp"

namespace satsuma::vm {

// 步骤 claim 领取事务的最终状态。
enum class StepClaimAcquireStatus {
    Acquired,
    Completed,
    Wait,
    ManualInterventionRequired,
};

// 步骤 claim 领取事务返回的状态和取证信息。
struct StepClaimAcquireResult {
    StepClaimAcquireStatus status{StepClaimAcquireStatus::Wait}; // 领取事务状态
    std::optional<StepClaimLease> claim; // 新 claim 或当前有效 claim
    std::optional<std::filesystem::path> archived_claim_path; // 被归档的过期 claim
};

// 步骤 claim 续租事务的最终状态。
enum class StepClaimRenewStatus {
    Renewed,
    OwnershipLost,
    LeaseExpired,
};

// 步骤 claim 续租事务返回的状态和最新有效租约。
struct StepClaimRenewResult {
    StepClaimRenewStatus status{StepClaimRenewStatus::OwnershipLost}; // 续租事务状态
    std::optional<StepClaimLease> claim; // 成功续租后的有效 claim
};

// canonical 结果发布事务的最终状态。
enum class StepResultPublishStatus {
    Published,
    OwnershipLost,
    LeaseExpired,
};

// job 暂存证据与 canonical 目标之间的一次发布映射。
struct StepResultEvidenceFile {
    std::filesystem::path staged_path;    // 当前 job 独占的完整暂存文件
    std::filesystem::path canonical_path; // Host 在 execution.json 中读取的稳定路径
};

// 返回 claim 对应的跨进程稳定锁文件路径。
[[nodiscard]] std::filesystem::path step_claim_lock_path(
    const std::filesystem::path& claim_path);

// 在单个锁事务中检查结果、恢复过期 claim 并领取新 claim。
[[nodiscard]] StepClaimAcquireResult acquire_step_claim_transaction(
    const std::filesystem::path& claim_path,
    const std::filesystem::path& canonical_result_path,
    const StepClaimLease& proposed_claim,
    const std::string& current_boot_id);

// 在单个锁事务中验证所有权并原子发布当前 job 的续租 sidecar。
[[nodiscard]] StepClaimRenewResult renew_step_claim_transaction(
    const std::filesystem::path& claim_path,
    const StepClaimLease& expected_owner,
    std::int64_t lease_duration_ms);

// 在稳定锁保护下读取基础 claim 与匹配 sidecar 合成的有效租约。
[[nodiscard]] StepClaimLease load_effective_step_claim(
    const std::filesystem::path& claim_path);

// 仅在 claim 仍归当前 job 且租约有效时原子发布 canonical 结果。
[[nodiscard]] StepResultPublishStatus publish_step_result_if_owned(
    const std::filesystem::path& claim_path,
    const StepClaimLease& expected_owner,
    const std::filesystem::path& canonical_result_path,
    const nlohmann::json& result,
    const std::vector<StepResultEvidenceFile>& evidence_files = {});

}  // namespace satsuma::vm
