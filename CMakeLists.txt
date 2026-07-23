# Satsuma TestLab 的顶层构建配置。
cmake_minimum_required(VERSION 3.25)

project(SatsumaTestLab VERSION 0.1.0 LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "Satsuma 0.1.0 currently supports Windows only")
endif()

option(SATSUMA_BUILD_TESTS "Build Satsuma tests" ON)

include(FetchContent)

# 优先复用包管理器提供的依赖；未提供时下载固定版本。
find_package(nlohmann_json 3.11 CONFIG QUIET)
if(NOT nlohmann_json_FOUND)
    FetchContent_Declare(
        nlohmann_json
        GIT_REPOSITORY https://github.com/nlohmann/json.git
        GIT_TAG v3.12.0
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(nlohmann_json)
endif()

add_library(SatsumaCore STATIC
    src/core/config.cpp
    src/core/id.cpp
    src/core/json_io.cpp
    src/core/path.cpp
    src/core/sha256.cpp
    src/core/task.cpp
)

target_include_directories(SatsumaCore PUBLIC include)
target_compile_features(SatsumaCore PUBLIC cxx_std_20)
target_compile_definitions(SatsumaCore PUBLIC UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(SatsumaCore PUBLIC nlohmann_json::nlohmann_json PRIVATE bcrypt)

add_executable(SatsumaHost
    src/host/controller.cpp
    src/host/main.cpp
)
target_include_directories(SatsumaHost PRIVATE src/host)
target_link_libraries(SatsumaHost PRIVATE SatsumaCore)

add_executable(SatsumaVM
    src/vm/agent.cpp
    src/vm/main.cpp
    src/vm/process_runner.cpp
)
target_include_directories(SatsumaVM PRIVATE src/vm)
target_link_libraries(SatsumaVM PRIVATE SatsumaCore)

set_target_properties(SatsumaHost SatsumaVM PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

if(MSVC)
    foreach(target SatsumaCore SatsumaHost SatsumaVM)
        target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8 /EHsc)
    endforeach()
endif()

if(SATSUMA_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
