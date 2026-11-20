cmake_minimum_required(VERSION 3.20)
project(cclaw_wifi_event_callback_host C)
find_package(Threads REQUIRED)
get_filename_component(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
add_executable(wifi_event_callback_host_test test_wifi_event_callback.c
    "${REPO_ROOT}/main/services/wifi_event_callback.c")
target_include_directories(wifi_event_callback_host_test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/stubs" "${REPO_ROOT}/main/services")
target_link_libraries(wifi_event_callback_host_test PRIVATE Threads::Threads)
enable_testing()
add_test(NAME wifi_event_callback_host COMMAND wifi_event_callback_host_test)
