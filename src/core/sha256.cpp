// 文件 SHA-256 计算实现。
#include "satsuma/core/sha256.hpp"

#include <array>
#include <iomanip>
#include <sstream>
#include <vector>

#include <windows.h>
#include <bcrypt.h>

#include "satsuma/core/errors.hpp"
#include "satsuma/core/path.hpp"

namespace satsuma {
namespace {

// 检查 CNG 状态并抛出统一错误。
void ensure_cng_success(const NTSTATUS status, const char* operation) {
    if (status < 0) {
        throw Error(std::string(operation) + " failed: " + std::to_string(status));
    }
}

}  // namespace

std::string sha256_file(const std::filesystem::path& path) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    HANDLE file = INVALID_HANDLE_VALUE;

    try {
        ensure_cng_success(
            BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0),
            "BCryptOpenAlgorithmProvider");

        DWORD object_size = 0;
        DWORD bytes_copied = 0;
        ensure_cng_success(
            BCryptGetProperty(
                algorithm,
                BCRYPT_OBJECT_LENGTH,
                reinterpret_cast<PUCHAR>(&object_size),
                sizeof(object_size),
                &bytes_copied,
                0),
            "BCryptGetProperty(BCRYPT_OBJECT_LENGTH)");

        DWORD hash_size = 0;
        ensure_cng_success(
            BCryptGetProperty(
                algorithm,
                BCRYPT_HASH_LENGTH,
                reinterpret_cast<PUCHAR>(&hash_size),
                sizeof(hash_size),
                &bytes_copied,
                0),
            "BCryptGetProperty(BCRYPT_HASH_LENGTH)");

        std::vector<unsigned char> hash_object(object_size);
        std::vector<unsigned char> digest(hash_size);
        ensure_cng_success(
            BCryptCreateHash(algorithm, &hash, hash_object.data(), object_size, nullptr, 0, 0),
            "BCryptCreateHash");

        file = CreateFileW(
            path.c_str(),
            GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            OPEN_EXISTING,
            FILE_FLAG_SEQUENTIAL_SCAN,
            nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            throw Error(
                "Cannot open file for hashing: " + path_to_utf8(path) +
                " (Win32 error " + std::to_string(GetLastError()) + ")");
        }

        std::array<unsigned char, 64 * 1024> buffer{};
        for (;;) {
            DWORD bytes_read = 0;
            if (!ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &bytes_read, nullptr)) {
                throw Error("ReadFile failed while hashing: " + std::to_string(GetLastError()));
            }
            if (bytes_read == 0) {
                break;
            }
            ensure_cng_success(BCryptHashData(hash, buffer.data(), bytes_read, 0), "BCryptHashData");
        }

        ensure_cng_success(BCryptFinishHash(hash, digest.data(), hash_size, 0), "BCryptFinishHash");
        CloseHandle(file);
        file = INVALID_HANDLE_VALUE;
        BCryptDestroyHash(hash);
        hash = nullptr;
        BCryptCloseAlgorithmProvider(algorithm, 0);
        algorithm = nullptr;

        std::ostringstream output;
        output << std::hex << std::setfill('0');
        for (const unsigned char byte : digest) {
            output << std::setw(2) << static_cast<unsigned int>(byte);
        }
        return output.str();
    } catch (...) {
        if (file != INVALID_HANDLE_VALUE) {
            CloseHandle(file);
        }
        if (hash != nullptr) {
            BCryptDestroyHash(hash);
        }
        if (algorithm != nullptr) {
            BCryptCloseAlgorithmProvider(algorithm, 0);
        }
        throw;
    }
}

}  // namespace satsuma
