cmake_minimum_required(VERSION 3.20)
project(cclaw_recovery_service_host C)
if(NOT DEFINED CCLAW_RUNTIME_ARCHIVE OR NOT EXISTS "${CCLAW_RUNTIME_ARCHIVE}")
    message(FATAL_ERROR "Set CCLAW_RUNTIME_ARCHIVE to a built POSIX c_claw_runtime library")
endif()
if(NOT DEFINED RECOVERY_TEST_ROOT OR NOT IS_ABSOLUTE "${RECOVERY_TEST_ROOT}")
    message(FATAL_ERROR "Set RECOVERY_TEST_ROOT to a new isolated absolute directory")
endif()
get_filename_component(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
add_executable(recovery_service_host_test
    test_recovery_service.c "${REPO_ROOT}/main/core/app_config.c"
    "${REPO_ROOT}/main/core/cclaw_runtime_features.c" "${REPO_ROOT}/main/services/recovery_service.c"
    "${REPO_ROOT}/main/services/session_selection.c"
    "${REPO_ROOT}/main/services/session_memory_turns.c"
    "${REPO_ROOT}/main/services/session_menu.c" "${REPO_ROOT}/main/services/history_view.c"
    "${REPO_ROOT}/main/services/recovery_console.c" "${REPO_ROOT}/main/services/manual_lua_service.c"
    "${REPO_ROOT}/main/services/memory_console.c" "${REPO_ROOT}/main/services/managed_memory_service.c"
    "${REPO_ROOT}/main/services/startup_managed_memory.c"
    "${REPO_ROOT}/main/services/startup_gate.c" "${REPO_ROOT}/main/services/startup_recovery.c")
target_include_directories(recovery_service_host_test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/stubs" "${REPO_ROOT}/main/core" "${REPO_ROOT}/main/services" "${REPO_ROOT}/main/ui"
    "${REPO_ROOT}/main/services/events"
    "${REPO_ROOT}/main/tools"
    "${REPO_ROOT}/third_party/cclaw-sdk/cclaw/core/include" "${REPO_ROOT}/third_party/cclaw-sdk/cclaw/ports/include"
    "${REPO_ROOT}/third_party/cclaw-sdk/cclaw/adapters/include")
target_compile_definitions(recovery_service_host_test PRIVATE
    CC_HAS_MEMORY=1
    APP_RECOVERY_ROOT="${RECOVERY_TEST_ROOT}/recovery" APP_RECOVERY_MEDIA_ROOT="${RECOVERY_TEST_ROOT}"
    RECOVERY_TEST_BASE="${RECOVERY_TEST_ROOT}")
target_link_options(recovery_service_host_test PRIVATE -Wl,--wrap=cc_filesystem_get_default
    -Wl,--wrap=cc_agent_runtime_abandon_run -Wl,--wrap=cc_runtime_controller_submit_text_with_run_id)
target_link_libraries(recovery_service_host_test PRIVATE "${CCLAW_RUNTIME_ARCHIVE}" pthread m)
enable_testing()
add_test(NAME recovery_service_host COMMAND recovery_service_host_test)
