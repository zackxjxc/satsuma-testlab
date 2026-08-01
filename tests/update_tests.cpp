// SatsumaVM 自更新文件切换、恢复和 presence 测试。
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>

#include <windows.h>

#include <nlohmann/json.hpp>

#include "satsuma/core/config.hpp"
#include "satsuma/core/id.hpp"
#include "satsuma/core/json_io.hpp"
#include "satsuma/core/path.hpp"
#include "satsuma/core/sha256.hpp"
#include "satsuma/core/update.hpp"
#include "update.hpp"

namespace {

// 在条件不满足时终止当前测试。
void expect(const bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

// 读取测试二进制占位内容。
[[nodiscard]] std::string read_text(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>(),
    };
}

// 写入测试二进制占位内容。
void write_text(const std::filesystem::path& path, const std::string& value) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary);
    output << value;
    if (!output) {
        throw std::runtime_error("cannot write update test file");
    }
}

// 单个更新状态机测试使用的文件集合。
struct UpdateFixture {
    std::filesystem::path root;              // 本次测试根目录
    satsuma::AgentUpdateManifest manifest;   // 合法候选清单
    satsuma::vm::AgentUpdatePaths paths;     // 状态机文件路径
};

// 创建旧正式文件、新候选、配置和本机 manifest。
[[nodiscard]] UpdateFixture make_fixture(
    const std::filesystem::path& parent,
    const std::string& name) {
    UpdateFixture fixture;
    fixture.root = parent / satsuma::path_from_utf8(name);
    const std::filesystem::path install_root = fixture.root / L"install";
    const std::filesystem::path bin_root = install_root / L"bin";
    const std::filesystem::path shared_root = fixture.root / L"shared";
    const std::filesystem::path update_directory =
        shared_root / L"updates" / L"vm_01" / L"update_001";
    std::filesystem::create_directories(update_directory);
    std::filesystem::create_directories(install_root / L"work");
    write_text(bin_root / L"SatsumaVM.exe", "old-binary");
    write_text(bin_root / L"SatsumaVM.new.exe", "new-binary");

    const std::filesystem::path config_path = install_root / L"agent.json";
    satsuma::write_json_atomic(config_path, {
        {"schema_version", 1},
        {"protocol_version", 1},
        {"lab_id", "test_lab"},
        {"vm_id", "vm_01"},
        {"agent_version", "0.1.0"},
        {"shared_root", satsuma::path_to_utf8(shared_root)},
        {"local_work_root", satsuma::path_to_utf8(install_root / L"work")},
        {"poll_interval_ms", 100},
        {"reconnect_interval_ms", 100},
    });

    fixture.manifest.lab_id = "test_lab";
    fixture.manifest.vm_id = "vm_01";
    fixture.manifest.update_id = "update_001";
    fixture.manifest.version = "0.1.1";
    fixture.manifest.binary = L"SatsumaVM.exe";
    fixture.manifest.size = std::filesystem::file_size(
        bin_root / L"SatsumaVM.new.exe");
    fixture.manifest.sha256 = satsuma::sha256_file(
        bin_root / L"SatsumaVM.new.exe");
    fixture.manifest.created_at = "2026-07-27T00:00:00.000Z";

    fixture.paths = {
        update_directory,
        install_root / L"update-manifest.json",
        update_directory / L"result.json",
        config_path,
        install_root / L"agent.json.update.bak",
        bin_root / L"SatsumaVM.exe",
        bin_root / L"SatsumaVM.new.exe",
        bin_root / L"SatsumaVM.bak.exe",
        install_root / L"update-state.json",
    };
    satsuma::write_json_atomic(fixture.paths.manifest, fixture.manifest);
    return fixture;
}

// 将普通更新测试夹具切换为 vm_01 到 vm_02 的身份迁移。
void enable_rebind(UpdateFixture& fixture) {
    fixture.manifest.protocol_version = 2;
    fixture.manifest.next_vm_id = "vm_02";
    satsuma::write_json_atomic(fixture.paths.manifest, fixture.manifest);
}

