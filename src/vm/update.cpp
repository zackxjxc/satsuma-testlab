// SatsumaVM 独立更新通道和助手模式实现。
#include "update.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <windows.h>

#include <nlohmann/json.hpp>

#include "service.hpp"
#include "hardware_identity.hpp"
#include "satsuma/core/errors.hpp"
#include "satsuma/core/id.hpp"
#include "satsuma/core/json_io.hpp"
#include "satsuma/core/path.hpp"
#include "satsuma/core/sha256.hpp"
#include "satsuma/core/update.hpp"
#include "satsuma/core/version.hpp"
#include "satsuma/core/windows_command_line.hpp"

namespace satsuma::vm {
namespace {

constexpr std::chrono::seconds kPresenceTimeout{120}; // 两段 presence 窗口不超过 Host 默认总预算
constexpr DWORD kUpdateHelperCreationFlags =
    CREATE_NO_WINDOW | CREATE_BREAKAWAY_FROM_JOB;

// 自动释放普通 Win32 HANDLE。
class UniqueHandle {
public:
    // 接管一个可空 HANDLE。
    explicit UniqueHandle(HANDLE value = nullptr) : value_(value) {}

    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;

    // 关闭有效 HANDLE。
    ~UniqueHandle() {
        if (value_ != nullptr && value_ != INVALID_HANDLE_VALUE) {
            CloseHandle(value_);
        }
    }

    // 返回底层 HANDLE。
    [[nodiscard]] HANDLE get() const noexcept {
        return value_;
    }

