# 在本机文件通道和专用 Fake vmrun 上验证双 VM 生命周期编排。
if(NOT DEFINED HOST_EXE OR
   NOT DEFINED VM_EXE OR
   NOT DEFINED FIXTURE_EXE OR
   NOT DEFINED VMRUN_EXE OR
   NOT DEFINED TEST_ROOT)
    message(FATAL_ERROR "Multi-VM lifecycle integration arguments are incomplete")
endif()

# 每次调用使用独立根目录，允许 Debug、Release 或开发者并发运行本测试。
string(RANDOM LENGTH 16 ALPHABET 0123456789abcdef invocation_id)
set(TEST_ROOT "${TEST_ROOT}/run-${invocation_id}")
file(MAKE_DIRECTORY "${TEST_ROOT}")
file(TO_CMAKE_PATH "${FIXTURE_EXE}" fixture_path)
file(TO_CMAKE_PATH "${VMRUN_EXE}" vmrun_path)

# 要求日志中的两个命令都存在且保持指定先后顺序。
function(require_log_order log_text first_command second_command context)
    string(FIND "${log_text}" "${first_command}" first_position)
    string(FIND "${log_text}" "${second_command}" second_position)
    if(first_position EQUAL -1 OR second_position EQUAL -1 OR
       NOT first_position LESS second_position)
        message(FATAL_ERROR
            "${context} command order is invalid: ${first_command} -> ${second_command}\n${log_text}")
    endif()
endfunction()

