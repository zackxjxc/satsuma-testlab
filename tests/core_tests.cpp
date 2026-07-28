// SatsumaCore 的无外部框架单元测试。
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>
#include <windows.h>

#include "satsuma/core/config.hpp"
#include "satsuma/core/claim.hpp"
#include "satsuma/core/errors.hpp"
#include "satsuma/core/id.hpp"
#include "satsuma/core/json_io.hpp"
#include "satsuma/core/lifecycle.hpp"
#include "satsuma/core/path.hpp"
#include "satsuma/core/sha256.hpp"
#include "satsuma/core/snapshot.hpp"
#include "satsuma/core/task.hpp"
#include "satsuma/core/update.hpp"
#include "satsuma/core/windows_command_line.hpp"

namespace {

// 在条件不满足时终止当前测试。
void expect(const bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

// 验证操作按安全约束抛出 Satsuma 错误。
void expect_error(const std::function<void()>& operation, const std::string& message) {
    try {
        operation();
    } catch (const satsuma::Error&) {
        return;
    }
    throw std::runtime_error(message);
}

// 验证路径边界、原子 JSON 和 SHA-256。
void test_file_primitives(const std::filesystem::path& root) {
    satsuma::validate_identifier("client_01", "test identifier");
    expect_error(
        [] { satsuma::validate_identifier("../client", "test identifier"); },
        "unsafe identifier was accepted");

    std::filesystem::create_directories(root);
    expect(
        satsuma::resolve_under_root(root, L"runs/test.json") == root / L"runs/test.json",
        "safe relative path was not resolved under root");
    expect_error(
        [&root] { static_cast<void>(satsuma::resolve_under_root(root, L"../escape.txt")); },
        "path traversal was accepted");
    expect_error(
        [&root] { static_cast<void>(satsuma::resolve_under_root(root, L"C:\\escape.txt")); },
        "absolute path was accepted");

    const std::filesystem::path json_path = root / L"state" / L"sample.json";
    satsuma::write_json_atomic(json_path, {{"message", "hello"}, {"value", 7}});
    const nlohmann::json value = satsuma::load_json(json_path);
    expect(value.at("message") == "hello" && value.at("value") == 7, "atomic JSON round trip failed");
    HANDLE blocking_reader = CreateFileW( // 模拟共享目录延迟释放的读取 lease
        json_path.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    expect(blocking_reader != INVALID_HANDLE_VALUE, "atomic JSON retry test could not open its reader");
    std::jthread release_reader([blocking_reader] {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        CloseHandle(blocking_reader);
    });
    const auto replace_started = std::chrono::steady_clock::now(); // 验证写入确实等待占用释放
    satsuma::write_json_atomic(json_path, {{"message", "updated"}, {"value", 8}});
    const auto replace_elapsed = std::chrono::steady_clock::now() - replace_started;
    release_reader.join();
    expect(replace_elapsed >= std::chrono::milliseconds(100), "atomic JSON write skipped its retry wait");
    const nlohmann::json updated = satsuma::load_json(json_path); // 读取后立即替换的回归结果
    expect(
        updated.at("message") == "updated" && updated.at("value") == 8,
        "atomic JSON replacement after read failed");

    const std::filesystem::path removed_parent = root / L"removed";
    expect_error(
        [&removed_parent] {
            satsuma::write_json_atomic_existing_parent(
                removed_parent / L"result.json",
                {{"status", "failed"}});
        },
        "existing-parent JSON write accepted a removed directory");
    expect(!std::filesystem::exists(removed_parent),
        "existing-parent JSON write recreated a removed directory");

    const std::filesystem::path rename_source = root / L"rename-source";
    const std::filesystem::path rename_destination = root / L"rename-destination";
    std::filesystem::create_directories(rename_source);
    {
        std::ofstream evidence(rename_source / L"evidence.txt", std::ios::binary);
        evidence << "evidence\n";
        expect(static_cast<bool>(evidence), "path rename test could not create its evidence");
    }
    HANDLE blocking_directory = CreateFileW( // 模拟安全软件短暂占用待发布目录
        rename_source.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS,
        nullptr);
    expect(blocking_directory != INVALID_HANDLE_VALUE, "path rename test could not open its directory");
    std::jthread release_directory([blocking_directory] {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        CloseHandle(blocking_directory);
    });
    const auto rename_started = std::chrono::steady_clock::now(); // 验证改名等待占用释放
    satsuma::rename_path_with_retry(rename_source, rename_destination);
    const auto rename_elapsed = std::chrono::steady_clock::now() - rename_started;
    release_directory.join();
    expect(rename_elapsed >= std::chrono::milliseconds(100), "path rename skipped its retry wait");
    expect(!std::filesystem::exists(rename_source), "path rename retry retained its source directory");
    expect(
        std::filesystem::is_regular_file(rename_destination / L"evidence.txt"),
        "path rename retry omitted directory contents");

    const std::filesystem::path hash_path = root / L"abc.txt";
    std::ofstream output(hash_path, std::ios::binary);
    output << "abc";
    output.close();
    expect(
        satsuma::sha256_file(hash_path) ==
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
        "SHA-256 known-answer test failed");
}

// 验证 VM 快照策略解析和基础快照所有权边界。
void test_snapshot_configuration(const std::filesystem::path& root) {
    nlohmann::json value = {
        {"schema_version", 1},
        {"lab_id", "snapshot_test"},
        {"provider", {{"type", "vmware_workstation"}, {"vmrun", "C:/vmrun.exe"}}},
        {"host", {{"archive_root", "C:/archive"}}},
        {"shared_folder", {{"host_root", "C:/share"}}},
        {"vms", {{{
            "id", "client"},
            {"vmx", "C:/Client.vmx"},
            {"agent_version", "0.1.0"},
            {"snapshots", {
                {"base", "clean"},
                {"ai_prefix", "satsuma-ai-"},
                {"max_ai_snapshots", 8},
            }},
        }}},
    };
    const std::filesystem::path config_path = root / L"snapshot-lab.json";
    satsuma::write_json_atomic(config_path, value);
    const satsuma::LabConfig config = satsuma::load_lab_config(config_path);
    expect(config.vms.at(0).snapshots.base == "clean", "snapshot base was not parsed");
    expect(config.vms.at(0).snapshots.max_ai_snapshots == 8, "snapshot quota was not parsed");
    value["vms"][0]["snapshots"]["base"] = "satsuma-ai-user-base";
    satsuma::write_json_atomic(config_path, value);
    expect_error(
        [&config_path] { static_cast<void>(satsuma::load_lab_config(config_path)); },
        "base snapshot using the AI prefix was accepted");
}

// 验证 Host 和 Agent 的关键路径不会依赖调用进程的当前目录。
void test_absolute_configuration_paths(const std::filesystem::path& root) {
    const nlohmann::json lab = {
        {"schema_version", 1},
        {"lab_id", "absolute_path_test"},
        {"provider", {{"type", "vmware_workstation"}, {"vmrun", "C:/vmrun.exe"}}},
        {"host", {{"archive_root", "C:/archive"}}},
        {"shared_folder", {{"host_root", "C:/share"}}},
        {"vms", {{
            {"id", "client"},
            {"vmx", "C:/Client.vmx"},
            {"agent_version", "0.1.0"},
            {"snapshots", {
                {"base", "clean"},
                {"ai_prefix", "satsuma-ai-"},
                {"max_ai_snapshots", 8},
            }},
        }}},
    };
    const std::filesystem::path lab_path = root / L"absolute-lab.json";

    const auto expect_lab_rejected = [&lab_path](
        const nlohmann::json& invalid,
        const std::string& message) {
        satsuma::write_json_atomic(lab_path, invalid);
        expect_error(
            [&lab_path] { static_cast<void>(satsuma::load_lab_config(lab_path)); },
            message);
    };

    nlohmann::json invalid_lab = lab;
    invalid_lab["provider"]["vmrun"] = "vmrun.exe";
    expect_lab_rejected(invalid_lab, "relative vmrun path was accepted");
    invalid_lab = lab;
    invalid_lab["host"]["archive_root"] = "archive";
    expect_lab_rejected(invalid_lab, "relative archive root was accepted");
    invalid_lab = lab;
    invalid_lab["shared_folder"]["host_root"] = "share";
    expect_lab_rejected(invalid_lab, "relative shared folder root was accepted");
    invalid_lab = lab;
    invalid_lab["vms"][0]["vmx"] = "Client.vmx";
    expect_lab_rejected(invalid_lab, "relative VMX path was accepted");
    invalid_lab = lab;
    invalid_lab["host"]["listen"] = "127.0.0.1:37100";
    expect_lab_rejected(invalid_lab, "removed Host network configuration was accepted");

    const nlohmann::json agent = {
        {"schema_version", 1},
        {"protocol_version", 2},
        {"lab_id", "absolute_path_test"},
        {"vm_id", "client"},
        {"agent_version", "0.1.0"},
        {"shared_root", "C:/share"},
        {"local_work_root", "C:/work"},
    };
    const std::filesystem::path agent_path = root / L"absolute-agent.json";
    const auto expect_agent_rejected = [&agent_path](
        const nlohmann::json& invalid,
        const std::string& message) {
        satsuma::write_json_atomic(agent_path, invalid);
        expect_error(
            [&agent_path] { static_cast<void>(satsuma::load_agent_config(agent_path)); },
            message);
    };

    nlohmann::json invalid_agent = agent;
    invalid_agent["shared_root"] = "share";
    expect_agent_rejected(invalid_agent, "relative Agent shared root was accepted");
    invalid_agent = agent;
    invalid_agent["local_work_root"] = "work";
    expect_agent_rejected(invalid_agent, "relative Agent work root was accepted");
    invalid_agent = agent;
    invalid_agent["host"] = "127.0.0.1:37100";
    expect_agent_rejected(invalid_agent, "removed Agent network configuration was accepted");
}

// 验证 AI 快照命名、重名检查和数量配额。
void test_ai_snapshot_plan() {
    satsuma::SnapshotConfig config;
    config.base = "clean";
    config.ai_prefix = "satsuma-ai-";
    config.max_ai_snapshots = 2;
    std::vector<std::string> existing = {"clean", "satsuma-ai-existing-20260722"};
    const std::string planned = satsuma::plan_ai_snapshot_name(
        config,
        existing,
        "network-ready",
        "20260723120000");
    expect(
        planned == "satsuma-ai-network-ready-20260723120000",
        "AI snapshot name did not follow the configured prefix");

    existing.push_back(planned);
    expect_error(
        [&config, &existing] {
            static_cast<void>(satsuma::plan_ai_snapshot_name(
                config,
                existing,
                "another",
                "20260723120100"));
        },
        "AI snapshot quota was not enforced");

    config.max_ai_snapshots = 8;
    expect_error(
        [&config, &existing] {
            static_cast<void>(satsuma::plan_ai_snapshot_name(
                config,
                existing,
                "network-ready",
                "20260723120000"));
        },
        "duplicate AI snapshot name was accepted");
}

// 验证快照删除只能作用于已存在的 AI 所有权名称。
void test_ai_snapshot_deletion() {
    satsuma::SnapshotConfig config;
    config.base = "clean";
    config.ai_prefix = "satsuma-ai-";
    config.max_ai_snapshots = 8;
    const std::vector<std::string> existing = {
        "clean",
        "manual-checkpoint",
        "satsuma-ai-obsolete-20260722",
    };
    satsuma::validate_ai_snapshot_deletion(config, existing, "satsuma-ai-obsolete-20260722");
    expect_error(
        [&config, &existing] { satsuma::validate_ai_snapshot_deletion(config, existing, "clean"); },
        "user base snapshot was accepted for deletion");
    expect_error(
        [&config, &existing] {
            satsuma::validate_ai_snapshot_deletion(config, existing, "manual-checkpoint");
        },
        "external snapshot was accepted for deletion");
    expect_error(
        [&config, &existing] {
            satsuma::validate_ai_snapshot_deletion(config, existing, "satsuma-ai-missing");
        },
        "missing AI snapshot was accepted for deletion");
}

// 验证运行清单和执行结果的 JSON 往返。
void test_protocol_round_trip() {
    satsuma::RunManifest manifest;
    manifest.lab_id = "test_lab";
    manifest.run_id = "run_1";
    manifest.request_id = "request_1";
    manifest.name = "round-trip";
    manifest.created_at = "2026-07-23T00:00:00.000Z";
    manifest.artifacts.push_back({
        "client",
        satsuma::path_from_utf8("artifacts/client/test.exe"),
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
    });
    satsuma::TaskStep step;
    step.id = "execute";
    step.vm = "client";
    step.type = "execute";
    step.program = satsuma::path_from_utf8("artifacts/client/test.exe");
    step.arguments = {"argument with spaces", "quote\"value"};
    step.run_as = satsuma::TaskRunAs::InteractiveUser;
    step.retry_safe = true;
    manifest.steps.push_back(step);

    const nlohmann::json encoded = manifest;
    expect(
        encoded.at("protocol_version") == satsuma::kRunManifestProtocolVersion,
        "run manifest did not use the current file protocol");
    expect(
        encoded.at("steps").at(0).at("run_as") == "interactive_user",
        "run manifest did not serialize the execute identity");
    const satsuma::RunManifest decoded = encoded.get<satsuma::RunManifest>();
    expect(decoded.run_id == manifest.run_id, "run manifest ID changed during JSON round trip");
    expect(decoded.steps.at(0).arguments == step.arguments, "task arguments changed during JSON round trip");
    expect(
        decoded.steps.at(0).run_as == satsuma::TaskRunAs::InteractiveUser,
        "task run identity changed during JSON round trip");
    expect(decoded.steps.at(0).retry_safe, "task retry safety changed during JSON round trip");

    satsuma::ExecutionResult result;
    result.run_id = "run_1";
    result.vm_id = "client";
    result.job_id = "job_1";
    result.step_id = "execute";
    result.status = "exited";
    result.run_as = satsuma::TaskRunAs::InteractiveUser;
    result.interactive_session_id = 23;
    result.exit_code = 0;
    result.stdout_path = "results/client/execute/stdout.log";
    result.stderr_path = "results/client/execute/stderr.log";
    result.started_at = "2026-07-23T00:00:00.000Z";
    result.finished_at = "2026-07-23T00:00:01.000Z";
    const nlohmann::json encoded_result = result;
    const satsuma::ExecutionResult decoded_result =
        encoded_result.get<satsuma::ExecutionResult>();
    expect(
        decoded_result.exit_code == std::uint32_t{0},
        "execution result exit code changed during JSON round trip");
    expect(
        decoded_result.run_as == satsuma::TaskRunAs::InteractiveUser &&
            decoded_result.interactive_session_id == std::uint32_t{23},
        "execution result identity changed during JSON round trip");

    nlohmann::json legacy_result = encoded_result;
    legacy_result.erase("run_as");
    legacy_result.erase("interactive_session_id");
    const satsuma::ExecutionResult decoded_legacy_result =
        legacy_result.get<satsuma::ExecutionResult>();
    expect(
        decoded_legacy_result.run_as == satsuma::TaskRunAs::System &&
            !decoded_legacy_result.interactive_session_id.has_value(),
        "legacy execution result did not default to the system identity");

    nlohmann::json invalid_result = encoded_result;
    invalid_result["run_as"] = "administrator";
    expect_error(
        [&invalid_result] {
            static_cast<void>(invalid_result.get<satsuma::ExecutionResult>());
        },
        "execution result accepted an unsupported run identity");

    invalid_result = encoded_result;
    invalid_result["run_as"] = "system";
    expect_error(
        [&invalid_result] {
            static_cast<void>(invalid_result.get<satsuma::ExecutionResult>());
        },
        "SYSTEM execution result accepted an interactive Session ID");

    invalid_result = encoded_result;
    invalid_result.erase("interactive_session_id");
    expect_error(
        [&invalid_result] {
            static_cast<void>(invalid_result.get<satsuma::ExecutionResult>());
        },
        "successful interactive result accepted a missing Session ID");
}

// 验证任务计划默认身份以及运行清单 v1/v2 的兼容门禁。
void test_task_run_as_protocol(const std::filesystem::path& root) {
    nlohmann::json execute_step = {
        {"id", "execute"},
        {"vm", "client"},
        {"type", "execute"},
        {"program", "artifacts/client/test.exe"},
    };
    nlohmann::json plan_value = {
        {"schema_version", 1},
        {"name", "run-as-policy"},
        {"steps", nlohmann::json::array({execute_step})},
    };
    const std::filesystem::path plan_path = root / L"run-as-plan.json";
    satsuma::write_json_atomic(plan_path, plan_value);
    expect(
        satsuma::load_task_plan(plan_path).steps.at(0).run_as == satsuma::TaskRunAs::System,
        "task plan did not default execute.run_as to system");

    plan_value["steps"][0]["run_as"] = "interactive_user";
    satsuma::write_json_atomic(plan_path, plan_value);
    expect(
        satsuma::load_task_plan(plan_path).steps.at(0).run_as ==
            satsuma::TaskRunAs::InteractiveUser,
        "task plan did not parse interactive_user");

    plan_value["steps"][0]["run_as"] = "administrator";
    satsuma::write_json_atomic(plan_path, plan_value);
    expect_error(
        [&plan_path] { static_cast<void>(satsuma::load_task_plan(plan_path)); },
        "task plan accepted an unsupported run identity");

    plan_value["steps"][0] = {
        {"id", "echo"},
        {"vm", "client"},
        {"type", "echo"},
        {"message", "hello"},
        {"run_as", "system"},
    };
    satsuma::write_json_atomic(plan_path, plan_value);
    expect_error(
        [&plan_path] { static_cast<void>(satsuma::load_task_plan(plan_path)); },
        "echo step accepted an explicit run identity");

    satsuma::RunManifest manifest;
    manifest.lab_id = "test_lab";
    manifest.run_id = "run_identity";
    manifest.request_id = "request_identity";
    manifest.name = "identity-protocol";
    manifest.created_at = "2026-07-27T00:00:00.000Z";
    satsuma::TaskStep step;
    step.id = "execute";
    step.vm = "client";
    step.type = "execute";
    step.program = satsuma::path_from_utf8("artifacts/client/test.exe");
    manifest.steps.push_back(step);

    const nlohmann::json version_two = manifest;
    expect(
        version_two.at("steps").at(0).at("run_as") == "system",
        "protocol v2 did not explicitly serialize system run_as");

    nlohmann::json missing_v2_identity = version_two;
    missing_v2_identity["steps"][0].erase("run_as");
    expect_error(
        [&missing_v2_identity] {
            static_cast<void>(missing_v2_identity.get<satsuma::RunManifest>());
        },
        "protocol v2 accepted an execute step without run_as");

    nlohmann::json version_one = version_two;
    version_one["protocol_version"] = satsuma::kLegacyRunManifestProtocolVersion;
    version_one["steps"][0].erase("run_as");
    const satsuma::RunManifest decoded_version_one =
        version_one.get<satsuma::RunManifest>();
    expect(
        decoded_version_one.steps.at(0).run_as == satsuma::TaskRunAs::System,
        "protocol v1 execute step did not use the implicit system identity");

    version_one["steps"][0]["run_as"] = "system";
    expect_error(
        [&version_one] {
            static_cast<void>(version_one.get<satsuma::RunManifest>());
        },
        "protocol v1 accepted an explicit run_as field");

    manifest.protocol_version = satsuma::kLegacyRunManifestProtocolVersion;
    const nlohmann::json serialized_version_one = manifest;
    expect(
        !serialized_version_one.at("steps").at(0).contains("run_as"),
        "protocol v1 serializer emitted run_as");

    manifest.steps.at(0).run_as = satsuma::TaskRunAs::InteractiveUser;
    expect_error(
        [&manifest] { static_cast<void>(nlohmann::json(manifest)); },
        "protocol v1 serialized an interactive_user step");

    manifest.protocol_version = satsuma::kRunManifestProtocolVersion;
    satsuma::TaskStep echo_step;
    echo_step.id = "echo";
    echo_step.vm = "client";
    echo_step.type = "echo";
    echo_step.message = "hello";
    manifest.steps = {echo_step};
    const nlohmann::json version_two_echo = manifest;
    expect(
        !version_two_echo.at("steps").at(0).contains("run_as"),
        "protocol v2 serialized run_as for an echo step");
    static_cast<void>(version_two_echo.get<satsuma::RunManifest>());
}

// 验证任务生命周期策略解析和普通 run 的安全边界所需模型。
void test_task_lifecycle_policy(const std::filesystem::path& root) {
    nlohmann::json value = {
        {"schema_version", 1},
        {"name", "lifecycle-policy"},
        {"steps", {{{"id", "execute"}, {"vm", "client"}, {"type", "echo"}, {"message", "run"}}}},
        {"lifecycle", {
            {"vms", {{
                {"vm", "client"},
                {"restore_before", "satsuma-ai-ready"},
                {"on_success", {{"action", "restore"}, {"snapshot", "satsuma-ai-ready"}}},
                {"on_failure", {{"action", "stop"}}},
            }}},
            {"finally", {{{"id", "cleanup"}, {"vm", "client"}, {"type", "echo"}, {"message", "done"}}}},
        }},
    };
    const std::filesystem::path plan_path = root / L"lifecycle-plan.json";
    satsuma::write_json_atomic(plan_path, value);
    const satsuma::TaskPlan plan = satsuma::load_task_plan(plan_path);
    expect(plan.lifecycle.has_value(), "task lifecycle policy was not parsed");
    expect(plan.lifecycle->vms.size() == 1, "task lifecycle VM policy count changed");
    expect(
        plan.lifecycle->vms.at(0).on_success.action == satsuma::VmCleanupAction::Restore,
        "task success cleanup action changed");
    expect(
        plan.lifecycle->finally_steps.at(0).id == "cleanup",
        "task finally step was not parsed");

    value["lifecycle"]["vms"][0]["on_failure"] = {
        {"action", "stop"},
        {"snapshot", "unexpected"},
    };
    satsuma::write_json_atomic(plan_path, value);
    expect_error(
        [&plan_path] { static_cast<void>(satsuma::load_task_plan(plan_path)); },
        "non-restore cleanup action accepted a snapshot");

    value["lifecycle"]["vms"][0]["on_failure"] = {{"action", "restore"}};
    satsuma::write_json_atomic(plan_path, value);
    expect_error(
        [&plan_path] { static_cast<void>(satsuma::load_task_plan(plan_path)); },
        "restore cleanup action accepted a missing snapshot");
}

// 验证用户任务会尽早拒绝未知字段和 Windows 等价的重复收集路径。
void test_task_input_validation(const std::filesystem::path& root) {
    const nlohmann::json plan = {
        {"schema_version", 1},
        {"name", "strict-task-input"},
        {"artifacts", {{
            {"source", "C:/fixture.exe"},
            {"vm", "client"},
            {"shared_destination", "artifacts/client/fixture.exe"},
        }}},
        {"steps", {{
            {"id", "execute"},
            {"vm", "client"},
            {"type", "execute"},
            {"program", "artifacts/client/fixture.exe"},
            {"collect_files", {"output/result.json"}},
        }}},
    };
    const std::filesystem::path plan_path = root / L"strict-task-input.json";
    const auto expect_plan_rejected = [&plan_path](
        const nlohmann::json& invalid,
        const std::string& message) {
        satsuma::write_json_atomic(plan_path, invalid);
        expect_error(
            [&plan_path] { static_cast<void>(satsuma::load_task_plan(plan_path)); },
            message);
    };

    nlohmann::json invalid = plan;
    invalid["unexpected"] = true;
    expect_plan_rejected(invalid, "task plan accepted an unknown top-level field");
    invalid = plan;
    invalid["artifacts"][0]["digest"] = "unused";
    expect_plan_rejected(invalid, "task artifact accepted an unknown field");
    invalid = plan;
    invalid["steps"][0]["run_ass"] = "interactive_user";
    expect_plan_rejected(invalid, "execute step accepted a misspelled run_as field");
    invalid = plan;
    invalid["steps"][0]["collect_files"] = {
        "output/result.json",
        "OUTPUT\\RESULT.JSON",
    };
    expect_plan_rejected(invalid, "execute step accepted duplicate Windows result paths");

    invalid = plan;
    invalid["lifecycle"] = {
        {"vms", {{
            {"vm", "client"},
            {"on_success", {{"action", "leave_running"}}},
            {"on_failure", {{"action", "stop"}, {"snapshpt", "clean"}}},
        }}},
    };
    expect_plan_rejected(invalid, "lifecycle cleanup policy accepted an unknown field");
}

// 验证生命周期迁移图、原子持久化和恢复失败终态。
void test_run_lifecycle(const std::filesystem::path& root) {
    const std::filesystem::path state_path = root / L"lifecycle" / L"state.json";
    satsuma::RunLifecycleState state = satsuma::make_run_lifecycle_state(
        "run_lifecycle_1",
        "2026-07-26T00:00:00.000Z");
    satsuma::persist_run_transition(
        state_path,
        state,
        satsuma::RunPhase::RestoringBefore,
        "2026-07-26T00:00:01.000Z",
        "restore requested");
    satsuma::persist_run_transition(
        state_path,
        state,
        satsuma::RunPhase::StartingVm,
        "2026-07-26T00:00:02.000Z",
        "snapshot restored");

    const satsuma::RunLifecycleState loaded = satsuma::load_run_lifecycle_state(state_path);
    expect(loaded.phase == satsuma::RunPhase::StartingVm, "persisted lifecycle phase changed");
    expect(loaded.sequence == 2 && loaded.transitions.size() == 2, "lifecycle history was not preserved");
    expect_error(
        [&state] {
            satsuma::apply_run_transition(
                state,
                satsuma::RunPhase::Completed,
                "2026-07-26T00:00:03.000Z",
                "invalid shortcut");
        },
        "invalid lifecycle shortcut was accepted");

    satsuma::RunLifecycleState recovery = satsuma::make_run_lifecycle_state(
        "run_lifecycle_2",
        "2026-07-26T00:00:00.000Z");
    satsuma::apply_run_transition(
        recovery,
        satsuma::RunPhase::RestoringBefore,
        "2026-07-26T00:00:01.000Z",
        "restore requested");
    satsuma::apply_run_transition(
        recovery,
        satsuma::RunPhase::RecoveryFailed,
        "2026-07-26T00:00:02.000Z",
        "vmrun restore failed");
    expect(satsuma::is_terminal_run_phase(recovery.phase), "recovery failure was not terminal");
    expect_error(
        [&recovery] {
            satsuma::apply_run_transition(
                recovery,
                satsuma::RunPhase::StartingVm,
                "2026-07-26T00:00:03.000Z",
                "unsafe retry");
        },
        "terminal recovery failure accepted another transition");
}

// 验证 claim 租约只允许显式安全步骤在过期后重试。
void test_claim_recovery_decision(const std::filesystem::path& root) {
    const satsuma::StepClaimLease safe = satsuma::make_step_claim_lease(
        "run_claim",
        "client",
        "echo",
        "job_claim",
        "session_old",
        "boot_old",
        1'000,
        5'000,
        true);
    expect(
        satsuma::evaluate_claim_recovery(safe, 5'999) ==
            satsuma::ClaimRecoveryDecision::Wait,
        "unexpired claim lease was retried");
    expect(
        satsuma::evaluate_claim_recovery(safe, 6'000) ==
            satsuma::ClaimRecoveryDecision::Retry,
        "safe expired claim was not released for a fenced retry");

    satsuma::StepClaimLease dangerous = safe;
    dangerous.step_id = "execute";
    dangerous.retry_safe = false;
    expect(
        satsuma::evaluate_claim_recovery(dangerous, 6'000) ==
            satsuma::ClaimRecoveryDecision::ManualInterventionRequired,
        "unsafe expired claim did not preserve the manual gate");

    const satsuma::StepClaimLease renewed =
        satsuma::renew_step_claim_lease(safe, 2'000, 5'000);
    expect(
        satsuma::same_step_claim_owner(safe, renewed) &&
            renewed.last_renewed_unix_ms == 2'000 &&
            renewed.lease_expires_unix_ms == 7'000 &&
            renewed.renewal_sequence == 1,
        "claim renewal changed ownership or did not extend the lease");
    expect_error(
        [&safe] {
            static_cast<void>(satsuma::renew_step_claim_lease(safe, 6'000, 5'000));
        },
        "expired claim lease was renewed");
    expect_error(
        [&safe] {
            static_cast<void>(satsuma::renew_step_claim_lease(safe, 1'000, 6'000));
        },
        "claim renewal accepted a repeated timestamp");
    expect_error(
        [&safe] {
            static_cast<void>(satsuma::renew_step_claim_lease(safe, 2'000, 3'000));
        },
        "claim renewal shortened the lease expiry");
    satsuma::StepClaimLease other_owner = renewed;
    other_owner.job_id = "job_other";
    expect(
        !satsuma::same_step_claim_owner(renewed, other_owner),
        "claim ownership comparison ignored the job ID");

    const std::filesystem::path claim_path = root / L"claim" / L"step.claim.json";
    satsuma::write_json_atomic(claim_path, safe);
    const satsuma::StepClaimLease loaded = satsuma::load_step_claim_lease(claim_path);
    expect(
        loaded.attempt == 1 && loaded.lease_expires_unix_ms == 6'000 &&
            loaded.last_renewed_unix_ms == 1'000 && loaded.renewal_sequence == 0,
        "claim lease round trip failed");
    expect(
        satsuma::step_claim_renewal_path(claim_path, renewed).filename() ==
            L"echo.claim-renewal-job_claim-0000000001.json",
        "claim renewal sidecar path did not bind the step, job, and sequence");
    expect_error(
        [&claim_path, &safe] {
            static_cast<void>(satsuma::step_claim_renewal_path(claim_path, safe));
        },
        "claim renewal sidecar accepted sequence zero");

    nlohmann::json legacy = safe;
    legacy["schema_version"] = 2;
    legacy.erase("last_renewed_at");
    legacy.erase("last_renewed_unix_ms");
    legacy.erase("renewal_sequence");
    const satsuma::StepClaimLease compatible = legacy.get<satsuma::StepClaimLease>();
    expect(
        compatible.last_renewed_at == compatible.claimed_at &&
            compatible.last_renewed_unix_ms == compatible.claimed_unix_ms &&
            compatible.renewal_sequence == 0,
        "legacy claim lease did not receive renewal defaults");
}

// 验证独立更新清单和终态结果的严格协议。
void test_agent_update_protocol(const std::filesystem::path& root) {
    satsuma::AgentUpdateManifest manifest;
    manifest.lab_id = "test_lab";
    manifest.vm_id = "client";
    manifest.update_id = "update_001";
    manifest.version = "0.1.1";
    manifest.binary = L"SatsumaVM.exe";
    manifest.size = 1234;
    manifest.sha256 = std::string(64, 'a');
    manifest.created_at = "2026-07-27T00:00:00.000Z";
    const nlohmann::json encoded = manifest;
    const satsuma::AgentUpdateManifest decoded =
        encoded.get<satsuma::AgentUpdateManifest>();
    expect(decoded.update_id == manifest.update_id, "update manifest ID changed");
    expect(decoded.binary == manifest.binary, "update manifest binary changed");
    expect(decoded.size == manifest.size, "update manifest size changed");
    expect(!encoded.contains("next_vm_id"), "protocol 1 update serialized next_vm_id");

    satsuma::AgentUpdateManifest rebind = manifest;
    rebind.protocol_version = 2;
    rebind.next_vm_id = "gateway";
    const nlohmann::json encoded_rebind = rebind;
    const satsuma::AgentUpdateManifest decoded_rebind =
        encoded_rebind.get<satsuma::AgentUpdateManifest>();
    expect(
        decoded_rebind.next_vm_id == rebind.next_vm_id,
        "update rebind identity changed");

    const std::filesystem::path manifest_path = root / L"update.json";
    satsuma::write_json_atomic(manifest_path, encoded);
    expect(
        satsuma::load_agent_update_manifest(manifest_path).version == manifest.version,
        "update manifest file did not round-trip");

    nlohmann::json invalid = encoded;
    invalid["sha256"] = "ABC";
    expect_error(
        [&invalid] { static_cast<void>(invalid.get<satsuma::AgentUpdateManifest>()); },
        "invalid update hash was accepted");
    invalid = encoded;
    invalid["binary"] = "nested/SatsumaVM.exe";
    expect_error(
        [&invalid] { static_cast<void>(invalid.get<satsuma::AgentUpdateManifest>()); },
        "nested update binary was accepted");
    invalid = encoded;
    invalid["size"] = 0;
    expect_error(
        [&invalid] { static_cast<void>(invalid.get<satsuma::AgentUpdateManifest>()); },
        "zero-sized update was accepted");
    invalid = encoded;
    invalid["protocol_version"] = 2;
    expect_error(
        [&invalid] { static_cast<void>(invalid.get<satsuma::AgentUpdateManifest>()); },
        "protocol 2 update without next_vm_id was accepted");
    invalid = encoded;
    invalid["next_vm_id"] = "gateway";
    expect_error(
        [&invalid] { static_cast<void>(invalid.get<satsuma::AgentUpdateManifest>()); },
        "protocol 1 update with next_vm_id was accepted");
    invalid = encoded_rebind;
    invalid["next_vm_id"] = "client";
    expect_error(
        [&invalid] { static_cast<void>(invalid.get<satsuma::AgentUpdateManifest>()); },
        "update rebind accepted its current identity as next_vm_id");

    satsuma::AgentUpdateResult result;
    result.update_id = manifest.update_id;
    result.vm_id = manifest.vm_id;
    result.version = manifest.version;
    result.status = "succeeded";
    result.rollback_status = "none";
    result.process_id = 4321;
    result.completed_at = "2026-07-27T00:01:00.000Z";
    const satsuma::AgentUpdateResult decoded_result =
        nlohmann::json(result).get<satsuma::AgentUpdateResult>();
    expect(decoded_result.process_id == 4321, "update result PID changed");

    nlohmann::json invalid_result = result;
    invalid_result["process_id"] = 0;
    expect_error(
        [&invalid_result] {
            static_cast<void>(invalid_result.get<satsuma::AgentUpdateResult>());
        },
        "successful update without a PID was accepted");
}

// 验证 CreateProcessW 参数引用和结尾反斜杠处理。
void test_windows_command_line() {
    expect(satsuma::quote_windows_argument(L"plain") == L"plain", "plain argument was quoted unexpectedly");
    expect(satsuma::quote_windows_argument(L"") == L"\"\"", "empty argument was not preserved");
    expect(
        satsuma::quote_windows_argument(L"hello world") == L"\"hello world\"",
        "argument with spaces was not quoted");
    expect(
        satsuma::quote_windows_argument(L"C:\\path with space\\") == L"\"C:\\path with space\\\\\"",
        "trailing backslash was not doubled before the closing quote");
}

}  // namespace

// 顺序运行核心测试，并清理本次专用临时目录。
int main() {
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / satsuma::path_from_utf8(satsuma::make_id("satsuma-core-test"));
    try {
        test_file_primitives(root);
        test_snapshot_configuration(root);
        test_absolute_configuration_paths(root);
        test_ai_snapshot_plan();
        test_ai_snapshot_deletion();
        test_protocol_round_trip();
        test_task_run_as_protocol(root);
        test_task_lifecycle_policy(root);
        test_task_input_validation(root);
        test_run_lifecycle(root);
        test_claim_recovery_decision(root);
        test_agent_update_protocol(root);
        test_windows_command_line();
        std::filesystem::remove_all(root);
        std::cout << "SatsumaCoreTests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "SatsumaCoreTests failed: " << error.what() << '\n';
        std::error_code cleanup_error;
        std::filesystem::remove_all(root, cleanup_error);
        return 1;
    }
}
