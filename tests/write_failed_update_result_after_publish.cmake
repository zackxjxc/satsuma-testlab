# 在 Host 发布更新后原子写入模拟的 Agent 失败终态。
if(NOT DEFINED UPDATE_ROOT OR NOT DEFINED VM_ID)
    message(FATAL_ERROR "Failed update result driver arguments are incomplete")
endif()

foreach(attempt RANGE 1 300)
    file(GLOB manifests "${UPDATE_ROOT}/${VM_ID}/*/update.json")
    foreach(manifest_path IN LISTS manifests)
        get_filename_component(update_directory "${manifest_path}" DIRECTORY)
        get_filename_component(update_directory_name "${update_directory}" NAME)
        if(update_directory_name MATCHES "^\\.")
            continue()
        endif()

        file(READ "${manifest_path}" manifest_json)
        string(JSON update_id GET "${manifest_json}" update_id)
        string(JSON version GET "${manifest_json}" version)
        string(JSON manifest_vm_id GET "${manifest_json}" vm_id)
        set(result_json [=[
{
  "schema_version": 1,
  "update_id": "@UPDATE_ID@",
  "vm_id": "@VM_ID@",
  "version": "@VERSION@",
  "status": "failed",
  "rollback_status": "succeeded",
  "process_id": 0,
  "error": "injected terminal update failure",
  "completed_at": "2026-07-31T00:00:00.000Z"
}
]=])
        string(REPLACE "@UPDATE_ID@" "${update_id}" result_json "${result_json}")
        string(REPLACE "@VM_ID@" "${manifest_vm_id}" result_json "${result_json}")
        string(REPLACE "@VERSION@" "${version}" result_json "${result_json}")
        file(WRITE "${update_directory}/.result.test.tmp" "${result_json}")
        file(RENAME
            "${update_directory}/.result.test.tmp"
            "${update_directory}/result.json")
        return()
    endforeach()
    execute_process(COMMAND "${CMAKE_COMMAND}" -E sleep 0.1)
endforeach()

message(FATAL_ERROR "Host did not publish an Agent update before the test deadline")
