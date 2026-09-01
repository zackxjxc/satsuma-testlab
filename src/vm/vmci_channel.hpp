// Guest Agent 的 VMCI 文件镜像与权威 claim 客户端。
#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "satsuma/core/claim_store.hpp"
#include "satsuma/core/config.hpp"
#include "satsuma/core/vmci.hpp"

namespace satsuma::vm {

class VmciChannel {
public:
    VmciChannel(const AgentConfig& config, std::string session_id);

    void update_config(const AgentConfig& config);
    void synchronize_inbound();
    void synchronize_outbound();

    [[nodiscard]] StepClaimAcquireResult acquire_claim(
        const StepClaimLease& proposed);
    [[nodiscard]] StepClaimRenewResult renew_claim(
        const StepClaimLease& owner,
        std::int64_t lease_duration_ms);
    [[nodiscard]] StepResultPublishStatus publish_result(
        const StepClaimLease& owner,
        const nlohmann::json& result,
        const std::vector<StepResultEvidenceFile>& evidence);
    [[nodiscard]] bool cancelled(const std::string& run_id);

private:
    [[nodiscard]] nlohmann::json request_base(const char* operation) const;
    void download_file(const nlohmann::json& descriptor);
    void upload_file(
        const std::filesystem::path& local_path,
        const std::filesystem::path& protocol_relative);
    void remove_stale_inbound(const std::vector<nlohmann::json>& descriptors);

    AgentConfig config_;
    std::string session_id_;
    transport::Client client_;
    transport::Client cancellation_client_;
};

}  // namespace satsuma::vm
