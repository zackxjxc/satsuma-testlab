# 在 Host 发布更新后原子写入模拟的 Agent 失败终态。
if(NOT DEFINED UPDATE_ROOT OR NOT DEFINED VM_ID)
    message(FATAL_ERROR "Failed update result driver arguments are incomplete")
endif()

foreach(attempt RANGE 1 100)
    file(GLOB manifests "${UPDATE_ROOT}/${VM_ID}/*/update.json")
    list(LENGTH manifests manifest_count)
    if(manifest_count GREATER 0)
        list(GET manifests 0 manifest_path)
        file(READ "${manifest_path}" manifest_json)
        string(JSON update_id GET "${manifest_json}" update_id)
        string(JSON version GET "${manifest_json}" version)
        string(JSON manifest_vm_id GET "${manifest_json}" vm_id)
        get_filename_component(update_directory "${manifest_path}" DIRECTORY)
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
    endif()
    execute_process(COMMAND "${CMAKE_COMMAND}" -E sleep 0.1)
endforeach()

message(FATAL_ERROR "Host did not publish an Agent update before the test deadline")
