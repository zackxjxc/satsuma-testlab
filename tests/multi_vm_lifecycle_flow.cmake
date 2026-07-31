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

# 核对日志中从指定命令开始的连续子序列。
function(require_log_sequence log_text context)
    set(remaining_log "${log_text}")
    foreach(expected_command IN LISTS ARGN)
        string(FIND "${remaining_log}" "${expected_command}" command_position)
        if(command_position EQUAL -1)
            message(FATAL_ERROR
                "${context} command sequence is invalid at ${expected_command}\n${log_text}")
        endif()
        string(LENGTH "${expected_command}" command_length)
        math(EXPR next_position "${command_position} + ${command_length}")
        string(SUBSTRING "${remaining_log}" ${next_position} -1 remaining_log)
    endforeach()
endfunction()

# 核对清理命令的精确序列，拒绝额外或重复策略。
function(require_cleanup_sequence log_text expected_cleanup context)
    string(REPLACE "\r\n" "\n" normalized_log "${log_text}")
    string(REPLACE "\n" ";" log_lines "${normalized_log}")
    set(actual_cleanup "")
    foreach(log_line IN LISTS log_lines)
        if(log_line MATCHES "^(stop|revertToSnapshot)\\|")
            list(APPEND actual_cleanup "${log_line}")
        endif()
    endforeach()
    if(NOT actual_cleanup STREQUAL expected_cleanup)
        message(FATAL_ERROR
            "${context} cleanup sequence is invalid\n"
            "expected: ${expected_cleanup}\nactual: ${actual_cleanup}\n${log_text}")
    endif()
endfunction()