    // 返回 HANDLE 是否有效。
    [[nodiscard]] explicit operator bool() const noexcept {
        return value_ != nullptr && value_ != INVALID_HANDLE_VALUE;
    }

private:
    HANDLE value_ = nullptr;  // 被管理的 HANDLE
};

// 更新事务使用的完整路径集合。
struct UpdatePaths {
    std::filesystem::path update_directory; // 共享更新目录
    std::filesystem::path manifest;         // 本机清单副本
    std::filesystem::path result;           // 共享终态结果
    std::filesystem::path config;           // 正式 Agent 配置
    std::filesystem::path config_backup;    // 本次配置备份
    std::filesystem::path formal_binary;    // 正式 SatsumaVM.exe
    std::filesystem::path new_binary;       // SatsumaVM.new.exe
    std::filesystem::path backup_binary;    // SatsumaVM.bak.exe
    std::filesystem::path state;            // 本机更新阶段状态
};

// 更新事务注入的 Service 和 presence 操作。
struct UpdateOperations {
    std::function<void()> stop_service;  // 停止当前 Service
    std::function<std::uint32_t()> start_service; // 启动正式 Service
    std::function<void(
        std::uint32_t,
        const std::string&,
        const std::string&)> wait_presence; // 等待指定版本上线
};

// 返回当前进程对应的可执行文件绝对路径。
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

// 使用固定安装布局构造本机更新路径。
[[nodiscard]] UpdatePaths make_update_paths(
    const AgentConfig& config,
    const std::filesystem::path& update_directory) {
    const std::filesystem::path storage_root = config.storage_root.empty()
        ? config.local_work_root.parent_path()
        : config.storage_root;
    const std::filesystem::path install_root =
        config.legacy_storage_layout || config.storage_root.empty()
        ? std::filesystem::absolute(storage_root)
        : std::filesystem::absolute(storage_root) / L"agent";
    const std::filesystem::path bin_root = install_root / L"bin";
    return {
        update_directory,
        install_root / L"update-manifest.json",
        update_directory / L"result.json",
        install_root / L"agent.json",
        install_root / L"agent.json.update.bak",
        bin_root / L"SatsumaVM.exe",
        bin_root / L"SatsumaVM.new.exe",
        bin_root / L"SatsumaVM.bak.exe",
        install_root / L"update-state.json",
    };
}

// 原子记录本机更新阶段。
void write_update_state(
    const UpdatePaths& paths,
    const AgentUpdateManifest& manifest,
    const std::string& phase,
    const std::string& error = {}) {
    nlohmann::json state = {
        {"schema_version", 1},
        {"vm_id", manifest.vm_id},
        {"update_id", manifest.update_id},
        {"version", manifest.version},
        {"phase", phase},
        {"error", error},
        {"updated_at", utc_timestamp()},
    };
    if (manifest.next_vm_id.has_value()) {
        state["next_vm_id"] = *manifest.next_vm_id;
    }
    write_json_atomic(paths.state, state);
}

// 返回用于失败路径的终态结果。
[[nodiscard]] AgentUpdateResult make_failed_result(
    const AgentUpdateManifest& manifest,
    const std::string& error,
    const std::string& rollback_status = "none") {
    return {
        1,
        manifest.update_id,
        manifest.vm_id,
        manifest.version,
        "failed",
        rollback_status,
        0,
        error,
        utc_timestamp(),
    };
}

// 原子写回更新终态。
void write_update_result(
    const std::filesystem::path& path,
    const AgentUpdateResult& result) {
    write_json_atomic_existing_parent(path, result);
}

// 返回 helper 在共享目录中预写但尚未发布的成功结果路径。
[[nodiscard]] std::filesystem::path pending_success_result_path(
    const UpdatePaths& paths) {
    return paths.update_directory / L".result.pending.json";
}

// 验证候选大小和 SHA-256。
void verify_update_binary(
    const std::filesystem::path& path,
    const AgentUpdateManifest& manifest) {
    if (!std::filesystem::is_regular_file(path)) {
        throw Error("Agent update binary is missing: " + path_to_utf8(path));
    }
    if (std::filesystem::file_size(path) != manifest.size) {
        throw Error("Agent update binary size does not match its manifest");
    }
    if (sha256_file(path) != manifest.sha256) {
        throw Error("Agent update binary SHA-256 does not match its manifest");
    }
}

// 删除一个本次更新文件，失败时不误报成功。
void remove_update_file(const std::filesystem::path& path) {
    std::error_code error;
    const bool removed = std::filesystem::remove(path, error);
    if (error || (!removed && std::filesystem::exists(path))) {
        throw Error(
            "Cannot remove update file " + path_to_utf8(path) +
            (error ? ": " + error.message() : ""));
    }
}

// 尽力删除 Agent 稍后可重试清理的文件。
void remove_update_file_best_effort(const std::filesystem::path& path) noexcept {
    std::error_code error;
    std::filesystem::remove(path, error);
}

// 停服后移除该 Service 拥有的旧 presence，避免 HGFS 拒绝新进程覆盖旧文件。
void remove_service_presence(const AgentConfig& config) {
    const std::filesystem::path canonical = hardware_presence_path(config);
    remove_update_file(canonical);
    const std::filesystem::path legacy =
        config.shared_root / L"agents" / path_from_utf8(config.vm_id + ".json");
    if (legacy != canonical) {
        remove_update_file(legacy);
    }
}

// 返回 presence 是否证明指定版本已经从目标 Service PID 上线。
[[nodiscard]] bool presence_matches(
    const std::filesystem::path& presence_path,
    const AgentConfig& config,
    const std::uint32_t process_id,
    const std::string& version,
    const std::string& update_id) {
    try {
        const nlohmann::json presence = load_json(presence_path);
        return presence.value("schema_version", 0) == 1 &&
               presence.value("protocol_version", 0) == config.protocol_version &&
               presence.value("lab_id", std::string{}) == config.lab_id &&
               presence.value("vm_id", std::string{}) == config.vm_id &&
               (config.hardware_id.empty() ||
                presence.value("hardware_id", std::string{}) == config.hardware_id) &&
               presence.value("agent_version", std::string{}) == version &&
               presence.value("update_id", std::string{}) == update_id &&
               presence.value("process_id", std::uint32_t{}) == process_id &&
               presence.value("status", std::string{}) == "idle" &&
               !presence.value("session_id", std::string{}).empty() &&
               !presence.value("boot_id", std::string{}).empty() &&
               !presence.value("updated_at", std::string{}).empty();
    } catch (...) {
        return false;
    }
}

// 有限等待新旧 Agent presence。
void wait_for_presence(
    const AgentConfig& config,
    const std::uint32_t process_id,
    const std::string& version,
    const std::string& update_id) {
    const std::filesystem::path presence_path = hardware_presence_path(config);
    const auto deadline = std::chrono::steady_clock::now() + kPresenceTimeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (presence_matches(
                presence_path,
                config,
                process_id,
                version,
                update_id)) {
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    throw Error("Timed out while waiting for matching Agent update presence");
}

// 验证本机配置仍是清单声明的来源身份，且迁移目标没有 presence 冲突。
[[nodiscard]] AgentConfig validate_update_source(
    const UpdatePaths& paths,
    const AgentUpdateManifest& manifest) {
    const AgentConfig config = load_runtime_agent_config(paths.config);
    if (config.lab_id != manifest.lab_id || config.vm_id != manifest.vm_id) {
        throw Error("Agent update config does not match the manifest source identity");
    }
    if (manifest.next_vm_id.has_value()) {
        const std::filesystem::path target_presence =
            config.shared_root / L"agents" /
            path_from_utf8(*manifest.next_vm_id + ".json");
        if (std::filesystem::exists(target_presence)) {
            throw Error(
                "Agent rebind target presence already exists: " +
                path_to_utf8(target_presence));
        }
    }
    return config;
}

// 原子写入更新后的 agent.json。
void write_updated_config(
    const UpdatePaths& paths,
    const AgentUpdateManifest& manifest) {
    nlohmann::json config = load_json(paths.config);
    config["protocol_version"] = kRunManifestProtocolVersion;
    config["agent_version"] = manifest.version;
    config["last_update_id"] = manifest.update_id;
    if (manifest.next_vm_id.has_value()) {
        config["vm_id"] = *manifest.next_vm_id;
    }
    write_json_atomic(paths.config, config);
}

// 从事务开始时读取的原始内容恢复 agent.json。
void restore_config(
    const UpdatePaths& paths,
    const nlohmann::json& original) {
    write_json_atomic(paths.config, original);
}

// 回滚迁移时只删除由本次候选 PID 发布的目标 presence。
void remove_rebind_presence_best_effort(
    const UpdatePaths& paths,
    const AgentUpdateManifest& manifest,
    const std::uint32_t process_id) noexcept {
    if (!manifest.next_vm_id.has_value() || process_id == 0) {
        return;
    }
    try {
        const AgentConfig config = load_runtime_agent_config(paths.config);
        if (config.vm_id != *manifest.next_vm_id) {
            return;
        }
        const std::filesystem::path presence_path = hardware_presence_path(config);
        if (presence_matches(
                presence_path,
                config,
                process_id,
                manifest.version,
                manifest.update_id)) {
            remove_update_file_best_effort(presence_path);
        }
        const std::filesystem::path legacy_presence_path =
            config.shared_root / L"agents" / path_from_utf8(config.vm_id + ".json");
        AgentConfig legacy_config = config;
        legacy_config.hardware_id.clear();
        if (legacy_presence_path != presence_path && presence_matches(
                legacy_presence_path,
                legacy_config,
                process_id,
                manifest.version,
                manifest.update_id)) {
            remove_update_file_best_effort(legacy_presence_path);
        }
    } catch (...) {
    }
}

// 删除 EXE 备份并完成不可逆的二进制提交。
void commit_binary_update(const UpdatePaths& paths) {
    if (std::filesystem::exists(paths.new_binary)) {
        throw Error("SatsumaVM.new.exe still exists after a successful update");
    }
    const std::filesystem::path partial =
        paths.new_binary.parent_path() / L"SatsumaVM.new.exe.partial";
    if (std::filesystem::exists(partial)) {
        throw Error("SatsumaVM.new.exe.partial still exists after a successful update");
    }

    // 备份删除失败时仍处于可回滚阶段，配置和状态证据尚未清理。
    remove_update_file(paths.backup_binary);
}

// 返回候选已经上线且 EXE 备份已经删除。
[[nodiscard]] bool binary_update_committed(const UpdatePaths& paths) {
    const std::filesystem::path partial =
        paths.new_binary.parent_path() / L"SatsumaVM.new.exe.partial";
    return std::filesystem::is_regular_file(paths.formal_binary) &&
           !std::filesystem::exists(paths.new_binary) &&
           !std::filesystem::exists(partial) &&
           !std::filesystem::exists(paths.backup_binary);
}

// 在二进制提交后清理其余本机暂存，state 最后删除。
void cleanup_committed_update(const UpdatePaths& paths) {
    remove_update_file(paths.config_backup);
    remove_update_file(paths.manifest);
    remove_update_file(paths.state);
}

// 确认成功补偿前所有本机更新产物均已删除。
[[nodiscard]] bool local_success_cleanup_complete(const UpdatePaths& paths) {
    return binary_update_committed(paths) &&
           !std::filesystem::exists(paths.config_backup) &&
           !std::filesystem::exists(paths.manifest) &&
           !std::filesystem::exists(paths.state);
}

// 仅通过同目录 rename 发布预写结果，绝不重建已被 Host 删除的目录。
[[nodiscard]] bool finalize_pending_success_result(
    const UpdatePaths& paths,
    const AgentUpdateManifest& manifest) {
    const std::filesystem::path pending = pending_success_result_path(paths);
    if (!std::filesystem::is_regular_file(pending)) {
        return false;
    }
    const AgentUpdateResult result = load_agent_update_result(pending);
    if (result.status != "succeeded" ||
        result.rollback_status != "none" ||
        result.update_id != manifest.update_id ||
        result.vm_id != manifest.vm_id ||
        result.version != manifest.version) {
        throw Error("Pending Agent update success result does not match its manifest");
    }

    std::error_code rename_error;
    std::filesystem::rename(pending, paths.result, rename_error);
    if (!rename_error) {
        return true;
    }
    if (std::filesystem::is_regular_file(paths.result)) {
        const AgentUpdateResult published = load_agent_update_result(paths.result);
        if (published.status == "succeeded" &&
            published.update_id == manifest.update_id &&
            published.vm_id == manifest.vm_id &&
            published.version == manifest.version) {
            remove_update_file_best_effort(pending);
            return true;
        }
    }
    if (!std::filesystem::exists(paths.update_directory)) {
        return true;
    }
    throw Error(
        "Cannot publish pending Agent update result: " + rename_error.message());
}

// 仅在本机提交已经完整清理后补发成功终态。
[[nodiscard]] bool recover_committed_update_success(
    const UpdatePaths& paths,
    const AgentUpdateManifest& manifest) {
    if (!binary_update_committed(paths)) {
        return false;
    }
    cleanup_committed_update(paths);
    if (!local_success_cleanup_complete(paths)) {
        throw Error("Committed Agent update retained local staging files");
    }
    return finalize_pending_success_result(paths, manifest);
}

// 身份切换后的新 Agent 从来源更新目录补发已提交的成功结果。
[[nodiscard]] bool recover_rebound_update_success(const AgentConfig& config) {
    if (config.last_update_id.empty()) {
        return false;
    }
    const std::filesystem::path updates_root = config.shared_root / L"updates";
    if (!std::filesystem::exists(updates_root)) {
        return false;
    }

    for (const auto& source_entry : std::filesystem::directory_iterator(updates_root)) {
        if (!source_entry.is_directory() ||
            source_entry.path().filename().native().starts_with(L".")) {
            continue;
        }
        const std::filesystem::path update_directory =
            source_entry.path() / path_from_utf8(config.last_update_id);
        const std::filesystem::path manifest_path = update_directory / L"update.json";
        if (!std::filesystem::is_regular_file(manifest_path)) {
            continue;
        }

        const AgentUpdateManifest manifest =
            load_agent_update_manifest(manifest_path);
        if (!manifest.next_vm_id.has_value() ||
            *manifest.next_vm_id != config.vm_id ||
            manifest.lab_id != config.lab_id ||
            manifest.version != config.agent_version ||
            manifest.update_id != config.last_update_id ||
            source_entry.path().filename() != path_from_utf8(manifest.vm_id)) {
            continue;
        }
        const UpdatePaths paths = make_update_paths(config, update_directory);
        return recover_committed_update_success(paths, manifest);
    }
    return false;
}

// 执行停止、改名、启动、presence 和简单恢复状态机。
[[nodiscard]] AgentUpdateResult run_update_transaction(
    const UpdatePaths& paths,
    const AgentUpdateManifest& manifest,
    const std::string& candidate_version,
    const UpdateOperations& operations) {
    bool stop_attempted = false;
    bool formal_backed_up = false;
    bool candidate_is_formal = false;
    bool config_updated = false;
    bool update_committed = false;
    std::uint32_t candidate_process_id = 0;
    nlohmann::json original_config;
    AgentUpdateResult committed_result;
    std::string failure;
    try {
        static_cast<void>(
            nlohmann::json(manifest).get<AgentUpdateManifest>());
        if (candidate_version != manifest.version) {
            throw Error(
                "Agent update candidate version " + candidate_version +
                " does not match manifest version " + manifest.version);
        }
        verify_update_binary(paths.new_binary, manifest);
        const AgentConfig source_config = validate_update_source(paths, manifest);
        if (!std::filesystem::is_regular_file(paths.formal_binary) ||
            std::filesystem::file_size(paths.formal_binary) == 0) {
            throw Error("Current formal SatsumaVM.exe cannot be used as an update backup");
        }
        write_update_state(paths, manifest, "validated");

        remove_update_file_best_effort(paths.config_backup);
        original_config = load_json(paths.config);
        std::filesystem::copy_file(
            paths.config,
            paths.config_backup,
            std::filesystem::copy_options::overwrite_existing);
        stop_attempted = true;
        operations.stop_service();
        remove_service_presence(source_config);
        write_update_state(paths, manifest, "service_stopped");

        remove_update_file_best_effort(paths.backup_binary);
        std::filesystem::rename(paths.formal_binary, paths.backup_binary);
        formal_backed_up = true;
        try {
            std::filesystem::rename(paths.new_binary, paths.formal_binary);
            candidate_is_formal = true;
        } catch (...) {
            std::filesystem::rename(paths.backup_binary, paths.formal_binary);
            formal_backed_up = false;
            throw;
        }
        write_update_state(paths, manifest, "files_switched");

        write_updated_config(paths, manifest);
        config_updated = true;
        write_update_state(paths, manifest, "config_updated");
        candidate_process_id = operations.start_service();
        operations.wait_presence(
            candidate_process_id,
            manifest.version,
            manifest.update_id);
        write_update_state(paths, manifest, "presence_verified");

        committed_result = AgentUpdateResult{
            1,
            manifest.update_id,
            manifest.vm_id,
            manifest.version,
            "succeeded",
            "none",
            candidate_process_id,
            "",
            utc_timestamp(),
        };
        // 先预写可补发结果，再删除备份形成不可逆提交；提交后只允许补偿收尾。
        write_update_result(
            pending_success_result_path(paths),
            committed_result);
        commit_binary_update(paths);
        update_committed = true;
        cleanup_committed_update(paths);
        static_cast<void>(finalize_pending_success_result(paths, manifest));
        return committed_result;
    } catch (const std::exception& error) {
        failure = error.what();
    } catch (...) {
        failure = "unknown Agent update failure";
    }

    if (update_committed) {
        // 新版已稳定上线；新 Service 将重试暂存清理和结果原子发布。
        return committed_result;
    }

    bool rollback_required =
        stop_attempted || formal_backed_up || candidate_is_formal || config_updated;
    remove_update_file_best_effort(pending_success_result_path(paths));
    if (!rollback_required) {
        const AgentUpdateResult result = make_failed_result(manifest, failure);
        write_update_state(paths, manifest, "failed", failure);
        write_update_result(paths.result, result);
        return result;
    }

    try {
        if (candidate_is_formal) {
            try {
                operations.stop_service();
            } catch (...) {
            }
            remove_service_presence(load_runtime_agent_config(paths.config));
            remove_update_file_best_effort(paths.new_binary);
            std::filesystem::rename(paths.formal_binary, paths.new_binary);
            candidate_is_formal = false;
        }
        if (formal_backed_up) {
            remove_update_file_best_effort(paths.formal_binary);
            std::filesystem::rename(paths.backup_binary, paths.formal_binary);
            formal_backed_up = false;
        }
        remove_rebind_presence_best_effort(
            paths,
            manifest,
            candidate_process_id);
        if (config_updated || std::filesystem::is_regular_file(paths.config_backup)) {
            restore_config(paths, original_config);
            config_updated = false;
        }

        const AgentConfig old_config = load_runtime_agent_config(paths.config);
        const std::uint32_t old_process_id = operations.start_service();
        operations.wait_presence(
            old_process_id,
            old_config.agent_version,
            old_config.last_update_id);
        const AgentUpdateResult result =
            make_failed_result(manifest, failure, "succeeded");
        write_update_state(paths, manifest, "rolled_back", failure);
        write_update_result(paths.result, result);
        return result;
    } catch (const std::exception& rollback_error) {
        failure += "; rollback failed: ";
        failure += rollback_error.what();
    } catch (...) {
        failure += "; rollback failed: unknown error";
    }

    const AgentUpdateResult result =
        make_failed_result(manifest, failure, "failed");
    write_update_state(paths, manifest, "rollback_failed", failure);
    write_update_result(paths.result, result);
    return result;
}

// 将共享候选复制到固定本机 new 路径并在停服前校验。
void stage_update_candidate(
    const UpdatePaths& paths,
    const AgentUpdateManifest& manifest) {
    const std::filesystem::path source = resolve_under_root(
        paths.update_directory,
        manifest.binary);
    const std::filesystem::path partial =
        paths.new_binary.parent_path() / L"SatsumaVM.new.exe.partial";
    remove_update_file_best_effort(partial);
    remove_update_file_best_effort(paths.new_binary);
    try {
        std::filesystem::copy_file(
            source,
            partial,
            std::filesystem::copy_options::none);
        verify_update_binary(partial, manifest);
        std::filesystem::rename(partial, paths.new_binary);
        write_json_atomic(paths.manifest, manifest);
        write_update_state(paths, manifest, "staged");
    } catch (...) {
        remove_update_file_best_effort(partial);
        remove_update_file_best_effort(paths.new_binary);
        remove_update_file_best_effort(paths.manifest);
        remove_update_file_best_effort(paths.state);
        throw;
    }
}

// 启动不属于 Agent Job Object 的独立更新助手。
[[nodiscard]] UniqueHandle launch_update_helper(const UpdatePaths& paths) {
    const std::vector<std::string> arguments = {
        "--config",
        path_to_utf8(paths.config),
        "--apply-update",
        path_to_utf8(paths.manifest),
    };
    std::vector<wchar_t> command = build_windows_command_line(
        paths.new_binary,
        arguments);
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(
            paths.new_binary.c_str(),
            command.data(),
            nullptr,
            nullptr,
            FALSE,
            kUpdateHelperCreationFlags,
            nullptr,
            paths.new_binary.parent_path().c_str(),
            &startup,
            &process)) {
        throw Error(
            "CreateProcessW(update helper) failed with Win32 error " +
            std::to_string(GetLastError()));
    }
    CloseHandle(process.hThread);
    return UniqueHandle(process.hProcess);
}

// 清理已写回失败结果的本机暂存。
void cleanup_finished_update(const UpdatePaths& paths) noexcept {
    remove_update_file_best_effort(paths.new_binary);
    remove_update_file_best_effort(
        paths.new_binary.parent_path() / L"SatsumaVM.new.exe.partial");
    remove_update_file_best_effort(paths.config_backup);
    remove_update_file_best_effort(paths.manifest);
    remove_update_file_best_effort(paths.state);
    remove_update_file_best_effort(pending_success_result_path(paths));
}

// 旧版本已重新运行但回滚确认曾超时，清理不会再影响正式文件的残留状态。
[[nodiscard]] bool recover_verified_failed_rollback(
    const UpdatePaths& paths,
    const AgentUpdateManifest& manifest,
    const std::filesystem::path& running_executable,
    const std::string& running_version) {
    if (!std::filesystem::is_regular_file(paths.result) ||
        !std::filesystem::is_regular_file(paths.state) ||
        std::filesystem::exists(paths.backup_binary)) {
        return false;
    }
    const AgentUpdateResult result = load_agent_update_result(paths.result);
    if (result.status != "failed" ||
        result.rollback_status != "failed" ||
        result.update_id != manifest.update_id ||
        result.vm_id != manifest.vm_id ||
        result.version != manifest.version) {
        return false;
    }
    const nlohmann::json state = load_json(paths.state);
    if (state.value("update_id", std::string{}) != manifest.update_id) {
        return false;
    }
    const AgentConfig config = load_runtime_agent_config(paths.config);
    if (config.lab_id != manifest.lab_id ||
        config.vm_id != manifest.vm_id ||
        config.agent_version != running_version ||
        config.last_update_id == manifest.update_id ||
        !std::filesystem::is_regular_file(paths.formal_binary)) {
        return false;
    }
    std::error_code equivalent_error;
    if (!std::filesystem::equivalent(
            running_executable,
            paths.formal_binary,
            equivalent_error) ||
        equivalent_error) {
        return false;
    }
    cleanup_finished_update(paths);
    return local_success_cleanup_complete(paths);
}

}  // namespace

bool process_pending_agent_update(
    const AgentConfig& config,
    const std::stop_token stop_token) {
    if (recover_rebound_update_success(config)) {
        return false;
    }
    const std::filesystem::path updates_root =
        config.shared_root / L"updates" / path_from_utf8(config.vm_id);
    if (!std::filesystem::exists(updates_root)) {
        return false;
    }

    std::vector<std::filesystem::path> update_directories;
    for (const auto& entry : std::filesystem::directory_iterator(updates_root)) {
        if (entry.is_directory() &&
            !entry.path().filename().native().starts_with(L".")) {
            update_directories.push_back(entry.path());
        }
    }
    std::sort(update_directories.begin(), update_directories.end());
    for (const std::filesystem::path& update_directory : update_directories) {
        if (stop_token.stop_requested()) {
            return true;
        }

        AgentUpdateManifest manifest;
        try {
            manifest = load_agent_update_manifest(update_directory / L"update.json");
            if (manifest.lab_id != config.lab_id || manifest.vm_id != config.vm_id) {
                throw Error("Agent update manifest does not target this Agent");
            }
        } catch (const std::exception& error) {
            AgentUpdateManifest invalid;
            invalid.lab_id = config.lab_id;
            invalid.vm_id = config.vm_id;
            invalid.update_id = path_to_utf8(update_directory.filename());
            invalid.version = "unknown";
            invalid.binary = L"SatsumaVM.exe";
            invalid.size = 1;
            invalid.sha256 = std::string(64, '0');
            invalid.created_at = utc_timestamp();
            write_update_result(
                update_directory / L"result.json",
                make_failed_result(invalid, error.what()));
            continue;
        }

        const UpdatePaths paths = make_update_paths(config, update_directory);
        if (std::filesystem::is_regular_file(paths.result)) {
            const AgentUpdateResult result = load_agent_update_result(paths.result);
            if (result.status == "failed" && result.rollback_status != "failed") {
                cleanup_finished_update(paths);
            } else if (result.status == "failed" &&
                       recover_verified_failed_rollback(
                           paths,
                           manifest,
                           current_executable_path(),
                           std::string(kVersion))) {
                // 回滚后的正式 Agent 已确认恢复，继续扫描后续更新目录。
            }
            continue;
        }
        if (std::filesystem::is_regular_file(paths.state)) {
            const nlohmann::json state = load_json(paths.state);
            if (state.value("update_id", std::string{}) == manifest.update_id) {
                if (config.agent_version == manifest.version &&
                    config.last_update_id == manifest.update_id) {
                    static_cast<void>(
                        recover_committed_update_success(paths, manifest));
                }
                continue;
            }
            throw Error("Another Agent update is already using the local staging files");
        }
        if (config.agent_version == manifest.version &&
            config.last_update_id == manifest.update_id) {
            static_cast<void>(recover_committed_update_success(paths, manifest));
            continue;
        }

        try {
            stage_update_candidate(paths, manifest);
            const UniqueHandle helper = launch_update_helper(paths);
            for (;;) {
                if (stop_token.stop_requested()) {
                    return true;
                }
                const DWORD wait_result = WaitForSingleObject(helper.get(), 100);
                if (wait_result == WAIT_OBJECT_0) {
                    if (std::filesystem::is_regular_file(paths.result)) {
                        const AgentUpdateResult result =
                            load_agent_update_result(paths.result);
                        if (result.rollback_status != "failed") {
                            cleanup_finished_update(paths);
                        }
                    }
                    break;
                }
                if (wait_result != WAIT_TIMEOUT) {
                    throw Error(
                        "WaitForSingleObject(update helper) failed with Win32 error " +
                        std::to_string(GetLastError()));
                }
            }
        } catch (const std::exception& error) {
            cleanup_finished_update(paths);
            write_update_result(
                paths.result,
                make_failed_result(manifest, error.what()));
        }
        return false;
    }
    return false;
}

int apply_agent_update_helper(
    const std::filesystem::path& config_path,
    const std::filesystem::path& manifest_path) {
    try {
        const AgentConfig config = load_runtime_agent_config(config_path);
        const AgentUpdateManifest manifest =
            load_agent_update_manifest(manifest_path);
        if (manifest.lab_id != config.lab_id || manifest.vm_id != config.vm_id) {
            throw Error("Agent update helper manifest does not target this Agent");
        }
        const std::filesystem::path update_directory =
            config.shared_root / L"updates" /
                path_from_utf8(config.vm_id) /
                path_from_utf8(manifest.update_id);
        UpdatePaths paths = make_update_paths(config, update_directory);
        paths.config = std::filesystem::canonical(config_path);
        paths.manifest = std::filesystem::canonical(manifest_path);
        const std::filesystem::path current =
            std::filesystem::canonical(current_executable_path());
        const std::filesystem::path expected_new =
            std::filesystem::canonical(paths.new_binary);
        if (_wcsicmp(current.c_str(), expected_new.c_str()) != 0) {
            throw Error("Agent update helper must run from SatsumaVM.new.exe");
        }

        const UpdateOperations operations{
            [&paths] {
                const AgentServiceStopResult result = stop_owned_agent_service(
                    paths.formal_binary,
                    paths.config);
                if (!result.existed) {
                    throw Error("SatsumaVM service does not exist");
                }
            },
            [&paths] {
                return start_owned_agent_service(
                    paths.formal_binary,
                    paths.config);
            },
            [&paths](
                const std::uint32_t process_id,
                const std::string& version,
                const std::string& update_id) {
                const AgentConfig current_config = load_runtime_agent_config(paths.config);
                wait_for_presence(
                    current_config,
                    process_id,
                    version,
                    update_id);
            },
        };
        const AgentUpdateResult result = run_update_transaction(
            paths,
            manifest,
            std::string(kVersion),
            operations);
        return result.status == "succeeded" ? 0 : 1;
    } catch (const std::exception& error) {
        try {
            const AgentUpdateManifest manifest =
                load_agent_update_manifest(manifest_path);
            const AgentConfig config = load_runtime_agent_config(config_path);
            const std::string effective_vm_id =
                manifest.next_vm_id.value_or(manifest.vm_id);
            if (config.lab_id != manifest.lab_id ||
                (config.vm_id != manifest.vm_id &&
                 config.vm_id != effective_vm_id)) {
                throw Error("Agent update helper cannot resolve its source update directory");
            }
            const std::filesystem::path update_directory =
                config.shared_root / L"updates" /
                    path_from_utf8(manifest.vm_id) /
                    path_from_utf8(manifest.update_id);
            write_update_result(
                update_directory / L"result.json",
                make_failed_result(manifest, error.what()));
        } catch (...) {
        }
        return 1;
    }
}

#ifdef SATSUMA_UPDATE_TESTS
AgentUpdateResult apply_agent_update_for_test(
    const AgentUpdatePaths& paths,
    const AgentUpdateManifest& manifest,
    const std::string& candidate_version,
    const AgentUpdateOperations& operations) {
    const UpdatePaths internal_paths{
        paths.update_directory,
        paths.manifest,
        paths.result,
        paths.config,
        paths.config_backup,
        paths.formal_binary,
        paths.new_binary,
        paths.backup_binary,
        paths.state,
    };
    const UpdateOperations internal_operations{
        operations.stop_service,
        operations.start_service,
        operations.wait_presence,
    };
    return run_update_transaction(
        internal_paths,
        manifest,
        candidate_version,
        internal_operations);
}

bool agent_update_presence_matches_for_test(
    const std::filesystem::path& presence_path,
    const AgentConfig& config,
    const std::uint32_t process_id,
    const std::string& version,
    const std::string& update_id) {
    return presence_matches(
        presence_path,
        config,
        process_id,
        version,
        update_id);
}

bool recover_committed_update_success_for_test(
    const AgentUpdatePaths& paths,
    const AgentUpdateManifest& manifest) {
    const UpdatePaths internal_paths{
        paths.update_directory,
        paths.manifest,
        paths.result,
        paths.config,
        paths.config_backup,
        paths.formal_binary,
        paths.new_binary,
        paths.backup_binary,
        paths.state,
    };
    return recover_committed_update_success(internal_paths, manifest);
}

bool recover_rebound_update_success_for_test(const AgentConfig& config) {
    return recover_rebound_update_success(config);
}

std::uint32_t agent_update_helper_creation_flags_for_test() {
    return kUpdateHelperCreationFlags;
}

bool recover_verified_failed_rollback_for_test(
    const AgentUpdatePaths& paths,
    const AgentUpdateManifest& manifest,
    const std::filesystem::path& running_executable,
    const std::string& running_version) {
    const UpdatePaths internal_paths{
        paths.update_directory,
        paths.manifest,
        paths.result,
        paths.config,
        paths.config_backup,
        paths.formal_binary,
        paths.new_binary,
        paths.backup_binary,
        paths.state,
    };
    return recover_verified_failed_rollback(
        internal_paths,
        manifest,
        running_executable,
        running_version);
}
#endif

}  // namespace satsuma::vm
