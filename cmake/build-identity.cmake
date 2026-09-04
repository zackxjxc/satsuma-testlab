# Derive build provenance from the checkout without changing its index or working tree.
foreach(required SATSUMA_SOURCE_DIR SATSUMA_IDENTITY_HEADER SATSUMA_IDENTITY_VERSION
        SATSUMA_REQUESTED_BUILD_NUMBER SATSUMA_REQUESTED_BUILD_ATTEMPT SATSUMA_REQUESTED_GIT_COMMIT)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "Missing build identity input: ${required}")
    endif()
endforeach()

find_package(Git QUIET)
set(SATSUMA_RESOLVED_GIT_COMMIT "unknown")
set(SATSUMA_SOURCE_FINGERPRINT "unknown")
if(GIT_FOUND)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" -C "${SATSUMA_SOURCE_DIR}" rev-parse --verify HEAD
        RESULT_VARIABLE git_result OUTPUT_VARIABLE git_head OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET
    )
    if(git_result EQUAL 0)
        if(NOT SATSUMA_REQUESTED_GIT_COMMIT MATCHES "^(auto|unknown)$" AND
           NOT SATSUMA_REQUESTED_GIT_COMMIT STREQUAL git_head)
            message(FATAL_ERROR "SATSUMA_GIT_COMMIT does not match the checkout HEAD: ${git_head}")
        endif()
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" -C "${SATSUMA_SOURCE_DIR}" status --porcelain=v1 --untracked-files=all
            OUTPUT_VARIABLE git_status OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY
        )
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" -c core.quotepath=false -C "${SATSUMA_SOURCE_DIR}"
                ls-files --cached --others --exclude-standard
            OUTPUT_VARIABLE source_files OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY
        )
        string(REPLACE "\n" ";" source_files "${source_files}")
        list(SORT source_files)
        set(source_identity "${git_head}\n${git_status}\n")
        foreach(source_file IN LISTS source_files)
            if(EXISTS "${SATSUMA_SOURCE_DIR}/${source_file}" AND
               NOT IS_DIRECTORY "${SATSUMA_SOURCE_DIR}/${source_file}")
                file(SHA256 "${SATSUMA_SOURCE_DIR}/${source_file}" source_hash)
                string(APPEND source_identity "${source_file}:${source_hash}\n")
            else()
                string(APPEND source_identity "${source_file}:missing\n")
            endif()
        endforeach()
        string(SHA256 SATSUMA_SOURCE_FINGERPRINT "${source_identity}")
        set(SATSUMA_RESOLVED_GIT_COMMIT "${git_head}")
        if(NOT git_status STREQUAL "")
            string(APPEND SATSUMA_RESOLVED_GIT_COMMIT "-dirty")
        endif()
    endif()
endif()

set(SATSUMA_RESOLVED_BUILD_NUMBER "${SATSUMA_REQUESTED_BUILD_NUMBER}")
if(SATSUMA_REQUESTED_BUILD_NUMBER STREQUAL "local")
    string(SUBSTRING "${SATSUMA_SOURCE_FINGERPRINT}" 0 12 source_short)
    set(SATSUMA_RESOLVED_BUILD_NUMBER "local.${source_short}")
endif()
set(SATSUMA_RESOLVED_BUILD_ATTEMPT "${SATSUMA_REQUESTED_BUILD_ATTEMPT}")
set(SATSUMA_RESOLVED_BUILD_ID
    "${SATSUMA_IDENTITY_VERSION}-${SATSUMA_RESOLVED_BUILD_NUMBER}.${SATSUMA_RESOLVED_BUILD_ATTEMPT}-g${SATSUMA_RESOLVED_GIT_COMMIT}")
configure_file(
    "${CMAKE_CURRENT_LIST_DIR}/build-identity.hpp.in"
    "${SATSUMA_IDENTITY_HEADER}"
    @ONLY
)