# 执行一个隔离场景并核对双 VM manifest、业务结果和逆序清理。
function(run_multi_vm_scenario name expected_exit expected_status vm_01_step vm_02_step
         expected_successful expected_failed expected_vm_01_status expected_vm_02_status
         fail_vm_02_cleanup fail_vm_02_soft_stop fail_vm_01_start)
    set(root "${TEST_ROOT}/${name}")
    set(share "${root}/share")
    set(archive "${root}/archive")
    set(vm_01_local "${root}/vm_01-local")
    set(vm_02_local "${root}/vm_02-local")
    file(MAKE_DIRECTORY
        "${share}"
        "${archive}"
        "${vm_01_local}"
        "${vm_02_local}")
    file(TO_CMAKE_PATH "${share}" share_path)
    file(TO_CMAKE_PATH "${archive}" archive_path)
    file(TO_CMAKE_PATH "${vm_01_local}" vm_01_local_path)
    file(TO_CMAKE_PATH "${vm_02_local}" vm_02_local_path)
    file(TO_CMAKE_PATH "${root}/VM 01.vmx" vm_01_vmx_path)
    file(TO_CMAKE_PATH "${root}/VM 02.vmx" vm_02_vmx_path)
    file(WRITE "${vm_01_vmx_path}" "# VM 01 test VMX\n")
    file(WRITE "${vm_02_vmx_path}" "# VM 02 test VMX\n")

    set(lab_json [=[
{
  "schema_version": 1,
  "lab_id": "multi_vm_lab",
  "provider": {"type": "vmware_workstation", "vmrun": "@VMRUN@"},
  "host": {"archive_root": "@ARCHIVE@"},
  "shared_folder": {"host_root": "@SHARE@"},
  "vms": [
    {
      "id": "vm_02",
      "vmx": "@VM_02_VMX@",
      "agent_version": "0.1.0",
      "snapshots": {"base": "clean", "ai_prefix": "satsuma-ai-", "max_ai_snapshots": 8}
    },
    {
      "id": "vm_01",
      "vmx": "@VM_01_VMX@",
      "agent_version": "0.1.0",
      "snapshots": {"base": "clean", "ai_prefix": "satsuma-ai-", "max_ai_snapshots": 8}
    }
  ]
}
]=])
    string(REPLACE "@VMRUN@" "${vmrun_path}" lab_json "${lab_json}")
    string(REPLACE "@ARCHIVE@" "${archive_path}" lab_json "${lab_json}")
    string(REPLACE "@SHARE@" "${share_path}" lab_json "${lab_json}")
    string(REPLACE "@VM_01_VMX@" "${vm_01_vmx_path}" lab_json "${lab_json}")
    string(REPLACE "@VM_02_VMX@" "${vm_02_vmx_path}" lab_json "${lab_json}")
    file(WRITE "${root}/lab.json" "${lab_json}")

    set(agent_json [=[
{
  "schema_version": 1,
  "protocol_version": 3,
  "lab_id": "multi_vm_lab",
  "vm_id": "@VM_ID@",
  "agent_version": "0.1.0",
  "shared_root": "@SHARE@",
  "local_work_root": "@LOCAL@",
  "poll_interval_ms": 100,
  "reconnect_interval_ms": 100
}
]=])
    string(REPLACE "@SHARE@" "${share_path}" vm_01_agent_json "${agent_json}")
    string(REPLACE "@LOCAL@" "${vm_01_local_path}" vm_01_agent_json "${vm_01_agent_json}")
    string(REPLACE "@VM_ID@" "vm_01" vm_01_agent_json "${vm_01_agent_json}")
    file(WRITE "${root}/vm_01-agent.json" "${vm_01_agent_json}")
    string(REPLACE "@SHARE@" "${share_path}" vm_02_agent_json "${agent_json}")
    string(REPLACE "@LOCAL@" "${vm_02_local_path}" vm_02_agent_json "${vm_02_agent_json}")
    string(REPLACE "@VM_ID@" "vm_02" vm_02_agent_json "${vm_02_agent_json}")
    file(WRITE "${root}/vm_02-agent.json" "${vm_02_agent_json}")

    set(run_id "multi_vm_${name}")
    set(plan_json [=[
{
  "schema_version": 2,
  "name": "multi VM lifecycle @NAME@",
  "run_id": "@RUN_ID@",
  "artifacts": [
    {
      "source": "@FIXTURE@",
      "vm": "vm_01",
      "shared_destination": "artifacts/vm_01/SatsumaTestFixture.exe"
    },
    {
      "source": "@FIXTURE@",
      "vm": "vm_02",
      "shared_destination": "artifacts/vm_02/SatsumaTestFixture.exe"
    }
  ],
  "cleanup": {
    "guest_work": {"on_success": "delete", "on_failure": "retain"},
    "shared_run": {"on_success": "archive_then_delete", "on_failure": "retain"}
  },
  "steps": [
    @VM_01_STEP@,
    @VM_02_STEP@
  ],
  "lifecycle": {
    "vms": [
      {
        "vm": "vm_01",
        "on_success": {"action": "stop"},
        "on_failure": {"action": "restore", "snapshot": "clean"}
      },
      {
        "vm": "vm_02",
        "on_success": {"action": "stop"},
        "on_failure": {"action": "restore", "snapshot": "clean"}
      }
    ],
    "finally": [
      {"id": "vm_01_finally", "vm": "vm_01", "type": "echo", "message": "vm_01 cleanup"},
      {"id": "vm_02_finally", "vm": "vm_02", "type": "echo", "message": "vm_02 cleanup"}
    ]
  }
}
]=])
    string(REPLACE "@NAME@" "${name}" plan_json "${plan_json}")
    string(REPLACE "@RUN_ID@" "${run_id}" plan_json "${plan_json}")
    string(REPLACE "@FIXTURE@" "${fixture_path}" plan_json "${plan_json}")
    string(REPLACE "@VM_01_STEP@" "${vm_01_step}" plan_json "${plan_json}")
    string(REPLACE "@VM_02_STEP@" "${vm_02_step}" plan_json "${plan_json}")
    file(WRITE "${root}/plan.json" "${plan_json}")

    set(lifecycle_state "${archive}/runs/${run_id}/lifecycle.json")
    set(vmrun_log "${root}/vmrun.log")
    set(ENV{SATSUMA_MULTI_VM_VMRUN_LOG} "${vmrun_log}")
    if(fail_vm_02_cleanup)
        set(ENV{SATSUMA_MULTI_VM_FAIL_VM_02_REVERT} "1")
    else()
        unset(ENV{SATSUMA_MULTI_VM_FAIL_VM_02_REVERT})
    endif()
    if(fail_vm_02_soft_stop)
        set(ENV{SATSUMA_MULTI_VM_FAIL_VM_02_SOFT_STOP} "1")
        set(ENV{SATSUMA_MULTI_VM_DELAYED_STOP_VMX} "${vm_02_vmx_path}")
    else()
        unset(ENV{SATSUMA_MULTI_VM_FAIL_VM_02_SOFT_STOP})
        unset(ENV{SATSUMA_MULTI_VM_DELAYED_STOP_VMX})
    endif()
    if(fail_vm_01_start)
        set(ENV{SATSUMA_MULTI_VM_FAIL_VM_01_START} "1")
        set(ENV{SATSUMA_MULTI_VM_DELAYED_START_VMX} "${vm_01_vmx_path}")
    else()
        unset(ENV{SATSUMA_MULTI_VM_FAIL_VM_01_START})
        unset(ENV{SATSUMA_MULTI_VM_DELAYED_START_VMX})
    endif()
    execute_process(
        COMMAND "${CMAKE_COMMAND}"
            "-DVM_EXE=${VM_EXE}"
            "-DVM_01_CONFIG=${root}/vm_01-agent.json"
            "-DVM_02_CONFIG=${root}/vm_02-agent.json"
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
       NOT EXISTS "${main_evidence}/results/vm_01/vm_01_step/execution.json" OR
       NOT EXISTS "${main_evidence}/results/vm_02/vm_02_step/execution.json")
        message(FATAL_ERROR "Multi-VM ${name} omitted main task evidence")
    endif()
    file(READ "${main_evidence}/results/vm_01/vm_01_step/execution.json" vm_01_execution)
    file(READ "${main_evidence}/results/vm_02/vm_02_step/execution.json" vm_02_execution)
    string(JSON vm_01_status GET "${vm_01_execution}" status)
    string(JSON vm_02_status GET "${vm_02_execution}" status)
    if(NOT vm_01_status STREQUAL expected_vm_01_status OR
       NOT vm_02_status STREQUAL expected_vm_02_status)
        message(FATAL_ERROR
            "Multi-VM ${name} result ownership is invalid: ${vm_01_status}, ${vm_02_status}")
    endif()
    set(finally_evidence "${archive}/runs/${run_id}/evidence/finally")
    if(NOT EXISTS "${finally_evidence}/task.json" OR
       NOT EXISTS "${finally_evidence}/results/vm_01/vm_01_finally/execution.json" OR
       NOT EXISTS "${finally_evidence}/results/vm_02/vm_02_finally/execution.json")
        message(FATAL_ERROR "Multi-VM ${name} omitted finally evidence")
    endif()
    file(READ "${archive}/runs/${run_id}/orchestration.json" orchestration_identity)
    string(JSON finally_run_id GET "${orchestration_identity}" finally_run_id)
    if(expected_status STREQUAL "COMPLETED")
        if(EXISTS "${share}/runs/${run_id}" OR EXISTS "${share}/runs/${finally_run_id}")
            message(FATAL_ERROR "Multi-VM ${name} retained shared runs after verified archive")
        endif()
    elseif(NOT EXISTS "${share}/runs/${run_id}")
        message(FATAL_ERROR "Multi-VM ${name} deleted failure evidence from the shared folder")
    endif()
    file(READ "${main_evidence}/task.json" main_manifest)
    string(JSON step_count LENGTH "${main_manifest}" steps)
    string(JSON first_vm GET "${main_manifest}" steps 0 vm)
    string(JSON second_vm GET "${main_manifest}" steps 1 vm)
    if(NOT step_count EQUAL 2 OR NOT first_vm STREQUAL "vm_01" OR
       NOT second_vm STREQUAL "vm_02")
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
    string(JSON diagnostic_count LENGTH "${host_output}" diagnostics)
    if(NOT diagnostic_count EQUAL 2)
        message(FATAL_ERROR "Multi-VM ${name} diagnostic count is invalid: ${host_output}")
    endif()
    foreach(diagnostic_index RANGE 0 1)
        string(JSON initial_status GET
            "${host_output}" diagnostics ${diagnostic_index} result initial_environment status)
        string(JSON recheck_attempts GET
            "${host_output}" diagnostics ${diagnostic_index} result environment_recheck_attempts)
        if(NOT initial_status STREQUAL "failed" OR recheck_attempts LESS 2)
            message(FATAL_ERROR
                "Multi-VM ${name} omitted delayed environment convergence evidence")
        endif()
    endforeach()
    string(JSON finally_expected GET "${host_output}" finally_report expected_steps)
    string(JSON finally_successful GET "${host_output}" finally_report successful_steps)
    string(JSON finally_failed GET "${host_output}" finally_report failed_steps)
    if(NOT finally_expected EQUAL 2 OR
       NOT finally_successful EQUAL 2 OR
       NOT finally_failed EQUAL 0)
        message(FATAL_ERROR "Multi-VM ${name} finally report is invalid: ${host_output}")
    endif()

    file(READ "${vmrun_log}" vmrun_commands)
    require_log_order(
        "${vmrun_commands}"
        "start|VM 01.vmx|nogui"
        "start|VM 02.vmx|nogui"
        "${name} lifecycle startup")
    require_log_order(
        "${vmrun_commands}"
        "checkToolsState|VM 01.vmx"
        "checkToolsState|VM 02.vmx"
        "${name} Agent diagnostics")
    if(expected_status STREQUAL "COMPLETED")
        set(expected_cleanup
            "stop|VM 02.vmx|soft;stop|VM 01.vmx|soft")
    else()
        set(expected_cleanup
            "stop|VM 02.vmx|hard;revertToSnapshot|VM 02.vmx|clean"
            "stop|VM 01.vmx|hard;revertToSnapshot|VM 01.vmx|clean")
    endif()
    require_cleanup_sequence(
        "${vmrun_commands}"
        "${expected_cleanup}"
        "${name}")
    if(fail_vm_02_soft_stop)
        require_log_sequence(
            "${vmrun_commands}"
            "${name} delayed stop reconciliation"
            "stop|VM 02.vmx|soft"
            "list\n"
            "list\n"
            "stop|VM 01.vmx|soft")
    endif()
    if(fail_vm_01_start)
        require_log_sequence(
            "${vmrun_commands}"
            "${name} delayed start reconciliation"
            "start|VM 01.vmx|nogui"
            "list\n"
            "list\n"
            "start|VM 02.vmx|nogui")
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
    if(expected_status STREQUAL "RECOVERY_FAILED")
        if(reentry_result EQUAL 0 OR
           NOT reentry_error MATCHES "manual_intervention_required: stale active lab lease")
            message(FATAL_ERROR
                "Multi-VM ${name} cleanup failure did not retain its lease: "
                "${reentry_error}\n${reentry_output}")
        endif()
        execute_process(
            COMMAND "${HOST_EXE}" lab status --config "${root}/lab.json"
            RESULT_VARIABLE lease_status_result
            OUTPUT_VARIABLE lease_status_output
            ERROR_VARIABLE lease_status_error
        )
        if(NOT lease_status_result EQUAL 0)
            message(FATAL_ERROR
                "Multi-VM ${name} cannot inspect retained lease: ${lease_status_error}")
        endif()
        string(JSON lease_state GET "${lease_status_output}" lease state)
        string(JSON lease_run_id GET "${lease_status_output}" lease run_id)
        if(NOT lease_state STREQUAL "active" OR NOT lease_run_id STREQUAL "${run_id}")
            message(FATAL_ERROR
                "Multi-VM ${name} cleanup failure changed retained lease: ${lease_status_output}")
        endif()
        file(SIZE "${vmrun_log}" vmrun_log_size_after_reentry)
        if(NOT vmrun_log_size_after_reentry EQUAL vmrun_log_size_before_reentry)
            message(FATAL_ERROR "Multi-VM ${name} blocked reentry called vmrun again")
        endif()
        return()
    endif()
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
    string(JSON initial_cleanup_actions GET "${host_output}" cleanup_actions)
    string(JSON reentry_cleanup_actions GET "${reentry_output}" cleanup_actions)
    if(NOT reentry_cleanup_actions STREQUAL initial_cleanup_actions)
        message(FATAL_ERROR
            "Multi-VM ${name} terminal reentry changed cleanup actions: ${reentry_output}")
    endif()
    file(SIZE "${vmrun_log}" vmrun_log_size_after_reentry)
    if(NOT vmrun_log_size_after_reentry EQUAL vmrun_log_size_before_reentry)
        message(FATAL_ERROR "Multi-VM ${name} terminal reentry called vmrun again")
    endif()
    unset(ENV{SATSUMA_MULTI_VM_VMRUN_LOG})
    unset(ENV{SATSUMA_MULTI_VM_FAIL_VM_02_REVERT})
    unset(ENV{SATSUMA_MULTI_VM_FAIL_VM_02_SOFT_STOP})
    unset(ENV{SATSUMA_MULTI_VM_DELAYED_STOP_VMX})
    unset(ENV{SATSUMA_MULTI_VM_FAIL_VM_01_START})
    unset(ENV{SATSUMA_MULTI_VM_DELAYED_START_VMX})
