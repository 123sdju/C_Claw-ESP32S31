cmake_minimum_required(VERSION 3.20)
project(cclaw_network_stop_host C)
find_package(Threads REQUIRED)
get_filename_component(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
add_executable(network_stop_host_test test_network_stop.c
    "${REPO_ROOT}/main/services/wifi_event_callback.c")
target_include_directories(network_stop_host_test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/stubs" "${REPO_ROOT}/main/services"
    "${REPO_ROOT}/third_party/cclaw-sdk/cclaw/core/include" "${REPO_ROOT}/third_party/cclaw-sdk/cclaw/ports/include"
    "${REPO_ROOT}/third_party/cclaw-sdk/cclaw/adapters/include")
target_compile_options(network_stop_host_test PRIVATE -ffunction-sections -fdata-sections)
target_link_options(network_stop_host_test PRIVATE -Wl,--gc-sections)
target_link_libraries(network_stop_host_test PRIVATE Threads::Threads)
enable_testing()
add_test(NAME network_stop_host COMMAND network_stop_host_test)
set_tests_properties(network_stop_host PROPERTIES TIMEOUT 10)
