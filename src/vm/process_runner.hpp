// Windows Job Object 进程树执行接口。
#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

namespace satsuma::vm {

// 单次被测进程执行请求。
struct ProcessRequest {
    std::filesystem::path program;          // VM 本地程序绝对路径
    std::vector<std::string> arguments;     // UTF-8 进程参数
    bool verbatim_arguments{false};         // 参数已由内部解释器适配器安全转义
    std::map<std::string, std::string> environment_overrides; // 子进程专用环境覆盖
    std::filesystem::path working_directory;// VM 本地工作目录
    std::filesystem::path stdout_path;      // 持续落盘的 stdout 路径
    std::filesystem::path stderr_path;      // 持续落盘的 stderr 路径
    std::chrono::milliseconds timeout;      // 完整进程树超时
    std::uint64_t max_output_bytes{64ULL * 1024 * 1024}; // stdout 与 stderr 合计上限
    std::stop_token stop_token;             // Agent 生命周期停止信号
};

// 单次进程执行的低层结果。
struct ProcessResult {
    std::optional<std::uint32_t> exit_code; // 进程正常退出时的退出码
    bool timed_out{false};                  // 是否因超时终止 Job Object
    bool output_limit_exceeded{false};      // 是否因日志超过上限终止
    std::int64_t duration_ms{0};            // 执行耗时
};

// 使用 Windows Job Object 启动并约束完整进程树。
class ProcessRunner {
public:
    // 执行请求并等待退出或超时。
    [[nodiscard]] ProcessResult run(const ProcessRequest& request) const;
};

}  // namespace satsuma::vm
