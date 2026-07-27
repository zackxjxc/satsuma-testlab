// VM Agent claim 文件事务、续租 sidecar 和结果 fencing 实现。
#include "claim_store.hpp"

#include <algorithm>
#include <chrono>
#include <limits>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>
#include <windows.h>

#include "satsuma/core/errors.hpp"
#include "satsuma/core/id.hpp"
#include "satsuma/core/json_io.hpp"
#include "satsuma/core/path.hpp"

namespace satsuma::vm {
namespace {

constexpr std::chrono::seconds kClaimLockTimeout{5}; // 短事务锁最长等待时间
constexpr std::chrono::milliseconds kClaimLockRetryDelay{10}; // 锁冲突重试间隔
constexpr std::chrono::seconds kClaimMoveTimeout{2}; // 共享文件移动冲突最长等待时间

// 自动关闭 claim 事务使用的 Win32 HANDLE。
class UniqueHandle {
public:
    // 接管一个可空 Win32 HANDLE。
    explicit UniqueHandle(HANDLE value = nullptr) noexcept : value_(value) {}

    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;

    // 支持转移 HANDLE 所有权。
    UniqueHandle(UniqueHandle&& other) noexcept : value_(other.release()) {}

    // 关闭旧 HANDLE 后接管新值。
    UniqueHandle& operator=(UniqueHandle&& other) noexcept {
        if (this != &other) {
            reset(other.release());
        }
        return *this;
    }

    // 离开作用域时关闭有效 HANDLE。
    ~UniqueHandle() {
        reset();
    }

    // 返回底层 HANDLE 是否有效。
    [[nodiscard]] explicit operator bool() const noexcept {
        return value_ != nullptr && value_ != INVALID_HANDLE_VALUE;
    }

    // 放弃所有权但不关闭 HANDLE。
    [[nodiscard]] HANDLE release() noexcept {
        const HANDLE value = value_;
        value_ = nullptr;
        return value;
    }

