// VM Agent 任务领取、执行和结果落盘实现。
#include "agent.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <iostream>
#include <limits>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>
#include <windows.h>

#include "satsuma/core/errors.hpp"
#include "satsuma/core/claim.hpp"
#include "satsuma/core/id.hpp"
#include "satsuma/core/json_io.hpp"
#include "satsuma/core/path.hpp"
#include "rpc_client.hpp"
#include "satsuma/core/sha256.hpp"
#include "interactive_process.hpp"
#include "update.hpp"

namespace satsuma::vm {
namespace {

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

// 在可取消操作边界统一转换停止请求。
void throw_if_stop_requested(const std::stop_token stop_token) {
    if (stop_token.stop_requested()) {
        throw Error("Agent stop requested");
    }
}

// 独占创建 claim 文件；已存在表示步骤已由其他会话领取。
[[nodiscard]] bool create_claim(const std::filesystem::path& path, const nlohmann::json& value) {
    std::filesystem::create_directories(path.parent_path());
    const std::string payload = value.dump(2) + "\n";
    HANDLE file = CreateFileW(
        path.c_str(),
        GENERIC_WRITE,
        FILE_SHARE_READ,
        nullptr,
        CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH,
        nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        if (GetLastError() == ERROR_FILE_EXISTS) {
            return false;
        }
        throw Error("Cannot create claim file (Win32 error " + std::to_string(GetLastError()) + ")");
    }

    DWORD bytes_written = 0;
    const BOOL write_ok = WriteFile(
        file,
        payload.data(),
        static_cast<DWORD>(payload.size()),
        &bytes_written,
        nullptr);
    const BOOL flush_ok = write_ok ? FlushFileBuffers(file) : FALSE;
    const DWORD error = (!write_ok || !flush_ok) ? GetLastError() : ERROR_SUCCESS;
    CloseHandle(file);
    if (!write_ok || !flush_ok || bytes_written != payload.size()) {
        DeleteFileW(path.c_str());
        throw Error("Cannot persist claim file (Win32 error " + std::to_string(error) + ")");
    }
    return true;
}

// 原子归档过期 claim；并发下只有一个 Agent 能成功取得回收权。
[[nodiscard]] bool archive_expired_claim(
    const std::filesystem::path& path,
    const std::uint32_t attempt) {
    std::filesystem::path archived = path;
    archived += path_from_utf8(".expired-attempt-" + std::to_string(attempt) + "-" + make_id("claim"));
    if (MoveFileExW(path.c_str(), archived.c_str(), MOVEFILE_WRITE_THROUGH)) {
        return true;
    }
    const DWORD error = GetLastError();
    if (error == ERROR_FILE_NOT_FOUND || error == ERROR_ALREADY_EXISTS) {
        return false;
    }
    throw Error("Cannot archive expired claim file (Win32 error " + std::to_string(error) + ")");
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

// 将 partial 日志原子发布为最终日志。
void publish_log(const std::filesystem::path& partial, const std::filesystem::path& final) {
    if (!std::filesystem::exists(partial)) {
        write_text(partial, "");
    }
    if (!MoveFileExW(partial.c_str(), final.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        throw Error("Cannot publish process log (Win32 error " + std::to_string(GetLastError()) + ")");
    }
}

// 返回与程序路径完全匹配的 Artifact 登记项。
[[nodiscard]] const ArtifactManifest* find_program_artifact(
    const RunManifest& manifest,
    const std::string& vm_id,
    const std::filesystem::path& program) {
    const auto match = std::find_if(
        manifest.artifacts.begin(),
        manifest.artifacts.end(),
        [&vm_id, &program](const ArtifactManifest& artifact) {
            return artifact.vm == vm_id && artifact.path == program;
        });
    return match == manifest.artifacts.end() ? nullptr : &*match;
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

}  // namespace

Agent::Agent(
    AgentConfig config,
    std::filesystem::path helper_executable)
    : config_(std::move(config)),
      session_id_(make_id("session")),
      boot_id_(make_id("boot")),
      helper_executable_(helper_executable.empty()
          ? current_executable_path()
          : std::filesystem::absolute(std::move(helper_executable))) {
    if (config_.protocol_version != kRunManifestProtocolVersion) {
        throw Error("Agent execution requires file protocol version 2");
    }
}

int Agent::run_once(const std::stop_token stop_token) {
    if (stop_token.stop_requested()) {
        return 0;
    }
    const std::filesystem::path runs_root = config_.shared_root / L"runs";
    std::filesystem::create_directories(runs_root);

    std::vector<std::filesystem::path> run_directories;
    for (const auto& entry : std::filesystem::directory_iterator(runs_root)) {
        if (!entry.is_directory() || entry.path().filename().native().starts_with(L".")) {
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
        const RunManifest manifest = load_run_manifest(run_directory / L"task.json");
        if (manifest.lab_id != config_.lab_id || manifest.protocol_version != config_.protocol_version) {
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
            if (std::filesystem::is_regular_file(result_directory / L"execution.json")) {
                continue;
            }

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
            std::uint32_t attempt = 1;  // 首次领取从 1 开始
            if (std::filesystem::is_regular_file(claim_path)) {
                StepClaimLease existing;
                try {
                    existing = load_step_claim_lease(claim_path);
                } catch (const std::exception& error) {
                    write_json_atomic(recovery_path, {
                        {"schema_version", 1},
                        {"status", "manual_intervention_required"},
                        {"reason", "claim cannot be parsed as a version 2 lease"},
                        {"error", error.what()},
                        {"observed_at", utc_timestamp()},
                    });
                    continue;
                }

                const ClaimRecoveryDecision decision = evaluate_claim_recovery(
                    existing,
                    unix_time_ms());
                if (decision == ClaimRecoveryDecision::Wait) {
                    continue;
                }
                if (decision == ClaimRecoveryDecision::ManualInterventionRequired) {
                    write_json_atomic(recovery_path, {
                        {"schema_version", 1},
                        {"status", "manual_intervention_required"},
                        {"reason", "expired claim belongs to an unsafe step"},
                        {"claim", existing},
                        {"current_boot_id", boot_id_},
                        {"observed_at", utc_timestamp()},
                    });
                    continue;
                }
                if (!archive_expired_claim(claim_path, existing.attempt)) {
                    continue;
                }
                if (existing.attempt == std::numeric_limits<std::uint32_t>::max()) {
                    throw Error("Step claim attempt limit has been reached");
                }
                attempt = existing.attempt + 1;
            }

            constexpr std::int64_t claim_grace_ms = 30'000;
            const std::int64_t lease_duration_ms =
                static_cast<std::int64_t>(step.timeout_seconds) * 1'000 + claim_grace_ms;
            const StepClaimLease claim = make_step_claim_lease(
                manifest.run_id,
                config_.vm_id,
                step.id,
                job_id,
                session_id_,
                boot_id_,
                unix_time_ms(),
                lease_duration_ms,
                step.retry_safe,
                attempt);
            if (!create_claim(claim_path, claim)) {
                continue;
            }
            std::error_code marker_error;
            std::filesystem::remove(recovery_path, marker_error);

            execute_step(run_directory, manifest, step, job_id, stop_token);
            ++executed_steps;
        }
    }
    return executed_steps;
}

void Agent::run_watch(const std::stop_token stop_token) {
    while (!stop_token.stop_requested()) {
        bool file_channel_available = false;
        try {
            write_presence();
            if (process_pending_agent_update(config_, stop_token)) {
                break;
            }
            static_cast<void>(run_once(stop_token));
            file_channel_available = true;
        } catch (const std::exception& error) {
            if (stop_token.stop_requested()) {
                break;
            }
            std::cerr << "SatsumaVM file channel unavailable: " << error.what() << '\n';
        }
        const int delay_ms = file_channel_available
            ? config_.poll_interval_ms
            : config_.reconnect_interval_ms;
        if (wait_for_stop(stop_token, std::chrono::milliseconds(delay_ms))) {
            break;
        }
    }
}

void Agent::write_presence() const {
    const nlohmann::json presence = {
        {"schema_version", 1},
        {"protocol_version", config_.protocol_version},
        {"lab_id", config_.lab_id},
        {"vm_id", config_.vm_id},
        {"agent_version", config_.agent_version},
        {"update_id", config_.last_update_id},
        {"session_id", session_id_},
        {"boot_id", boot_id_},
        {"process_id", GetCurrentProcessId()},
        {"status", "idle"},
        {"updated_at", utc_timestamp()},
    };
    write_json_atomic(
        config_.shared_root / L"agents" / path_from_utf8(config_.vm_id + ".json"),
        presence);
}

bool Agent::synchronize_rpc() {
    RpcClient client(config_, session_id_, boot_id_);
    if (!client.connected()) {
        static_cast<void>(client.connect());
    }

    const HostDirective directive = client.heartbeat("idle", "");
    if (directive.action == "stop") {
        throw Error("Host stopped the Agent session: " + directive.message);
    }
    if (directive.action != "poll") {
        return false;
    }

    const TaskReference task = client.poll_task();
    if (task.type == "error") {
        throw Error("Host task polling failed: " + task.manifest);
    }
    if (task.has_task) {
        validate_identifier(task.run_id, "run_id");
        validate_relative_path(path_from_utf8(task.manifest));
    }
    return task.has_task;
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

        const std::filesystem::path shared_file = resolve_under_root(run_directory, artifact.path);
        if (!std::filesystem::is_regular_file(shared_file) || sha256_file(shared_file) != artifact.sha256) {
            throw Error("Artifact is missing or has an invalid hash: " + path_to_utf8(artifact.path));
        }

        const std::filesystem::path local_file = interactive_session != nullptr
            ? interactive_session->deploy_file(shared_file, artifact.path)
            : resolve_under_root(local_run_directory, artifact.path);
        if (interactive_session == nullptr) {
            std::filesystem::create_directories(local_file.parent_path());
            std::filesystem::copy_file(
                shared_file,
                local_file,
                std::filesystem::copy_options::overwrite_existing);
        }
        if (sha256_file(local_file) != artifact.sha256) {
            throw Error("Local Artifact hash mismatch after copy: " + path_to_utf8(artifact.path));
        }
    }
}

void Agent::execute_step(
    const std::filesystem::path& run_directory,
    const RunManifest& manifest,
    const TaskStep& step,
    const std::string& job_id,
    const std::stop_token stop_token) {
    const std::filesystem::path result_directory = resolve_under_root(
        run_directory,
        std::filesystem::path(L"results") / path_from_utf8(config_.vm_id) / path_from_utf8(step.id));
    std::filesystem::create_directories(result_directory);

    const std::filesystem::path stdout_partial = result_directory / L"stdout.log.partial";
    const std::filesystem::path stderr_partial = result_directory / L"stderr.log.partial";
    const std::filesystem::path stdout_final = result_directory / L"stdout.log";
    const std::filesystem::path stderr_final = result_directory / L"stderr.log";

    ExecutionResult result;
    result.run_id = manifest.run_id;
    result.vm_id = config_.vm_id;
    result.job_id = job_id;
    result.step_id = step.id;
    result.run_as = step.run_as;
    result.started_at = utc_timestamp();
    result.stdout_path = path_to_utf8(std::filesystem::relative(stdout_final, run_directory));
    result.stderr_path = path_to_utf8(std::filesystem::relative(stderr_final, run_directory));
    write_state(run_directory, "running", job_id);

    try {
        throw_if_stop_requested(stop_token);
        const auto start_time = std::chrono::steady_clock::now();
        if (step.type == "echo") {
            write_text(stdout_partial, step.message + "\n");
            write_text(stderr_partial, "");
            result.exit_code = 0;
            result.duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start_time).count();
        } else {
            if (find_program_artifact(manifest, config_.vm_id, step.program) == nullptr) {
                throw Error("Execute program is not a registered Artifact: " + path_to_utf8(step.program));
            }

            std::optional<InteractiveUserSession> interactive_session;
            std::filesystem::path local_run_directory;
            if (step.run_as == TaskRunAs::InteractiveUser) {
                interactive_session.emplace(
                    InteractiveUserSession::acquire(
                        config_.lab_id,
                        manifest.run_id));
                local_run_directory = interactive_session->working_directory();
                result.interactive_session_id = interactive_session->session_id();
            } else {
                local_run_directory = resolve_under_root(
                    config_.local_work_root,
                    path_from_utf8(manifest.run_id));
                std::filesystem::create_directories(local_run_directory);
            }
            deploy_artifacts(
                run_directory,
                local_run_directory,
                manifest,
                stop_token,
                interactive_session ? &*interactive_session : nullptr);

            ProcessRequest request;
            request.program = resolve_under_root(local_run_directory, step.program);
            request.arguments = step.arguments;
            request.working_directory = local_run_directory;
            request.stdout_path = stdout_partial;
            request.stderr_path = stderr_partial;
            request.timeout = std::chrono::seconds(step.timeout_seconds);
            request.stop_token = stop_token;
            const ProcessResult process_result = interactive_session
                ? interactive_session->run(helper_executable_, request)
                : runner_.run(request);
            result.exit_code = process_result.exit_code;
            result.timed_out = process_result.timed_out;
            result.duration_ms = process_result.duration_ms;

            for (const auto& collect_file : step.collect_files) {
                throw_if_stop_requested(stop_token);
                const std::filesystem::path source = resolve_under_root(local_run_directory, collect_file);
                if (!std::filesystem::is_regular_file(source)) {
                    throw Error("Declared result file does not exist: " + path_to_utf8(collect_file));
                }
                const std::filesystem::path destination = resolve_under_root(
                    result_directory,
                    std::filesystem::path(L"files") / collect_file);
                std::filesystem::create_directories(destination.parent_path());
                std::filesystem::copy_file(source, destination, std::filesystem::copy_options::overwrite_existing);
                result.files.push_back({
                    path_to_utf8(std::filesystem::relative(destination, run_directory)),
                    sha256_file(destination),
                });
            }
        }

        result.status = result.timed_out ? "timed_out" : "exited";
    } catch (const std::exception& error) {
        result.status = "failed";
        result.error = error.what();
    }

    try {
        publish_log(stdout_partial, stdout_final);
        publish_log(stderr_partial, stderr_final);
    } catch (const std::exception& error) {
        result.status = "failed";
        if (!result.error.empty()) {
            result.error += "; ";
        }
        result.error += error.what();
    }
    result.finished_at = utc_timestamp();
    write_json_atomic(result_directory / L"execution.json", result);
    write_state(run_directory, "idle", "");
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