endfunction()

set(vm_01_success_step
    [=[{"id": "vm_01_step", "vm": "vm_01", "type": "echo", "message": "vm_01 completed"}]=])
set(vm_01_failure_step [=[
{
  "id": "vm_01_step",
  "vm": "vm_01",
  "type": "execute",
  "program": "artifacts/vm_01/SatsumaTestFixture.exe",
  "arguments": ["--message", "vm_01 business failure"],
  "timeout_seconds": 5,
  "collect_files": ["missing-vm_01-result.txt"]
}
]=])
set(vm_02_success_step
    [=[{"id": "vm_02_step", "vm": "vm_02", "type": "echo", "message": "vm_02 completed"}]=])
set(vm_02_failure_step [=[
{
  "id": "vm_02_step",
  "vm": "vm_02",
  "type": "execute",
  "program": "artifacts/vm_02/SatsumaTestFixture.exe",
  "arguments": ["--message", "vm_02 business failure"],
  "timeout_seconds": 5,
  "collect_files": ["missing-result.txt"]
}
]=])

run_multi_vm_scenario(
    success 0 COMPLETED "${vm_01_success_step}" "${vm_02_success_step}"
    2 0 exited exited FALSE FALSE FALSE)
run_multi_vm_scenario(
    stop_reconciled 0 COMPLETED "${vm_01_success_step}" "${vm_02_success_step}"
    2 0 exited exited FALSE TRUE FALSE)
