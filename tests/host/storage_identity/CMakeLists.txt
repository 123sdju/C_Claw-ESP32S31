cmake_minimum_required(VERSION 3.20)
project(cclaw_storage_identity_host C)
if(NOT DEFINED CCLAW_RUNTIME_ARCHIVE OR NOT EXISTS "${CCLAW_RUNTIME_ARCHIVE}")
    message(FATAL_ERROR "Set CCLAW_RUNTIME_ARCHIVE to a built POSIX c_claw_runtime library")
endif()
get_filename_component(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
add_executable(storage_identity_host_test test_storage_identity.c
    "${REPO_ROOT}/main/services/storage_nvs_service.c")
target_include_directories(storage_identity_host_test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/stubs" "${REPO_ROOT}/main/services"
    "${REPO_ROOT}/third_party/cclaw-sdk/cclaw/core/include" "${REPO_ROOT}/third_party/cclaw-sdk/cclaw/ports/include")
target_link_libraries(storage_identity_host_test PRIVATE "${CCLAW_RUNTIME_ARCHIVE}" pthread m)
enable_testing()
add_test(NAME storage_identity_host COMMAND storage_identity_host_test)
