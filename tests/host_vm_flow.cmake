# 在本机目录中模拟共享文件夹并验证完整 Host/VM 流程。
if(NOT DEFINED HOST_EXE OR NOT DEFINED VM_EXE OR NOT DEFINED FIXTURE_EXE OR NOT DEFINED TEST_ROOT)
    message(FATAL_ERROR "Host/VM integration test arguments are incomplete")
endif()

file(REMOVE_RECURSE "${TEST_ROOT}")
file(MAKE_DIRECTORY "${TEST_ROOT}/share" "${TEST_ROOT}/local" "${TEST_ROOT}/archive")

file(TO_CMAKE_PATH "${TEST_ROOT}/share" share_path)
file(TO_CMAKE_PATH "${TEST_ROOT}/local" local_path)
file(TO_CMAKE_PATH "${TEST_ROOT}/archive" archive_path)
file(TO_CMAKE_PATH "${FIXTURE_EXE}" fixture_path)

set(lab_json [=[
{
  "schema_version": 1,
  "lab_id": "integration_lab",
  "provider": {
    "type": "vmware_workstation",
    "vmrun": "C:/Program Files/VMware/VMware Workstation/vmrun.exe"
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
      "vmx": "C:/VM/Client.vmx",
      "agent_version": "0.1.0",
      "management_ip": "127.0.0.1"
    }
  ]
}
]=])
string(REPLACE "@ARCHIVE@" "${archive_path}" lab_json "${lab_json}")
string(REPLACE "@SHARE@" "${share_path}" lab_json "${lab_json}")
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
