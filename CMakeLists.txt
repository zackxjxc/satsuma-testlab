# Satsuma TestLab 的顶层构建配置。
cmake_minimum_required(VERSION 3.25)

project(SatsumaTestLab VERSION 0.1.0 LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "Satsuma 0.1.0 currently supports Windows only")
endif()

option(SATSUMA_BUILD_TESTS "Build Satsuma tests" ON)
option(SATSUMA_ENABLE_REAL_VMWARE_SMOKE "Expose the destructive real VMware smoke target" OFF)
set(SATSUMA_REAL_LAB_CONFIG "" CACHE FILEPATH "lab.json used by the real VMware smoke target")
set(SATSUMA_REAL_VM_ID "" CACHE STRING "VM ID used by the real VMware smoke target")
set(SATSUMA_REAL_SNAPSHOT "" CACHE STRING "Snapshot restored by the real VMware smoke target")
set(SATSUMA_REAL_VMWARE_CONFIRM "" CACHE STRING "Explicit confirmation for destructive VMware smoke")

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

# coro_rpc 和 struct_pack 固定使用 yalantinglibs 0.6.1 正式版本。
find_package(yalantinglibs CONFIG QUIET)
if(yalantinglibs_FOUND)
    set(SATSUMA_YLT_TARGET yalantinglibs::yalantinglibs)
else()
    set(YLT_ENABLE_DL OFF CACHE BOOL "Disable dynamic loader integration" FORCE)
    set(YLT_ENABLE_SSL OFF CACHE BOOL "Disable TLS until authentication is implemented" FORCE)
    FetchContent_Declare(
        satsuma_yalantinglibs
        GIT_REPOSITORY https://github.com/alibaba/yalantinglibs.git
        GIT_TAG 0.6.1
        GIT_SHALLOW TRUE
        GIT_PROGRESS TRUE
    )
    FetchContent_MakeAvailable(satsuma_yalantinglibs)
    set(SATSUMA_YLT_TARGET yalantinglibs)
endif()

add_library(SatsumaCore STATIC
    src/core/config.cpp
    src/core/endpoint.cpp
    src/core/id.cpp
    src/core/json_io.cpp
    src/core/lifecycle.cpp
    src/core/path.cpp
    src/core/rpc_protocol.cpp
    src/core/sha256.cpp
    src/core/snapshot.cpp
    src/core/task.cpp
    src/core/windows_command_line.cpp
)

target_include_directories(SatsumaCore PUBLIC include)
target_compile_features(SatsumaCore PUBLIC cxx_std_20)
target_compile_definitions(
    SatsumaCore
    PUBLIC UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00 WINVER=0x0A00
)
target_link_libraries(SatsumaCore PUBLIC nlohmann_json::nlohmann_json PRIVATE bcrypt)

add_executable(SatsumaHost
    src/host/controller.cpp
    src/host/diagnostics.cpp
    src/host/main.cpp
    src/host/orchestrator.cpp
    src/host/rpc_server.cpp
    src/host/rpc_service.cpp
    src/vmware/vmrun_provider.cpp
)
target_include_directories(SatsumaHost PRIVATE src/host src/vmware)
target_link_libraries(SatsumaHost PRIVATE SatsumaCore ${SATSUMA_YLT_TARGET})

add_executable(SatsumaVM
    src/vm/agent.cpp
    src/vm/autostart.cpp
    src/vm/main.cpp
    src/vm/process_runner.cpp
    src/vm/rpc_client.cpp
)
target_include_directories(SatsumaVM PRIVATE src/vm)
target_link_libraries(
    SatsumaVM
    PRIVATE SatsumaCore ${SATSUMA_YLT_TARGET} advapi32 ole32 oleaut32 taskschd ws2_32
)

set_target_properties(SatsumaHost SatsumaVM PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

# 独立示例软件不链接 SatsumaCore，用于真实 Artifact 执行验收。
add_executable(SatsumaDemoApp examples/demo_app.cpp)
target_compile_features(SatsumaDemoApp PRIVATE cxx_std_20)
target_link_libraries(SatsumaDemoApp PRIVATE nlohmann_json::nlohmann_json)
set_target_properties(SatsumaDemoApp PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

# 为每个构建配置生成包含真实绝对 Artifact 路径的任务文件。
set(SATSUMA_DEMO_INPUT "${CMAKE_CURRENT_SOURCE_DIR}/examples/demo-input.txt")
set(SATSUMA_DEMO_TASK [=[
{
  "schema_version": 1,
  "name": "demo-app-real-vm",
  "artifacts": [
    {
      "source": "$<TARGET_FILE:SatsumaDemoApp>",
      "vm": "client",
      "shared_destination": "artifacts/client/SatsumaDemoApp.exe"
    },
    {
      "source": "@DEMO_INPUT@",
      "vm": "client",
      "shared_destination": "artifacts/client/demo-input.txt"
    }
  ],
  "steps": [
    {
      "id": "run_demo_app",
      "vm": "client",
      "type": "execute",
      "program": "artifacts/client/SatsumaDemoApp.exe",
      "arguments": [
        "--input", "artifacts/client/demo-input.txt",
        "--result", "output/demo-result.json",
        "--transformed", "output/transformed.txt",
        "--label", "real-vm-artifact-test",
        "--emit-warning"
      ],
      "timeout_seconds": 30,
      "collect_files": ["output/demo-result.json", "output/transformed.txt"]
    }
  ]
}
]=])
string(REPLACE "@DEMO_INPUT@" "${SATSUMA_DEMO_INPUT}" SATSUMA_DEMO_TASK "${SATSUMA_DEMO_TASK}")
file(GENERATE
    OUTPUT "${CMAKE_BINARY_DIR}/examples/demo-app-task-$<CONFIG>.json"
    CONTENT "${SATSUMA_DEMO_TASK}"
)

if(MSVC)
    foreach(target SatsumaCore SatsumaHost SatsumaVM SatsumaDemoApp)
        target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8 /EHsc)
    endforeach()
    target_compile_options(SatsumaHost PRIVATE /external:anglebrackets /external:W0)
    target_compile_options(SatsumaVM PRIVATE /external:anglebrackets /external:W0)
endif()

if(SATSUMA_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
