// AI 派生快照命名和配额策略。
#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "satsuma/core/config.hpp"

namespace satsuma {

// 校验用途、配额和重名后返回完整 AI 快照名。
[[nodiscard]] std::string plan_ai_snapshot_name(
    const SnapshotConfig& config,
    const std::vector<std::string>& existing_snapshots,
    std::string_view purpose,
    std::string_view timestamp);

// 仅允许删除现有且属于配置前缀的 AI 快照。
void validate_ai_snapshot_deletion(
    const SnapshotConfig& config,
    const std::vector<std::string>& existing_snapshots,
    std::string_view snapshot_name);

}  // namespace satsuma
