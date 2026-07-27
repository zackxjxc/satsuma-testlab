// Satsuma Agent 步骤 claim 租约模型和恢复判定实现。
#include "satsuma/core/claim.hpp"

#include <chrono>
#include <limits>
#include <utility>

#include <nlohmann/json.hpp>

#include "satsuma/core/errors.hpp"
#include "satsuma/core/id.hpp"
#include "satsuma/core/json_io.hpp"
#include "satsuma/core/path.hpp"

namespace satsuma {
namespace {

// 验证 claim 字段和租约时间范围的一致性。
void validate_claim_lease(const StepClaimLease& claim) {
    if (claim.schema_version != 2 && claim.schema_version != 3) {
        throw Error("Step claim lease requires schema_version 2 or 3");
    }
    validate_identifier(claim.run_id, "claim run_id");
    validate_identifier(claim.vm_id, "claim vm_id");
    validate_identifier(claim.step_id, "claim step_id");
    validate_identifier(claim.job_id, "claim job_id");
    validate_identifier(claim.session_id, "claim session_id");
    validate_identifier(claim.boot_id, "claim boot_id");
    if (claim.claimed_at.empty() || claim.last_renewed_at.empty() ||
        claim.claimed_unix_ms < 0 ||
        claim.last_renewed_unix_ms < claim.claimed_unix_ms ||
        claim.lease_expires_unix_ms <= claim.last_renewed_unix_ms) {
        throw Error("Step claim lease contains an invalid time range");
    }
    if (claim.attempt == 0) {
        throw Error("Step claim attempt must be at least 1");
    }
}

}  // namespace

std::int64_t unix_time_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

StepClaimLease make_step_claim_lease(
    std::string run_id,
    std::string vm_id,
    std::string step_id,
    std::string job_id,
    std::string session_id,
    std::string boot_id,
    const std::int64_t claimed_unix_ms,
    const std::int64_t lease_duration_ms,
    const bool retry_safe,
    const std::uint32_t attempt) {
    if (lease_duration_ms <= 0 ||
        claimed_unix_ms > std::numeric_limits<std::int64_t>::max() - lease_duration_ms) {
        throw Error("Step claim lease duration is outside the supported range");
    }
    StepClaimLease claim;
    claim.run_id = std::move(run_id);
    claim.vm_id = std::move(vm_id);
    claim.step_id = std::move(step_id);
    claim.job_id = std::move(job_id);
    claim.session_id = std::move(session_id);
    claim.boot_id = std::move(boot_id);
    claim.claimed_at = utc_timestamp();
    claim.claimed_unix_ms = claimed_unix_ms;
    claim.last_renewed_at = claim.claimed_at;
    claim.last_renewed_unix_ms = claimed_unix_ms;
    claim.lease_expires_unix_ms = claimed_unix_ms + lease_duration_ms;
    claim.retry_safe = retry_safe;
    claim.attempt = attempt;
    validate_claim_lease(claim);
    return claim;
}

StepClaimLease renew_step_claim_lease(
    const StepClaimLease& claim,
    const std::int64_t renewed_unix_ms,
    const std::int64_t lease_duration_ms) {
    validate_claim_lease(claim);
    if (renewed_unix_ms <= claim.last_renewed_unix_ms ||
        renewed_unix_ms >= claim.lease_expires_unix_ms) {
        throw Error("Cannot renew an expired or time-reversed step claim lease");
    }
    if (lease_duration_ms <= 0 ||
        renewed_unix_ms > std::numeric_limits<std::int64_t>::max() - lease_duration_ms) {
        throw Error("Step claim lease duration is outside the supported range");
    }
    const std::int64_t renewed_expiry = renewed_unix_ms + lease_duration_ms;
    if (renewed_expiry <= claim.lease_expires_unix_ms) {
        throw Error("Step claim renewal must extend the lease expiry");
    }
    if (claim.renewal_sequence == std::numeric_limits<std::uint32_t>::max()) {
        throw Error("Step claim renewal sequence limit has been reached");
    }

    StepClaimLease renewed = claim;
    renewed.last_renewed_at = utc_timestamp();
    renewed.last_renewed_unix_ms = renewed_unix_ms;
    renewed.lease_expires_unix_ms = renewed_expiry;
    ++renewed.renewal_sequence;
    validate_claim_lease(renewed);
    return renewed;
}

bool same_step_claim_owner(
    const StepClaimLease& left,
    const StepClaimLease& right) noexcept {
    return left.schema_version == right.schema_version &&
        left.run_id == right.run_id &&
        left.vm_id == right.vm_id &&
        left.step_id == right.step_id &&
        left.job_id == right.job_id &&
        left.session_id == right.session_id &&
        left.boot_id == right.boot_id &&
        left.claimed_unix_ms == right.claimed_unix_ms &&
        left.retry_safe == right.retry_safe &&
        left.attempt == right.attempt;
}

std::filesystem::path step_claim_renewal_path(
    const std::filesystem::path& claim_path,
    const StepClaimLease& claim) {
    validate_claim_lease(claim);
    return claim_path.parent_path() /
        path_from_utf8(claim.step_id + ".claim-renewal-" + claim.job_id + ".json");
}

ClaimRecoveryDecision evaluate_claim_recovery(
    const StepClaimLease& claim,
    const std::int64_t now_unix_ms,
    const std::string& current_boot_id) {
    validate_claim_lease(claim);
    validate_identifier(current_boot_id, "current boot_id");
    if (now_unix_ms < claim.lease_expires_unix_ms || current_boot_id == claim.boot_id) {
        return ClaimRecoveryDecision::Wait;
    }
    return claim.retry_safe
        ? ClaimRecoveryDecision::Retry
        : ClaimRecoveryDecision::ManualInterventionRequired;
}

StepClaimLease load_step_claim_lease(const std::filesystem::path& path) {
    try {
        return load_json(path).get<StepClaimLease>();
    } catch (const nlohmann::json::exception& error) {
        throw Error("Invalid step claim lease: " + std::string(error.what()));
    }
}

void to_json(nlohmann::json& value, const StepClaimLease& claim) {
    value = {
        {"schema_version", claim.schema_version},
        {"run_id", claim.run_id},
        {"vm_id", claim.vm_id},
        {"step_id", claim.step_id},
        {"job_id", claim.job_id},
        {"session_id", claim.session_id},
        {"boot_id", claim.boot_id},
        {"claimed_at", claim.claimed_at},
        {"claimed_unix_ms", claim.claimed_unix_ms},
        {"last_renewed_at", claim.last_renewed_at},
        {"last_renewed_unix_ms", claim.last_renewed_unix_ms},
        {"lease_expires_unix_ms", claim.lease_expires_unix_ms},
        {"renewal_sequence", claim.renewal_sequence},
        {"retry_safe", claim.retry_safe},
        {"attempt", claim.attempt},
    };
}

void from_json(const nlohmann::json& value, StepClaimLease& claim) {
    claim.schema_version = value.value("schema_version", 0);
    claim.run_id = value.at("run_id").get<std::string>();
    claim.vm_id = value.at("vm_id").get<std::string>();
    claim.step_id = value.at("step_id").get<std::string>();
    claim.job_id = value.at("job_id").get<std::string>();
    claim.session_id = value.at("session_id").get<std::string>();
    claim.boot_id = value.at("boot_id").get<std::string>();
    claim.claimed_at = value.at("claimed_at").get<std::string>();
    claim.claimed_unix_ms = value.at("claimed_unix_ms").get<std::int64_t>();
    if (claim.schema_version == 3) {
        claim.last_renewed_at = value.at("last_renewed_at").get<std::string>();
        claim.last_renewed_unix_ms = value.at("last_renewed_unix_ms").get<std::int64_t>();
        claim.renewal_sequence = value.at("renewal_sequence").get<std::uint32_t>();
    } else {
        claim.last_renewed_at = claim.claimed_at;
        claim.last_renewed_unix_ms = claim.claimed_unix_ms;
        claim.renewal_sequence = 0;
    }
    claim.lease_expires_unix_ms = value.at("lease_expires_unix_ms").get<std::int64_t>();
    claim.retry_safe = value.at("retry_safe").get<bool>();
    claim.attempt = value.at("attempt").get<std::uint32_t>();
    validate_claim_lease(claim);
}

}  // namespace satsuma
