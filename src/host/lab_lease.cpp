// Host 实验室进程互斥和持久写租约实现。
#include "lab_lease.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cwctype>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <stop_token>
#include <thread>

#include <windows.h>

#include "satsuma/core/errors.hpp"
#include "satsuma/core/id.hpp"
#include "satsuma/core/json_io.hpp"
#include "satsuma/core/path.hpp"
#include "satsuma/core/task.hpp"

namespace satsuma::host {
namespace {

constexpr std::chrono::seconds kRenewInterval{2}; // 持久租约续期间隔

// 自动关闭 Win32 HANDLE。
class UniqueHandle {
public:
    explicit UniqueHandle(HANDLE value = nullptr) : value_(value) {}
    ~UniqueHandle() {
        if (value_ != nullptr) {
            CloseHandle(value_);
        }
    }
    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;
    UniqueHandle(UniqueHandle&& other) noexcept : value_(other.value_) {
        other.value_ = nullptr;
    }
    UniqueHandle& operator=(UniqueHandle&& other) noexcept {
        if (this != &other) {
            if (value_ != nullptr) {
                CloseHandle(value_);
            }
            value_ = other.value_;
            other.value_ = nullptr;
        }
        return *this;
    }
    [[nodiscard]] HANDLE get() const noexcept { return value_; }

private:
    HANDLE value_; // 被管理的句柄
};

// 返回固定持久租约路径。
[[nodiscard]] std::filesystem::path lease_path(const LabConfig& config) {
    return config.host.archive_root / L"coordination" / L"lab-lease.json";
}

// 生成稳定且不暴露配置路径的互斥名摘要。
[[nodiscard]] std::wstring mutex_name(
    const LabConfig& config,
    const std::filesystem::path& config_path) {
    const std::wstring normalized = std::filesystem::weakly_canonical(config_path).native();
    std::uint64_t hash = 1469598103934665603ULL;
    for (const wchar_t character : normalized) {
        hash ^= static_cast<std::uint64_t>(std::towlower(character));
        hash *= 1099511628211ULL;
    }
    std::wostringstream text;
    text << L"Local\\SatsumaHost-" << path_from_utf8(config.lab_id).native() << L'-'
         << std::hex << std::setw(16) << std::setfill(L'0') << hash;
    return text.str();
}

// 判断 PID 是否仍对应活动进程。
[[nodiscard]] bool process_is_alive(const std::uint32_t process_id) noexcept {
    if (process_id == 0) {
        return false;
    }
    UniqueHandle process(OpenProcess(SYNCHRONIZE, FALSE, process_id));
    return process.get() != nullptr && WaitForSingleObject(process.get(), 0) == WAIT_TIMEOUT;
}

// 在写操作前独占进程互斥。
[[nodiscard]] UniqueHandle acquire_mutex(const std::wstring& name) {
    UniqueHandle handle(CreateMutexW(nullptr, FALSE, name.c_str()));
    if (handle.get() == nullptr) {
        throw Error("CreateMutexW failed with Win32 error " + std::to_string(GetLastError()));
    }
    if (WaitForSingleObject(handle.get(), 0) != WAIT_OBJECT_0) {
        throw Error("Another SatsumaHost write session is active for this lab");
    }
    return handle;
}

// 返回共享运行是否已经产生全部规范执行结果。
[[nodiscard]] bool run_is_terminal(const LabConfig& config, const std::string& run_id) {
    const std::filesystem::path run_directory = config.shared_folder.host_root /
        L"runs" / path_from_utf8(run_id);
    if (!std::filesystem::is_regular_file(run_directory / L"task.json")) {
        return false;
    }
    const RunManifest manifest = load_run_manifest(run_directory / L"task.json");
    for (const TaskStep& step : manifest.steps) {
        if (!std::filesystem::is_regular_file(
                run_directory / L"results" / path_from_utf8(step.vm) /
                path_from_utf8(step.id) / L"execution.json")) {
            return false;
        }
    }
    return true;
}

}  // namespace

struct LabLease::State {
    UniqueHandle mutex;                 // 当前进程持有的命名互斥
    std::filesystem::path path;         // 持久租约文件
    nlohmann::json lease;               // 最近一次写入内容
    std::mutex write_mutex;             // 续租与终态写入串行化
    std::jthread renewer;                // 原子续租线程
    bool released{false};               // 是否已写入终态
};

LabLease::LabLease(std::unique_ptr<State> state) : state_(std::move(state)) {}

LabLease::~LabLease() {
    if (state_ != nullptr) {
        if (state_->renewer.joinable()) {
            state_->renewer.request_stop();
            state_->renewer.join();
        }
        ReleaseMutex(state_->mutex.get());
    }
}

std::unique_ptr<LabLease> LabLease::acquire(
    const LabConfig& config,
    const std::filesystem::path& config_path,
    const std::string& command,
    std::optional<std::string> run_id,
    const bool recovery) {
    auto state = std::make_unique<State>();
    state->mutex = acquire_mutex(mutex_name(config, config_path));
    state->path = lease_path(config);
    if (std::filesystem::is_regular_file(state->path)) {
        const nlohmann::json previous = load_json(state->path);
        if (previous.value("state", std::string{}) == "active") {
            const std::uint32_t process_id = previous.value("host_process_id", 0U);
            const std::string previous_run = previous.value("run_id", std::string{});
            if (process_is_alive(process_id)) {
                throw Error("Lab write lease is held by active Host process " +
                    std::to_string(process_id));
            }
            if (!recovery || !run_id.has_value() || previous_run != *run_id) {
                throw Error(
                    "manual_intervention_required: stale active lab lease for run " +
                    (previous_run.empty() ? std::string("<none>") : previous_run));
            }
        }
    }

    state->lease = {
        {"schema_version", 1},
        {"lab_id", config.lab_id},
        {"lease_id", make_id("lease")},
        {"host_process_id", GetCurrentProcessId()},
        {"command", command},
        {"run_id", run_id.value_or("")},
        {"state", "active"},
        {"acquired_at", utc_timestamp()},
        {"renewed_at", utc_timestamp()},
    };
    write_json_atomic(state->path, state->lease);
    State* raw_state = state.get();
    state->renewer = std::jthread([raw_state](const std::stop_token stop_token) {
        std::mutex wait_mutex;
        std::condition_variable_any condition;
        std::unique_lock wait_lock(wait_mutex);
        while (!stop_token.stop_requested()) {
            condition.wait_for(wait_lock, stop_token, kRenewInterval, [] { return false; });
            if (stop_token.stop_requested()) {
                return;
            }
            std::scoped_lock write_lock(raw_state->write_mutex);
            if (raw_state->released) {
                return;
            }
            raw_state->lease["renewed_at"] = utc_timestamp();
            write_json_atomic(raw_state->path, raw_state->lease);
        }
    });
    return std::unique_ptr<LabLease>(new LabLease(std::move(state)));
}

void LabLease::attach_run(const std::string& run_id) {
    validate_identifier(run_id, "lab lease run_id");
    std::scoped_lock lock(state_->write_mutex);
    state_->lease["run_id"] = run_id;
    state_->lease["renewed_at"] = utc_timestamp();
    write_json_atomic(state_->path, state_->lease);
}

void LabLease::release(const std::string& terminal_state) {
    if (state_->renewer.joinable()) {
        state_->renewer.request_stop();
        state_->renewer.join();
    }
    std::scoped_lock lock(state_->write_mutex);
    state_->lease["state"] = terminal_state;
    state_->lease["renewed_at"] = utc_timestamp();
    state_->lease["released_at"] = utc_timestamp();
    write_json_atomic(state_->path, state_->lease);
    state_->released = true;
}

nlohmann::json LabLease::status(const LabConfig& config) {
    const std::filesystem::path path = lease_path(config);
    if (!std::filesystem::is_regular_file(path)) {
        return {{"status", "available"}, {"lease", nullptr}};
    }
    nlohmann::json lease = load_json(path);
    const bool alive = lease.value("state", std::string{}) == "active" &&
        process_is_alive(lease.value("host_process_id", 0U));
    lease["host_process_alive"] = alive;
    return {
        {"status", lease.value("state", std::string{}) == "active"
            ? (alive ? "busy" : "manual_intervention_required")
            : "available"},
        {"lease", std::move(lease)},
    };
}

nlohmann::json LabLease::force_unlock(
    const LabConfig& config,
    const std::filesystem::path& config_path) {
    UniqueHandle mutex = acquire_mutex(mutex_name(config, config_path));
    const std::filesystem::path path = lease_path(config);
    if (!std::filesystem::is_regular_file(path)) {
        ReleaseMutex(mutex.get());
        return {{"status", "already_available"}};
    }
    nlohmann::json lease = load_json(path);
    if (lease.value("state", std::string{}) == "active" &&
        process_is_alive(lease.value("host_process_id", 0U))) {
        ReleaseMutex(mutex.get());
        throw Error("Cannot force-unlock a lease whose Host process is still active");
    }
    lease["state"] = "abandoned";
    lease["released_at"] = utc_timestamp();
    write_json_atomic(path, lease);
    ReleaseMutex(mutex.get());
    return {{"status", "unlocked"}, {"lease", std::move(lease)}};
}

nlohmann::json LabLease::finalize_run(
    const LabConfig& config,
    const std::filesystem::path& config_path,
    const std::string& run_id) {
    validate_identifier(run_id, "finalize run_id");
    UniqueHandle mutex = acquire_mutex(mutex_name(config, config_path));
    const std::filesystem::path path = lease_path(config);
    nlohmann::json lease = load_json(path);
    if (lease.value("state", std::string{}) != "active" ||
        lease.value("run_id", std::string{}) != run_id) {
        ReleaseMutex(mutex.get());
        throw Error("Active lab lease does not belong to run " + run_id);
    }
    if (process_is_alive(lease.value("host_process_id", 0U))) {
        ReleaseMutex(mutex.get());
        throw Error("Cannot finalize while the lease Host process is active");
    }
    if (!run_is_terminal(config, run_id)) {
        ReleaseMutex(mutex.get());
        throw Error("Run is not terminal and cannot release its lab lease: " + run_id);
    }
    lease["state"] = "released";
    lease["released_at"] = utc_timestamp();
    write_json_atomic(path, lease);
    ReleaseMutex(mutex.get());
    return {{"status", "finalized"}, {"run_id", run_id}};
}

}  // namespace satsuma::host
