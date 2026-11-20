cmake_minimum_required(VERSION 3.20)
project(cclaw_config_stage_host C)
if(NOT DEFINED CCLAW_RUNTIME_ARCHIVE OR NOT EXISTS "${CCLAW_RUNTIME_ARCHIVE}")
    message(FATAL_ERROR "Set CCLAW_RUNTIME_ARCHIVE to a built POSIX c_claw_runtime library")
endif()
get_filename_component(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
add_executable(config_stage_host_test test_config_stage.c "${REPO_ROOT}/main/core/app_config.c")
target_include_directories(config_stage_host_test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/stubs" "${REPO_ROOT}/main/core" "${REPO_ROOT}/main/services"
    "${REPO_ROOT}/third_party/cclaw-sdk/cclaw/core/include" "${REPO_ROOT}/third_party/cclaw-sdk/cclaw/ports/include")
target_link_options(config_stage_host_test PRIVATE
    -Wl,--wrap=cc_filesystem_get_default -Wl,--wrap=cc_secret_store_get_default)
target_link_libraries(config_stage_host_test PRIVATE "${CCLAW_RUNTIME_ARCHIVE}" pthread m)
enable_testing()
add_test(NAME config_stage_host COMMAND config_stage_host_test)
