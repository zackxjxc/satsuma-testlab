// VM Agent claim 固定租约策略和后台续租会话接口。
#pragma once

#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <stop_token>
#include <string>
#include <thread>

#include "claim_store.hpp"

namespace satsuma::vm {

namespace detail {
struct ClaimRenewalState;
}

// Agent 主动续租使用的固定时间策略。
struct ClaimLeasePolicy {
    std::chrono::milliseconds lease_duration{std::chrono::seconds(120)}; // 每次租约窗口
    std::chrono::milliseconds renewal_interval{std::chrono::seconds(30)}; // 正常续租周期
    std::chrono::milliseconds retry_interval{std::chrono::seconds(1)}; // 瞬时错误重试周期
    std::chrono::milliseconds safety_margin{std::chrono::seconds(10)}; // 到期前取消余量
};

// 可注入的续租事务，测试可用它稳定模拟共享目录故障。
using ClaimRenewOperation = std::function<StepClaimRenewResult(
    const std::filesystem::path&,
    const StepClaimLease&,
    std::int64_t)>;

// 验证固定租约策略能留出至少两个正常续租机会和取消余量。
void validate_claim_lease_policy(const ClaimLeasePolicy& policy);

// 在后台持续续租，并在所有权丢失或安全截止前失败时发出停止信号。
class ClaimRenewalSession {
public:
    // 启动当前 owner 的续租线程。
    ClaimRenewalSession(
        std::filesystem::path claim_path,
        StepClaimLease owner,
        ClaimLeasePolicy policy,
        ClaimRenewOperation renew_operation = {});

    ClaimRenewalSession(const ClaimRenewalSession&) = delete;
    ClaimRenewalSession& operator=(const ClaimRenewalSession&) = delete;
    ClaimRenewalSession(ClaimRenewalSession&&) = delete;
    ClaimRenewalSession& operator=(ClaimRenewalSession&&) = delete;

    // 停止并回收仍在运行的后台线程。
    ~ClaimRenewalSession();

    // 返回 lease-loss 信号，调用方用它取消 Artifact 和 Job Object。
    [[nodiscard]] std::stop_token lease_loss_token() const noexcept;

    // 返回最近一次成功续租后的有效 claim 快照。
    [[nodiscard]] StepClaimLease current_claim() const;

    // 返回最终 lease-loss 原因；尚未失权时为空。
    [[nodiscard]] std::string loss_reason() const;

    // 正常完成结果发布后停止并等待续租线程退出。
    void finish() noexcept;

private:
    std::shared_ptr<detail::ClaimRenewalState> state_; // 线程和调用方共享状态
    std::jthread worker_; // 可停止的后台续租线程
};

}  // namespace satsuma::vm
