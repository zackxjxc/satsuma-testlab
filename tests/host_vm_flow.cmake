# 在本机目录中模拟共享文件夹并验证完整 Host/VM 流程。
if(NOT DEFINED HOST_EXE OR
   NOT DEFINED VM_EXE OR
   NOT DEFINED FIXTURE_EXE OR
   NOT DEFINED VMRUN_EXE OR
   NOT DEFINED TEST_ROOT)
    message(FATAL_ERROR "Host/VM integration test arguments are incomplete")
endif()

file(REMOVE_RECURSE "${TEST_ROOT}")
file(MAKE_DIRECTORY "${TEST_ROOT}/share" "${TEST_ROOT}/local" "${TEST_ROOT}/archive")

file(TO_CMAKE_PATH "${TEST_ROOT}/share" share_path)
file(TO_CMAKE_PATH "${TEST_ROOT}/local" local_path)
file(TO_CMAKE_PATH "${TEST_ROOT}/archive" archive_path)
file(TO_CMAKE_PATH "${FIXTURE_EXE}" fixture_path)
file(TO_CMAKE_PATH "${VMRUN_EXE}" vmrun_path)
file(TO_CMAKE_PATH "${TEST_ROOT}/Client VM.vmx" vmx_path)
file(TO_CMAKE_PATH "${TEST_ROOT}/Hard VM.vmx" hard_vmx_path)
file(WRITE "${vmx_path}" "# Test VMX placeholder\n")
file(WRITE "${hard_vmx_path}" "# Test VMX placeholder\n")

set(lab_json [=[
{
  "schema_version": 1,
  "lab_id": "integration_lab",
  "provider": {
    "type": "vmware_workstation",
    "vmrun": "@VMRUN@"
  },
  "host": {
    "listen": "127.0.0.1:37100",
    "archive_root": "@ARCHIVE@"
  },
  "shared_folder": {
    "host_root": "@SHARE@",
    "guest_root": "@SHARE@"
  },
  "vms": [
    {
      "id": "client",
      "role": "client",
      "vmx": "@VMX@",
      "agent_version": "0.1.0",
      "snapshots": {
        "base": "clean",
        "ai_prefix": "satsuma-ai-",
        "max_ai_snapshots": 8
      },
      "management_ip": "127.0.0.1"
    },
    {
      "id": "hard-stop",
      "role": "hard-stop-test",
      "vmx": "@HARD_VMX@",
      "agent_version": "0.1.0",
      "snapshots": {
        "base": "clean",
        "ai_prefix": "satsuma-ai-",
        "max_ai_snapshots": 8
      },
      "management_ip": "127.0.0.2"
    }
  ]
}
]=])
string(REPLACE "@ARCHIVE@" "${archive_path}" lab_json "${lab_json}")
string(REPLACE "@SHARE@" "${share_path}" lab_json "${lab_json}")
string(REPLACE "@VMRUN@" "${vmrun_path}" lab_json "${lab_json}")
string(REPLACE "@VMX@" "${vmx_path}" lab_json "${lab_json}")
string(REPLACE "@HARD_VMX@" "${hard_vmx_path}" lab_json "${lab_json}")
file(WRITE "${TEST_ROOT}/lab.json" "${lab_json}")

set(agent_json [=[
{
  "schema_version": 1,
  "protocol_version": 1,
  "lab_id": "integration_lab",
  "vm_id": "client",
  "agent_version": "0.1.0",
  "host": "127.0.0.1:37100",
  "shared_root": "@SHARE@",
  "local_work_root": "@LOCAL@",
  "poll_interval_ms": 100,
  "reconnect_interval_ms": 100,
  "rpc_timeout_ms": 1000
}
]=])
string(REPLACE "@SHARE@" "${share_path}" agent_json "${agent_json}")
string(REPLACE "@LOCAL@" "${local_path}" agent_json "${agent_json}")
file(WRITE "${TEST_ROOT}/agent.json" "${agent_json}")

set(task_json [=[
{
  "schema_version": 1,
  "name": "host-vm-integration",
  "run_id": "integration_run",
  "artifacts": [
    {
      "source": "@FIXTURE@",
      "vm": "client",
      "shared_destination": "artifacts/client/SatsumaTestFixture.exe"
    }
  ],
  "steps": [
    {
      "id": "execute_fixture",
      "vm": "client",
      "type": "execute",
      "program": "artifacts/client/SatsumaTestFixture.exe",
      "arguments": ["--message", "hello from fixture", "--output", "generated/result.json"],
      "timeout_seconds": 10,
      "collect_files": ["generated/result.json"]
    },
    {
      "id": "timeout_fixture",
      "vm": "client",
      "type": "execute",
      "program": "artifacts/client/SatsumaTestFixture.exe",
      "arguments": ["--sleep-ms", "3000"],
      "timeout_seconds": 1,
      "collect_files": []
    }
  ]
}
]=])
string(REPLACE "@FIXTURE@" "${fixture_path}" task_json "${task_json}")
file(WRITE "${TEST_ROOT}/task.json" "${task_json}")

