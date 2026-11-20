cmake_minimum_required(VERSION 3.20)
project(cclaw_usb_storage_host C)
get_filename_component(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
add_executable(usb_storage_host_test test_usb_storage.c)
target_include_directories(usb_storage_host_test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/stubs" "${REPO_ROOT}/main/services")
enable_testing()
add_test(NAME usb_storage_host COMMAND usb_storage_host_test)
