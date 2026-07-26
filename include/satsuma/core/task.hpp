// Satsuma 任务清单、步骤和执行结果模型。
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json_fwd.hpp>

namespace satsuma {

// Host 输入任务中的 Artifact 描述。
struct ArtifactInput {
    std::filesystem::path source;     // Host 源文件绝对路径
    std::string vm;                   // 目标虚拟机 ID
    std::filesystem::path destination;// 运行目录内的相对目标路径
    std::optional<std::string> sha256;// 可选的期望 SHA-256
};

// VM 可见任务清单中的 Artifact 描述。
struct ArtifactManifest {
    std::string vm;                    // 目标虚拟机 ID
    std::filesystem::path path;        // 运行目录内的相对路径
    std::string sha256;                // Host 确认的 SHA-256
};

// 单个 VM 执行步骤。
struct TaskStep {
    std::string id;                         // 运行内唯一步骤 ID
    std::string vm;                         // 目标虚拟机 ID
    std::string type;                       // echo 或 execute
    std::filesystem::path program;          // Artifact 相对路径
    std::vector<std::string> arguments;     // 原样传递的进程参数
    std::string message;                    // echo 步骤内容
    int timeout_seconds{120};               // 进程树超时秒数
    bool retry_safe{false};                 // 旧 claim 到期后是否允许新启动身份重试
    std::vector<std::filesystem::path> collect_files; // 待收集的工作目录相对路径
};

// 生命周期结束时对单台 VM 执行的动作。
enum class VmCleanupAction {
    LeaveRunning,
    Stop,
    Restore,
};

// 成功或失败后的单台 VM 清理策略。
struct VmCleanupPolicy {
    VmCleanupAction action{VmCleanupAction::LeaveRunning};  // 清理动作
    std::optional<std::string> snapshot;                    // restore 动作使用的快照
};

// 单台 VM 在任务生命周期中的恢复和清理策略。
struct VmLifecyclePolicy {
    std::string vm;                               // 目标虚拟机 ID
    std::optional<std::string> restore_before;    // 执行前恢复的快照
    VmCleanupPolicy on_success;                   // 业务成功后的清理策略
    VmCleanupPolicy on_failure;                   // 业务失败后的清理策略
};

// Host 编排器专用的任务生命周期策略。
struct TaskLifecyclePolicy {
    std::vector<VmLifecyclePolicy> vms;  // 按 VM 定义的恢复和清理策略
    std::vector<TaskStep> finally_steps; // 无论业务结果如何都需要执行的步骤
};

// AI 或用户提供的任务计划。
struct TaskPlan {
    int schema_version{1};                 // 输入 schema 版本
    std::string name;                      // 任务显示名称
    std::optional<std::string> run_id;     // 可选的调用方运行 ID
    std::vector<ArtifactInput> artifacts;  // 待部署文件
    std::vector<TaskStep> steps;           // 有序步骤列表
    std::optional<TaskLifecyclePolicy> lifecycle; // 可选的 Host 生命周期策略
};

// Host 物化后供 VM 领取的不可变任务清单。
struct RunManifest {
    int schema_version{1};                    // 清单 schema 版本
    int protocol_version{1};                  // Host/VM 文件协议版本
    std::string lab_id;                       // 实验室稳定 ID
    std::string run_id;                       // 本次运行唯一 ID
    std::string request_id;                   // Host 请求唯一 ID
    std::string name;                         // 任务显示名称
    std::string created_at;                   // UTC 创建时间
    std::vector<ArtifactManifest> artifacts;  // 已登记 Artifact
    std::vector<TaskStep> steps;              // 有序步骤列表
};

// VM 收集的单个结果文件摘要。
struct CollectedFile {
    std::string path;    // 结果目录内相对路径
    std::string sha256;  // 文件 SHA-256
};

// VM 为每个步骤生成的执行结果。
struct ExecutionResult {
    int schema_version{1};          // 结果 schema 版本
    std::string run_id;             // 本次运行唯一 ID
    std::string vm_id;              // 执行 VM ID
    std::string job_id;             // 本次领取生成的 Job ID
    std::string step_id;            // 步骤 ID
    std::string status;             // exited、timed_out 或 failed
    std::optional<std::uint32_t> exit_code; // 进程退出码
    bool timed_out{false};           // 是否由超时终止
    std::int64_t duration_ms{0};     // 执行耗时
    std::string stdout_path;         // 结果目录内 stdout 相对路径
    std::string stderr_path;         // 结果目录内 stderr 相对路径
    std::vector<CollectedFile> files; // 已收集结果文件及摘要
    std::string error;               // Agent 错误说明
    std::string started_at;          // UTC 开始时间
    std::string finished_at;         // UTC 完成时间
};

// 读取并验证用户任务计划。
[[nodiscard]] TaskPlan load_task_plan(const std::filesystem::path& path);

// 读取并验证 VM 可见运行清单。
[[nodiscard]] RunManifest load_run_manifest(const std::filesystem::path& path);

// 返回 VM 清理动作的稳定协议名称。
[[nodiscard]] std::string_view vm_cleanup_action_name(VmCleanupAction action);

// 将运行清单转换为 JSON。
void to_json(nlohmann::json& value, const RunManifest& manifest);

// 从 JSON 解析运行清单。
void from_json(const nlohmann::json& value, RunManifest& manifest);

// 将执行结果转换为 JSON。
void to_json(nlohmann::json& value, const ExecutionResult& result);

// 从 JSON 解析执行结果。
void from_json(const nlohmann::json& value, ExecutionResult& result);

}  // namespace satsuma
