cmake_minimum_required(VERSION 3.20)
project(cclaw_web_lifecycle_host C)
get_filename_component(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
add_executable(web_lifecycle_host_test test_web_lifecycle.c "${REPO_ROOT}/main/transports/web_console.c")
target_include_directories(web_lifecycle_host_test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/stubs" "${REPO_ROOT}/main/transports" "${REPO_ROOT}/main/core"
    "${REPO_ROOT}/main/services" "${REPO_ROOT}/third_party/cclaw-sdk/cclaw/core/include" "${REPO_ROOT}/third_party/cclaw-sdk/cclaw/ports/include")
target_link_libraries(web_lifecycle_host_test PRIVATE pthread)
enable_testing()
add_test(NAME web_lifecycle_host COMMAND web_lifecycle_host_test)

add_executable(web_limits_host_test test_web_lifecycle.c
    "${REPO_ROOT}/main/transports/web_console.c" "${REPO_ROOT}/main/transports/web_console_events.c")
target_compile_definitions(web_limits_host_test PRIVATE CC_TEST_WEB_LIMITS=1)
target_include_directories(web_limits_host_test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/stubs" "${REPO_ROOT}/main/transports" "${REPO_ROOT}/main/core"
    "${REPO_ROOT}/main/services" "${REPO_ROOT}/third_party/cclaw-sdk/cclaw/core/include" "${REPO_ROOT}/third_party/cclaw-sdk/cclaw/ports/include")
target_link_libraries(web_limits_host_test PRIVATE pthread)
add_test(NAME web_limits_host COMMAND web_limits_host_test)
