# 驱动测试 Agent，直到 Host 生命周期状态进入终态。
if(NOT DEFINED VM_EXE OR NOT DEFINED AGENT_CONFIG OR NOT DEFINED LIFECYCLE_STATE)
    message(FATAL_ERROR "Agent lifecycle driver arguments are incomplete")
endif()

foreach(attempt RANGE 1 300)
    if(DEFINED STATE_ROOT AND INJECT_ATOMIC_JSON_TEMPORARY)
        file(GLOB published_manifests "${STATE_ROOT}/runs/*/task.json")
        foreach(published_manifest IN LISTS published_manifests)
            get_filename_component(run_directory "${published_manifest}" DIRECTORY)
            file(MAKE_DIRECTORY "${run_directory}/state")
            file(WRITE "${run_directory}/state/.tmp-write-regression" "partial JSON")
        endforeach()
    endif()

    execute_process(
        COMMAND "${VM_EXE}" --config "${AGENT_CONFIG}" --once
        RESULT_VARIABLE vm_result
        OUTPUT_VARIABLE vm_output
        ERROR_VARIABLE vm_error
    )
    if(NOT vm_result EQUAL 0)
        message(FATAL_ERROR "SatsumaVM lifecycle run failed: ${vm_error}\n${vm_output}")
    endif()

    if(EXISTS "${LIFECYCLE_STATE}")
        execute_process(
            COMMAND "${CMAKE_COMMAND}" -E cat "${LIFECYCLE_STATE}"
            RESULT_VARIABLE lifecycle_read_result
            OUTPUT_VARIABLE lifecycle_json
            ERROR_QUIET
        )
        if(lifecycle_read_result EQUAL 0 AND lifecycle_json MATCHES
           "\"phase\": \"(completed|failed|recovery_failed|manual_intervention_required)\"")
            return()
        endif()
    endif()
    execute_process(COMMAND "${CMAKE_COMMAND}" -E sleep 0.1)
endforeach()

message(FATAL_ERROR "Host lifecycle did not reach a terminal phase")
