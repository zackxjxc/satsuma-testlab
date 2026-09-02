// Host 权威 claim 租约和结果 fencing 实现。
#include "satsuma/core/claim_store.hpp"

#include <limits>
#include <mutex>
#include <set>
#include <string>

#include <nlohmann/json.hpp>
#include <windows.h>

#include "satsuma/core/errors.hpp"
#include "satsuma/core/json_io.hpp"
#include "satsuma/core/path.hpp"

namespace satsuma::vm {
namespace {

// VMCI 网关逐条处理请求；该锁同时保护绕过网关的进程内测试入口。
std::mutex claim_store_mutex;

// 验证新领取请求尚未伪造 Host 才能确定的字段。
void validate_proposed_claim(const StepClaimLease& claim) {
    const StepClaimLease validated = nlohmann::json(claim).get<StepClaimLease>();
    if (validated.renewal_sequence != 0 || validated.attempt != 1) {
        throw Error("Proposed step claim must start at attempt 1 and renewal 0");
    }
}

// 验证持久化 claim 与当前请求指向同一个步骤。
void validate_claim_scope(
    const StepClaimLease& persisted,
    const StepClaimLease& proposed) {
    if (persisted.run_id != proposed.run_id ||
        persisted.vm_id != proposed.vm_id ||
        persisted.step_id != proposed.step_id ||
        persisted.retry_safe != proposed.retry_safe) {
        throw StepClaimStateError("Persisted step claim does not match the requested step");
    }
}

// 将损坏或不可读的 Host claim 统一提升为人工恢复状态。
[[nodiscard]] StepClaimLease load_claim_record(
    const std::filesystem::path& path,
    const std::string& description) {
    try {
        return load_step_claim_lease(path);
    } catch (const std::exception& error) {
        throw StepClaimStateError(
            "Invalid " + description + ": " + std::string(error.what()));
    }
}

// 使用 Host 当前时间刷新一次新领取的租约窗口。
[[nodiscard]] StepClaimLease make_acquired_claim(
    const StepClaimLease& proposed,
    const std::int64_t claimed_unix_ms,
    const std::uint32_t attempt) {
    const std::int64_t lease_duration_ms =
        proposed.lease_expires_unix_ms - proposed.claimed_unix_ms;
    return make_step_claim_lease(
        proposed.run_id,
        proposed.vm_id,
        proposed.step_id,
        proposed.job_id,
        proposed.session_id,
        proposed.boot_id,
        claimed_unix_ms,
        lease_duration_ms,
        proposed.retry_safe,
        attempt);
}

// 返回一次安全重试前保留的确定性 claim 证据路径。
[[nodiscard]] std::filesystem::path claim_attempt_path(
    const std::filesystem::path& claim_path,
    const std::uint32_t attempt) {
    std::filesystem::path archived = claim_path;
    archived += path_from_utf8(".attempt-" + std::to_string(attempt) + ".json");
    return archived;
}

// 保留被安全重试替换的旧 claim；重复恢复必须得到完全相同的证据。
void archive_claim_attempt(
    const std::filesystem::path& claim_path,
    const StepClaimLease& claim) {
    const std::filesystem::path archived = claim_attempt_path(claim_path, claim.attempt);
    if (std::filesystem::exists(archived)) {
        if (!std::filesystem::is_regular_file(archived) ||
            nlohmann::json(load_claim_record(archived, "archived step claim")) !=
                nlohmann::json(claim)) {
            throw StepClaimStateError("Archived step claim attempt conflicts with current state");
        }
        return;
    }
    write_json_atomic_existing_parent(archived, claim);
}

// 检查 canonical 结果中的稳定身份是否与 claim 完全一致。
void validate_result_owner(
    const nlohmann::json& result,
    const StepClaimLease& expected_owner) {
    if (result.value("run_id", std::string{}) != expected_owner.run_id ||
        result.value("vm_id", std::string{}) != expected_owner.vm_id ||
        result.value("step_id", std::string{}) != expected_owner.step_id ||
        result.value("job_id", std::string{}) != expected_owner.job_id) {
        throw Error("Canonical step result does not match its claim owner");
    }
}

// 验证已存在结果确实属于当前步骤及持久化 owner。
void validate_completed_result(
    const std::filesystem::path& result_path,
    const std::filesystem::path& claim_path,
    const StepClaimLease& proposed_claim) {
    try {
        const nlohmann::json result = load_json(result_path);
        if (result.value("run_id", std::string{}) != proposed_claim.run_id ||
            result.value("vm_id", std::string{}) != proposed_claim.vm_id ||
            result.value("step_id", std::string{}) != proposed_claim.step_id) {
            throw StepClaimStateError("Canonical step result does not match the requested step");
        }
        if (std::filesystem::exists(claim_path)) {
            if (!std::filesystem::is_regular_file(claim_path)) {
                throw StepClaimStateError("Step claim path is not a regular file");
            }
            const StepClaimLease owner = load_claim_record(claim_path, "step claim");
            if (owner.job_id != result.value("job_id", std::string{})) {
                throw StepClaimStateError(
                    "Canonical step result does not match the persisted claim owner");
            }
        }
    } catch (const StepClaimStateError&) {
        throw;
    } catch (const std::exception& error) {
        throw StepClaimStateError(
            "Invalid canonical step result: " + std::string(error.what()));
    }
}

// 验证结果证据路径和目标唯一性。
void validate_evidence_files(const std::vector<StepResultEvidenceFile>& evidence_files) {
    std::set<std::filesystem::path> canonical_paths;
    for (const StepResultEvidenceFile& evidence : evidence_files) {
        if (evidence.staged_path.empty() || evidence.canonical_path.empty() ||
            evidence.staged_path == evidence.canonical_path) {
            throw Error("Step result evidence contains an invalid path mapping");
        }
        if (!std::filesystem::is_regular_file(evidence.staged_path)) {
            throw Error("Staged step result evidence is not a regular file");
        }
        if (!canonical_paths.insert(evidence.canonical_path).second) {
            throw Error("Step result evidence contains a duplicate canonical path");
        }
    }
}

// 将一个完整暂存文件原子发布到 canonical 路径。
void publish_evidence_file(const StepResultEvidenceFile& evidence) {
    if (!MoveFileExW(
            evidence.staged_path.c_str(),
            evidence.canonical_path.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        throw Error(
            "MoveFileExW(publish step result evidence) failed with Win32 error " +
            std::to_string(GetLastError()));
    }
}

}  // namespace

StepClaimAcquireResult acquire_step_claim_transaction(
    const std::filesystem::path& claim_path,
    const std::filesystem::path& canonical_result_path,
    const StepClaimLease& proposed_claim) {
    validate_proposed_claim(proposed_claim);
    std::lock_guard lock(claim_store_mutex);

    if (std::filesystem::exists(canonical_result_path)) {
        if (!std::filesystem::is_regular_file(canonical_result_path)) {
            throw StepClaimStateError("Canonical step result path is not a regular file");
        }
        validate_completed_result(canonical_result_path, claim_path, proposed_claim);
        return {StepClaimAcquireStatus::Completed, std::nullopt};
    }

    std::filesystem::create_directories(claim_path.parent_path());
    const std::int64_t now_unix_ms = unix_time_ms();
    if (!std::filesystem::exists(claim_path)) {
        const StepClaimLease acquired = make_acquired_claim(proposed_claim, now_unix_ms, 1);
        write_json_atomic_existing_parent(claim_path, acquired);
        return {StepClaimAcquireStatus::Acquired, acquired};
    }
    if (!std::filesystem::is_regular_file(claim_path)) {
        throw StepClaimStateError("Step claim path is not a regular file");
    }

    const StepClaimLease existing = load_claim_record(claim_path, "step claim");
    validate_claim_scope(existing, proposed_claim);
    const ClaimRecoveryDecision decision = evaluate_claim_recovery(existing, now_unix_ms);
    if (decision == ClaimRecoveryDecision::Wait) {
        return {StepClaimAcquireStatus::Wait, existing};
    }
    if (decision == ClaimRecoveryDecision::ManualInterventionRequired) {
        return {StepClaimAcquireStatus::ManualInterventionRequired, existing};
    }
    if (existing.attempt == std::numeric_limits<std::uint32_t>::max()) {
        throw Error("Step claim attempt limit has been reached");
    }

    archive_claim_attempt(claim_path, existing);
    const StepClaimLease acquired = make_acquired_claim(
        proposed_claim,
        now_unix_ms,
        existing.attempt + 1);
    write_json_atomic_existing_parent(claim_path, acquired);
    return {StepClaimAcquireStatus::Acquired, acquired};
}

StepClaimRenewResult renew_step_claim_transaction(
    const std::filesystem::path& claim_path,
    const StepClaimLease& expected_owner,
    const std::int64_t lease_duration_ms) {
    std::lock_guard lock(claim_store_mutex);
    if (!std::filesystem::is_regular_file(claim_path)) {
        return {StepClaimRenewStatus::OwnershipLost, std::nullopt};
    }

    const StepClaimLease current = load_claim_record(claim_path, "step claim");
    if (!same_step_claim_owner(current, expected_owner)) {
        return {StepClaimRenewStatus::OwnershipLost, std::nullopt};
    }
    if (current.renewal_sequence > expected_owner.renewal_sequence) {
        return {StepClaimRenewStatus::Renewed, current};
    }
    if (current.renewal_sequence < expected_owner.renewal_sequence ||
        nlohmann::json(current) != nlohmann::json(expected_owner)) {
        throw StepClaimStateError("Persisted step claim does not match the renewal request");
    }

    const std::int64_t renewed_unix_ms = unix_time_ms();
    if (renewed_unix_ms >= current.lease_expires_unix_ms) {
        return {StepClaimRenewStatus::LeaseExpired, current};
    }
    const StepClaimLease renewed = renew_step_claim_lease(
        current,
        renewed_unix_ms,
        lease_duration_ms);
    write_json_atomic_existing_parent(claim_path, renewed);
    return {StepClaimRenewStatus::Renewed, renewed};
}

StepResultPublishStatus publish_step_result_if_owned(
    const std::filesystem::path& claim_path,
    const StepClaimLease& expected_owner,
    const std::filesystem::path& canonical_result_path,
    const nlohmann::json& result,
    const std::vector<StepResultEvidenceFile>& evidence_files) {
    validate_result_owner(result, expected_owner);
    validate_evidence_files(evidence_files);
    std::lock_guard lock(claim_store_mutex);
    if (!std::filesystem::is_regular_file(claim_path)) {
        return StepResultPublishStatus::OwnershipLost;
    }

    const StepClaimLease current = load_claim_record(claim_path, "step claim");
    if (!same_step_claim_owner(current, expected_owner)) {
        return StepResultPublishStatus::OwnershipLost;
    }
    if (unix_time_ms() >= current.lease_expires_unix_ms) {
        return StepResultPublishStatus::LeaseExpired;
    }

    if (std::filesystem::is_regular_file(canonical_result_path)) {
        const nlohmann::json existing_result = load_json(canonical_result_path);
        if (existing_result.value("job_id", std::string{}) != expected_owner.job_id) {
            throw Error("Canonical step result is already owned by another job");
        }
        return StepResultPublishStatus::Published;
    }
    for (const StepResultEvidenceFile& evidence : evidence_files) {
        std::filesystem::create_directories(evidence.canonical_path.parent_path());
        publish_evidence_file(evidence);
    }
    write_json_atomic_existing_parent(canonical_result_path, result);
    return StepResultPublishStatus::Published;
}

}  // namespace satsuma::vm
