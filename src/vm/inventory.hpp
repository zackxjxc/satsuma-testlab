// Guest 环境清单采集、缓存和 VMCI 状态发布接口。
#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "satsuma/core/config.hpp"

namespace satsuma::vm {

// 维护单个 Agent 进程会话内不变的环境快照。
class InventoryPublisher {
public:
    // 保存当前 Agent 身份以及本次启动标识。
    explicit InventoryPublisher(const AgentConfig& config);

    // 身份绑定变化后更新发布配置并失效当前会话缓存。
    void update_config(const AgentConfig& config);

    // 首次采集或处理显式刷新请求，并修复缺失、损坏的镜像文件。
    void synchronize();

    // 返回当前已发布快照的 SHA-256；尚未成功发布时为空。
    [[nodiscard]] const std::string& digest() const noexcept;

    // 返回当前快照的采集时间；尚未成功发布时为空。
    [[nodiscard]] const std::string& observed_at() const noexcept;

    // 返回当前缓存中已验证可用的固定解释器路径。
    [[nodiscard]] std::filesystem::path script_engine_path(std::string_view engine) const;

private:
    // 采集新的系统快照，失败时不改变上一份有效缓存。
    [[nodiscard]] nlohmann::json collect(const std::string& request_id) const;

    // 原子写入缓存并更新内容摘要。
    void publish(const nlohmann::json& inventory);

    AgentConfig config_;                    // 当前 Agent 配置快照
    std::optional<nlohmann::json> cache_;   // 当前会话内的有效快照
    std::string digest_;                    // 已发布文件 SHA-256
    std::string observed_at_;               // 当前快照采集时间
    std::string handled_request_id_;        // 已完成的显式刷新请求
};

}  // namespace satsuma::vm