// 验证成功更新严格清理 new、bak、manifest 和状态文件。
void test_successful_update(const std::filesystem::path& root) {
    UpdateFixture fixture = make_fixture(root, "success");
    const satsuma::AgentConfig original_config =
        satsuma::load_agent_config(fixture.paths.config);
    const std::filesystem::path canonical_presence =
        original_config.shared_root / L"agents" / L"vm_01.json";
    satsuma::write_json_atomic(canonical_presence, {{"status", "idle"}});
    int stop_calls = 0;
    int start_calls = 0;
    int presence_calls = 0;
    const satsuma::vm::AgentUpdateOperations operations{
        [&stop_calls] { ++stop_calls; },
        [&start_calls, &canonical_presence] {
            expect(!std::filesystem::exists(canonical_presence),
                "candidate Service started before stale presence cleanup");
            ++start_calls;
            return std::uint32_t{4321};
        },
        [&presence_calls](
            const std::uint32_t process_id,
            const std::string& version,
            const std::string& update_id) {
            ++presence_calls;
            expect(process_id == 4321, "success presence used the wrong PID");
            expect(version == "0.1.1", "success presence used the wrong version");
            expect(update_id == "update_001", "success presence used the wrong update ID");
        },
    };
    const satsuma::AgentUpdateResult result =
        satsuma::vm::apply_agent_update_for_test(
            fixture.paths,
            fixture.manifest,
            "0.1.1",
            operations);
    expect(result.status == "succeeded", "valid update did not succeed");
    expect(stop_calls == 1 && start_calls == 1 && presence_calls == 1,
        "successful update operation count changed");
    expect(read_text(fixture.paths.formal_binary) == "new-binary",
        "successful update did not publish the candidate");
    for (const std::filesystem::path& path : {
             fixture.paths.new_binary,
             fixture.paths.backup_binary,
             fixture.paths.config_backup,
             fixture.paths.manifest,
             fixture.paths.state}) {
        expect(!std::filesystem::exists(path),
            "successful update retained a staging or backup file");
    }
    const nlohmann::json config = satsuma::load_json(fixture.paths.config);
    expect(config.at("protocol_version") == satsuma::kRunManifestProtocolVersion &&
           config.at("agent_version") == "0.1.1",
        "successful update did not persist the current protocol and version");
    expect(config.at("last_update_id") == "update_001",
        "successful update did not persist the update ID");
    expect(satsuma::load_agent_update_result(fixture.paths.result).process_id == 4321,
        "successful result did not persist the Service PID");
}

// 验证身份迁移在新 Service 上线前原子写入目标 vm_id。
void test_successful_rebind(const std::filesystem::path& root) {
    UpdateFixture fixture = make_fixture(root, "rebind-success");
    enable_rebind(fixture);
    int stop_calls = 0;
    int start_calls = 0;
    int presence_calls = 0;
    const satsuma::vm::AgentUpdateOperations operations{
        [&stop_calls] { ++stop_calls; },
        [&start_calls] {
            ++start_calls;
            return std::uint32_t{4401};
        },
        [&fixture, &presence_calls](
            const std::uint32_t process_id,
            const std::string& version,
            const std::string& update_id) {
            ++presence_calls;
            const satsuma::AgentConfig config =
                satsuma::load_agent_config(fixture.paths.config);
            expect(config.vm_id == "vm_02", "rebind presence used the source identity");
            expect(process_id == 4401 && version == "0.1.1" && update_id == "update_001",
                "rebind presence used the wrong update identity");
        },
    };

    const satsuma::AgentUpdateResult result =
        satsuma::vm::apply_agent_update_for_test(
            fixture.paths,
            fixture.manifest,
            "0.1.1",
            operations);
    expect(result.status == "succeeded" && result.vm_id == "vm_01",
        "valid rebind did not report success through its source identity");
    expect(stop_calls == 1 && start_calls == 1 && presence_calls == 1,
        "successful rebind operation count changed");
    const satsuma::AgentConfig config =
        satsuma::load_agent_config(fixture.paths.config);
    expect(config.vm_id == "vm_02" &&
           config.protocol_version == satsuma::kRunManifestProtocolVersion &&
           config.agent_version == "0.1.1" &&
           config.last_update_id == "update_001",
        "successful rebind did not persist the target identity");
}

