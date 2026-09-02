// VM Agent 任务领取、执行和结果落盘实现。
#include "agent.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <iostream>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>
#include <windows.h>

#include "satsuma/core/errors.hpp"
#include "satsuma/core/claim.hpp"
#include "satsuma/core/id.hpp"
#include "satsuma/core/json_io.hpp"
#include "satsuma/core/path.hpp"
#include "satsuma/core/claim_store.hpp"
#include "hardware_identity.hpp"
#include "satsuma/core/sha256.hpp"
#include "interactive_process.hpp"
#include "vmci_channel.hpp"
#include "update.hpp"

namespace satsuma::vm {
namespace {

[[nodiscard]] bool use_test_local_mirror() noexcept {
#ifdef SATSUMA_TEST_LOCAL_MIRROR
    wchar_t value[2]{};
    return GetEnvironmentVariableW(
        L"SATSUMA_TEST_LOCAL_MIRROR",
        value,
        static_cast<DWORD>(std::size(value))) == 1 && value[0] == L'1';
#else
    return false;
#endif
}

// 在轮询间隔内等待停止请求，收到请求时立即返回。
[[nodiscard]] bool wait_for_stop(
    const std::stop_token stop_token,
    const std::chrono::milliseconds delay) {
    std::mutex mutex;
    std::condition_variable_any condition;
    std::unique_lock lock(mutex);
    condition.wait_for(lock, stop_token, delay, [] { return false; });
    return stop_token.stop_requested();
}

// 返回 SYSTEM 与交互用户共用的本次 VM 工作目录。
[[nodiscard]] std::filesystem::path resolve_local_run_directory(
    const AgentConfig& config,
    const std::string& run_id) {
    return resolve_under_root(
        config.local_work_root,
        path_from_utf8(config.lab_id) / path_from_utf8(run_id) / path_from_utf8(config.vm_id));
}

// 在可取消操作边界统一转换停止请求。
void throw_if_stop_requested(const std::stop_token stop_token) {
    if (stop_token.stop_requested()) {
        throw Error("Agent stop requested");
    }
}

// 使用 Win32 写穿方式创建 UTF-8 日志。
void write_text(const std::filesystem::path& path, const std::string& content) {
    std::filesystem::create_directories(path.parent_path());
    HANDLE file = CreateFileW(
        path.c_str(),
        GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH,
        nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        throw Error("Cannot create log file (Win32 error " + std::to_string(GetLastError()) + ")");
    }
    DWORD bytes_written = 0;
    const BOOL write_ok = WriteFile(
        file,
        content.data(),
        static_cast<DWORD>(content.size()),
        &bytes_written,
        nullptr);
    const BOOL flush_ok = write_ok ? FlushFileBuffers(file) : FALSE;
    const DWORD error = (!write_ok || !flush_ok) ? GetLastError() : ERROR_SUCCESS;
    CloseHandle(file);
    if (!write_ok || !flush_ok || bytes_written != content.size()) {
        throw Error("Cannot write log file (Win32 error " + std::to_string(error) + ")");
    }
}

// 将补充错误追加到执行结果并保持稳定分隔符。
void append_result_error(ExecutionResult& result, const std::string& error) {
    result.status = "failed";
    if (!result.error.empty()) {
        result.error += "; ";
    }
    result.error += error;
}

// 尽力记录单个损坏运行，避免它阻塞本地镜像中的后续任务。
void write_run_error_best_effort(
    const std::filesystem::path& run_directory,
    const AgentConfig& config,
    const std::string& error) noexcept {
    try {
        if (!std::filesystem::is_regular_file(run_directory / L"task.json")) {
            return;
        }
        const std::filesystem::path state_directory = run_directory / L"state";
        std::filesystem::create_directories(state_directory);
        write_json_atomic(
            state_directory / path_from_utf8(config.vm_id + "-agent-error.json"),
            {
                {"schema_version", 1},
                {"lab_id", config.lab_id},
                {"vm_id", config.vm_id},
                {"status", "invalid_run"},
                {"error", error},
                {"observed_at", utc_timestamp()},
            });
    } catch (...) {
    }
}

// 将 Guest 本地日志复制到当前 job 的共享暂存文件。
void stage_local_log(
    const std::filesystem::path& local,
    const std::filesystem::path& staged,
    ExecutionResult& result) {
    try {
        if (!std::filesystem::is_regular_file(local)) {
            write_text(local, "");
        }
        std::filesystem::create_directories(staged.parent_path());
        std::filesystem::copy_file(
            local,
            staged,
            std::filesystem::copy_options::overwrite_existing);
    } catch (const std::exception& error) {
        append_result_error(result, error.what());
        if (!std::filesystem::is_regular_file(staged)) {
            write_text(staged, "");
        }
    }
}

// 尽力保留失权 job 的结果摘要，不创建 canonical execution.json。
void write_stale_result_best_effort(
    const std::filesystem::path& job_directory,
    const ExecutionResult& result,
    const std::string& claim_status,
    const std::string& claim_error) noexcept {
    try {
        nlohmann::json stale = result;
        stale["claim_status"] = claim_status;
        stale["claim_error"] = claim_error;
        write_json_atomic_existing_parent(
            job_directory / L"stale-execution.json",
            stale);
    } catch (...) {
    }
}

// 返回与可执行文件或脚本路径完全匹配的 Artifact 登记项。
[[nodiscard]] const ArtifactManifest* find_artifact(
    const RunManifest& manifest,
    const std::string& vm_id,
    const std::filesystem::path& path) {
    const auto match = std::find_if(
        manifest.artifacts.begin(),
        manifest.artifacts.end(),
        [&vm_id, &path](const ArtifactManifest& artifact) {
            return artifact.vm == vm_id && artifact.path == path;
        });
    return match == manifest.artifacts.end() ? nullptr : &*match;
}

// 防止 cmd.exe /C 在调用脚本前展开用户参数中的环境变量引用。
[[nodiscard]] std::string escape_cmd_token(const std::string& argument) {
    std::string escaped = "\"";
    escaped.reserve(argument.size() + 8);
    for (const unsigned char character : argument) {
        if (character == '\0' || character == '\r' || character == '\n') {
            throw Error("CMD script arguments cannot contain NUL or line breaks");
        }
        if (character == '%') {
            escaped += "%SATSUMA_CMD_PERCENT%";
        } else if (character == '"') {
            escaped.push_back('"');
            escaped.push_back('^');
            escaped.push_back(static_cast<char>(character));
            escaped.push_back('"');
        } else {
            escaped.push_back(static_cast<char>(character));
        }
    }
    escaped.push_back('"');
    return escaped;
}

// 返回当前 SatsumaVM helper 的绝对路径。
[[nodiscard]] std::filesystem::path current_executable_path() {
    std::wstring buffer(32'768, L'\0');
    const DWORD length = GetModuleFileNameW(
        nullptr,
        buffer.data(),
        static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size()) {
        throw Error(
            "Cannot resolve SatsumaVM executable path (Win32 error " +
            std::to_string(GetLastError()) + ")");
    }
    buffer.resize(length);
    return std::filesystem::path(std::move(buffer));
}

// 限制 presence 中诊断错误的长度，避免异常信息无限膨胀共享状态文件。
[[nodiscard]] std::string bounded_runtime_error(std::string message) {
    constexpr std::size_t kMaxRuntimeErrorBytes = 1024;
    if (message.size() > kMaxRuntimeErrorBytes) {
        message.resize(kMaxRuntimeErrorBytes);
        message += "...";
    }
    return message;
}

// 响应 Host 的显式 Guest 工作目录清理请求并原子发布结果。
void process_guest_cleanup_request(
    const std::filesystem::path& run_directory,
    const RunManifest& manifest,
    const AgentConfig& config) {
    const std::filesystem::path state_directory = run_directory / L"state";
    const std::filesystem::path request_path =
        state_directory / path_from_utf8(config.vm_id + "-cleanup-request.json");
    if (!std::filesystem::is_regular_file(request_path)) {
        return;
    }

    const std::filesystem::path result_path =
        state_directory / path_from_utf8(config.vm_id + "-cleanup.json");
    if (std::filesystem::is_regular_file(result_path)) {
        return;
    }

    const nlohmann::json request = load_json(request_path);
    const std::string request_id = request.value("request_id", std::string{});
    validate_identifier(request_id, "cleanup request_id");
    if (request.value("schema_version", 0) != 1 ||
        request.value("lab_id", std::string{}) != config.lab_id ||
        request.value("run_id", std::string{}) != manifest.run_id ||
        request.value("vm_id", std::string{}) != config.vm_id ||
        request.value("target", std::string{}) != "guest_work") {
        throw Error("Guest cleanup request identity is invalid");
    }

    const std::filesystem::path local_run_directory =
        resolve_local_run_directory(config, manifest.run_id);
    std::error_code cleanup_error;
    const std::uintmax_t deleted_paths =
        std::filesystem::remove_all(local_run_directory, cleanup_error);
    nlohmann::json result = {
        {"schema_version", 1},
        {"lab_id", config.lab_id},
        {"run_id", manifest.run_id},
        {"vm_id", config.vm_id},
        {"request_id", request_id},
        {"target", "guest_work"},
        {"status", cleanup_error ? "failed" : "deleted"},
        {"deleted_path_count", deleted_paths},
        {"failed_path_count", cleanup_error ? 1 : 0},
        {"finished_at", utc_timestamp()},
    };
    if (cleanup_error) {
        result["error"] =
            "Cannot delete Guest work directory (error " +
            std::to_string(cleanup_error.value()) + ")";
    }
    write_json_atomic(result_path, result);
}

}  // namespace

// 单步骤从 Guest 本地执行到 Host 规范发布共享的显式工作区。
struct Agent::ExecutionWorkspace {
    std::filesystem::path result_directory;
    std::filesystem::path job_directory;
    std::filesystem::path stdout_staged;
    std::filesystem::path stderr_staged;
    std::filesystem::path stdout_final;
    std::filesystem::path stderr_final;
    std::filesystem::path local_run_directory;
    std::filesystem::path local_job_directory;
    std::filesystem::path stdout_local;
    std::filesystem::path stderr_local;
    std::vector<StepResultEvidenceFile> evidence_files;
    ExecutionResult result;
};

Agent::Agent(
    AgentConfig config,
    std::filesystem::path helper_executable,
    AgentRuntimeOptions runtime_options)
    : config_(std::move(config)),
      session_id_(make_id("session")),
      boot_id_(make_id("boot")),
      session_started_at_(utc_timestamp()),
      binary_sha256_(sha256_file(current_executable_path())),
      helper_executable_(helper_executable.empty()
          ? current_executable_path()
          : std::filesystem::absolute(std::move(helper_executable))),
      runtime_options_(std::move(runtime_options)),
      inventory_(config_) {
    if (config_.protocol_version != kRunManifestProtocolVersion) {
        throw Error("Agent execution requires the current VMCI protocol version");
    }
    validate_claim_lease_policy(runtime_options_.claim_lease_policy);
    if (config_.transport.vmci_port != 0 && !use_test_local_mirror()) {
        vmci_channel_ = std::make_unique<VmciChannel>(config_, session_id_);
        if (!runtime_options_.claim_acquire_operation) {
            runtime_options_.claim_acquire_operation = [this](
                const std::filesystem::path&,
                const std::filesystem::path&,
                const StepClaimLease& claim) {
                return vmci_channel_->acquire_claim(claim);
            };
        }
        if (!runtime_options_.claim_renew_operation) {
            runtime_options_.claim_renew_operation = [this](
                const std::filesystem::path&,
                const StepClaimLease& claim,
                const std::int64_t duration) {
                return vmci_channel_->renew_claim(claim, duration);
            };
        }
        if (!runtime_options_.result_publish_operation) {
            runtime_options_.result_publish_operation = [this](
                const std::filesystem::path&,
                const StepClaimLease& claim,
                const std::filesystem::path&,
                const nlohmann::json& result,
                const std::vector<StepResultEvidenceFile>& evidence) {
                return vmci_channel_->publish_result(claim, result, evidence);
            };
        }
        if (!runtime_options_.cancellation_check) {
            runtime_options_.cancellation_check = [this](
                const std::filesystem::path&,
                const std::string& run_id) {
                return vmci_channel_->cancelled(run_id);
            };
        }
    }
}

Agent::~Agent() = default;

int Agent::run_once(const std::stop_token stop_token) {
    if (stop_token.stop_requested()) {
        return 0;
    }
    if (vmci_channel_) {
        vmci_channel_->synchronize_inbound();
    }
    if (refresh_agent_binding(config_)) {
        inventory_.update_config(config_);
        if (vmci_channel_) {
            vmci_channel_->update_config(config_);
        }
    }
    inventory_.synchronize();
    write_presence();
    if (vmci_channel_) {
        vmci_channel_->synchronize_outbound();
    }
    const int executed = execute_pending_runs(stop_token);
    if (vmci_channel_) {
        vmci_channel_->synchronize_outbound();
    }
    return executed;
}

int Agent::execute_pending_runs(const std::stop_token stop_token) {
    if (stop_token.stop_requested() || config_.identity_unbound) {
        return 0;
    }
    const std::filesystem::path runs_root = config_.mirror_root / L"runs";
    std::filesystem::create_directories(runs_root);

    std::vector<std::filesystem::path> run_directories;
    for (const auto& entry : std::filesystem::directory_iterator(runs_root)) {
        if (!entry.is_directory() || entry.path().filename().native().starts_with(L".")) {
            continue;
        }
        const DWORD attributes = GetFileAttributesW(entry.path().c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES ||
            (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
            continue;
        }
        if (std::filesystem::is_regular_file(entry.path() / L"task.json")) {
            run_directories.push_back(entry.path());
        }
    }
    std::sort(run_directories.begin(), run_directories.end());

    int executed_steps = 0;
    for (const auto& run_directory : run_directories) {
        if (stop_token.stop_requested()) {
            break;
        }
        try {
            const RunManifest manifest = load_run_manifest(run_directory / L"task.json");
            if (manifest.lab_id != config_.lab_id ||
                manifest.protocol_version != config_.protocol_version) {
                continue;
            }
            if (path_from_utf8(manifest.run_id) != run_directory.filename()) {
                throw Error("Run manifest ID does not match its directory name");
            }

            for (const auto& step : manifest.steps) {
                if (stop_token.stop_requested()) {
                    return executed_steps;
                }
                if (step.vm != config_.vm_id) {
                    continue;
                }

                const std::filesystem::path result_directory = resolve_under_root(
                    run_directory,
                    std::filesystem::path(L"results") /
                        path_from_utf8(config_.vm_id) /
                        path_from_utf8(step.id));
                const std::filesystem::path result_path = result_directory / L"execution.json";

                const std::string job_id = make_id("job");
                const std::filesystem::path claim_path = resolve_under_root(
                    run_directory,
                    std::filesystem::path(L"state") /
                        path_from_utf8(config_.vm_id) /
                        path_from_utf8(step.id + ".claim.json"));
                const std::filesystem::path recovery_path = resolve_under_root(
                    run_directory,
                    std::filesystem::path(L"state") /
                        path_from_utf8(config_.vm_id) /
                        path_from_utf8(step.id + ".claim-recovery.json"));
                const StepClaimLease proposed_claim = make_step_claim_lease(
                    manifest.run_id,
                    config_.vm_id,
                    step.id,
                    job_id,
                    session_id_,
                    boot_id_,
                    unix_time_ms(),
                    runtime_options_.claim_lease_policy.lease_duration.count(),
                    step.retry_safe);
                StepClaimAcquireResult acquisition;
                try {
                    acquisition = runtime_options_.claim_acquire_operation
                        ? runtime_options_.claim_acquire_operation(
                            claim_path, result_path, proposed_claim)
                        : acquire_step_claim_transaction(
                            claim_path, result_path, proposed_claim);
                } catch (const StepClaimStateError& error) {
                    write_json_atomic(recovery_path, {
                        {"schema_version", 1},
                        {"status", "manual_intervention_required"},
                        {"reason", "claim state failed validation"},
                        {"error", error.what()},
                        {"current_boot_id", boot_id_},
                        {"observed_at", utc_timestamp()},
                    });
                    continue;
                }
                if (acquisition.status == StepClaimAcquireStatus::Completed ||
                    acquisition.status == StepClaimAcquireStatus::Wait) {
                    if (acquisition.status == StepClaimAcquireStatus::Completed &&
                        vmci_channel_) {
                        std::filesystem::create_directories(result_directory);
                        write_text(result_directory / L".vmci-complete", "completed\n");
                    }
                    continue;
                }
                if (acquisition.status == StepClaimAcquireStatus::ManualInterventionRequired) {
                    if (!acquisition.claim.has_value()) {
                        throw Error("Manual claim recovery result omitted the persisted claim");
                    }
                    write_json_atomic(recovery_path, {
                        {"schema_version", 1},
                        {"status", "manual_intervention_required"},
                        {"reason", "expired claim belongs to an unsafe step"},
                        {"claim", *acquisition.claim},
                        {"current_boot_id", boot_id_},
                        {"observed_at", utc_timestamp()},
                    });
                    continue;
                }
                if (acquisition.status != StepClaimAcquireStatus::Acquired ||
                    !acquisition.claim.has_value()) {
                    throw Error("Claim acquisition transaction returned an invalid state");
                }
                std::error_code marker_error;
                std::filesystem::remove(recovery_path, marker_error);

                execute_step(
                    run_directory,
                    manifest,
                    step,
                    claim_path,
                    *acquisition.claim,
                    stop_token);
                ++executed_steps;
            }
            const bool current_vm_complete = std::all_of(
                manifest.steps.begin(),
                manifest.steps.end(),
                [&run_directory, this](const TaskStep& step) {
                    const std::filesystem::path result_directory =
                        run_directory / L"results" / path_from_utf8(config_.vm_id) /
                        path_from_utf8(step.id);
                    return step.vm != config_.vm_id ||
                        std::filesystem::is_regular_file(result_directory / L"execution.json") ||
                        std::filesystem::is_regular_file(result_directory / L".vmci-complete");
                });
            if (current_vm_complete) {
                process_guest_cleanup_request(run_directory, manifest, config_);
            }
        } catch (const std::exception& error) {
            write_run_error_best_effort(run_directory, config_, error.what());
        }
    }
    return executed_steps;
}

void Agent::run_watch(const std::stop_token stop_token) {
    while (!stop_token.stop_requested()) {
        bool vmci_channel_available = false;
        try {
            if (vmci_channel_) {
                vmci_channel_->synchronize_inbound();
            }
            if (refresh_agent_binding(config_)) {
                inventory_.update_config(config_);
                if (vmci_channel_) {
                    vmci_channel_->update_config(config_);
                }
            }
            inventory_.synchronize();
            write_presence();
            if (vmci_channel_) {
                vmci_channel_->synchronize_outbound();
            }
            if (!config_.identity_unbound &&
                process_pending_agent_update(config_, stop_token)) {
                if (vmci_channel_) {
                    vmci_channel_->synchronize_outbound();
                }
                break;
            }
            if (!config_.identity_unbound) {
                static_cast<void>(execute_pending_runs(stop_token));
            }
            if (vmci_channel_) {
                vmci_channel_->synchronize_outbound();
            }
            vmci_channel_available = true;
        } catch (const std::exception& error) {
            if (stop_token.stop_requested()) {
                break;
            }
            ++vmci_channel_failure_count_;
            ++consecutive_vmci_channel_failures_;
            last_vmci_channel_error_ = bounded_runtime_error(error.what());
            last_vmci_channel_error_at_ = utc_timestamp();
            std::cerr << "SatsumaVM VMCI channel unavailable: " << error.what() << '\n';
        }
        if (vmci_channel_available && consecutive_vmci_channel_failures_ != 0) {
            consecutive_vmci_channel_failures_ = 0;
            last_vmci_channel_recovered_at_ = utc_timestamp();
        }
        const int delay_ms = vmci_channel_available
            ? config_.poll_interval_ms
            : config_.reconnect_interval_ms;
        if (wait_for_stop(stop_token, std::chrono::milliseconds(delay_ms))) {
            break;
        }
    }
}

void Agent::write_presence() const {
    nlohmann::json runtime = {
        {"started_at", session_started_at_},
        {"vmci_channel_failure_count", vmci_channel_failure_count_},
        {"consecutive_vmci_channel_failures", consecutive_vmci_channel_failures_},
    }; // 无事件时不发布无效的空时间戳
    if (!last_vmci_channel_error_.empty()) {
        runtime["last_vmci_channel_error"] = last_vmci_channel_error_;
    }
    if (!last_vmci_channel_error_at_.empty()) {
        runtime["last_vmci_channel_error_at"] = last_vmci_channel_error_at_;
    }
    if (!last_vmci_channel_recovered_at_.empty()) {
        runtime["last_vmci_channel_recovered_at"] = last_vmci_channel_recovered_at_;
    }
    const nlohmann::json presence = {
        {"schema_version", 2},
        {"protocol_version", config_.protocol_version},
        {"lab_id", config_.lab_id},
        {"vm_id", config_.vm_id},
        {"hardware_id", config_.hardware_id},
        {"agent_version", config_.agent_version},
        {"update_id", config_.last_update_id},
        {"session_id", session_id_},
        {"boot_id", boot_id_},
        {"process_id", GetCurrentProcessId()},
        {"binary_sha256", binary_sha256_},
        {"status", config_.identity_unbound ? "unbound" : "idle"},
        {"inventory", {
            {"observed_at", inventory_.observed_at()},
            {"sha256", inventory_.digest()},
        }},
        {"updated_at", utc_timestamp()},
        {"runtime", std::move(runtime)},
    };
    write_json_atomic(hardware_presence_path(config_), presence);
    if (!config_.hardware_id.empty()) {
        write_json_atomic(
            config_.mirror_root / L"agents" / L"sessions" /
                path_from_utf8(config_.hardware_id) /
                path_from_utf8(session_id_ + ".json"),
            presence);
    }
    if (config_.vm_id_configured && config_.vm_id != config_.hardware_id) {
        write_json_atomic(
            config_.mirror_root / L"agents" / path_from_utf8(config_.vm_id + ".json"),
            presence);
    }
    write_hardware_migration_marker(config_);
}

void Agent::deploy_artifacts(
    const std::filesystem::path& run_directory,
    const std::filesystem::path& local_run_directory,
    const RunManifest& manifest,
    const std::stop_token stop_token,
    const InteractiveUserSession* interactive_session) const {
    for (const auto& artifact : manifest.artifacts) {
        throw_if_stop_requested(stop_token);
        if (artifact.vm != config_.vm_id) {
            continue;
        }

        const std::filesystem::path artifact_file = resolve_under_root(run_directory, artifact.path);
        if (!std::filesystem::is_regular_file(artifact_file) ||
            std::filesystem::file_size(artifact_file) > kMaxArtifactBytes ||
            sha256_file(artifact_file) != artifact.sha256) {
            throw Error("Artifact is missing or has an invalid hash: " + path_to_utf8(artifact.path));
        }

        const std::filesystem::path local_file = interactive_session != nullptr
            ? interactive_session->deploy_file(artifact_file, artifact.path)
            : resolve_under_root(local_run_directory, artifact.path);
        if (interactive_session == nullptr) {
            std::filesystem::create_directories(local_file.parent_path());
            std::filesystem::copy_file(
                artifact_file,
                local_file,
                std::filesystem::copy_options::overwrite_existing);
        }
        if (sha256_file(local_file) != artifact.sha256) {
            throw Error("Local Artifact hash mismatch after copy: " + path_to_utf8(artifact.path));
        }
    }
}

void Agent::execute_step_payload(
    const std::filesystem::path& run_directory,
    const RunManifest& manifest,
    const TaskStep& step,
    const StepClaimLease& claim,
    const std::stop_token stop_token,
    ExecutionWorkspace& workspace) {
    throw_if_stop_requested(stop_token);
    const auto start_time = std::chrono::steady_clock::now();
    std::filesystem::create_directories(workspace.local_job_directory);
    if (step.type == "echo") {
        write_text(workspace.stdout_local, step.message + "\n");
        write_text(workspace.stderr_local, "");
        workspace.result.exit_code = 0;
        workspace.result.duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start_time).count();
        workspace.result.status = "exited";
        return;
    }

    const std::filesystem::path& executable_artifact = step.type == "script"
        ? step.script
        : step.program;
    if (find_artifact(manifest, config_.vm_id, executable_artifact) == nullptr) {
        throw Error(
            "Executable file is not a registered Artifact: " +
            path_to_utf8(executable_artifact));
    }

    std::optional<InteractiveUserSession> interactive_session;
    if (step.run_as == TaskRunAs::InteractiveUser) {
        interactive_session.emplace(InteractiveUserSession::acquire(
            config_.lab_id,
            manifest.run_id,
            config_.local_work_root,
            config_.vm_id));
        workspace.local_run_directory = interactive_session->working_directory();
        workspace.local_job_directory = resolve_under_root(
            workspace.local_run_directory,
            std::filesystem::path(L".satsuma") / L"jobs" / path_from_utf8(claim.job_id));
        std::filesystem::create_directories(workspace.local_job_directory);
        workspace.stdout_local = workspace.local_job_directory / L"stdout.log.partial";
        workspace.stderr_local = workspace.local_job_directory / L"stderr.log.partial";
        workspace.result.interactive_session_id = interactive_session->session_id();
    } else {
        std::filesystem::create_directories(workspace.local_run_directory);
    }
    deploy_artifacts(
        run_directory,
        workspace.local_run_directory,
        manifest,
        stop_token,
        interactive_session ? &*interactive_session : nullptr);

    ProcessRequest request;
    if (step.type == "script") {
        inventory_.synchronize();
        request.program = inventory_.script_engine_path(script_engine_name(step.engine));
        const std::string script_path = path_to_utf8(
            resolve_under_root(workspace.local_run_directory, step.script));
        if (step.engine == ScriptEngine::Cmd) {
            std::string command = "\"" + escape_cmd_token(script_path);
            for (const std::string& argument : step.arguments) {
                command += " " + escape_cmd_token(argument);
            }
            command.push_back('"');
            request.arguments = {"/D", "/Q", "/V:OFF", "/S", "/C", command};
            request.verbatim_arguments = true;
            request.environment_overrides["SATSUMA_CMD_PERCENT"] = "%";
        } else if (step.engine == ScriptEngine::WindowsPowerShell) {
            request.arguments = {
                "-NoLogo", "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass",
                "-File", script_path,
            };
        } else {
            request.arguments = {
                "-NoLogo", "-NoProfile", "-NonInteractive", "-File", script_path,
            };
        }
        if (step.engine != ScriptEngine::Cmd) {
            request.arguments.insert(
                request.arguments.end(),
                step.arguments.begin(),
                step.arguments.end());
        }
    } else {
        request.program = resolve_under_root(workspace.local_run_directory, step.program);
        request.arguments = step.arguments;
    }
    request.working_directory = workspace.local_run_directory;
    request.stdout_path = workspace.stdout_local;
    request.stderr_path = workspace.stderr_local;
    request.timeout = std::chrono::seconds(step.timeout_seconds);
    request.max_output_bytes = kDefaultMaxOutputBytes;
    request.stop_token = stop_token;
    const ProcessResult process_result = interactive_session
        ? interactive_session->run(helper_executable_, request)
        : runner_.run(request);
    throw_if_stop_requested(stop_token);
    workspace.result.exit_code = process_result.exit_code;
    workspace.result.duration_ms = process_result.duration_ms;
    if (process_result.output_limit_exceeded) {
        throw Error("Process output exceeded the 64 MiB limit");
    }
    workspace.result.status = process_result.timed_out ? "timed_out" : "exited";
    if (process_result.timed_out) {
        workspace.result.error =
            "Process Job tree exceeded the step timeout of " +
            std::to_string(step.timeout_seconds) + " seconds";
    }

    std::uintmax_t collected_total_bytes = 0;
    for (const auto& collect_file : step.collect_files) {
        throw_if_stop_requested(stop_token);
        const std::filesystem::path source = resolve_under_root(
            workspace.local_run_directory,
            collect_file);
        if (!std::filesystem::is_regular_file(source)) {
            throw Error("Declared result file does not exist: " + path_to_utf8(collect_file));
        }
        const std::uintmax_t collected_size = std::filesystem::file_size(source);
        if (collected_size > kMaxCollectedFileBytes ||
            collected_total_bytes > kMaxCollectedTotalBytes - collected_size) {
            throw Error("Declared result files exceed the collection size limit");
        }
        collected_total_bytes += collected_size;
        const std::filesystem::path staged_destination = resolve_under_root(
            workspace.job_directory,
            std::filesystem::path(L"files") / collect_file);
        const std::filesystem::path canonical_destination = resolve_under_root(
            workspace.result_directory,
            std::filesystem::path(L"files") / collect_file);
        std::filesystem::create_directories(staged_destination.parent_path());
        std::filesystem::create_directories(canonical_destination.parent_path());
        std::filesystem::copy_file(
            source,
            staged_destination,
            std::filesystem::copy_options::overwrite_existing);
        workspace.result.files.push_back({
            path_to_utf8(std::filesystem::relative(canonical_destination, run_directory)),
            sha256_file(staged_destination),
        });
        workspace.evidence_files.push_back({staged_destination, canonical_destination});
    }
}

void Agent::publish_step_execution(
    const std::filesystem::path& run_directory,
    const std::filesystem::path& claim_path,
    const StepClaimLease& claim,
    const std::stop_token lease_loss_token,
    ClaimRenewalSession& renewal_session,
    ExecutionWorkspace& workspace) {
    stage_local_log(
        workspace.stdout_local,
        workspace.stdout_staged,
        workspace.result);
    stage_local_log(
        workspace.stderr_local,
        workspace.stderr_staged,
        workspace.result);
    workspace.evidence_files.insert(
        workspace.evidence_files.begin(),
        {
            {workspace.stdout_staged, workspace.stdout_final},
            {workspace.stderr_staged, workspace.stderr_final},
        });
    workspace.result.finished_at = utc_timestamp();

    if (lease_loss_token.stop_requested()) {
        renewal_session.finish();
        write_stale_result_best_effort(
            workspace.job_directory,
            workspace.result,
            "ownership_lost",
            renewal_session.loss_reason());
        return;
    }

    const StepResultPublishStatus publish_status = runtime_options_.result_publish_operation
        ? runtime_options_.result_publish_operation(
            claim_path,
            claim,
            workspace.result_directory / L"execution.json",
            workspace.result,
            workspace.evidence_files)
        : publish_step_result_if_owned(
            claim_path,
            claim,
            workspace.result_directory / L"execution.json",
            workspace.result,
            workspace.evidence_files);
    renewal_session.finish();
    if (publish_status != StepResultPublishStatus::Published) {
        const std::string claim_status = publish_status == StepResultPublishStatus::LeaseExpired
            ? "lease_expired"
            : "ownership_lost";
        write_stale_result_best_effort(
            workspace.job_directory,
            workspace.result,
            claim_status,
            renewal_session.loss_reason());
        return;
    }
    if (vmci_channel_) {
        write_text(workspace.result_directory / L".vmci-complete", "completed\n");
    }

    std::error_code cleanup_error;
    std::filesystem::remove_all(workspace.job_directory, cleanup_error);
    std::filesystem::remove_all(workspace.local_job_directory, cleanup_error);
    write_state(run_directory, "idle", "");
}

void Agent::execute_step(
    const std::filesystem::path& run_directory,
    const RunManifest& manifest,
    const TaskStep& step,
    const std::filesystem::path& claim_path,
    const StepClaimLease& claim,
    const std::stop_token parent_stop_token) {
    ClaimRenewalSession renewal_session(
        claim_path,
        claim,
        runtime_options_.claim_lease_policy,
        runtime_options_.claim_renew_operation);
    std::stop_source execution_stop_source; // 合并 Service 停止和 claim 失权
    const std::stop_token lease_loss_token = renewal_session.lease_loss_token();
    std::stop_callback parent_stop_callback(
        parent_stop_token,
        [&execution_stop_source] { execution_stop_source.request_stop(); });
    std::stop_callback lease_loss_callback(
        lease_loss_token,
        [&execution_stop_source] { execution_stop_source.request_stop(); });
    const std::stop_token execution_stop_token = execution_stop_source.get_token();
    const std::filesystem::path cancellation_path = run_directory / L"cancel.json";
    const CancellationCheck cancellation_check = runtime_options_.cancellation_check;
    std::jthread cancellation_monitor(
        [&execution_stop_source, cancellation_path, run_id = manifest.run_id,
         cancellation_check](
            const std::stop_token monitor_stop) {
            while (!monitor_stop.stop_requested()) {
                try {
                    if (cancellation_check && cancellation_check(cancellation_path, run_id)) {
                        execution_stop_source.request_stop();
                        return;
                    }
                    if (!cancellation_check &&
                        std::filesystem::is_regular_file(cancellation_path)) {
                        const nlohmann::json cancellation = load_json(cancellation_path);
                        if (cancellation.value("schema_version", 0) == 1 &&
                            cancellation.value("run_id", std::string{}) == run_id) {
                            execution_stop_source.request_stop();
                            return;
                        }
                    }
                } catch (...) {
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        });

    ExecutionWorkspace workspace;
    workspace.result_directory = resolve_under_root(
        run_directory,
        std::filesystem::path(L"results") / path_from_utf8(config_.vm_id) / path_from_utf8(step.id));
    std::filesystem::create_directories(workspace.result_directory);

    workspace.job_directory = resolve_under_root(
        workspace.result_directory,
        std::filesystem::path(L".jobs") / path_from_utf8(claim.job_id));
    std::filesystem::create_directories(workspace.job_directory);

    workspace.stdout_staged = workspace.job_directory / L"stdout.log";
    workspace.stderr_staged = workspace.job_directory / L"stderr.log";
    workspace.stdout_final = workspace.result_directory / L"stdout.log";
    workspace.stderr_final = workspace.result_directory / L"stderr.log";
    workspace.local_run_directory = resolve_local_run_directory(config_, manifest.run_id);
    workspace.local_job_directory = resolve_under_root(
        workspace.local_run_directory,
        std::filesystem::path(L".satsuma") / L"jobs" / path_from_utf8(claim.job_id));
    workspace.stdout_local = workspace.local_job_directory / L"stdout.log.partial";
    workspace.stderr_local = workspace.local_job_directory / L"stderr.log.partial";

    workspace.result.run_id = manifest.run_id;
    workspace.result.vm_id = config_.vm_id;
    workspace.result.job_id = claim.job_id;
    workspace.result.step_id = step.id;
    workspace.result.run_as = step.run_as;
    workspace.result.started_at = utc_timestamp();
    workspace.result.stdout_path = path_to_utf8(
        std::filesystem::relative(workspace.stdout_final, run_directory));
    workspace.result.stderr_path = path_to_utf8(
        std::filesystem::relative(workspace.stderr_final, run_directory));
    write_state(run_directory, "running", claim.job_id);

    try {
        execute_step_payload(
            run_directory,
            manifest,
            step,
            claim,
            execution_stop_token,
            workspace);
    } catch (const std::exception& error) {
        if (std::filesystem::is_regular_file(cancellation_path)) {
            workspace.result.status = "failed";
            workspace.result.error = "Run cancellation requested";
        } else if (workspace.result.status == "timed_out") {
            workspace.result.error += "; secondary error: ";
            workspace.result.error += error.what();
        } else {
            workspace.result.status = "failed";
            workspace.result.error = error.what();
        }
    }

    publish_step_execution(
        run_directory,
        claim_path,
        claim,
        lease_loss_token,
        renewal_session,
        workspace);
}

void Agent::write_state(
    const std::filesystem::path& run_directory,
    const std::string& status,
    const std::string& job_id) const {
    const nlohmann::json state = {
        {"schema_version", 1},
        {"protocol_version", config_.protocol_version},
        {"lab_id", config_.lab_id},
        {"vm_id", config_.vm_id},
        {"agent_version", config_.agent_version},
        {"update_id", config_.last_update_id},
        {"session_id", session_id_},
        {"boot_id", boot_id_},
        {"status", status},
        {"job_id", job_id},
        {"updated_at", utc_timestamp()},
    };
    write_json_atomic(
        run_directory / L"state" / path_from_utf8(config_.vm_id + "-agent.json"),
        state);
}

}  // namespace satsuma::vm