# 执行一个隔离场景并核对双 VM manifest、业务结果和逆序清理。
function(run_multi_vm_scenario name expected_exit expected_status gateway_step
         expected_successful expected_failed fail_gateway_cleanup)
    set(root "${TEST_ROOT}/${name}")
    set(share "${root}/share")
    set(archive "${root}/archive")
    set(client_local "${root}/client-local")
    set(gateway_local "${root}/gateway-local")
    file(MAKE_DIRECTORY
        "${share}"
        "${archive}"
        "${client_local}"
        "${gateway_local}")
    file(TO_CMAKE_PATH "${share}" share_path)
    file(TO_CMAKE_PATH "${archive}" archive_path)
    file(TO_CMAKE_PATH "${client_local}" client_local_path)
    file(TO_CMAKE_PATH "${gateway_local}" gateway_local_path)
    file(TO_CMAKE_PATH "${root}/Client VM.vmx" client_vmx_path)
    file(TO_CMAKE_PATH "${root}/Gateway VM.vmx" gateway_vmx_path)
    file(WRITE "${client_vmx_path}" "# Client test VMX\n")
    file(WRITE "${gateway_vmx_path}" "# Gateway test VMX\n")

    set(lab_json [=[
{
  "schema_version": 1,
  "lab_id": "multi_vm_lab",
  "provider": {"type": "vmware_workstation", "vmrun": "@VMRUN@"},
  "host": {"listen": "127.0.0.1:37100", "archive_root": "@ARCHIVE@"},
  "shared_folder": {"host_root": "@SHARE@", "guest_root": "@SHARE@"},
  "vms": [
    {
      "id": "client",
      "role": "client",
      "vmx": "@CLIENT_VMX@",
      "agent_version": "0.1.0",
      "snapshots": {"base": "clean", "ai_prefix": "satsuma-ai-", "max_ai_snapshots": 8}
    },
    {
      "id": "gateway",
      "role": "gateway",
      "vmx": "@GATEWAY_VMX@",
      "agent_version": "0.1.0",
      "snapshots": {"base": "clean", "ai_prefix": "satsuma-ai-", "max_ai_snapshots": 8}
    }
  ]
}
]=])
    string(REPLACE "@VMRUN@" "${vmrun_path}" lab_json "${lab_json}")
    string(REPLACE "@ARCHIVE@" "${archive_path}" lab_json "${lab_json}")
    string(REPLACE "@SHARE@" "${share_path}" lab_json "${lab_json}")
    string(REPLACE "@CLIENT_VMX@" "${client_vmx_path}" lab_json "${lab_json}")
    string(REPLACE "@GATEWAY_VMX@" "${gateway_vmx_path}" lab_json "${lab_json}")
    file(WRITE "${root}/lab.json" "${lab_json}")

    set(agent_json [=[
{
  "schema_version": 1,
  "protocol_version": 2,
  "lab_id": "multi_vm_lab",
  "vm_id": "@VM_ID@",
  "agent_version": "0.1.0",
  "host": "127.0.0.1:37100",
  "shared_root": "@SHARE@",
  "local_work_root": "@LOCAL@",
  "poll_interval_ms": 100,
  "reconnect_interval_ms": 100,
  "rpc_timeout_ms": 1000
}
]=])
    string(REPLACE "@SHARE@" "${share_path}" client_agent_json "${agent_json}")
    string(REPLACE "@LOCAL@" "${client_local_path}" client_agent_json "${client_agent_json}")
    string(REPLACE "@VM_ID@" "client" client_agent_json "${client_agent_json}")
    file(WRITE "${root}/client-agent.json" "${client_agent_json}")
    string(REPLACE "@SHARE@" "${share_path}" gateway_agent_json "${agent_json}")
    string(REPLACE "@LOCAL@" "${gateway_local_path}" gateway_agent_json "${gateway_agent_json}")
    string(REPLACE "@VM_ID@" "gateway" gateway_agent_json "${gateway_agent_json}")
    file(WRITE "${root}/gateway-agent.json" "${gateway_agent_json}")

    set(run_id "multi_vm_${name}")
    set(plan_json [=[
{
  "schema_version": 1,
  "name": "multi VM lifecycle @NAME@",
  "run_id": "@RUN_ID@",
  "artifacts": [
    {
      "source": "@FIXTURE@",
      "vm": "gateway",
      "shared_destination": "artifacts/gateway/SatsumaTestFixture.exe"
    }
  ],
  "steps": [
    {"id": "client_step", "vm": "client", "type": "echo", "message": "client completed"},
    @GATEWAY_STEP@
  ],
  "lifecycle": {
    "vms": [
      {
        "vm": "client",
        "on_success": {"action": "stop"},
        "on_failure": {"action": "restore", "snapshot": "clean"}
      },
      {
        "vm": "gateway",
        "on_success": {"action": "stop"},
        "on_failure": {"action": "restore", "snapshot": "clean"}
      }
    ],
    "finally": [
      {"id": "client_finally", "vm": "client", "type": "echo", "message": "client cleanup"},
      {"id": "gateway_finally", "vm": "gateway", "type": "echo", "message": "gateway cleanup"}
    ]
  }
}
]=])
    string(REPLACE "@NAME@" "${name}" plan_json "${plan_json}")
    string(REPLACE "@RUN_ID@" "${run_id}" plan_json "${plan_json}")
    string(REPLACE "@FIXTURE@" "${fixture_path}" plan_json "${plan_json}")
    string(REPLACE "@GATEWAY_STEP@" "${gateway_step}" plan_json "${plan_json}")
    file(WRITE "${root}/plan.json" "${plan_json}")

    set(lifecycle_state "${archive}/runs/${run_id}/lifecycle.json")
    set(vmrun_log "${root}/vmrun.log")
    set(ENV{SATSUMA_MULTI_VM_VMRUN_LOG} "${vmrun_log}")
    if(fail_gateway_cleanup)
        set(ENV{SATSUMA_MULTI_VM_FAIL_GATEWAY_REVERT} "1")
    else()
        unset(ENV{SATSUMA_MULTI_VM_FAIL_GATEWAY_REVERT})
    endif()
    execute_process(
        COMMAND "${CMAKE_COMMAND}"
            "-DVM_EXE=${VM_EXE}"
            "-DCLIENT_CONFIG=${root}/client-agent.json"
            "-DGATEWAY_CONFIG=${root}/gateway-agent.json"
            "-DSHARED_ROOT=${share}"
            "-DRUN_ID=${run_id}"
            "-DLIFECYCLE_STATE=${lifecycle_state}"
            -P "${CMAKE_CURRENT_LIST_DIR}/run_two_agents_until_lifecycle_terminal.cmake"
        COMMAND "${HOST_EXE}" orchestrate
            --config "${root}/lab.json"
            --plan "${root}/plan.json"
            --timeout-seconds 15
        RESULTS_VARIABLE results
        OUTPUT_VARIABLE host_output
        ERROR_VARIABLE process_error
    )
    unset(ENV{SATSUMA_MULTI_VM_VMRUN_LOG})
    unset(ENV{SATSUMA_MULTI_VM_FAIL_GATEWAY_REVERT})
    if(NOT results STREQUAL "0;${expected_exit}")
        message(FATAL_ERROR
            "Multi-VM ${name} scenario failed (${results}): ${process_error}\n${host_output}")
    endif()
    string(FIND "${host_output}" "\"status\": \"${expected_status}\"" status_position)
    if(status_position EQUAL -1)
        message(FATAL_ERROR "Multi-VM ${name} returned unexpected output: ${host_output}")
    endif()

    if(NOT EXISTS "${lifecycle_state}")
        message(FATAL_ERROR "Multi-VM ${name} omitted lifecycle state")
    endif()
    file(READ "${lifecycle_state}" lifecycle_json)
    if(expected_status STREQUAL "COMPLETED")
        set(expected_phase "completed")
    elseif(expected_status STREQUAL "FAILED")
        set(expected_phase "failed")
    else()
        set(expected_phase "recovery_failed")
    endif()
    string(JSON lifecycle_phase GET "${lifecycle_json}" phase)
    if(NOT lifecycle_phase STREQUAL expected_phase)
        message(FATAL_ERROR "Multi-VM ${name} persisted phase ${lifecycle_phase}")
    endif()

    set(main_evidence "${archive}/runs/${run_id}/evidence/main")
    if(NOT EXISTS "${main_evidence}/task.json" OR
       NOT EXISTS "${main_evidence}/results/client/client_step/execution.json" OR
       NOT EXISTS "${main_evidence}/results/gateway/gateway_step/execution.json")
        message(FATAL_ERROR "Multi-VM ${name} omitted main task evidence")
    endif()
    set(finally_evidence "${archive}/runs/${run_id}/evidence/finally")
    if(NOT EXISTS "${finally_evidence}/task.json" OR
       NOT EXISTS "${finally_evidence}/results/client/client_finally/execution.json" OR
       NOT EXISTS "${finally_evidence}/results/gateway/gateway_finally/execution.json")
        message(FATAL_ERROR "Multi-VM ${name} omitted finally evidence")
    endif()
    file(READ "${main_evidence}/task.json" main_manifest)
    string(JSON step_count LENGTH "${main_manifest}" steps)
    string(JSON first_vm GET "${main_manifest}" steps 0 vm)
    string(JSON second_vm GET "${main_manifest}" steps 1 vm)
    if(NOT step_count EQUAL 2 OR NOT first_vm STREQUAL "client" OR
       NOT second_vm STREQUAL "gateway")
        message(FATAL_ERROR "Multi-VM ${name} main manifest did not preserve both VM steps")
    endif()
    string(JSON expected_steps GET "${host_output}" report expected_steps)
    string(JSON successful_steps GET "${host_output}" report successful_steps)
    string(JSON failed_steps GET "${host_output}" report failed_steps)
    if(NOT expected_steps EQUAL 2 OR
       NOT successful_steps EQUAL expected_successful OR
       NOT failed_steps EQUAL expected_failed)
        message(FATAL_ERROR "Multi-VM ${name} report counts are invalid: ${host_output}")
    endif()

    file(READ "${vmrun_log}" vmrun_commands)
    require_log_order(
        "${vmrun_commands}"
        "start|Client VM.vmx|nogui"
        "start|Gateway VM.vmx|nogui"
        "${name} lifecycle startup")
    require_log_order(
        "${vmrun_commands}"
        "checkToolsState|Client VM.vmx"
        "checkToolsState|Gateway VM.vmx"
        "${name} Agent diagnostics")
    if(expected_status STREQUAL "COMPLETED")
        require_log_order(
            "${vmrun_commands}"
            "stop|Gateway VM.vmx|soft"
            "stop|Client VM.vmx|soft"
            "${name} success cleanup")
    else()
        require_log_order(
            "${vmrun_commands}"
            "stop|Gateway VM.vmx|hard"
            "revertToSnapshot|Gateway VM.vmx|clean"
            "${name} gateway failure cleanup")
        require_log_order(
            "${vmrun_commands}"
            "revertToSnapshot|Gateway VM.vmx|clean"
            "stop|Client VM.vmx|hard"
            "${name} reverse failure cleanup")
        require_log_order(
            "${vmrun_commands}"
            "stop|Client VM.vmx|hard"
            "revertToSnapshot|Client VM.vmx|clean"
            "${name} client failure cleanup")
    endif()

    # 终态重入不得再次调用 VMware，且清理动作结构必须保持稳定。
    file(SIZE "${vmrun_log}" vmrun_log_size_before_reentry)
    execute_process(
        COMMAND "${HOST_EXE}" orchestrate
            --config "${root}/lab.json"
            --plan "${root}/plan.json"
            --timeout-seconds 15
        RESULT_VARIABLE reentry_result
        OUTPUT_VARIABLE reentry_output
        ERROR_VARIABLE reentry_error
    )
    if(NOT reentry_result EQUAL expected_exit)
        message(FATAL_ERROR
            "Multi-VM ${name} terminal reentry failed: ${reentry_error}\n${reentry_output}")
    endif()
    string(JSON reentry_status GET "${reentry_output}" status)
    string(JSON reentry_resumed GET "${reentry_output}" resumed)
    if(NOT reentry_status STREQUAL expected_status OR
       NOT reentry_resumed)
        message(FATAL_ERROR
            "Multi-VM ${name} terminal reentry changed stable output: ${reentry_output}")
    endif()
    if(expected_status STREQUAL "RECOVERY_FAILED")
        string(FIND "${host_output}" "\"cleanup_actions\"" initial_cleanup_position)
        string(FIND "${reentry_output}" "\"cleanup_actions\"" reentry_cleanup_position)
        if(NOT initial_cleanup_position EQUAL -1 OR NOT reentry_cleanup_position EQUAL -1)
            message(FATAL_ERROR
                "Multi-VM ${name} exposed non-persisted cleanup status")
        endif()
    else()
        string(JSON initial_cleanup_actions GET "${host_output}" cleanup_actions)
        string(JSON reentry_cleanup_actions GET "${reentry_output}" cleanup_actions)
        if(NOT reentry_cleanup_actions STREQUAL initial_cleanup_actions)
            message(FATAL_ERROR
                "Multi-VM ${name} terminal reentry changed cleanup actions: ${reentry_output}")
        endif()
    endif()
    file(SIZE "${vmrun_log}" vmrun_log_size_after_reentry)
    if(NOT vmrun_log_size_after_reentry EQUAL vmrun_log_size_before_reentry)
        message(FATAL_ERROR "Multi-VM ${name} terminal reentry called vmrun again")
    endif()
endfunction()

set(gateway_success_step
    [=[{"id": "gateway_step", "vm": "gateway", "type": "echo", "message": "gateway completed"}]=])
set(gateway_failure_step [=[
{
  "id": "gateway_step",
  "vm": "gateway",
  "type": "execute",
  "program": "artifacts/gateway/SatsumaTestFixture.exe",
  "arguments": ["--message", "gateway business failure"],
  "timeout_seconds": 5,
  "collect_files": ["missing-result.txt"]
}
]=])

run_multi_vm_scenario(success 0 COMPLETED "${gateway_success_step}" 2 0 FALSE)
run_multi_vm_scenario(failure 1 FAILED "${gateway_failure_step}" 1 1 FALSE)
run_multi_vm_scenario(cleanup_failure 4 RECOVERY_FAILED "${gateway_failure_step}" 1 1 TRUE)

unset(ENV{SATSUMA_MULTI_VM_VMRUN_LOG})
unset(ENV{SATSUMA_MULTI_VM_FAIL_GATEWAY_REVERT})