// 验证迁移目标已有 presence 时不会停止当前 Service。
void test_rebind_target_collision(const std::filesystem::path& root) {
    UpdateFixture fixture = make_fixture(root, "rebind-collision");
    enable_rebind(fixture);
    const satsuma::AgentConfig config =
        satsuma::load_agent_config(fixture.paths.config);
    satsuma::write_json_atomic(
        config.shared_root / L"agents" / L"vm_02.json",
        {{"vm_id", "vm_02"}});
    bool service_called = false;
    const satsuma::vm::AgentUpdateOperations operations{
        [&service_called] { service_called = true; },
        [&service_called] {
            service_called = true;
            return std::uint32_t{1};
        },
        [&service_called](std::uint32_t, const std::string&, const std::string&) {
            service_called = true;
        },
    };

    const satsuma::AgentUpdateResult result =
        satsuma::vm::apply_agent_update_for_test(
            fixture.paths,
            fixture.manifest,
            "0.1.1",
            operations);
    expect(result.status == "failed" && result.rollback_status == "none",
        "rebind target collision entered the update transaction");
    expect(!service_called, "rebind target collision touched the Service");
    expect(satsuma::load_agent_config(fixture.paths.config).vm_id == "vm_01",
        "rebind target collision changed the source identity");
}

// 验证目标 presence 失败会恢复来源配置并重新确认旧 Service。
void test_rebind_presence_failure_rollback(const std::filesystem::path& root) {
    UpdateFixture fixture = make_fixture(root, "rebind-presence-failure");
    enable_rebind(fixture);
    int stop_calls = 0;
    int start_calls = 0;
    int presence_calls = 0;
    const satsuma::vm::AgentUpdateOperations operations{
        [&stop_calls] { ++stop_calls; },
        [&start_calls] {
            ++start_calls;
            return static_cast<std::uint32_t>(4500 + start_calls);
        },
        [&fixture, &presence_calls](
            const std::uint32_t process_id,
            const std::string& version,
            const std::string& update_id) {
            ++presence_calls;
            const satsuma::AgentConfig config =
                satsuma::load_agent_config(fixture.paths.config);
            if (presence_calls == 1) {
                expect(config.vm_id == "vm_02", "rebind did not start with target config");
                satsuma::write_json_atomic(
                    config.shared_root / L"agents" / L"vm_02.json",
                    {
                        {"schema_version", 1},
                        {"protocol_version", config.protocol_version},
                        {"lab_id", config.lab_id},
                        {"vm_id", config.vm_id},
                        {"agent_version", version},
                        {"update_id", update_id},
                        {"session_id", "session-rebind-test"},
                        {"boot_id", "boot-rebind-test"},
                        {"process_id", process_id},
                        {"status", "idle"},
                        {"updated_at", "2026-07-27T00:01:00.000Z"},
                    });
                throw std::runtime_error("injected rebind presence timeout");
            }
            expect(config.vm_id == "vm_01" && version == "0.1.0" && update_id.empty(),
                "rebind rollback did not restore the source identity");
        },
    };

    const satsuma::AgentUpdateResult result =
        satsuma::vm::apply_agent_update_for_test(
            fixture.paths,
            fixture.manifest,
            "0.1.1",
            operations);
    expect(result.status == "failed" && result.rollback_status == "succeeded",
        "rebind presence failure did not roll back");
    expect(stop_calls == 2 && start_calls == 2 && presence_calls == 2,
        "rebind rollback operation count changed");
    expect(satsuma::load_agent_config(fixture.paths.config).vm_id == "vm_01",
        "rebind rollback retained the target identity");
    expect(!std::filesystem::exists(
            fixture.root / L"shared" / L"agents" / L"vm_02.json"),
        "rebind rollback retained its candidate target presence");
}

// 验证新身份可在本机清理信息丢失后从来源目录补发成功终态。
void test_rebind_cross_identity_recovery(const std::filesystem::path& root) {
    UpdateFixture fixture = make_fixture(root, "rebind-cross-identity-recovery");
    enable_rebind(fixture);
    satsuma::write_json_atomic(
        fixture.paths.update_directory / L"update.json",
        fixture.manifest);

    nlohmann::json config = satsuma::load_json(fixture.paths.config);
    config["protocol_version"] = satsuma::kRunManifestProtocolVersion;
    config["vm_id"] = "vm_02";
    config["agent_version"] = fixture.manifest.version;
    config["last_update_id"] = fixture.manifest.update_id;
    satsuma::write_json_atomic(fixture.paths.config, config);
    std::filesystem::remove(fixture.paths.new_binary);
    std::filesystem::remove(fixture.paths.manifest);

    satsuma::AgentUpdateResult success;
    success.update_id = fixture.manifest.update_id;
    success.vm_id = fixture.manifest.vm_id;
    success.version = fixture.manifest.version;
    success.status = "succeeded";
    success.rollback_status = "none";
    success.process_id = 4601;
    success.completed_at = "2026-07-27T00:01:00.000Z";
    satsuma::write_json_atomic(
        fixture.paths.update_directory / L".result.pending.json",
        success);

    expect(
        satsuma::vm::recover_rebound_update_success_for_test(
            satsuma::load_agent_config(fixture.paths.config)),
        "rebound Agent did not recover its source update result");
    expect(std::filesystem::is_regular_file(fixture.paths.result),
        "rebound Agent did not publish the recovered result");
    expect(!std::filesystem::exists(
            fixture.paths.update_directory / L".result.pending.json"),
        "rebound Agent retained the pending source result");
}

