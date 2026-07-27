// VM Agent claim 后台续租和安全截止处理实现。
#include "claim_renewal.hpp"

#include <algorithm>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include <utility>

#include "satsuma/core/errors.hpp"

namespace satsuma::vm {

namespace detail {

// 后台线程与 Agent 执行线程共享的最小同步状态。
struct ClaimRenewalState {
    std::mutex mutex; // 保护 current_claim、safe_deadline 和 loss_reason
    std::filesystem::path claim_path; // 当前步骤的基础 claim 路径
    StepClaimLease current_claim; // 最近一次成功续租后的 claim
    ClaimLeasePolicy policy; // 当前会话使用的固定时间策略
    ClaimRenewOperation renew_operation; // 实际或测试注入的续租事务
    std::chrono::steady_clock::time_point safe_deadline; // 单调时钟安全截止点
    std::string loss_reason; // 最终失权或续租失败原因
    std::stop_source lease_loss_source; // 通知 Agent 终止当前步骤
};

}  // namespace detail

namespace {

// 根据持久化截止时间刷新不受系统时钟回退影响的安全截止点。
[[nodiscard]] std::chrono::steady_clock::time_point calculate_safe_deadline(
    const StepClaimLease& claim,
    const ClaimLeasePolicy& policy) {
    const std::int64_t remaining_ms = std::max<std::int64_t>(
        0,
        claim.lease_expires_unix_ms - unix_time_ms() - policy.safety_margin.count());
    return std::chrono::steady_clock::now() + std::chrono::milliseconds(remaining_ms);
}

// 使用 stop_token 执行可立即唤醒的有限等待。
[[nodiscard]] bool wait_for_stop(
    const std::stop_token stop_token,
    const std::chrono::milliseconds delay) {
    std::mutex mutex;
    std::condition_variable_any condition;
    std::unique_lock lock(mutex);
    condition.wait_for(lock, stop_token, delay, [] { return false; });
    return stop_token.stop_requested();
}

// 保存最终原因并向 Agent 发出 lease-loss 信号。
void request_lease_loss(
    const std::shared_ptr<detail::ClaimRenewalState>& state,
    std::string reason) {
    {
        std::lock_guard lock(state->mutex);
        if (state->loss_reason.empty()) {
            state->loss_reason = std::move(reason);
        }
    }
    state->lease_loss_source.request_stop();
}

// 返回当前 claim 和单调时钟安全截止点的一致快照。
[[nodiscard]] std::pair<StepClaimLease, std::chrono::steady_clock::time_point>
current_lease_snapshot(
    const std::shared_ptr<detail::ClaimRenewalState>& state) {
    std::lock_guard lock(state->mutex);
    return {state->current_claim, state->safe_deadline};
}

// 保存一次成功续租，并清除已经恢复的瞬时错误。
void store_renewed_claim(
    const std::shared_ptr<detail::ClaimRenewalState>& state,
    StepClaimLease renewed) {
    std::lock_guard lock(state->mutex);
    state->safe_deadline = calculate_safe_deadline(renewed, state->policy);
    state->current_claim = std::move(renewed);
}

// 运行正常周期、瞬时错误重试和安全截止处理。
void run_renewal_loop(
    const std::shared_ptr<detail::ClaimRenewalState>& state,
    const std::stop_token stop_token) {
    std::chrono::milliseconds delay = state->policy.renewal_interval;
    std::string last_error;
    while (!stop_token.stop_requested()) {
        auto [owner, safe_deadline] = current_lease_snapshot(state);
        const auto now = std::chrono::steady_clock::now();
        if (now >= safe_deadline) {
            request_lease_loss(
                state,
                last_error.empty()
                    ? "Step claim reached its renewal safety deadline"
                    : "Step claim renewal failed before its safety deadline: " + last_error);
            return;
        }

        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            safe_deadline - now);
        if (wait_for_stop(stop_token, std::min(delay, remaining))) {
            return;
        }
        if (std::chrono::steady_clock::now() >= safe_deadline) {
            continue;
        }

        try {
            StepClaimRenewResult result = state->renew_operation(
                state->claim_path,
                owner,
                state->policy.lease_duration.count());
            if (result.status == StepClaimRenewStatus::OwnershipLost) {
                request_lease_loss(state, "Step claim ownership was lost before renewal");
                return;
            }
            if (result.status == StepClaimRenewStatus::LeaseExpired) {
                request_lease_loss(state, "Step claim lease expired before renewal");
                return;
            }
            if (!result.claim.has_value()) {
                throw Error("Step claim renewal returned no effective lease");
            }
            store_renewed_claim(state, std::move(*result.claim));
            last_error.clear();
            delay = state->policy.renewal_interval;
        } catch (const std::exception& error) {
            last_error = error.what();
            delay = state->policy.retry_interval;
        }
    }
}

}  // namespace

void validate_claim_lease_policy(const ClaimLeasePolicy& policy) {
    if (policy.lease_duration.count() <= 0 ||
        policy.renewal_interval.count() <= 0 ||
        policy.retry_interval.count() <= 0 ||
        policy.safety_margin.count() <= 0) {
        throw Error("Claim lease policy durations must be positive");
    }
    if (policy.renewal_interval > policy.lease_duration / 3) {
        throw Error("Claim lease duration must allow at least three renewal intervals");
    }
    if (policy.renewal_interval + policy.safety_margin >= policy.lease_duration) {
        throw Error("Claim lease policy does not leave a usable renewal window");
    }
}

ClaimRenewalSession::ClaimRenewalSession(
    std::filesystem::path claim_path,
    StepClaimLease owner,
    ClaimLeasePolicy policy,
    ClaimRenewOperation renew_operation) {
    validate_claim_lease_policy(policy);
    if (owner.schema_version != 3) {
        throw Error("Claim renewal session requires a schema version 3 owner");
    }
    if (!renew_operation) {
        renew_operation = renew_step_claim_transaction;
    }

    state_ = std::make_shared<detail::ClaimRenewalState>();
    state_->claim_path = std::move(claim_path);
    state_->current_claim = std::move(owner);
    state_->policy = policy;
    state_->renew_operation = std::move(renew_operation);
    state_->safe_deadline = calculate_safe_deadline(state_->current_claim, policy);
    worker_ = std::jthread(
        [state = state_](const std::stop_token stop_token) {
            run_renewal_loop(state, stop_token);
        });
}

ClaimRenewalSession::~ClaimRenewalSession() {
    finish();
}

std::stop_token ClaimRenewalSession::lease_loss_token() const noexcept {
    return state_->lease_loss_source.get_token();
}

StepClaimLease ClaimRenewalSession::current_claim() const {
    std::lock_guard lock(state_->mutex);
    return state_->current_claim;
}

std::string ClaimRenewalSession::loss_reason() const {
    std::lock_guard lock(state_->mutex);
    return state_->loss_reason;
}

void ClaimRenewalSession::finish() noexcept {
    if (!worker_.joinable()) {
        return;
    }
    worker_.request_stop();
    try {
        worker_.join();
    } catch (...) {
    }
}

}  // namespace satsuma::vm
