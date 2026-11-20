cmake_minimum_required(VERSION 3.20)
project(cclaw_startup_gate_host C)
get_filename_component(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
add_executable(startup_gate_host_test test_startup_gate.c "${REPO_ROOT}/main/services/startup_gate.c")
target_include_directories(startup_gate_host_test PRIVATE
    "${REPO_ROOT}/tests/host/event_view/stubs" "${REPO_ROOT}/main/core"
    "${REPO_ROOT}/main/services" "${REPO_ROOT}/main/services/events"
    "${REPO_ROOT}/third_party/cclaw-sdk/cclaw/core/include" "${REPO_ROOT}/third_party/cclaw-sdk/cclaw/ports/include"
    "${REPO_ROOT}/third_party/cclaw-sdk/cclaw/adapters/include")
target_link_libraries(startup_gate_host_test PRIVATE pthread)
enable_testing()
add_test(NAME startup_gate_host COMMAND startup_gate_host_test)