execute_process(
    COMMAND "${HOST_EXE}" vm start --id client
    WORKING_DIRECTORY "${TEST_ROOT}"
    RESULT_VARIABLE vm_start_result
    OUTPUT_VARIABLE vm_start_output
    ERROR_VARIABLE vm_start_error
)
if(NOT vm_start_result EQUAL 0)
    message(FATAL_ERROR "SatsumaHost vm start failed: ${vm_start_error}\n${vm_start_output}")
endif()
string(FIND "${vm_start_output}" "\"status\": \"started\"" vm_start_position)
if(vm_start_position EQUAL -1)
    message(FATAL_ERROR "SatsumaHost vm start returned unexpected output: ${vm_start_output}")
endif()

execute_process(
    COMMAND "${HOST_EXE}" vm stop --id client
    WORKING_DIRECTORY "${TEST_ROOT}"
    RESULT_VARIABLE vm_stop_result
    OUTPUT_VARIABLE vm_stop_output
    ERROR_VARIABLE vm_stop_error
)
if(NOT vm_stop_result EQUAL 0)
    message(FATAL_ERROR "SatsumaHost vm stop failed: ${vm_stop_error}\n${vm_stop_output}")
endif()
string(FIND "${vm_stop_output}" "\"status\": \"stopped\"" vm_stop_position)
if(vm_stop_position EQUAL -1)
    message(FATAL_ERROR "SatsumaHost vm stop returned unexpected output: ${vm_stop_output}")
endif()

execute_process(
    COMMAND "${HOST_EXE}" vm stop --id hard-stop --mode hard
    WORKING_DIRECTORY "${TEST_ROOT}"
    RESULT_VARIABLE vm_hard_stop_result
    OUTPUT_VARIABLE vm_hard_stop_output
    ERROR_VARIABLE vm_hard_stop_error
)
if(NOT vm_hard_stop_result EQUAL 0)
    message(FATAL_ERROR "SatsumaHost hard stop failed: ${vm_hard_stop_error}\n${vm_hard_stop_output}")
endif()
string(FIND "${vm_hard_stop_output}" "\"mode\": \"hard\"" vm_hard_stop_position)
if(vm_hard_stop_position EQUAL -1)
    message(FATAL_ERROR "SatsumaHost hard stop returned unexpected output: ${vm_hard_stop_output}")
endif()

execute_process(
    COMMAND "${HOST_EXE}" vm restore --id client --snapshot clean
    WORKING_DIRECTORY "${TEST_ROOT}"
    RESULT_VARIABLE vm_restore_result
    OUTPUT_VARIABLE vm_restore_output
    ERROR_VARIABLE vm_restore_error
)
if(NOT vm_restore_result EQUAL 0)
    message(FATAL_ERROR "SatsumaHost vm restore failed: ${vm_restore_error}\n${vm_restore_output}")
endif()
string(FIND "${vm_restore_output}" "\"status\": \"restored\"" vm_restore_position)
if(vm_restore_position EQUAL -1)
    message(FATAL_ERROR "SatsumaHost vm restore returned unexpected output: ${vm_restore_output}")
endif()

execute_process(
    COMMAND "${HOST_EXE}" snapshot list --vm client
    WORKING_DIRECTORY "${TEST_ROOT}"
    RESULT_VARIABLE snapshot_list_result
    OUTPUT_VARIABLE snapshot_list_output
    ERROR_VARIABLE snapshot_list_error
)
if(NOT snapshot_list_result EQUAL 0)
    message(FATAL_ERROR "SatsumaHost snapshot list failed: ${snapshot_list_error}\n${snapshot_list_output}")
endif()
string(FIND "${snapshot_list_output}" "\"ownership\": \"user_base\"" snapshot_list_position)
if(snapshot_list_position EQUAL -1)
    message(FATAL_ERROR "SatsumaHost snapshot list did not protect the base snapshot: ${snapshot_list_output}")
endif()

execute_process(
    COMMAND "${HOST_EXE}" snapshot delete-ai --vm client --snapshot clean
    WORKING_DIRECTORY "${TEST_ROOT}"
    RESULT_VARIABLE base_delete_result
    OUTPUT_VARIABLE base_delete_output
    ERROR_VARIABLE base_delete_error
)
if(base_delete_result EQUAL 0)
    message(FATAL_ERROR "SatsumaHost allowed deletion of the user base snapshot: ${base_delete_output}")
endif()

execute_process(
    COMMAND "${HOST_EXE}" snapshot create-ai --vm client --name network-ready
    WORKING_DIRECTORY "${TEST_ROOT}"
    RESULT_VARIABLE snapshot_create_result
    OUTPUT_VARIABLE snapshot_create_output
    ERROR_VARIABLE snapshot_create_error
)
if(NOT snapshot_create_result EQUAL 0)
    message(FATAL_ERROR "SatsumaHost snapshot create-ai failed: ${snapshot_create_error}\n${snapshot_create_output}")
