# 回移 libzmq master 已修复的 Windows VMCI listener 单字符错误。
if(NOT DEFINED LIBZMQ_SOURCE_DIR)
    message(FATAL_ERROR "LIBZMQ_SOURCE_DIR is required")
endif()

set(vmci_listener_file "${LIBZMQ_SOURCE_DIR}/src/vmci_listener.cpp")
file(READ "${vmci_listener_file}" vmci_listener_content)

set(vmci_listener_old "    if (s == INVALID_SOCKET) {")
set(vmci_listener_new "    if (_s == INVALID_SOCKET) {")
string(FIND "${vmci_listener_content}" "${vmci_listener_old}" vmci_listener_old_position)
if(NOT vmci_listener_old_position EQUAL -1)
    string(REPLACE
        "${vmci_listener_old}"
        "${vmci_listener_new}"
        vmci_listener_content
        "${vmci_listener_content}")
    file(WRITE "${vmci_listener_file}" "${vmci_listener_content}")
else()
    string(FIND "${vmci_listener_content}" "${vmci_listener_new}" vmci_listener_new_position)
    if(vmci_listener_new_position EQUAL -1)
        message(FATAL_ERROR "Unsupported libzmq vmci_listener.cpp content")
    endif()
endif()
