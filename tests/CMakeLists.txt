# Satsuma 核心单元测试和 Host/VM 集成测试。
add_executable(SatsumaCoreTests core_tests.cpp)
target_link_libraries(SatsumaCoreTests PRIVATE SatsumaCore)

add_executable(SatsumaTestFixture fixture.cpp)
target_compile_features(SatsumaTestFixture PRIVATE cxx_std_20)
set_target_properties(SatsumaTestFixture PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

if(MSVC)
    target_compile_options(SatsumaCoreTests PRIVATE /W4 /permissive- /utf-8 /EHsc)
    target_compile_options(SatsumaTestFixture PRIVATE /W4 /permissive- /utf-8 /EHsc)
endif()

add_test(NAME SatsumaCoreTests COMMAND SatsumaCoreTests)
add_test(
    NAME SatsumaHostVmFlow
    COMMAND "${CMAKE_COMMAND}"
        "-DHOST_EXE=$<TARGET_FILE:SatsumaHost>"
        "-DVM_EXE=$<TARGET_FILE:SatsumaVM>"
        "-DFIXTURE_EXE=$<TARGET_FILE:SatsumaTestFixture>"
        "-DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/host-vm-flow"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/host_vm_flow.cmake"
)
