// Host RPC 会话状态和任务选择实现。
#include "satsuma/rpc/service.hpp"

#include <algorithm>
#include <filesystem>
#include <utility>
#include <vector>

#include "satsuma/core/errors.hpp"
#include "satsuma/core/id.hpp"
#include "satsuma/core/path.hpp"
#include "satsuma/core/task.hpp"

namespace satsuma::host {

RpcService::RpcService(LabConfig config) : config_(std::move(config)) {}

SessionInfo RpcService::register_agent(AgentHello request) {
    SessionInfo response;
    response.host_time = utc_timestamp();
    response.session_id = request.session_id;
    try {
        validate_rpc_request(request, config_.lab_id);
        const VmConfig* vm = find_vm(config_, request.vm_id);
        if (vm == nullptr) {
            throw Error("Agent references an unknown VM: " + request.vm_id);
        }
        if (vm->agent_version != request.agent_version) {
            throw Error(
                "Agent version mismatch for " + request.vm_id +
                ": expected " + vm->agent_version + ", received " + request.agent_version);
        }

        SessionEntry entry;
        entry.session_id = request.session_id;
        entry.boot_id = request.boot_id;
        entry.agent_version = request.agent_version;
        entry.status = "idle";
        entry.updated_at = response.host_time;
        {
            std::scoped_lock lock(mutex_);
            sessions_.insert_or_assign(request.vm_id, std::move(entry));
        }
        response.accepted = true;
    } catch (const std::exception& error) {
        response.message = error.what();
    }
    return response;
}

HostDirective RpcService::heartbeat(AgentStatus request) {
    HostDirective response;
    try {
        validate_rpc_request(request, config_.lab_id);
        std::scoped_lock lock(mutex_);
        const auto session = sessions_.find(request.vm_id);
        if (session == sessions_.end() ||
            session->second.session_id != request.session_id ||
            session->second.boot_id != request.boot_id) {
            response.action = "stop";
            response.message = "Agent session is not registered";
            return response;
        }
        session->second.status = std::move(request.status);
        session->second.job_id = std::move(request.job_id);
        session->second.updated_at = utc_timestamp();
        response.action = "poll";
    } catch (const std::exception& error) {
        response.action = "stop";
        response.message = error.what();
    }
    return response;
}

TaskReference RpcService::poll_task(PollRequest request) {
    TaskReference response;
    try {
        validate_rpc_request(request, config_.lab_id);
        if (!matches_session(request.vm_id, request.session_id, request.boot_id)) {
            throw Error("Agent session is not registered");
        }

        const std::filesystem::path runs_root = config_.shared_folder.host_root / L"runs";
        if (!std::filesystem::is_directory(runs_root)) {
            return response;
        }

        std::vector<std::filesystem::path> run_directories;
        for (const auto& entry : std::filesystem::directory_iterator(runs_root)) {
            if (entry.is_directory() && !entry.path().filename().native().starts_with(L".")) {
                run_directories.push_back(entry.path());
            }
        }
        std::sort(run_directories.begin(), run_directories.end());

        for (const auto& run_directory : run_directories) {
            const std::filesystem::path task_path = run_directory / L"task.json";
            if (!std::filesystem::is_regular_file(task_path)) {
                continue;
            }
            const RunManifest manifest = load_run_manifest(task_path);
            if (manifest.lab_id != config_.lab_id) {
                continue;
            }
            for (const auto& step : manifest.steps) {
                if (step.vm != request.vm_id) {
                    continue;
                }
                const std::filesystem::path result =
                    run_directory / L"results" / path_from_utf8(request.vm_id) /
                    path_from_utf8(step.id) / L"execution.json";
                const std::filesystem::path claim =
                    run_directory / L"state" / path_from_utf8(request.vm_id) /
                    path_from_utf8(step.id + ".claim.json");
                if (!std::filesystem::exists(result) && !std::filesystem::exists(claim)) {
                    response.has_task = true;
                    response.type = "run_manifest";
                    response.run_id = manifest.run_id;
                    response.manifest = path_to_utf8(
                        std::filesystem::relative(task_path, config_.shared_folder.host_root));
                    std::replace(response.manifest.begin(), response.manifest.end(), '\\', '/');
                    return response;
                }
            }
        }
    } catch (const std::exception& error) {
        response.has_task = false;
        response.type = "error";
        response.manifest = error.what();
    }
    return response;
}

RpcAck RpcService::report_job(JobStatus request) {
    RpcAck response;
    try {
        validate_rpc_request(request, config_.lab_id);
        std::scoped_lock lock(mutex_);
        const auto session = sessions_.find(request.vm_id);
        if (session == sessions_.end() ||
            session->second.session_id != request.session_id ||
            session->second.boot_id != request.boot_id) {
            throw Error("Agent session is not registered");
        }
        session->second.status = request.status == "running" ? "running" : "idle";
        session->second.job_id = request.status == "running" ? request.job_id : "";
        session->second.updated_at = utc_timestamp();
        response.accepted = true;
    } catch (const std::exception& error) {
        response.message = error.what();
    }
    return response;
}

std::size_t RpcService::session_count() const {
    std::scoped_lock lock(mutex_);
    return sessions_.size();
}

bool RpcService::matches_session(
    const std::string& vm_id,
    const std::string& session_id,
    const std::string& boot_id) const {
    std::scoped_lock lock(mutex_);
    const auto session = sessions_.find(vm_id);
    return session != sessions_.end() &&
        session->second.session_id == session_id &&
        session->second.boot_id == boot_id;
}

}  // namespace satsuma::host
