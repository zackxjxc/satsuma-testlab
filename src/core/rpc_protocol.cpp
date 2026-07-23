// Host 与 VM RPC 请求校验实现。
#include "satsuma/core/rpc_protocol.hpp"

#include <array>
#include <string>

#include "satsuma/core/errors.hpp"
#include "satsuma/core/id.hpp"

namespace satsuma {
namespace {

// 验证所有 Agent 请求共有的协议和会话身份字段。
void validate_identity(
    const int protocol_version,
    const std::string& lab_id,
    const std::string& vm_id,
    const std::string& session_id,
    const std::string& boot_id,
    const std::string& request_id,
    const std::string_view expected_lab_id) {
    if (protocol_version != kRpcProtocolVersion) {
        throw Error("Unsupported RPC protocol version: " + std::to_string(protocol_version));
    }
    validate_identifier(lab_id, "lab_id");
    validate_identifier(vm_id, "vm_id");
    validate_identifier(session_id, "session_id");
    validate_identifier(boot_id, "boot_id");
    validate_identifier(request_id, "request_id");
    if (lab_id != expected_lab_id) {
        throw Error("RPC request belongs to a different lab_id");
    }
}

// 验证字符串是否属于允许的状态集合。
template <std::size_t Size>
void validate_status(
    const std::string& status,
    const std::array<std::string_view, Size>& allowed) {
    for (const std::string_view value : allowed) {
        if (status == value) {
            return;
        }
    }
    throw Error("Unsupported RPC status: " + status);
}

}  // namespace

void validate_rpc_request(const AgentHello& request, const std::string_view expected_lab_id) {
    validate_identity(
        request.protocol_version,
        request.lab_id,
        request.vm_id,
        request.session_id,
        request.boot_id,
        request.request_id,
        expected_lab_id);
    if (request.agent_version.empty() || request.agent_version.size() > 64) {
        throw Error("agent_version must contain between 1 and 64 characters");
    }
}

void validate_rpc_request(const AgentStatus& request, const std::string_view expected_lab_id) {
    validate_identity(
        request.protocol_version,
        request.lab_id,
        request.vm_id,
        request.session_id,
        request.boot_id,
        request.request_id,
        expected_lab_id);
    validate_status(request.status, std::array<std::string_view, 3>{"idle", "running", "error"});
    if (!request.job_id.empty()) {
        validate_identifier(request.job_id, "job_id");
    }
}

void validate_rpc_request(const PollRequest& request, const std::string_view expected_lab_id) {
    validate_identity(
        request.protocol_version,
        request.lab_id,
        request.vm_id,
        request.session_id,
        request.boot_id,
        request.request_id,
        expected_lab_id);
}

void validate_rpc_request(const JobStatus& request, const std::string_view expected_lab_id) {
    validate_identity(
        request.protocol_version,
        request.lab_id,
        request.vm_id,
        request.session_id,
        request.boot_id,
        request.request_id,
        expected_lab_id);
    validate_identifier(request.run_id, "run_id");
    validate_identifier(request.job_id, "job_id");
    validate_identifier(request.step_id, "step_id");
    validate_status(
        request.status,
        std::array<std::string_view, 4>{"running", "exited", "timed_out", "failed"});
    if (!request.has_exit_code && request.exit_code != 0) {
        throw Error("exit_code must be zero when has_exit_code is false");
    }
}

}  // namespace satsuma
