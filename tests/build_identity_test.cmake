find_package(Git REQUIRED)
string(RANDOM LENGTH 12 ALPHABET 0123456789abcdef test_id)
set(test_root "${TEST_OUTPUT}/identity-${test_id}")
set(source_fixture "${test_root}/source")
set(identity_header "${test_root}/build_identity.hpp")
file(MAKE_DIRECTORY "${test_root}")
# A local clone reuses the repository's real commits; the source checkout is never modified.
execute_process(
    COMMAND "${GIT_EXECUTABLE}" clone --quiet --shared "${SOURCE_ROOT}" "${source_fixture}"
    COMMAND_ERROR_IS_FATAL ANY
)
execute_process(
    COMMAND "${GIT_EXECUTABLE}" -C "${source_fixture}" rev-parse HEAD
    OUTPUT_VARIABLE expected_head OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY
)
function(generate_identity expected_commit)
    execute_process(
        COMMAND "${CMAKE_COMMAND}"
            "-DSATSUMA_SOURCE_DIR=${source_fixture}"
            "-DSATSUMA_IDENTITY_HEADER=${identity_header}"
            "-DSATSUMA_IDENTITY_VERSION=0.3.4"
            "-DSATSUMA_REQUESTED_BUILD_NUMBER=local"
            "-DSATSUMA_REQUESTED_BUILD_ATTEMPT=0"
            "-DSATSUMA_REQUESTED_GIT_COMMIT=${expected_commit}"
            -P "${SOURCE_ROOT}/cmake/build-identity.cmake"
        RESULT_VARIABLE identity_result ERROR_VARIABLE identity_error
    )
    set(identity_result "${identity_result}" PARENT_SCOPE)
    set(identity_error "${identity_error}" PARENT_SCOPE)
endfunction()

generate_identity(auto)
if(NOT identity_result EQUAL 0)
    message(FATAL_ERROR "Clean identity generation failed: ${identity_error}")
endif()
file(READ "${identity_header}" clean_identity)
if(NOT clean_identity MATCHES "SATSUMA_GIT_COMMIT \"${expected_head}\"" OR
   NOT clean_identity MATCHES "SATSUMA_BUILD_NUMBER \"local\\.[0-9a-f]+\"")
    message(FATAL_ERROR "Clean checkout provenance is missing or inaccurate")
endif()

file(WRITE "${source_fixture}/identity-fixture.txt" "first content\n")
generate_identity(auto)
file(READ "${identity_header}" dirty_identity)
if(NOT identity_result EQUAL 0 OR NOT dirty_identity MATCHES "${expected_head}-dirty" OR
   dirty_identity STREQUAL clean_identity)
    message(FATAL_ERROR "Untracked source changes were not marked dirty")
endif()

file(WRITE "${source_fixture}/identity-fixture.txt" "second content\n")
generate_identity(auto)
file(READ "${identity_header}" changed_identity)
if(NOT identity_result EQUAL 0 OR changed_identity STREQUAL dirty_identity)
    message(FATAL_ERROR "Changing untracked source content did not change the local build identity")
endif()
file(TIMESTAMP "${identity_header}" before_timestamp "%s")
generate_identity(auto)
file(READ "${identity_header}" unchanged_identity)
file(TIMESTAMP "${identity_header}" after_timestamp "%s")
if(NOT identity_result EQUAL 0 OR NOT changed_identity STREQUAL unchanged_identity OR
   NOT before_timestamp STREQUAL after_timestamp)
    message(FATAL_ERROR "An unchanged checkout rewrote its build identity")
endif()

generate_identity(0000000000000000000000000000000000000000)
if(identity_result EQUAL 0 OR NOT identity_error MATCHES "does not match the checkout HEAD")
    message(FATAL_ERROR "A false requested commit was accepted")
endif()

file(REAL_PATH "${TEST_OUTPUT}" test_parent)
file(REAL_PATH "${test_root}" cleanup_path)
string(FIND "${cleanup_path}" "${test_parent}/identity-" cleanup_prefix)
if(NOT cleanup_prefix EQUAL 0)
    message(FATAL_ERROR "Identity test cleanup path escaped its output directory")
endif()
file(REMOVE_RECURSE "${cleanup_path}")
