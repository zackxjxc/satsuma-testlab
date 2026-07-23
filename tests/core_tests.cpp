// SatsumaCore 的无外部框架单元测试。
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>

#include <nlohmann/json.hpp>

#include "satsuma/core/errors.hpp"
#include "satsuma/core/id.hpp"
#include "satsuma/core/json_io.hpp"
#include "satsuma/core/path.hpp"
#include "satsuma/core/sha256.hpp"
#include "satsuma/core/task.hpp"

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

}  // namespace

// 顺序运行核心测试，并清理本次专用临时目录。
int main() {
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / satsuma::path_from_utf8(satsuma::make_id("satsuma-core-test"));
    try {
        test_file_primitives(root);
        test_protocol_round_trip();
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