// 验证提交后的暂存清理失败由新 Agent 重试，不执行残缺回滚。
void test_committed_cleanup_retry(const std::filesystem::path& root) {
    UpdateFixture fixture = make_fixture(root, "cleanup-failure");
    int stop_calls = 0;
    int start_calls = 0;
    int presence_calls = 0;
    const satsuma::vm::AgentUpdateOperations operations{
        [&stop_calls] { ++stop_calls; },
        [&start_calls] {
            ++start_calls;
            return static_cast<std::uint32_t>(6000 + start_calls);
        },
        [&fixture, &presence_calls](
            const std::uint32_t,
            const std::string& version,
            const std::string& update_id) {
            ++presence_calls;
            if (presence_calls == 1) {
                expect(version == "0.1.1" && update_id == "update_001",
                    "cleanup failure did not verify the candidate identity");
                std::filesystem::remove(fixture.paths.config_backup);
                std::filesystem::create_directories(fixture.paths.config_backup);
                write_text(fixture.paths.config_backup / L"blocker", "busy");
                return;
            }
            expect(version == "0.1.0" && update_id.empty(),
                "cleanup failure did not verify the restored identity");
        },
    };

    const satsuma::AgentUpdateResult result =
        satsuma::vm::apply_agent_update_for_test(
            fixture.paths,
            fixture.manifest,
            "0.1.1",
            operations);
    expect(result.status == "succeeded" && result.rollback_status == "none",
        "committed update was incorrectly reported as rolled back");
    expect(stop_calls == 1 && start_calls == 1 && presence_calls == 1,
        "committed cleanup failure unexpectedly touched the Service again");
    expect(read_text(fixture.paths.formal_binary) == "new-binary",
        "committed cleanup failure changed the active executable");
    expect(!std::filesystem::exists(fixture.paths.backup_binary),
        "committed cleanup failure retained SatsumaVM.bak.exe");
    expect(!std::filesystem::exists(fixture.paths.result) &&
           std::filesystem::is_regular_file(
               fixture.paths.update_directory / L".result.pending.json"),
        "committed cleanup failure published success before cleanup completed");
    expect(std::filesystem::is_regular_file(fixture.paths.state),
        "committed cleanup failure removed its retry state");

    std::filesystem::remove_all(fixture.paths.config_backup);
    expect(
        satsuma::vm::recover_committed_update_success_for_test(
            fixture.paths,
            fixture.manifest),
        "new Agent did not finish committed update cleanup");
    const nlohmann::json config = satsuma::load_json(fixture.paths.config);
    expect(config.at("protocol_version") == satsuma::kRunManifestProtocolVersion &&
           config.at("agent_version") == "0.1.1" &&
           config.at("last_update_id") == "update_001",
        "committed cleanup retry changed the updated protocol or config");
    expect(std::filesystem::is_regular_file(fixture.paths.result) &&
           !std::filesystem::exists(fixture.paths.state) &&
           !std::filesystem::exists(fixture.paths.manifest),
        "committed cleanup retry did not finish all staging cleanup");
}

