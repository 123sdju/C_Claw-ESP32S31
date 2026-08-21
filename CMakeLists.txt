# ESP-IDF boilerplate must remain in this order.
cmake_minimum_required(VERSION 3.22)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

set(CC_PROFILE "esp32s31-product" CACHE STRING "ESP32S31 CClaw product profile")
set(CC_TARGET_PLATFORM "esp32" CACHE STRING "CClaw target platform")
set(CCLAW_SDK_DIR
    "${CMAKE_CURRENT_LIST_DIR}/third_party/cclaw-sdk"
    CACHE PATH "Pinned CClaw SDK checkout")
set(CCLAW_PROFILE_FILE
    "${CMAKE_CURRENT_LIST_DIR}/cmake/esp32s31-profile.cmake"
    CACHE FILEPATH "ESP32S31-owned CClaw profile")
set(CCLAW_PROFILE_LOCK_OPTIONS ON CACHE BOOL
    "Lock SDK feature options to the ESP32S31 product profile" FORCE)

project(CClawESP32S31 VERSION 0.1.0)

if(NOT EXISTS "${CCLAW_SDK_DIR}/cclaw/CMakeLists.txt")
    message(FATAL_ERROR
        "CClaw SDK submodule is missing. Run: git submodule update --init --recursive")
endif()

add_subdirectory(
    "${CCLAW_SDK_DIR}/cclaw"
    "${CMAKE_BINARY_DIR}/cclaw-sdk")

idf_component_get_property(main_lib main COMPONENT_LIB)
target_link_libraries(${main_lib} PRIVATE CClaw::runtime)

idf_component_get_property(esp_video_lib espressif__esp_video COMPONENT_LIB)
if(TARGET ${esp_video_lib})
    target_compile_definitions(${esp_video_lib} PRIVATE LOG_LOCAL_LEVEL=3)
endif()