    // 关闭旧 HANDLE 并保存新值。
    void reset(HANDLE value = nullptr) noexcept {
        if (value_ != nullptr && value_ != INVALID_HANDLE_VALUE) {
            CloseHandle(value_);
        }
        value_ = value;
    }

private:
    HANDLE value_; // 被管理的锁文件 HANDLE
};

// 将 Win32 错误码转换为稳定错误文本。
[[nodiscard]] std::string win32_error(const std::string& operation, const DWORD code) {
    return operation + " failed with Win32 error " + std::to_string(code);
}

// 判断原子移动冲突是否允许有限重试。
[[nodiscard]] bool is_transient_move_error(const DWORD error) noexcept {
    return error == ERROR_ACCESS_DENIED ||
        error == ERROR_SHARING_VIOLATION ||
        error == ERROR_LOCK_VIOLATION;
}

// 在有限时间内重试同一次移动，避免重新读取并刷新共享层 lease。
[[nodiscard]] DWORD move_file_with_retry(
    const std::filesystem::path& source,
    const std::filesystem::path& destination,
    const DWORD flags) {
    const auto deadline = std::chrono::steady_clock::now() + kClaimMoveTimeout;
    DWORD move_error = ERROR_SUCCESS; // 最后一次 MoveFileExW 错误
    for (;;) {
        if (MoveFileExW(source.c_str(), destination.c_str(), flags)) {
            return ERROR_SUCCESS;
        }
        move_error = GetLastError();
        if (!is_transient_move_error(move_error) ||
            std::chrono::steady_clock::now() >= deadline) {
            return move_error;
        }
        std::this_thread::sleep_for(kClaimLockRetryDelay);
    }
}

// 获取按步骤持久化的短时独占锁。
[[nodiscard]] UniqueHandle acquire_claim_lock(
    const std::filesystem::path& claim_path,
    const bool create_parent) {
    const std::filesystem::path parent = claim_path.parent_path();
    if (!parent.empty()) {
        if (create_parent) {
            std::filesystem::create_directories(parent);
        } else if (!std::filesystem::is_directory(parent)) {
            throw Error("Step claim parent directory does not exist: " + path_to_utf8(parent));
        }
    }

    const std::filesystem::path lock_path = step_claim_lock_path(claim_path);
    const auto deadline = std::chrono::steady_clock::now() + kClaimLockTimeout;
    for (;;) {
        UniqueHandle lock(CreateFileW(
            lock_path.c_str(),
            GENERIC_READ | GENERIC_WRITE,
            0,
            nullptr,
            OPEN_ALWAYS,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH,
            nullptr));
        if (lock) {
            return lock;
        }

        const DWORD error = GetLastError();
        if (error != ERROR_SHARING_VIOLATION ||
            std::chrono::steady_clock::now() >= deadline) {
            throw Error(win32_error("CreateFileW(step claim lock)", error));
        }
        std::this_thread::sleep_for(kClaimLockRetryDelay);
    }
}

// 检查一份新 claim 是否为尚未续租的 schema v3 基础记录。
void validate_proposed_claim(const StepClaimLease& claim) {
    const StepClaimLease validated = nlohmann::json(claim).get<StepClaimLease>();
    if (validated.schema_version != 3 ||
        validated.attempt != 1 ||
        validated.renewal_sequence != 0 ||
        validated.last_renewed_at != validated.claimed_at ||
        validated.last_renewed_unix_ms != validated.claimed_unix_ms) {
        throw Error("Proposed step claim must be an initial schema version 3 lease");
    }
}

// 将锁内无法解析的持久化 claim 转换为可人工门禁的状态错误。
[[nodiscard]] StepClaimLease load_persisted_claim(
    const std::filesystem::path& path,
    const std::string& description) {
    try {
        return load_step_claim_lease(path);
    } catch (const std::exception& error) {
        throw StepClaimStateError(
            "Invalid " + description + ": " + error.what());
    }
}

// 返回旧版 job 唯一续租 sidecar 路径，供滚动升级兼容读取。
[[nodiscard]] std::filesystem::path legacy_step_claim_renewal_path(
    const std::filesystem::path& claim_path,
    const StepClaimLease& claim) {
    return claim_path.parent_path() /
        path_from_utf8(claim.step_id + ".claim-renewal-" + claim.job_id + ".json");
}

// 表示从不可变 sidecar 文件名解析出的续租序号和路径。
struct SequencedRenewalPath {
    std::uint32_t sequence{0}; // 文件名中的续租序号
    std::filesystem::path path; // 完整 sidecar 路径
};

// 查找当前 owner 的不可变续租 sidecar，并拒绝伪装成协议文件的无效序号。
[[nodiscard]] std::vector<SequencedRenewalPath> find_sequenced_renewal_paths(
    const std::filesystem::path& claim_path,
    const StepClaimLease& owner) {
    const std::wstring prefix = path_from_utf8(
        owner.step_id + ".claim-renewal-" + owner.job_id + "-").native();
    const std::wstring suffix = L".json";
    constexpr std::size_t sequence_width = 10;
    std::vector<SequencedRenewalPath> paths;
    for (const std::filesystem::directory_entry& entry :
         std::filesystem::directory_iterator(claim_path.parent_path())) {
        const std::wstring filename = entry.path().filename().native();
        if (filename.size() != prefix.size() + sequence_width + suffix.size() ||
            filename.compare(0, prefix.size(), prefix) != 0 ||
            filename.compare(filename.size() - suffix.size(), suffix.size(), suffix) != 0) {
            continue;
        }

        std::uint64_t parsed_sequence = 0; // 防止十位数字在解析时溢出
        for (std::size_t index = 0; index < sequence_width; ++index) {
            const wchar_t digit = filename[prefix.size() + index];
            if (digit < L'0' || digit > L'9') {
                throw StepClaimStateError(
                    "Step claim renewal sidecar filename contains an invalid sequence");
            }
            parsed_sequence = parsed_sequence * 10 + static_cast<std::uint64_t>(digit - L'0');
        }
        if (parsed_sequence == 0 ||
            parsed_sequence > std::numeric_limits<std::uint32_t>::max()) {
            throw StepClaimStateError(
                "Step claim renewal sidecar filename sequence is outside the supported range");
        }
        paths.push_back({static_cast<std::uint32_t>(parsed_sequence), entry.path()});
    }
    std::sort(
        paths.begin(),
        paths.end(),
        [](const SequencedRenewalPath& left, const SequencedRenewalPath& right) {
            return left.sequence < right.sequence;
        });
    return paths;
}

// 验证后一份续租记录严格延长同一 owner 的前一份记录。
void validate_renewal_extension(
    const StepClaimLease& previous,
    const StepClaimLease& renewal) {
    if (!same_step_claim_owner(previous, renewal) ||
        renewal.renewal_sequence <= previous.renewal_sequence ||
        renewal.last_renewed_unix_ms <= previous.last_renewed_unix_ms ||
        renewal.lease_expires_unix_ms <= previous.lease_expires_unix_ms) {
        throw StepClaimStateError(
            "Step claim renewal sidecar does not extend its owning claim");
    }
}

// 判断新 owner 是否已经遗留同 job 的旧版或序号化续租记录。
[[nodiscard]] bool has_step_claim_renewals(
    const std::filesystem::path& claim_path,
    const StepClaimLease& owner) {
    return std::filesystem::exists(legacy_step_claim_renewal_path(claim_path, owner)) ||
        !find_sequenced_renewal_paths(claim_path, owner).empty();
}

// 返回基础 claim 与其不可变 sidecar 序列合成的当前有效租约。
[[nodiscard]] StepClaimLease load_effective_claim_unlocked(
    const std::filesystem::path& claim_path) {
    const StepClaimLease base = load_persisted_claim(claim_path, "step claim");
    if (base.schema_version != 3) {
        return base;
    }

    StepClaimLease effective = base; // 按持久化顺序推进的有效租约
    const std::filesystem::path legacy_path =
        legacy_step_claim_renewal_path(claim_path, base);
    if (std::filesystem::exists(legacy_path)) {
        if (!std::filesystem::is_regular_file(legacy_path)) {
            throw StepClaimStateError("Legacy step claim renewal sidecar is not a regular file");
        }
        const StepClaimLease legacy = load_persisted_claim(
            legacy_path,
            "legacy step claim renewal sidecar");
        validate_renewal_extension(effective, legacy);
        effective = legacy;
    }

    for (const SequencedRenewalPath& candidate :
         find_sequenced_renewal_paths(claim_path, base)) {
        if (candidate.sequence != effective.renewal_sequence + 1) {
            throw StepClaimStateError(
                "Step claim renewal sidecar sequence contains a gap or duplicate");
        }
        if (!std::filesystem::is_regular_file(candidate.path)) {
            throw StepClaimStateError("Step claim renewal sidecar is not a regular file");
        }
        const StepClaimLease renewal = load_persisted_claim(
            candidate.path,
            "step claim renewal sidecar");
        if (renewal.renewal_sequence != candidate.sequence ||
            step_claim_renewal_path(claim_path, renewal) != candidate.path) {
            throw StepClaimStateError(
                "Step claim renewal sidecar does not match its immutable path");
        }
        validate_renewal_extension(effective, renewal);
        effective = renewal;
    }
    return effective;
}

// 使用请求身份和指定 attempt 创建一份时间窗口刷新的基础 claim。
[[nodiscard]] StepClaimLease make_acquired_claim(
    const StepClaimLease& proposed_claim,
    const std::int64_t claimed_unix_ms,
    const std::uint32_t attempt) {
    const std::int64_t lease_duration_ms =
        proposed_claim.lease_expires_unix_ms - proposed_claim.claimed_unix_ms;
    return make_step_claim_lease(
        proposed_claim.run_id,
        proposed_claim.vm_id,
        proposed_claim.step_id,
        proposed_claim.job_id,
        proposed_claim.session_id,
        proposed_claim.boot_id,
        claimed_unix_ms,
        lease_duration_ms,
        proposed_claim.retry_safe,
        attempt);
}

// 返回不会与其他事务临时文件冲突的待发布路径。
[[nodiscard]] std::filesystem::path make_staged_claim_path(
    const std::filesystem::path& claim_path) {
    return claim_path.parent_path() /
        path_from_utf8(".pending-" + make_id("claim"));
}

// 将完整 JSON 先写到唯一文件，供锁内原子重命名发布。
[[nodiscard]] std::filesystem::path stage_claim_json(
    const std::filesystem::path& claim_path,
    const StepClaimLease& claim) {
    const std::filesystem::path staged_path = make_staged_claim_path(claim_path);
    write_json_atomic_existing_parent(staged_path, claim);
    return staged_path;
}

// 尽力删除未发布的事务临时文件。
void remove_file_best_effort(const std::filesystem::path& path) noexcept {
    std::error_code error;
    std::filesystem::remove(path, error);
}

// 将待发布文件原子移动到当前不存在的 claim 记录路径。
void publish_staged_claim(
    const std::filesystem::path& staged_path,
    const std::filesystem::path& claim_path) {
    if (!MoveFileExW(staged_path.c_str(), claim_path.c_str(), MOVEFILE_WRITE_THROUGH)) {
        const DWORD error = GetLastError();
        remove_file_best_effort(staged_path);
        throw Error(win32_error("MoveFileExW(publish step claim record)", error));
    }
}

// 返回保留原 attempt 和唯一取证 ID 的过期 claim 路径。
[[nodiscard]] std::filesystem::path make_archived_claim_path(
    const std::filesystem::path& claim_path,
    const std::uint32_t attempt) {
    std::filesystem::path archived_path = claim_path;
    archived_path += path_from_utf8(
        ".expired-attempt-" + std::to_string(attempt) + "-" + make_id("claim"));
    return archived_path;
}

// 在锁内归档旧 claim，并在发布失败时恢复旧所有权记录。
void replace_expired_claim(
    const std::filesystem::path& claim_path,
    const std::filesystem::path& archived_path,
    const std::filesystem::path& staged_path) {
    const DWORD archive_error = move_file_with_retry(
        claim_path,
        archived_path,
        MOVEFILE_WRITE_THROUGH);
    if (archive_error != ERROR_SUCCESS) {
        remove_file_best_effort(staged_path);
        throw Error(win32_error("MoveFileExW(archive expired step claim)", archive_error));
    }
    if (MoveFileExW(staged_path.c_str(), claim_path.c_str(), MOVEFILE_WRITE_THROUGH)) {
        return;
    }

    const DWORD publish_error = GetLastError();
    const BOOL restored = MoveFileExW(
        archived_path.c_str(),
        claim_path.c_str(),
        MOVEFILE_WRITE_THROUGH);
    const DWORD restore_error = restored ? ERROR_SUCCESS : GetLastError();
    remove_file_best_effort(staged_path);
    if (!restored) {
        throw Error(
            win32_error("MoveFileExW(publish recovered step claim)", publish_error) +
            "; " + win32_error("MoveFileExW(restore expired step claim)", restore_error));
    }
    throw Error(win32_error("MoveFileExW(publish recovered step claim)", publish_error));
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

// 验证已存在的 canonical 结果确实属于当前步骤及持久化 owner。
void validate_completed_result(
    const std::filesystem::path& result_path,
    const std::filesystem::path& claim_path,
    const StepClaimLease& proposed_claim) {
    try {
        const nlohmann::json result = load_json(result_path);
        const std::string result_job_id = result.value("job_id", std::string{});
        validate_identifier(result_job_id, "canonical result job_id");
        if (result.value("run_id", std::string{}) != proposed_claim.run_id ||
            result.value("vm_id", std::string{}) != proposed_claim.vm_id ||
            result.value("step_id", std::string{}) != proposed_claim.step_id) {
            throw StepClaimStateError(
                "Canonical step result does not match the requested step");
        }
        if (std::filesystem::exists(claim_path)) {
            if (!std::filesystem::is_regular_file(claim_path)) {
                throw StepClaimStateError("Step claim path is not a regular file");
            }
            const StepClaimLease owner = load_effective_claim_unlocked(claim_path);
            if (owner.job_id != result_job_id) {
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

// 在取得 claim 锁前验证待发布证据路径和目标唯一性。
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
        if (!std::filesystem::is_directory(evidence.canonical_path.parent_path())) {
            throw Error("Canonical step result evidence parent does not exist");
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
        throw Error(win32_error("MoveFileExW(publish step result evidence)", GetLastError()));
    }
}

}  // namespace

std::filesystem::path step_claim_lock_path(const std::filesystem::path& claim_path) {
    std::filesystem::path lock_path = claim_path;
    lock_path.replace_extension(L".lock");
    return lock_path;
}

StepClaimAcquireResult acquire_step_claim_transaction(
    const std::filesystem::path& claim_path,
    const std::filesystem::path& canonical_result_path,
    const StepClaimLease& proposed_claim,
    const std::string& current_boot_id) {
    validate_proposed_claim(proposed_claim);
    if (proposed_claim.boot_id != current_boot_id) {
        throw Error("Proposed step claim boot identity does not match the current Agent");
    }

    const UniqueHandle lock = acquire_claim_lock(claim_path, true);
    const std::int64_t now_unix_ms = unix_time_ms(); // 锁内时间用于恢复和新租约
    if (std::filesystem::exists(canonical_result_path)) {
        if (!std::filesystem::is_regular_file(canonical_result_path)) {
            throw StepClaimStateError(
                "Canonical step result path is not a regular file");
        }
        validate_completed_result(canonical_result_path, claim_path, proposed_claim);
        return {StepClaimAcquireStatus::Completed, std::nullopt, std::nullopt};
    }

    if (!std::filesystem::exists(claim_path)) {
        const StepClaimLease acquired = make_acquired_claim(proposed_claim, now_unix_ms, 1);
        if (has_step_claim_renewals(claim_path, acquired)) {
            throw Error("Renewal sidecars already exist for a new step claim job");
        }
        const std::filesystem::path staged_path = stage_claim_json(claim_path, acquired);
        publish_staged_claim(staged_path, claim_path);
        return {StepClaimAcquireStatus::Acquired, acquired, std::nullopt};
    }
    if (!std::filesystem::is_regular_file(claim_path)) {
        throw StepClaimStateError("Step claim path is not a regular file");
    }

    const StepClaimLease existing = load_effective_claim_unlocked(claim_path);
    const ClaimRecoveryDecision decision = evaluate_claim_recovery(
        existing,
        now_unix_ms);
    if (decision == ClaimRecoveryDecision::Wait) {
        return {StepClaimAcquireStatus::Wait, existing, std::nullopt};
    }
    if (decision == ClaimRecoveryDecision::ManualInterventionRequired) {
        return {
            StepClaimAcquireStatus::ManualInterventionRequired,
            existing,
            std::nullopt,
        };
    }
    if (existing.attempt == std::numeric_limits<std::uint32_t>::max()) {
        throw Error("Step claim attempt limit has been reached");
    }

    const StepClaimLease acquired = make_acquired_claim(
        proposed_claim,
        now_unix_ms,
        existing.attempt + 1);
    if (has_step_claim_renewals(claim_path, acquired)) {
        throw Error("Renewal sidecars already exist for the recovered step claim job");
    }

    const std::filesystem::path staged_path = stage_claim_json(claim_path, acquired);
    const std::filesystem::path archived_path =
        make_archived_claim_path(claim_path, existing.attempt);
    replace_expired_claim(claim_path, archived_path, staged_path);
    return {StepClaimAcquireStatus::Acquired, acquired, archived_path};
}

StepClaimRenewResult renew_step_claim_transaction(
    const std::filesystem::path& claim_path,
    const StepClaimLease& expected_owner,
    const std::int64_t lease_duration_ms) {
    const UniqueHandle lock = acquire_claim_lock(claim_path, false);
    if (!std::filesystem::is_regular_file(claim_path)) {
        return {StepClaimRenewStatus::OwnershipLost, std::nullopt};
    }

    const StepClaimLease current = load_effective_claim_unlocked(claim_path);
    if (current.schema_version != 3 ||
        !same_step_claim_owner(current, expected_owner)) {
        return {StepClaimRenewStatus::OwnershipLost, std::nullopt};
    }

    const std::int64_t renewed_unix_ms = unix_time_ms(); // 禁止用锁外旧时间复活租约
    if (renewed_unix_ms >= current.lease_expires_unix_ms) {
        return {StepClaimRenewStatus::LeaseExpired, current};
    }
    const StepClaimLease renewed = renew_step_claim_lease(
        current,
        renewed_unix_ms,
        lease_duration_ms);
    if (renewed.lease_expires_unix_ms <= current.lease_expires_unix_ms) {
        throw Error("Step claim renewal must extend the current lease deadline");
    }
    const std::filesystem::path renewal_path =
        step_claim_renewal_path(claim_path, renewed);
    if (std::filesystem::exists(renewal_path)) {
        throw StepClaimStateError("Immutable step claim renewal sidecar already exists");
    }
    const std::filesystem::path staged_path = stage_claim_json(renewal_path, renewed);
    publish_staged_claim(staged_path, renewal_path);
    return {StepClaimRenewStatus::Renewed, renewed};
}

StepClaimLease load_effective_step_claim(const std::filesystem::path& claim_path) {
    const UniqueHandle lock = acquire_claim_lock(claim_path, false);
    if (!std::filesystem::is_regular_file(claim_path)) {
        throw Error("Step claim does not exist");
    }
    return load_effective_claim_unlocked(claim_path);
}

StepResultPublishStatus publish_step_result_if_owned(
    const std::filesystem::path& claim_path,
    const StepClaimLease& expected_owner,
    const std::filesystem::path& canonical_result_path,
    const nlohmann::json& result,
    const std::vector<StepResultEvidenceFile>& evidence_files) {
    validate_result_owner(result, expected_owner);
    validate_evidence_files(evidence_files);
    const UniqueHandle lock = acquire_claim_lock(claim_path, false);
    if (!std::filesystem::is_regular_file(claim_path)) {
        return StepResultPublishStatus::OwnershipLost;
    }

    const StepClaimLease current = load_effective_claim_unlocked(claim_path);
    if (!same_step_claim_owner(current, expected_owner)) {
        return StepResultPublishStatus::OwnershipLost;
    }
    const std::int64_t now_unix_ms = unix_time_ms(); // 锁内截止时间决定发布权
    if (now_unix_ms >= current.lease_expires_unix_ms) {
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
        publish_evidence_file(evidence);
    }
    write_json_atomic_existing_parent(canonical_result_path, result);
    return StepResultPublishStatus::Published;
}

}  // namespace satsuma::vm
