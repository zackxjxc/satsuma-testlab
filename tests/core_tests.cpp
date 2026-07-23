// SatsumaCore 的无外部框架单元测试。
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>
#include <ylt/struct_pack.hpp>

#include "satsuma/core/config.hpp"
#include "satsuma/core/errors.hpp"
#include "satsuma/core/id.hpp"
#include "satsuma/core/json_io.hpp"
#include "satsuma/core/path.hpp"
#include "satsuma/core/rpc_protocol.hpp"
#include "satsuma/core/sha256.hpp"
#include "satsuma/core/snapshot.hpp"
#include "satsuma/core/task.hpp"
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
        {"host", {{"listen", "127.0.0.1:37100"}, {"archive_root", "C:/archive"}}},
        {"shared_folder", {{"host_root", "C:/share"}, {"guest_root", "C:/share"}}},
        {"vms", {{{
            "id", "client"},
            {"vmx", "C:/Client.vmx"},
            {"agent_version", "0.1.0"},
            {"snapshots", {
                {"base", "clean"},
                {"ai_prefix", "satsuma-ai-"},
                {"max_ai_snapshots", 8},
            }},
            {"management_ip", "127.0.0.1"},
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
    manifest.steps.push_back(step);

    const nlohmann::json encoded = manifest;
    const satsuma::RunManifest decoded = encoded.get<satsuma::RunManifest>();
    expect(decoded.run_id == manifest.run_id, "run manifest ID changed during JSON round trip");
    expect(decoded.steps.at(0).arguments == step.arguments, "task arguments changed during JSON round trip");

    satsuma::ExecutionResult result;
    result.run_id = "run_1";
    result.vm_id = "client";
    result.job_id = "job_1";
    result.step_id = "execute";
    result.status = "exited";
    result.exit_code = 0;
    result.stdout_path = "results/client/execute/stdout.log";
    result.stderr_path = "results/client/execute/stderr.log";
    result.started_at = "2026-07-23T00:00:00.000Z";
    result.finished_at = "2026-07-23T00:00:01.000Z";
    const satsuma::ExecutionResult decoded_result = nlohmann::json(result).get<satsuma::ExecutionResult>();
    expect(
        decoded_result.exit_code == std::uint32_t{0},
        "execution result exit code changed during JSON round trip");
}

// 验证 RPC 请求的版本、实验室和状态边界。
void test_rpc_protocol_validation() {
    satsuma::AgentHello hello;
    hello.lab_id = "test_lab";
    hello.vm_id = "client";
    hello.session_id = "session_1";
    hello.boot_id = "boot_1";
    hello.request_id = "request_1";
    hello.agent_version = "0.1.0";
    satsuma::validate_rpc_request(hello, "test_lab");

    const auto encoded = struct_pack::serialize(hello);
    const auto decoded = struct_pack::deserialize<satsuma::AgentHello>(encoded);
    expect(decoded.has_value(), "AgentHello could not be deserialized by struct_pack");
    expect(decoded.value().request_id == hello.request_id, "AgentHello changed during struct_pack round trip");

    expect_error(
        [&hello] {
            satsuma::AgentHello invalid = hello;
            invalid.protocol_version = 2;
            satsuma::validate_rpc_request(invalid, "test_lab");
        },
        "incompatible RPC protocol version was accepted");

    satsuma::AgentStatus status;
    status.lab_id = hello.lab_id;
    status.vm_id = hello.vm_id;
    status.session_id = hello.session_id;
    status.boot_id = hello.boot_id;
    status.request_id = "request_2";
    status.status = "idle";
    satsuma::validate_rpc_request(status, "test_lab");
    status.status = "unknown";
    expect_error(
        [&status] { satsuma::validate_rpc_request(status, "test_lab"); },
        "unknown Agent status was accepted");
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
        test_ai_snapshot_plan();
        test_ai_snapshot_deletion();
        test_protocol_round_trip();
        test_rpc_protocol_validation();
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
