cmake_minimum_required(VERSION 3.25)
project(VoiceIsolatorRTX VERSION 1.0.2 LANGUAGES C CXX)

if(NOT MSVC OR NOT CMAKE_CXX_COMPILER_ARCHITECTURE_ID STREQUAL "x64")
    message(FATAL_ERROR "Build for Windows x64 using MSVC (Visual Studio 2022, -A x64).")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(SDK_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/vst3sdk")
if(NOT EXISTS "${SDK_ROOT}/public.sdk/source/vst/vstaudioeffect.h")
    message(FATAL_ERROR "Initialize the VST3 SDK submodules as described in README.md.")
endif()

set(SMTG_ENABLE_VST3_PLUGIN_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SMTG_ENABLE_VST3_HOSTING_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SMTG_ENABLE_VSTGUI_SUPPORT OFF CACHE BOOL "" FORCE)
set(SMTG_CREATE_PLUGIN_LINK OFF CACHE BOOL "" FORCE)
set(SMTG_RUN_VST_VALIDATOR OFF CACHE BOOL "" FORCE)
set(SMTG_CREATE_BUNDLE_FOR_WINDOWS ON CACHE BOOL "" FORCE)
add_subdirectory("${SDK_ROOT}" sdk-build EXCLUDE_FROM_ALL)
set(public_sdk_SOURCE_DIR "${SDK_ROOT}/public.sdk")

smtg_add_vst3plugin(VoiceIsolatorRTX
    PACKAGE_NAME "VoiceIsolatorRTX"
    SOURCES_LIST src/plugin.cpp src/bridge.cpp src/editor.cpp)
target_link_libraries(VoiceIsolatorRTX PRIVATE sdk ole32 advapi32 comctl32)
target_compile_definitions(VoiceIsolatorRTX PRIVATE VOICE_ISOLATOR_RTX NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_options(VoiceIsolatorRTX PRIVATE /PDBALTPATH:%_PDB%)

configure_file("LICENSE" "${CMAKE_CURRENT_BINARY_DIR}/Voice-Isolator-RTX-LICENSE.txt" COPYONLY)
configure_file("${SDK_ROOT}/LICENSE.txt" "${CMAKE_CURRENT_BINARY_DIR}/Steinberg-SDK-LICENSE.txt" COPYONLY)
smtg_target_add_plugin_resources(VoiceIsolatorRTX RESOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/runtime/worker.py"
    "${CMAKE_CURRENT_SOURCE_DIR}/licenses/Inworld-LICENSE.txt"
    "${CMAKE_CURRENT_SOURCE_DIR}/licenses/Inworld-NOTICE.txt"
    "${CMAKE_CURRENT_BINARY_DIR}/Voice-Isolator-RTX-LICENSE.txt"
    "${CMAKE_CURRENT_BINARY_DIR}/Steinberg-SDK-LICENSE.txt")