// 验证新版 Agent 不会在 bak 或 state 尚存时提前发布成功。
void test_success_recovery_cleanup_gate(const std::filesystem::path& root) {
    UpdateFixture fixture = make_fixture(root, "success-recovery-gate");
    const std::filesystem::path pending =
        fixture.paths.update_directory / L".result.pending.json";
    satsuma::AgentUpdateResult success;
    success.update_id = fixture.manifest.update_id;
    success.vm_id = fixture.manifest.vm_id;
    success.version = fixture.manifest.version;
    success.status = "succeeded";
    success.rollback_status = "none";
    success.process_id = 7001;
    success.completed_at = "2026-07-27T00:01:00.000Z";
    satsuma::write_json_atomic(pending, success);
    write_text(fixture.paths.backup_binary, "old-binary");
    satsuma::write_json_atomic(fixture.paths.state, {
        {"update_id", fixture.manifest.update_id},
        {"phase", "presence_verified"},
    });
    std::filesystem::remove(fixture.paths.new_binary);
    std::filesystem::remove(fixture.paths.manifest);

    expect(
        !satsuma::vm::recover_committed_update_success_for_test(
            fixture.paths,
            fixture.manifest),
        "success recovery ignored the local state marker");
    std::filesystem::remove(fixture.paths.state);
    expect(
        !satsuma::vm::recover_committed_update_success_for_test(
            fixture.paths,
            fixture.manifest),
        "success recovery ignored SatsumaVM.bak.exe");
    std::filesystem::remove(fixture.paths.backup_binary);
    expect(
        satsuma::vm::recover_committed_update_success_for_test(
            fixture.paths,
            fixture.manifest),
        "success recovery rejected a fully cleaned local update");
    expect(std::filesystem::is_regular_file(fixture.paths.result) &&
           !std::filesystem::exists(pending),
        "success recovery did not atomically publish the pending result");

    std::filesystem::remove_all(fixture.paths.update_directory);
    expect(
        !satsuma::vm::recover_committed_update_success_for_test(
            fixture.paths,
            fixture.manifest),
        "late success recovery unexpectedly republished a removed update");
    expect(!std::filesystem::exists(fixture.paths.update_directory),
        "late success recovery recreated the Host-cleaned update directory");
}

// 验证哈希失败发生在任何 Service 操作之前。
void test_hash_failure(const std::filesystem::path& root) {
    UpdateFixture fixture = make_fixture(root, "hash-failure");
    fixture.manifest.sha256 = std::string(64, '0');
    bool service_called = false;
    const satsuma::vm::AgentUpdateOperations operations{
        [&service_called] { service_called = true; },
        [&service_called] {
            service_called = true;
            return std::uint32_t{1};
        },
        [&service_called](std::uint32_t, const std::string&, const std::string&) {
            service_called = true;
        },
    };
    const satsuma::AgentUpdateResult result =
        satsuma::vm::apply_agent_update_for_test(
            fixture.paths,
            fixture.manifest,
            "0.1.1",
            operations);
    expect(result.status == "failed" && result.rollback_status == "none",
        "hash failure entered a rollback transaction");
    expect(!service_called, "hash failure touched the Service");
    expect(read_text(fixture.paths.formal_binary) == "old-binary",
        "hash failure changed the formal binary");
}

// 验证停服失败保持正式文件并重新确认旧 Agent。
void test_stop_failure(const std::filesystem::path& root) {
    UpdateFixture fixture = make_fixture(root, "stop-failure");
    int start_calls = 0;
    const satsuma::vm::AgentUpdateOperations operations{
        [] { throw std::runtime_error("injected stop failure"); },
        [&start_calls] {
            ++start_calls;
            return std::uint32_t{5001};
        },
        [](const std::uint32_t, const std::string& version, const std::string& update_id) {
            expect(version == "0.1.0" && update_id.empty(),
                "stop failure did not verify the old identity");
        },
    };
    const satsuma::AgentUpdateResult result =
        satsuma::vm::apply_agent_update_for_test(
            fixture.paths,
            fixture.manifest,
            "0.1.1",
            operations);
    expect(result.status == "failed" && result.rollback_status == "succeeded",
        "stop failure did not recover the old Service");
    expect(start_calls == 1, "stop failure did not restart the old Service once");
    expect(read_text(fixture.paths.formal_binary) == "old-binary",
        "stop failure changed the formal binary");
}

