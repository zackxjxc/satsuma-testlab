#include <Windows.h>

#include <array>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

struct ExpectedExecutable {
    std::wstring_view path;
    std::wstring_view original_filename;
    std::wstring_view description;
};

class VersionResource {
public:
    explicit VersionResource(const std::wstring& path) {
        DWORD ignored = 0;
        const DWORD size = GetFileVersionInfoSizeW(path.c_str(), &ignored);
        if (size == 0) {
            throw std::runtime_error("GetFileVersionInfoSizeW failed");
        }

        data_.resize(size);
        if (!GetFileVersionInfoW(path.c_str(), 0, size, data_.data())) {
            throw std::runtime_error("GetFileVersionInfoW failed");
        }
    }

    [[nodiscard]] const VS_FIXEDFILEINFO& fixed_info() const {
        void* value = nullptr;
        UINT size = 0;
        if (!VerQueryValueW(data_.data(), L"\\", &value, &size) ||
            value == nullptr || size < sizeof(VS_FIXEDFILEINFO)) {
            throw std::runtime_error("fixed version information is missing");
        }
        return *static_cast<const VS_FIXEDFILEINFO*>(value);
    }

    [[nodiscard]] std::wstring string_value(std::wstring_view key) const {
        const std::wstring query = L"\\StringFileInfo\\040904B0\\" + std::wstring(key);
        void* value = nullptr;
        UINT size = 0;
        if (!VerQueryValueW(data_.data(), query.c_str(), &value, &size) ||
            value == nullptr || size == 0) {
            throw std::runtime_error("version string is missing");
        }
        return static_cast<const wchar_t*>(value);
    }

private:
    std::vector<unsigned char> data_;
};

[[nodiscard]] std::array<DWORD, 4> file_version(const VS_FIXEDFILEINFO& info) {
    return {
        HIWORD(info.dwFileVersionMS),
        LOWORD(info.dwFileVersionMS),
        HIWORD(info.dwFileVersionLS),
        LOWORD(info.dwFileVersionLS),
    };
}

[[nodiscard]] std::array<DWORD, 4> product_version(const VS_FIXEDFILEINFO& info) {
    return {
        HIWORD(info.dwProductVersionMS),
        LOWORD(info.dwProductVersionMS),
        HIWORD(info.dwProductVersionLS),
        LOWORD(info.dwProductVersionLS),
    };
}

bool expect_equal(
    std::wstring_view executable,
    std::wstring_view field,
    std::wstring_view actual,
    std::wstring_view expected
) {
    if (actual == expected) {
        return true;
    }
    std::wcerr << executable << L": " << field << L" should be '" << expected
               << L"' but was '" << actual << L"'\n";
    return false;
}

bool verify_executable(
    const ExpectedExecutable& executable,
    const std::array<DWORD, 4>& expected_numeric_version,
    std::wstring_view expected_display_version
) {
    try {
        const VersionResource resource{std::wstring(executable.path)};
        const auto& fixed = resource.fixed_info();
        bool valid = true;

        if (file_version(fixed) != expected_numeric_version) {
            std::wcerr << executable.path << L": fixed file version is incorrect\n";
            valid = false;
        }
        if (product_version(fixed) != expected_numeric_version) {
            std::wcerr << executable.path << L": fixed product version is incorrect\n";
            valid = false;
        }

        valid &= expect_equal(
            executable.path,
            L"FileVersion",
            resource.string_value(L"FileVersion"),
            expected_display_version
        );
        valid &= expect_equal(
            executable.path,
            L"ProductVersion",
            resource.string_value(L"ProductVersion"),
            expected_display_version
        );
        valid &= expect_equal(
            executable.path,
            L"ProductName",
            resource.string_value(L"ProductName"),
            L"Satsuma TestLab"
        );
        valid &= expect_equal(
            executable.path,
            L"OriginalFilename",
            resource.string_value(L"OriginalFilename"),
            executable.original_filename
        );
        valid &= expect_equal(
            executable.path,
            L"FileDescription",
            resource.string_value(L"FileDescription"),
            executable.description
        );
        return valid;
    } catch (const std::exception& error) {
        std::wcerr << executable.path << L": " << error.what() << L'\n';
        return false;
    }
}

} // namespace

int wmain(int argc, wchar_t* argv[]) {
    if (argc != 6) {
        std::cerr << "usage: SatsumaVersionInfoTests <major> <minor> <patch> <host> <vm>\n";
        return 2;
    }

    const std::array<DWORD, 4> expected_numeric_version{
        std::stoul(argv[1]),
        std::stoul(argv[2]),
        std::stoul(argv[3]),
        0,
    };
    const std::wstring expected_display_version =
        std::wstring(argv[1]) + L'.' + argv[2] + L'.' + argv[3];
    const std::array<ExpectedExecutable, 2> executables{{
        {argv[4], L"SatsumaHost.exe", L"Satsuma TestLab Host"},
        {argv[5], L"SatsumaVM.exe", L"Satsuma TestLab Guest Agent"},
    }};

    bool valid = true;
    for (const auto& executable : executables) {
        valid &= verify_executable(executable, expected_numeric_version, expected_display_version);
    }
    return valid ? 0 : 1;
}
