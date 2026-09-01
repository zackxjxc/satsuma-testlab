// Host 权威 claim 事务、续租 sidecar 和结果 fencing 接口。
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json_fwd.hpp>

#include "satsuma/core/claim.hpp"
#include "satsuma/core/errors.hpp"

namespace satsuma::vm {

class StepClaimStateError : public Error {
public:
    using Error::Error;
};

enum class StepClaimAcquireStatus {
    Acquired,
    Completed,
    Wait,
    ManualInterventionRequired,
};

struct StepClaimAcquireResult {
    StepClaimAcquireStatus status{StepClaimAcquireStatus::Wait};
    std::optional<StepClaimLease> claim;
    std::optional<std::filesystem::path> archived_claim_path;
};

enum class StepClaimRenewStatus {
    Renewed,
    OwnershipLost,
    LeaseExpired,
};

struct StepClaimRenewResult {
    StepClaimRenewStatus status{StepClaimRenewStatus::OwnershipLost};
    std::optional<StepClaimLease> claim;
};

enum class StepResultPublishStatus {
    Published,
    OwnershipLost,
    LeaseExpired,
};

struct StepResultEvidenceFile {
    std::filesystem::path staged_path;
    std::filesystem::path canonical_path;
};

[[nodiscard]] std::filesystem::path step_claim_lock_path(
    const std::filesystem::path& claim_path);

[[nodiscard]] StepClaimAcquireResult acquire_step_claim_transaction(
    const std::filesystem::path& claim_path,
    const std::filesystem::path& canonical_result_path,
    const StepClaimLease& proposed_claim,
    const std::string& current_boot_id);

[[nodiscard]] StepClaimRenewResult renew_step_claim_transaction(
    const std::filesystem::path& claim_path,
    const StepClaimLease& expected_owner,
    std::int64_t lease_duration_ms);

[[nodiscard]] StepClaimLease load_effective_step_claim(
    const std::filesystem::path& claim_path);

[[nodiscard]] StepResultPublishStatus publish_step_result_if_owned(
    const std::filesystem::path& claim_path,
    const StepClaimLease& expected_owner,
    const std::filesystem::path& canonical_result_path,
    const nlohmann::json& result,
    const std::vector<StepResultEvidenceFile>& evidence_files = {});

}  // namespace satsuma::vm