// 验证新 Service 启动失败会恢复旧文件和旧配置。
void test_start_failure_rollback(const std::filesystem::path& root) {
    UpdateFixture fixture = make_fixture(root, "start-failure");
    int stop_calls = 0;
    int start_calls = 0;
    const satsuma::vm::AgentUpdateOperations operations{
        [&stop_calls] { ++stop_calls; },
        [&start_calls] {
            ++start_calls;
            if (start_calls == 1) {
                throw std::runtime_error("injected new start failure");
            }
            return std::uint32_t{6002};
        },
        [](const std::uint32_t, const std::string& version, const std::string& update_id) {
            expect(version == "0.1.0" && update_id.empty(),
                "start rollback did not verify the old identity");
        },
    };
    const satsuma::AgentUpdateResult result =
        satsuma::vm::apply_agent_update_for_test(
            fixture.paths,
            fixture.manifest,
            "0.1.1",
            operations);
    expect(result.status == "failed" && result.rollback_status == "succeeded",
        "new Service start failure did not roll back");
    expect(stop_calls == 2 && start_calls == 2,
        "start failure rollback operation count changed");
    expect(read_text(fixture.paths.formal_binary) == "old-binary",
        "start failure did not restore the old executable");
    expect(read_text(fixture.paths.new_binary) == "new-binary",
        "start failure did not move the failed candidate back to new");
    const nlohmann::json config = satsuma::load_json(fixture.paths.config);
    expect(config.at("protocol_version") == satsuma::kLegacyRunManifestProtocolVersion &&
        config.at("agent_version") == "0.1.0" &&
        !config.contains("last_update_id"),
        "start failure did not restore the old protocol and config");
}

