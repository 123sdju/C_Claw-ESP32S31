cmake_minimum_required(VERSION 3.20)
project(cclaw_event_view_host C)
if(NOT DEFINED CCLAW_RUNTIME_ARCHIVE OR NOT EXISTS "${CCLAW_RUNTIME_ARCHIVE}")
    message(FATAL_ERROR "Set CCLAW_RUNTIME_ARCHIVE to a built POSIX c_claw_runtime library")
endif()
get_filename_component(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
add_executable(event_view_host_test test_event_view.c
    "${REPO_ROOT}/main/services/events/app_event_display_sink.c"
    "${REPO_ROOT}/main/services/events/app_event_ring.c"
    "${REPO_ROOT}/main/ui/board_ui_snapshot.c")
target_include_directories(event_view_host_test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/stubs" "${REPO_ROOT}/main/core"
    "${REPO_ROOT}/main/services" "${REPO_ROOT}/main/services/events" "${REPO_ROOT}/main/ui"
    "${REPO_ROOT}/third_party/cclaw-sdk/cclaw/core/include" "${REPO_ROOT}/third_party/cclaw-sdk/cclaw/ports/include"
    "${REPO_ROOT}/third_party/cclaw-sdk/cclaw/adapters/include")
target_compile_options(event_view_host_test PRIVATE -ffunction-sections -fdata-sections)
target_link_options(event_view_host_test PRIVATE -Wl,--gc-sections)
target_link_libraries(event_view_host_test PRIVATE "${CCLAW_RUNTIME_ARCHIVE}" pthread m)
add_executable(history_page_host_test test_history_page.c "${REPO_ROOT}/main/services/history_view.c")
target_include_directories(history_page_host_test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/stubs" "${REPO_ROOT}/main/core" "${REPO_ROOT}/main/services"
    "${REPO_ROOT}/main/services/events" "${REPO_ROOT}/third_party/cclaw-sdk/cclaw/core/include"
    "${REPO_ROOT}/third_party/cclaw-sdk/cclaw/ports/include" "${REPO_ROOT}/third_party/cclaw-sdk/cclaw/adapters/include")
target_link_libraries(history_page_host_test PRIVATE "${CCLAW_RUNTIME_ARCHIVE}" pthread m)
enable_testing()
add_test(NAME event_view_host COMMAND event_view_host_test)
add_test(NAME history_page_host COMMAND history_page_host_test)
