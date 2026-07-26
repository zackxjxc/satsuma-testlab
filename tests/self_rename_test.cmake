# 验证运行中候选可完成 old -> bak、new -> formal 和成功清理。
if(NOT DEFINED FIXTURE_EXE OR
   NOT DEFINED TEST_ROOT)
    message(FATAL_ERROR "self rename test arguments are incomplete")
endif()

file(REMOVE_RECURSE "${TEST_ROOT}")
file(MAKE_DIRECTORY "${TEST_ROOT}")
set(new_executable "${TEST_ROOT}/SatsumaVM.new.exe")
set(formal_executable "${TEST_ROOT}/SatsumaVM.exe")
set(backup_executable "${TEST_ROOT}/SatsumaVM.bak.exe")
file(COPY_FILE "${FIXTURE_EXE}" "${new_executable}" ONLY_IF_DIFFERENT)
file(WRITE "${formal_executable}" "old-agent")
file(SHA256 "${new_executable}" expected_hash)

execute_process(
    COMMAND "${new_executable}" --replace-self
        "${formal_executable}" "${backup_executable}"
    RESULT_VARIABLE rename_result
    OUTPUT_VARIABLE rename_output
    ERROR_VARIABLE rename_error
    TIMEOUT 10
)
if(NOT rename_result EQUAL 0)
    message(FATAL_ERROR
        "running executable could not rename itself (${rename_result}): "
        "${rename_output}${rename_error}")
endif()
if(EXISTS "${new_executable}" OR
   EXISTS "${backup_executable}" OR
   NOT EXISTS "${formal_executable}")
    message(FATAL_ERROR
        "self replacement did not publish one clean formal executable")
endif()
file(SHA256 "${formal_executable}" actual_hash)
if(NOT actual_hash STREQUAL expected_hash)
    message(FATAL_ERROR "self rename changed the executable content")
endif()
