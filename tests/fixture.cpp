// Host/VM 集成测试使用的无害被测程序。
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

// 解析测试参数、输出日志并生成声明的结果文件。
int main(const int argc, char* argv[]) {
    try {
        std::string message = "fixture-ok";
        std::filesystem::path output_path;
        std::filesystem::path ready_path;  // 进程进入测试主体后的同步标记
        std::filesystem::path rename_self_path;  // 更新助手自重命名探针目标
        std::filesystem::path replace_backup_path;  // 完整更新探针使用的旧版备份
        int sleep_ms = 0;
        for (int index = 1; index < argc; ++index) {
            const std::string argument = argv[index];
            if (argument == "--message" && index + 1 < argc) {
                message = argv[++index];
            } else if (argument == "--output" && index + 1 < argc) {
                output_path = argv[++index];
            } else if (argument == "--ready-file" && index + 1 < argc) {
                ready_path = argv[++index];
            } else if (argument == "--sleep-ms" && index + 1 < argc) {
                sleep_ms = std::stoi(argv[++index]);
            } else if (argument == "--rename-self" && index + 1 < argc) {
                rename_self_path = argv[++index];
            } else if (argument == "--replace-self" && index + 2 < argc) {
                rename_self_path = argv[++index];
                replace_backup_path = argv[++index];
            } else {
                throw std::runtime_error("unknown fixture argument: " + argument);
            }
        }

        if (!ready_path.empty()) {
            if (!ready_path.parent_path().empty()) {
                std::filesystem::create_directories(ready_path.parent_path());
            }
            std::ofstream ready(ready_path, std::ios::binary);
            ready << "ready\n";
            if (!ready) {
                throw std::runtime_error("failed to write fixture ready marker");
            }
        }
        if (sleep_ms > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));
        }
        if (!rename_self_path.empty()) {
            if (!replace_backup_path.empty()) {
                std::filesystem::rename(
                    rename_self_path,
                    replace_backup_path);
            }
            std::filesystem::rename(
                std::filesystem::absolute(argv[0]),
                rename_self_path);
            if (!replace_backup_path.empty()) {
                std::filesystem::remove(replace_backup_path);
            }
        }
        std::cout << message << '\n';
        if (!output_path.empty()) {
            std::filesystem::create_directories(output_path.parent_path());
            std::ofstream output(output_path, std::ios::binary);
            output << "{\"status\":\"passed\",\"message\":\"" << message << "\"}\n";
            if (!output) {
                throw std::runtime_error("failed to write fixture output");
            }
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "SatsumaTestFixture error: " << error.what() << '\n';
        return 1;
    }
}
