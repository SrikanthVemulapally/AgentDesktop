# tests/CMakeLists.txt
# Unit-test suite for AgentDesktop — built with GoogleTest via FetchContent.
#
# Test targets
# ============
#   AgentDesktopTests — single executable covering all test files.
#
# Running
# =======
#   cmake --build . --target AgentDesktopTests
#   ctest --test-dir . --output-on-failure
#   # Or directly:
#   ./bin/AgentDesktopTests

cmake_minimum_required(VERSION 3.20)

# ── GoogleTest ────────────────────────────────────────────────────────────────
include(FetchContent)
FetchContent_Declare(googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.14.0
    GIT_SHALLOW    TRUE)
# Prevent GoogleTest from overriding our compiler/linker settings on Windows
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

# ── Test sources ──────────────────────────────────────────────────────────────
set(TEST_SOURCES
    test_frame_hash.cpp
    test_image_encoder.cpp
    test_mcp_dispatch.cpp
    test_platform_mock.cpp
    test_integration.cpp
)

# ── Application sources needed by tests (no GLFW / OpenGL deps) ──────────────
# Core units under test (no platform, no GLFW, no OpenGL)
set(TESTABLE_SOURCES
    ${CMAKE_SOURCE_DIR}/src/capture/CaptureThread.cpp
    ${CMAKE_SOURCE_DIR}/src/mcp/ImageEncoder.cpp
    ${CMAKE_SOURCE_DIR}/src/mcp/McpTools.cpp
)

# Platform implementation for integration tests
if(WIN32)
    list(APPEND TESTABLE_SOURCES
        ${CMAKE_SOURCE_DIR}/src/platform/PlatformWin.cpp)
elseif(APPLE)
    list(APPEND TESTABLE_SOURCES
        ${CMAKE_SOURCE_DIR}/src/platform/PlatformMac.mm)
elseif(UNIX)
    list(APPEND TESTABLE_SOURCES
        ${CMAKE_SOURCE_DIR}/src/platform/PlatformLinux.cpp)
endif()

add_executable(AgentDesktopTests ${TEST_SOURCES} ${TESTABLE_SOURCES})

target_include_directories(AgentDesktopTests PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${SINGLE_HDR_DIR})          # nlohmann/json, stb_image_write — set in parent

target_link_libraries(AgentDesktopTests PRIVATE
    GTest::gtest_main
    GTest::gmock)

if(WIN32)
    target_link_libraries(AgentDesktopTests PRIVATE
        user32 gdi32 kernel32)
elseif(APPLE)
    target_link_libraries(AgentDesktopTests PRIVATE
        "-framework CoreGraphics"
        "-framework ScreenCaptureKit"
        "-framework AppKit"
        "-framework Foundation"
        "-framework ApplicationServices"
        "-framework Accessibility")
elseif(UNIX)
    find_package(X11 REQUIRED)
    target_link_libraries(AgentDesktopTests PRIVATE
        ${X11_LIBRARIES} ${X11_Xtst_LIB} pthread)
endif()

if(WIN32)
    target_compile_definitions(AgentDesktopTests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

# ── CTest integration ─────────────────────────────────────────────────────────
include(GoogleTest)
gtest_discover_tests(AgentDesktopTests)

set_target_properties(AgentDesktopTests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY                "${CMAKE_SOURCE_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG          "${CMAKE_SOURCE_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE        "${CMAKE_SOURCE_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${CMAKE_SOURCE_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL     "${CMAKE_SOURCE_DIR}/bin")
