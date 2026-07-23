// AI 派生快照命名和配额策略实现。
#include "satsuma/core/snapshot.hpp"

#include <algorithm>

#include "satsuma/core/errors.hpp"
#include "satsuma/core/id.hpp"

namespace satsuma {

std::string plan_ai_snapshot_name(
    const SnapshotConfig& config,
    const std::vector<std::string>& existing_snapshots,
    const std::string_view purpose,
    const std::string_view timestamp) {
    validate_identifier(purpose, "snapshot purpose");
    validate_identifier(timestamp, "snapshot timestamp");

    const std::size_t ai_snapshot_count = static_cast<std::size_t>(std::count_if(
        existing_snapshots.begin(),
        existing_snapshots.end(),
        [&config](const std::string& name) { return name.starts_with(config.ai_prefix); }));
    if (ai_snapshot_count >= static_cast<std::size_t>(config.max_ai_snapshots)) {
        throw Error("AI snapshot quota has been reached");
    }

    const std::string name = config.ai_prefix + std::string(purpose) + "-" + std::string(timestamp);
    if (name.size() > 128) {
        throw Error("Generated AI snapshot name exceeds 128 characters");
    }
    if (std::find(existing_snapshots.begin(), existing_snapshots.end(), name) != existing_snapshots.end()) {
        throw Error("Generated AI snapshot name already exists: " + name);
    }
    return name;
}

void validate_ai_snapshot_deletion(
    const SnapshotConfig& config,
    const std::vector<std::string>& existing_snapshots,
    const std::string_view snapshot_name) {
    validate_identifier(snapshot_name, "snapshot name");
    if (snapshot_name == config.base) {
        throw Error("User base snapshot is read-only: " + std::string(snapshot_name));
    }
    if (!snapshot_name.starts_with(config.ai_prefix)) {
        throw Error("Snapshot is not owned by the configured AI prefix: " + std::string(snapshot_name));
    }
    if (std::find(existing_snapshots.begin(), existing_snapshots.end(), snapshot_name) ==
        existing_snapshots.end()) {
        throw Error("AI snapshot does not exist: " + std::string(snapshot_name));
    }
}

}  // namespace satsuma
