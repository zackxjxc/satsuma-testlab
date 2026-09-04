// Exercise the production transfer path through real RPC, without VMware or a Host process.
#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <windows.h>

#include "vmci_channel.hpp"
#include "satsuma/core/id.hpp"
#include "satsuma/core/json_io.hpp"
#include "satsuma/core/path.hpp"
#include "satsuma/core/sha256.hpp"

namespace {

void expect(const bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

std::string protocol_path(const std::filesystem::path& path) {
    auto value = satsuma::path_to_utf8(path);
    std::replace(value.begin(), value.end(), '\\', '/');
    return value;
}

std::filesystem::path path_at_length(std::filesystem::path root, const std::size_t length) {
    expect(length > root.native().size() + 5, "temporary test root exceeds requested path boundary");
    while (length - root.native().size() > 200) root /= std::wstring(60, L'长');
    expect(length > root.native().size() + 5, "invalid long-path test setup");
    root /= std::wstring(length - root.native().size() - 5, L'证') + L".bin";
    expect(root.native().size() == length, "test path length differs from boundary");
    return root;
}

void write_bytes(const std::filesystem::path& path, const std::vector<std::byte>& bytes) {
    std::filesystem::create_directories(satsuma::windows_file_path(path.parent_path()));
    std::ofstream output(std::filesystem::path(satsuma::windows_file_path(path)), std::ios::binary);
    output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    output.close();
    expect(static_cast<bool>(output), "cannot prepare test payload");
}

struct LockedFile {
    explicit LockedFile(const std::filesystem::path& path, const DWORD share)
        : handle(CreateFileW(satsuma::windows_file_path(path).c_str(), GENERIC_READ,
              share, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)) {
        expect(handle != INVALID_HANDLE_VALUE, "cannot lock test file");
    }
    ~LockedFile() { CloseHandle(handle); }
    LockedFile(const LockedFile&) = delete;
    LockedFile& operator=(const LockedFile&) = delete;
    HANDLE handle;
};

template<class Action>
void expect_io_error(Action action, const char* api, const std::filesystem::path& path,
    const std::initializer_list<DWORD> expected_codes = {ERROR_SHARING_VIOLATION}) {
    try {
        action();
    } catch (const std::exception& error) {
        const std::string text = error.what();
        expect(text.find(api) != std::string::npos, "file error omitted failing API: " + text);
        expect(std::any_of(expected_codes.begin(), expected_codes.end(), [&](const DWORD code) {
            return text.find("Win32 error " + std::to_string(code) + ";") != std::string::npos;
        }), "file error omitted the expected Win32 error code: " + text);
        expect(text.find("path_length_utf16=" + std::to_string(path.native().size())) != std::string::npos,
               "file error omitted actual UTF-16 path length: " + text);
        expect(text.find(satsuma::path_to_utf8(path)) != std::string::npos,
               "file error omitted actual path: " + text);
        return;
    }
    throw std::runtime_error("locked file unexpectedly accepted");
}

void run(const std::filesystem::path& root) {
    satsuma::AgentConfig config;
    config.mirror_root = root / L"mirror";
    config.lab_id = "files_lab";
    config.vm_id = "vm_files";
    config.hardware_id = "hardware_files";
    config.transport.request_timeout_ms = 2000;
    std::vector<std::byte> payload(satsuma::transport::kVmciChunkBytes + 73);
    for (std::size_t i = 0; i < payload.size(); ++i) payload[i] = static_cast<std::byte>(i % 251);
    const auto source = root / L"source.bin";
    write_bytes(source, payload);
    const auto digest = satsuma::sha256_file(source);
    const auto empty_source = root / L"empty.bin";
    write_bytes(empty_source, {});
    const auto empty_digest = satsuma::sha256_file(empty_source);

    std::vector<std::filesystem::path> targets;
    nlohmann::json descriptors = nlohmann::json::array();
    std::map<std::string, std::vector<std::byte>> downloads;
    std::map<std::string, std::vector<std::byte>> uploaded;
    std::map<std::string, std::string> hashes;
    for (const std::size_t length : {259U, 260U, 261U, 540U}) {
        const auto target = path_at_length(config.mirror_root / L"runs" / L"run_files" /
            satsuma::path_from_utf8("boundary-" + std::to_string(length)), length);
        const auto relative = protocol_path(target.lexically_relative(config.mirror_root));
        const bool empty = length == 261;
        targets.push_back(target);
        downloads[relative] = empty ? std::vector<std::byte>{} : payload;
        hashes[relative] = empty ? empty_digest : digest;
        descriptors.push_back({{"path", relative}, {"size", downloads.at(relative).size()},
                               {"sha256", hashes.at(relative)}});
    }

    std::atomic_size_t download_requests{0};
    std::mutex uploaded_mutex;
    const std::string endpoint = "tcp://127.0.0.1:43914";
    satsuma::transport::Server server(endpoint, [&](const satsuma::transport::Message& request) {
        satsuma::transport::Message response;
        const auto operation = request.metadata.at("operation").get<std::string>();
        if (operation == "index") {
            response.metadata = {{"files", descriptors}, {"complete", true}};
        } else if (operation == "download") {
            ++download_requests;
            const auto path = request.metadata.at("path").get<std::string>();
            const auto& bytes = downloads.at(path);
            const auto offset = request.metadata.at("offset").get<std::size_t>();
            const auto count = std::min(bytes.size() - offset, satsuma::transport::kVmciChunkBytes);
            response.payload.assign(bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                bytes.begin() + static_cast<std::ptrdiff_t>(offset + count));
            response.metadata = {{"offset", offset}, {"total_size", bytes.size()},
                {"sha256", hashes.at(path)}, {"eof", offset + count == bytes.size()}};
        } else if (operation == "upload") {
            const std::lock_guard lock(uploaded_mutex);
            const auto path = request.metadata.at("path").get<std::string>();
            auto& bytes = uploaded[path];
            const auto offset = request.metadata.at("offset").get<std::size_t>();
            if (offset == 0) bytes.clear();
            expect(offset == bytes.size(), "upload chunk offset skipped data");
            bytes.insert(bytes.end(), request.payload.begin(), request.payload.end());
            response.metadata = {{"next_offset", bytes.size()}};
        } else if (operation == "result_publish") {
            response.metadata = {{"status", "published"}};
        } else {
            throw std::runtime_error("unexpected file transfer operation");
        }
        return response;
    }, std::chrono::milliseconds(20));
    std::jthread listener([&](const std::stop_token stop) { server.run(stop); });
    satsuma::vm::VmciChannel channel(config, "session_files", endpoint);
    channel.synchronize_inbound();
    const auto long_metadata = targets.back().parent_path() / L"claim.json";
    satsuma::write_json_atomic_existing_parent(long_metadata, {{"long_path", true}});
    expect(satsuma::load_json(long_metadata).at("long_path") == true,
           "existing-parent JSON write rejected a long directory");
    const auto requests_after_download = download_requests.load();
    channel.synchronize_inbound();
    expect(download_requests == requests_after_download, "unchanged download was not cached");
    satsuma::StepClaimLease owner{};
    owner.run_id = "run_files";
    owner.vm_id = "vm_files";
    owner.step_id = "transfer";
    owner.job_id = "job_files";
    const auto canonical = [&](const std::filesystem::path& target) {
        return config.mirror_root / L"runs" / L"run_files" / L"results" / L"vm_files" /
            L"transfer" / L"files" / target.filename();
    };
    for (const auto& target : targets) {
        const auto relative = protocol_path(target.lexically_relative(config.mirror_root));
        expect(satsuma::sha256_file(target) == hashes.at(relative), "long download hash changed");
        {
            const std::lock_guard lock(uploaded_mutex);
            uploaded.clear();
        }
        const auto published = channel.publish_result(owner, {{"status", "exited"}}, {{target, canonical(target)}});
        expect(published == satsuma::vm::StepResultPublishStatus::Published, "long upload did not publish");
        // The local-to-protocol staging layout may change; assert payload bytes, not directory names.
        const std::lock_guard lock(uploaded_mutex);
        expect(uploaded.size() == 1 && uploaded.begin()->second == downloads.at(relative),
               "uploaded bytes differ at the long-path boundary");
    }

    {
        const auto& target = targets.back();
        const LockedFile locked(target, 0);
        expect_io_error([&] { static_cast<void>(channel.publish_result(owner, {}, {{target, canonical(target)}})); },
            "CreateFileW(VMCI transfer)", target);
    }
    {
        const auto& target = targets.back();
        const std::vector<std::byte> original{std::byte{'o'}, std::byte{'l'}, std::byte{'d'}};
        write_bytes(target, original);
        const auto original_hash = satsuma::sha256_file(target);
        const LockedFile locked(target, FILE_SHARE_READ);
        expect_io_error([&] { channel.synchronize_inbound(); }, "MoveFileExW(VMCI file publish)", target,
            {ERROR_ACCESS_DENIED, ERROR_SHARING_VIOLATION});
        expect(satsuma::sha256_file(target) == original_hash, "failed publish modified existing download");
        for (const auto& item : std::filesystem::directory_iterator(
                 satsuma::windows_file_path(target.parent_path()))) {
            expect(item.path().extension() != L".part", "failed download retained staged partial file");
        }
    }
    channel.synchronize_inbound();
    expect(satsuma::sha256_file(targets.back()) == digest, "download did not recover after unlock");
}

}  // namespace

int main() {
    const auto root = std::filesystem::temp_directory_path() /
        satsuma::path_from_utf8(satsuma::make_id("satsuma-vmci-files"));
    int exit_code = 0;
    try {
        run(root);
        std::cout << "SatsumaVmciFileTests passed\n";
    } catch (const std::exception& error) {
        std::cerr << "SatsumaVmciFileTests failed: " << error.what() << '\n';
        exit_code = 1;
    }
    std::error_code cleanup_error;
    std::filesystem::remove_all(satsuma::windows_file_path(root), cleanup_error);
    return exit_code;
}
