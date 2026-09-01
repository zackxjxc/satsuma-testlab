# 在主任务发布后注入过期危险 claim，并驱动 Agent 发布人工门禁。
if(NOT DEFINED VM_EXE OR
   NOT DEFINED AGENT_CONFIG OR
   NOT DEFINED STATE_ROOT OR
   NOT DEFINED RUN_ID OR
   NOT DEFINED LIFECYCLE_STATE)
    message(FATAL_ERROR "Expired claim injection arguments are incomplete")
endif()

set(run_directory "${STATE_ROOT}/runs/${RUN_ID}")
set(claim_path "${run_directory}/state/vm_01/main_echo.claim.json")
set(claim_injected FALSE)
foreach(attempt RANGE 1 300)
    if(EXISTS "${run_directory}/task.json" AND NOT claim_injected)
        file(MAKE_DIRECTORY "${run_directory}/state/vm_01")
        file(WRITE "${claim_path}" [=[
{
  "schema_version": 3,
  "run_id": "@RUN_ID@",
  "vm_id": "vm_01",
  "step_id": "main_echo",
  "job_id": "job_old",
  "session_id": "session_old",
  "boot_id": "boot_old",
  "claimed_at": "2026-07-26T00:00:00.000Z",
  "claimed_unix_ms": 1000,
  "last_renewed_at": "2026-07-26T00:00:00.000Z",
  "last_renewed_unix_ms": 1000,
  "lease_expires_unix_ms": 2000,
  "renewal_sequence": 0,
  "retry_safe": false,
  "attempt": 1
}
]=])
        file(READ "${claim_path}" claim_json)
        string(REPLACE "@RUN_ID@" "${RUN_ID}" claim_json "${claim_json}")
        file(WRITE "${claim_path}" "${claim_json}")
        set(claim_injected TRUE)
    endif()

    execute_process(
        COMMAND "${VM_EXE}" --config "${AGENT_CONFIG}" --once
        RESULT_VARIABLE vm_result
        OUTPUT_VARIABLE vm_output
        ERROR_VARIABLE vm_error
    )
    if(NOT vm_result EQUAL 0)
        message(FATAL_ERROR "SatsumaVM claim injection run failed: ${vm_error}\n${vm_output}")
    endif()
    if(EXISTS "${LIFECYCLE_STATE}")
        execute_process(
            COMMAND "${CMAKE_COMMAND}" -E cat "${LIFECYCLE_STATE}"
            RESULT_VARIABLE lifecycle_read_result
            OUTPUT_VARIABLE lifecycle_json
            ERROR_QUIET
        )
        if(lifecycle_read_result EQUAL 0 AND
           lifecycle_json MATCHES "\"phase\": \"manual_intervention_required\"")
            return()
        endif()
    endif()
    execute_process(COMMAND "${CMAKE_COMMAND}" -E sleep 0.1)
endforeach()

message(FATAL_ERROR "Injected claim did not produce a lifecycle manual gate")
