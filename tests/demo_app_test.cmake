# 独立示例软件的本机执行和结果文件测试。
if(NOT DEFINED APP OR NOT EXISTS "${APP}")
    message(FATAL_ERROR "SatsumaDemoApp executable is required")
endif()
if(NOT DEFINED INPUT OR NOT EXISTS "${INPUT}")
    message(FATAL_ERROR "Demo input file is required")
endif()
if(NOT DEFINED TEST_ROOT)
    message(FATAL_ERROR "Demo test root is required")
endif()

file(REMOVE_RECURSE "${TEST_ROOT}")
file(MAKE_DIRECTORY "${TEST_ROOT}")
# 本次测试生成的两个结果文件
set(result_file "${TEST_ROOT}/demo-result.json")
set(transformed_file "${TEST_ROOT}/transformed.txt")

execute_process(
    COMMAND "${APP}"
        --input "${INPUT}"
        --result "${result_file}"
        --transformed "${transformed_file}"
        --label ctest
        --emit-warning
    RESULT_VARIABLE app_result
    OUTPUT_VARIABLE app_output
    ERROR_VARIABLE app_error
)
if(NOT app_result EQUAL 0)
    message(FATAL_ERROR "SatsumaDemoApp failed (${app_result}): ${app_error}")
endif()
if(NOT app_output MATCHES "demo-app processed")
    message(FATAL_ERROR "SatsumaDemoApp stdout summary is missing: ${app_output}")
endif()
if(NOT app_error MATCHES "simulated warning")
    message(FATAL_ERROR "SatsumaDemoApp stderr diagnostic is missing: ${app_error}")
endif()

file(READ "${result_file}" result_json)
string(JSON result_status GET "${result_json}" status)
string(JSON result_label GET "${result_json}" label)
string(JSON result_lines GET "${result_json}" input_lines)
if(NOT result_status STREQUAL "success" OR NOT result_label STREQUAL "ctest" OR NOT result_lines EQUAL 3)
    message(FATAL_ERROR "SatsumaDemoApp result JSON is invalid: ${result_json}")
endif()

file(READ "${transformed_file}" transformed_content)
if(NOT transformed_content MATCHES "SATSUMA EXTERNAL ARTIFACT TEST")
    message(FATAL_ERROR "SatsumaDemoApp transformed output is invalid: ${transformed_content}")
endif()

file(REMOVE_RECURSE "${TEST_ROOT}")
