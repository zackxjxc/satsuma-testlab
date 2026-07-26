// Satsuma Host 运行生命周期状态和持久化接口。
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json_fwd.hpp>

namespace satsuma {

// Host 编排过程中可持久化的运行阶段。
enum class RunPhase {
    Preparing,
    RestoringBefore,
    StartingVm,
    WaitingAgent,
    Deploying,
    Executing,
    CollectingEvidence,
    RunningFinally,
    Recovering,
    Completed,
    Failed,
    RecoveryFailed,
    ManualInterventionRequired,
};

// 单次生命周期状态迁移记录。
struct RunTransition {
    std::uint64_t sequence{0};  // 从 1 开始单调递增的迁移序号
    RunPhase from{RunPhase::Preparing};  // 迁移前阶段
    RunPhase to{RunPhase::Preparing};    // 迁移后阶段
    std::string occurred_at;             // UTC 迁移时间
    std::string message;                 // 迁移原因或结果摘要
};

// Host 重启后可恢复读取的完整运行状态。
struct RunLifecycleState {
    int schema_version{1};                    // 生命周期状态 schema 版本
    std::string run_id;                       // 对应的运行 ID
    RunPhase phase{RunPhase::Preparing};      // 当前持久化阶段
    std::uint64_t sequence{0};                // 最新迁移序号
    std::string updated_at;                   // 最近更新时间
    std::vector<RunTransition> transitions;   // 完整迁移历史
};

// 返回生命周期阶段的稳定协议名称。
[[nodiscard]] std::string_view run_phase_name(RunPhase phase);

// 从稳定协议名称解析生命周期阶段。
[[nodiscard]] RunPhase parse_run_phase(std::string_view name);

// 判断阶段是否禁止继续自动迁移。
[[nodiscard]] bool is_terminal_run_phase(RunPhase phase) noexcept;

// 创建尚未执行任何外部操作的初始状态。
[[nodiscard]] RunLifecycleState make_run_lifecycle_state(
    std::string run_id,
    std::string timestamp);

// 验证并在内存中应用一次状态迁移。
void apply_run_transition(
    RunLifecycleState& state,
    RunPhase next,
    std::string timestamp,
    std::string message);

// 原子保存新状态，成功后再更新调用方持有的状态。
void persist_run_transition(
    const std::filesystem::path& path,
    RunLifecycleState& state,
    RunPhase next,
    std::string timestamp,
    std::string message);

// 读取并验证已持久化的生命周期状态。
[[nodiscard]] RunLifecycleState load_run_lifecycle_state(const std::filesystem::path& path);

// 将生命周期状态转换为 JSON。
void to_json(nlohmann::json& value, const RunLifecycleState& state);

// 从 JSON 解析并验证生命周期状态。
void from_json(const nlohmann::json& value, RunLifecycleState& state);

}  // namespace satsuma