run_multi_vm_scenario(
    start_recon 0 COMPLETED "${vm_01_success_step}" "${vm_02_success_step}"
    2 0 exited exited FALSE FALSE TRUE)
run_multi_vm_scenario(
    vm_02_failure 1 FAILED "${vm_01_success_step}" "${vm_02_failure_step}"
    1 1 exited failed FALSE FALSE FALSE)
run_multi_vm_scenario(
    vm_01_failure 1 FAILED "${vm_01_failure_step}" "${vm_02_success_step}"
    1 1 failed exited FALSE FALSE FALSE)
run_multi_vm_scenario(
    cleanup_failure 4 RECOVERY_FAILED "${vm_01_success_step}" "${vm_02_failure_step}"
    1 1 exited failed TRUE FALSE FALSE)

unset(ENV{SATSUMA_MULTI_VM_VMRUN_LOG})
unset(ENV{SATSUMA_MULTI_VM_FAIL_VM_02_REVERT})
unset(ENV{SATSUMA_MULTI_VM_FAIL_VM_02_SOFT_STOP})
unset(ENV{SATSUMA_MULTI_VM_DELAYED_STOP_VMX})
unset(ENV{SATSUMA_MULTI_VM_FAIL_VM_01_START})
unset(ENV{SATSUMA_MULTI_VM_DELAYED_START_VMX})

# 成功运行不保留大体积临时 VM/Artifact；失败会在到达此处前终止并保留证据。
file(REMOVE_RECURSE "${TEST_ROOT}")