// 验证第二次改名失败会立即把 bak 恢复为正式文件。
void test_switch_failure_rollback(const std::filesystem::path& root) {
    UpdateFixture fixture = make_fixture(root, "switch-failure");
    const HANDLE lock = CreateFileW(
        fixture.paths.new_binary.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (lock == INVALID_HANDLE_VALUE) {
        throw std::runtime_error("cannot lock update candidate for switch test");
    }

    int start_calls = 0;
    const satsuma::vm::AgentUpdateOperations operations{
        [] {},
        [&start_calls] {
            ++start_calls;
            return std::uint32_t{6101};
        },
        [](const std::uint32_t, const std::string& version, const std::string& update_id) {
            expect(version == "0.1.0" && update_id.empty(),
                "switch rollback did not verify the old identity");
        },
    };
    const satsuma::AgentUpdateResult result =
        satsuma::vm::apply_agent_update_for_test(
            fixture.paths,
            fixture.manifest,
            "0.1.1",
            operations);
    CloseHandle(lock);
    expect(result.status == "failed" && result.rollback_status == "succeeded",
        "switch failure did not restore the old version");
    expect(start_calls == 1, "switch failure did not restart the old Service");
    expect(read_text(fixture.paths.formal_binary) == "old-binary",
        "switch failure did not restore the formal executable");
    expect(!std::filesystem::exists(fixture.paths.backup_binary),
        "switch failure retained bak after restoring it");
}

// 验证新版本 presence 超时会停止候选并恢复旧版本。
void test_presence_failure_rollback(const std::filesystem::path& root) {
    UpdateFixture fixture = make_fixture(root, "presence-failure");
    const satsuma::AgentConfig original_config =
        satsuma::load_agent_config(fixture.paths.config);
    const std::filesystem::path canonical_presence =
        original_config.shared_root / L"agents" / L"vm_01.json";
    satsuma::write_json_atomic(canonical_presence, {{"status", "idle"}});
    int stop_calls = 0;
    int start_calls = 0;
    int presence_calls = 0;
    const satsuma::vm::AgentUpdateOperations operations{
        [&stop_calls] { ++stop_calls; },
        [&start_calls, &canonical_presence] {
            expect(!std::filesystem::exists(canonical_presence),
                "Service started before stale presence cleanup");
            ++start_calls;
            return start_calls == 1 ? std::uint32_t{6201} : std::uint32_t{6202};
        },
        [&presence_calls, &canonical_presence](
            const std::uint32_t,
            const std::string& version,
            const std::string& update_id) {
            ++presence_calls;
            if (presence_calls == 1) {
                satsuma::write_json_atomic(canonical_presence, {{"status", "idle"}});
                throw std::runtime_error("injected presence timeout");
            }
            expect(version == "0.1.0" && update_id.empty(),
                "presence rollback did not verify the old identity");
        },
    };
    const satsuma::AgentUpdateResult result =
        satsuma::vm::apply_agent_update_for_test(
            fixture.paths,
            fixture.manifest,
            "0.1.1",
            operations);
    expect(result.status == "failed" && result.rollback_status == "succeeded",
        "presence failure did not roll back");
    expect(stop_calls == 2 && start_calls == 2 && presence_calls == 2,
        "presence rollback operation count changed");
    expect(read_text(fixture.paths.formal_binary) == "old-binary",
        "presence failure did not restore the old executable");
}

// 验证旧 Service 无法恢复时保留本机候选和状态证据。
void test_rollback_failure_preserves_evidence(const std::filesystem::path& root) {
    UpdateFixture fixture = make_fixture(root, "rollback-failure");
    const satsuma::vm::AgentUpdateOperations operations{
        [] {},
        []() -> std::uint32_t {
            throw std::runtime_error("injected start failure");
        },
        [](std::uint32_t, const std::string&, const std::string&) {},
    };
    const satsuma::AgentUpdateResult result =
        satsuma::vm::apply_agent_update_for_test(
            fixture.paths,
            fixture.manifest,
            "0.1.1",
            operations);
    expect(result.status == "failed" && result.rollback_status == "failed",
        "rollback failure was not reported");
    expect(std::filesystem::is_regular_file(fixture.paths.new_binary),
        "rollback failure deleted the failed candidate evidence");
    expect(std::filesystem::is_regular_file(fixture.paths.config_backup),
        "rollback failure deleted the config backup evidence");
    expect(std::filesystem::is_regular_file(fixture.paths.state),
        "rollback failure deleted the state evidence");
}

// 验证重启后的旧 Agent 可清理已实际回滚但确认超时的残留状态。
void test_failed_rollback_restart_recovery(const std::filesystem::path& root) {
    UpdateFixture fixture = make_fixture(root, "failed-rollback-restart");
    write_text(fixture.paths.config_backup, "old-config-backup");
    satsuma::write_json_atomic(fixture.paths.state, {
        {"update_id", fixture.manifest.update_id},
        {"phase", "rollback_failed"},
    });
    satsuma::AgentUpdateResult failed;
    failed.update_id = fixture.manifest.update_id;
    failed.vm_id = fixture.manifest.vm_id;
    failed.version = fixture.manifest.version;
    failed.status = "failed";
    failed.rollback_status = "failed";
    failed.error = "injected rollback presence timeout";
    failed.completed_at = "2026-07-27T00:01:00.000Z";
    satsuma::write_json_atomic(fixture.paths.result, failed);

    expect(
        satsuma::vm::recover_verified_failed_rollback_for_test(
            fixture.paths,
            fixture.manifest,
            fixture.paths.formal_binary,
            "0.1.0"),
        "restarted old Agent did not recover a verified rollback");
    expect(
        std::filesystem::is_regular_file(fixture.paths.formal_binary) &&
        std::filesystem::is_regular_file(fixture.paths.result) &&
        !std::filesystem::exists(fixture.paths.new_binary) &&
        !std::filesystem::exists(fixture.paths.config_backup) &&
        !std::filesystem::exists(fixture.paths.manifest) &&
        !std::filesystem::exists(fixture.paths.state),
        "verified rollback recovery changed formal evidence or retained local staging");

    UpdateFixture ambiguous = make_fixture(root, "failed-rollback-ambiguous");
    write_text(ambiguous.paths.backup_binary, "ambiguous-backup");
    satsuma::write_json_atomic(ambiguous.paths.state, {
        {"update_id", ambiguous.manifest.update_id},
        {"phase", "rollback_failed"},
    });
    failed.update_id = ambiguous.manifest.update_id;
    satsuma::write_json_atomic(ambiguous.paths.result, failed);
    expect(
        !satsuma::vm::recover_verified_failed_rollback_for_test(
            ambiguous.paths,
            ambiguous.manifest,
            ambiguous.paths.formal_binary,
            "0.1.0") &&
        std::filesystem::is_regular_file(ambiguous.paths.backup_binary) &&
        std::filesystem::is_regular_file(ambiguous.paths.state),
        "ambiguous failed rollback was cleaned without manual verification");
}

// 验证 bak 删除失败仍处于回滚阶段并保留配置与状态证据。
void test_backup_commit_failure_preserves_evidence(
    const std::filesystem::path& root) {
    UpdateFixture fixture = make_fixture(root, "backup-commit-failure");
    HANDLE backup_lock = INVALID_HANDLE_VALUE;
    const satsuma::vm::AgentUpdateOperations operations{
        [] {},
        [] { return std::uint32_t{6301}; },
        [&fixture, &backup_lock](
            const std::uint32_t,
            const std::string&,
            const std::string&) {
            backup_lock = CreateFileW(
                fixture.paths.backup_binary.c_str(),
                GENERIC_READ,
                FILE_SHARE_READ,
                nullptr,
                OPEN_EXISTING,
                FILE_ATTRIBUTE_NORMAL,
                nullptr);
            if (backup_lock == INVALID_HANDLE_VALUE) {
                throw std::runtime_error("cannot lock Agent backup for commit test");
            }
        },
    };
    const satsuma::AgentUpdateResult result =
        satsuma::vm::apply_agent_update_for_test(
            fixture.paths,
            fixture.manifest,
            "0.1.1",
            operations);
    if (backup_lock != INVALID_HANDLE_VALUE) {
        CloseHandle(backup_lock);
    }

    expect(result.status == "failed" && result.rollback_status == "failed",
        "locked backup did not report a failed pre-commit rollback");
    expect(std::filesystem::is_regular_file(fixture.paths.backup_binary),
        "backup commit failure deleted SatsumaVM.bak.exe evidence");
    expect(std::filesystem::is_regular_file(fixture.paths.config_backup),
        "backup commit failure deleted the config backup evidence");
    expect(std::filesystem::is_regular_file(fixture.paths.state),
        "backup commit failure deleted the state evidence");
}

// 验证 presence 同时绑定 PID、版本和更新 ID。
void test_presence_identity(const std::filesystem::path& root) {
    UpdateFixture fixture = make_fixture(root, "presence");
    satsuma::AgentConfig config = satsuma::load_agent_config(fixture.paths.config);
    const std::filesystem::path presence = fixture.root / L"presence.json";
    satsuma::write_json_atomic(presence, {
        {"schema_version", 1},
        {"protocol_version", 1},
        {"lab_id", "test_lab"},
        {"vm_id", "vm_01"},
        {"agent_version", "0.1.1"},
        {"update_id", "update_001"},
        {"session_id", "session-test"},
        {"boot_id", "boot-test"},
        {"process_id", 7001},
        {"status", "idle"},
        {"updated_at", "2026-07-27T00:01:00.000Z"},
    });
    expect(
        satsuma::vm::agent_update_presence_matches_for_test(
            presence, config, 7001, "0.1.1", "update_001"),
        "matching update presence was rejected");
    expect(
        !satsuma::vm::agent_update_presence_matches_for_test(
            presence, config, 7002, "0.1.1", "update_001"),
        "stale update presence PID was accepted");
    expect(
        !satsuma::vm::agent_update_presence_matches_for_test(
            presence, config, 7001, "0.1.0", "update_001"),
        "wrong update presence version was accepted");
    expect(
        !satsuma::vm::agent_update_presence_matches_for_test(
            presence, config, 7001, "0.1.1", "update_002"),
        "wrong update presence ID was accepted");
}

// 验证更新 Helper 显式脱离 Agent Service 的 Job Object。
void test_update_helper_breakaway_flag() {
    const std::uint32_t flags =
        satsuma::vm::agent_update_helper_creation_flags_for_test();
    expect((flags & CREATE_NO_WINDOW) != 0,
        "update helper lost its hidden-window flag");
    expect((flags & CREATE_BREAKAWAY_FROM_JOB) != 0,
        "update helper can still be terminated with the Agent Service job");
}

}  // namespace

// 顺序运行更新状态机测试并清理临时目录。
int main() {
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        satsuma::path_from_utf8(satsuma::make_id("satsuma-update-test"));
    try {
        test_successful_update(root);
        test_successful_rebind(root);
        test_rebind_target_collision(root);
        test_rebind_presence_failure_rollback(root);
        test_rebind_cross_identity_recovery(root);
        test_committed_cleanup_retry(root);
        test_success_recovery_cleanup_gate(root);
        test_hash_failure(root);
        test_stop_failure(root);
        test_start_failure_rollback(root);
        test_switch_failure_rollback(root);
        test_presence_failure_rollback(root);
        test_rollback_failure_preserves_evidence(root);
        test_failed_rollback_restart_recovery(root);
        test_backup_commit_failure_preserves_evidence(root);
        test_presence_identity(root);
        test_update_helper_breakaway_flag();
        std::filesystem::remove_all(root);
        std::cout << "SatsumaVmUpdateTests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "SatsumaVmUpdateTests failed: " << error.what() << '\n';
        std::error_code cleanup_error;
        std::filesystem::remove_all(root, cleanup_error);
        return 1;
    }
}