endif()
string(FIND "${snapshot_create_output}" "\"status\": \"created\"" snapshot_create_position)
if(snapshot_create_position EQUAL -1)
    message(FATAL_ERROR "SatsumaHost snapshot create-ai returned unexpected output: ${snapshot_create_output}")
endif()
file(GLOB snapshot_metadata "${archive_path}/snapshots/client/*.json")
list(LENGTH snapshot_metadata snapshot_metadata_count)
if(NOT snapshot_metadata_count EQUAL 1)
    message(FATAL_ERROR "SatsumaHost did not create exactly one snapshot metadata record")
endif()
list(GET snapshot_metadata 0 snapshot_metadata_path)
file(READ "${snapshot_metadata_path}" snapshot_metadata_json)
string(FIND "${snapshot_metadata_json}" "\"status\": \"created\"" snapshot_metadata_position)
if(snapshot_metadata_position EQUAL -1)
    message(FATAL_ERROR "Snapshot metadata did not record successful creation: ${snapshot_metadata_json}")
endif()

execute_process(
    COMMAND "${HOST_EXE}" snapshot delete-ai --vm client --snapshot satsuma-ai-obsolete
    WORKING_DIRECTORY "${TEST_ROOT}"
    RESULT_VARIABLE snapshot_delete_result
    OUTPUT_VARIABLE snapshot_delete_output
    ERROR_VARIABLE snapshot_delete_error
)
if(NOT snapshot_delete_result EQUAL 0)
    message(FATAL_ERROR "SatsumaHost snapshot delete-ai failed: ${snapshot_delete_error}\n${snapshot_delete_output}")
endif()
string(FIND "${snapshot_delete_output}" "\"status\": \"deleted\"" snapshot_delete_position)
if(snapshot_delete_position EQUAL -1)
    message(FATAL_ERROR "SatsumaHost snapshot delete-ai returned unexpected output: ${snapshot_delete_output}")
endif()
set(deleted_metadata_path "${archive_path}/snapshots/client/satsuma-ai-obsolete.json")
file(READ "${deleted_metadata_path}" deleted_metadata_json)
string(FIND "${deleted_metadata_json}" "\"status\": \"deleted\"" deleted_metadata_position)
if(deleted_metadata_position EQUAL -1)
    message(FATAL_ERROR "Snapshot deletion metadata was not finalized: ${deleted_metadata_json}")
endif()

execute_process(
    COMMAND "${HOST_EXE}" run --config "${TEST_ROOT}/lab.json" --plan "${TEST_ROOT}/task.json"
    RESULT_VARIABLE host_result
    OUTPUT_VARIABLE host_output
    ERROR_VARIABLE host_error
)
if(NOT host_result EQUAL 0)
    message(FATAL_ERROR "SatsumaHost run failed: ${host_error}\n${host_output}")
endif()

execute_process(
    COMMAND "${VM_EXE}" --config "${TEST_ROOT}/agent.json" --once
    RESULT_VARIABLE vm_result
    OUTPUT_VARIABLE vm_output
    ERROR_VARIABLE vm_error
)
if(NOT vm_result EQUAL 0)
    message(FATAL_ERROR "SatsumaVM failed: ${vm_error}\n${vm_output}")
endif()

execute_process(
    COMMAND "${HOST_EXE}" report --config "${TEST_ROOT}/lab.json" --run integration_run
    RESULT_VARIABLE report_result
    OUTPUT_VARIABLE report_output
    ERROR_VARIABLE report_error
)
if(NOT report_result EQUAL 0)
    message(FATAL_ERROR "SatsumaHost report failed: ${report_error}\n${report_output}")
endif()

string(FIND "${report_output}" "\"successful_steps\": 1" success_position)
if(success_position EQUAL -1)
    message(FATAL_ERROR "Host report did not contain one successful step: ${report_output}")
endif()
string(FIND "${report_output}" "\"failed_steps\": 1" failed_position)
if(failed_position EQUAL -1)
    message(FATAL_ERROR "Host report did not contain one timed-out step: ${report_output}")
endif()

set(result_root "${TEST_ROOT}/share/runs/integration_run/results/client/execute_fixture")
if(NOT EXISTS "${result_root}/execution.json" OR
   NOT EXISTS "${result_root}/stdout.log" OR
   NOT EXISTS "${result_root}/files/generated/result.json")
    message(FATAL_ERROR "Expected execution evidence was not created")
endif()

set(timeout_result "${TEST_ROOT}/share/runs/integration_run/results/client/timeout_fixture/execution.json")
if(NOT EXISTS "${timeout_result}")
    message(FATAL_ERROR "Timed-out execution evidence was not created")
endif()
file(READ "${timeout_result}" timeout_json)
string(FIND "${timeout_json}" "\"timed_out\": true" timed_out_position)
if(timed_out_position EQUAL -1)
    message(FATAL_ERROR "Process timeout was not recorded: ${timeout_json}")
endif()
