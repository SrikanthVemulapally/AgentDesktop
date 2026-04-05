cmake_minimum_required(VERSION 3.20)
project(AgentDesktop LANGUAGES CXX)

# =============================================================================
# Project-wide settings
# =============================================================================

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)   # strict ISO C++ only (no GNU extensions)

include(FetchContent)
set(FETCHCONTENT_QUIET OFF)

# Enable CTest
enable_testing()

# Static CRT on MSVC — self-contained exe, no MSVC runtime DLLs required.
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# =============================================================================
# External dependencies (fetched once, cached in build tree)
# =============================================================================

# ── GLFW 3.4 ─────────────────────────────────────────────────────────────────
FetchContent_Declare(glfw
    GIT_REPOSITORY https://github.com/glfw/glfw.git
    GIT_TAG        3.4
    GIT_SHALLOW    TRUE)
set(GLFW_BUILD_DOCS     OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL        OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(glfw)

# ── Dear ImGui v1.91.5 ────────────────────────────────────────────────────────
FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        v1.91.5
    GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(imgui)

set(IMGUI_DIR ${imgui_SOURCE_DIR})
set(IMGUI_SOURCES
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_glfw.cpp
    ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp)

# ── Single-header libraries (downloaded at configure time) ───────────────────
set(SINGLE_HDR_DIR "${CMAKE_CURRENT_BINARY_DIR}/include")
file(MAKE_DIRECTORY "${SINGLE_HDR_DIR}/nlohmann")

if(NOT EXISTS "${SINGLE_HDR_DIR}/nlohmann/json.hpp")
    file(DOWNLOAD
        "https://github.com/nlohmann/json/releases/download/v3.11.3/json.hpp"
        "${SINGLE_HDR_DIR}/nlohmann/json.hpp" TLS_VERIFY ON
        STATUS _dl_status)
    list(GET _dl_status 0 _dl_code)
    if(NOT _dl_code EQUAL 0)
        message(FATAL_ERROR "Failed to download nlohmann/json.hpp")
    endif()
endif()

if(NOT EXISTS "${SINGLE_HDR_DIR}/stb_image.h")
    file(DOWNLOAD
        "https://raw.githubusercontent.com/nothings/stb/master/stb_image.h"
        "${SINGLE_HDR_DIR}/stb_image.h" TLS_VERIFY ON)
endif()

if(NOT EXISTS "${SINGLE_HDR_DIR}/stb_image_write.h")
    file(DOWNLOAD
        "https://raw.githubusercontent.com/nothings/stb/master/stb_image_write.h"
        "${SINGLE_HDR_DIR}/stb_image_write.h" TLS_VERIFY ON)
endif()

# =============================================================================
# Source file lists
# =============================================================================

# ── Core library sources (shared between app and tests) ───────────────────────
# These units have no dependency on GLFW, OpenGL, or the platform layer,
# so they can be compiled into both the main app and the test suite.
set(CORE_SOURCES
    src/capture/CaptureThread.cpp
    src/mcp/ImageEncoder.cpp
    src/mcp/McpTools.cpp
    src/mcp/McpServer.cpp
)

# ── GUI-only sources ──────────────────────────────────────────────────────────
set(APP_SOURCES
    src/main.cpp
    src/app/App.cpp
    src/app/Theme.cpp
    ${CORE_SOURCES}
)

# ── Platform-specific source ──────────────────────────────────────────────────
if(WIN32)
    list(APPEND APP_SOURCES src/platform/PlatformWin.cpp)
elseif(APPLE)
    list(APPEND APP_SOURCES src/platform/PlatformMac.mm)
    set_source_files_properties(src/platform/PlatformMac.mm PROPERTIES
        COMPILE_FLAGS "-fobjc-arc")
elseif(UNIX)
    list(APPEND APP_SOURCES src/platform/PlatformLinux.cpp)
endif()

# =============================================================================
# AgentDesktop executable
# =============================================================================

add_executable(AgentDesktop ${APP_SOURCES} ${IMGUI_SOURCES})

target_include_directories(AgentDesktop PRIVATE
    src
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
    ${SINGLE_HDR_DIR})

target_link_libraries(AgentDesktop PRIVATE glfw)

if(WIN32)
    target_compile_definitions(AgentDesktop PRIVATE
        UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(AgentDesktop PRIVATE
        opengl32 user32 gdi32 kernel32 dwmapi)

elseif(APPLE)
    find_package(OpenGL REQUIRED)
    target_link_libraries(AgentDesktop PRIVATE
        OpenGL::GL
        "-framework CoreGraphics"
        "-framework ScreenCaptureKit"
        "-framework AppKit"
        "-framework Foundation"
        "-framework ApplicationServices"
        "-framework Accessibility")

elseif(UNIX)
    find_package(OpenGL REQUIRED)
    find_package(X11    REQUIRED)
    target_link_libraries(AgentDesktop PRIVATE
        OpenGL::GL
        ${X11_LIBRARIES}
        ${X11_Xtst_LIB}
        ${CMAKE_DL_LIBS}
        pthread)
    target_include_directories(AgentDesktop PRIVATE ${X11_INCLUDE_DIR})
endif()

set_target_properties(AgentDesktop PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY                "${CMAKE_SOURCE_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG          "${CMAKE_SOURCE_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE        "${CMAKE_SOURCE_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${CMAKE_SOURCE_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL     "${CMAKE_SOURCE_DIR}/bin")

# =============================================================================
# Test suite
# =============================================================================
# Pass the single-header directory to the tests subdirectory
add_subdirectory(tests)
