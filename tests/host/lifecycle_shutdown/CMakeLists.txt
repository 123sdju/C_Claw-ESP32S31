cmake_minimum_required(VERSION 3.20)
project(cclaw_lifecycle_shutdown_host C)
get_filename_component(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
add_executable(lifecycle_shutdown_host_test test_lifecycle_shutdown.c
    "${REPO_ROOT}/main/core/app_shutdown_sequence.c"
    "${REPO_ROOT}/main/core/app_usb_owner_guard.c")
target_include_directories(lifecycle_shutdown_host_test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/stubs" "${CMAKE_CURRENT_SOURCE_DIR}/../recovery_service/stubs" "${REPO_ROOT}/main/core")
find_package(Threads REQUIRED)
target_link_libraries(lifecycle_shutdown_host_test PRIVATE Threads::Threads)
enable_testing()
add_test(NAME lifecycle_shutdown_host COMMAND lifecycle_shutdown_host_test)
