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
