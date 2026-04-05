# Architecture

## Overview

AgentDesktop is a single-binary, cross-platform C++17 application that provides
an isolated virtual desktop for AI agents. The same executable runs in two modes:

| Mode | Invocation | Purpose |
|------|-----------|---------|
| **GUI** | `AgentDesktop` | Interactive viewer — GLFW + OpenGL + Dear ImGui |
| **MCP** | `AgentDesktop --mcp` | Stdio JSON-RPC 2.0 server — no GUI, no GLFW |

---

## Folder Structure

```
AgentDesktop/
├── src/
│   ├── main.cpp                  Entry point; mode selection (GUI vs --mcp)
│   ├── platform/
│   │   ├── Platform.h            Abstract interface + data types
│   │   ├── PlatformWin.cpp       Windows: CreateDesktopW / GDI / PostMessage
│   │   ├── PlatformMac.mm        macOS:   CGVirtualDisplay / SCKit / CGEvent
│   │   └── PlatformLinux.cpp     Linux:   Xvfb / XGetImage / XTest
│   ├── capture/
│   │   ├── FrameHash.h           FNV-1a 32-bit frame deduplication hash
│   │   ├── CaptureThread.h       Public API
│   │   └── CaptureThread.cpp     Background capture loop
│   ├── mcp/
│   │   ├── ImageEncoder.h/.cpp   BGRA→PNG→Base64 + downscaling
│   │   ├── McpTools.h/.cpp       One function per MCP tool
│   │   └── McpServer.h/.cpp      JSON-RPC 2.0 stdio loop + tool dispatch
│   └── app/
│       ├── LogEntry.h            Log entry data type
│       ├── Theme.h/.cpp          ImGui colour palette + style
│       └── App.h/.cpp            Main application class
├── tests/
│   ├── CMakeLists.txt
│   ├── test_frame_hash.cpp
│   ├── test_image_encoder.cpp
│   ├── test_mcp_dispatch.cpp
│   └── test_platform_mock.cpp
├── docs/
│   ├── ARCHITECTURE.md           This document
│   ├── MCP.md                    MCP integration guide
│   └── CONTRIBUTING.md           Development guide
├── scripts/
│   ├── build.ps1                 Windows build script
│   └── build.sh                  macOS / Linux build script
└── CMakeLists.txt                Root build file
```

---

## Layer Diagram

```
┌─────────────────────────────────────────────────────────┐
│                        main.cpp                         │
│            --mcp?  ──►  McpServer::run()                │
│            else    ──►  GLFW + ImGui render loop        │
└────────────┬──────────────────────────────┬────────────┘
             │ GUI mode                      │ MCP mode
             ▼                              ▼
    ┌────────────────┐           ┌─────────────────────┐
    │  AgentDesktopApp│           │     McpServer        │
    │  (App.h/cpp)   │           │  JSON-RPC dispatcher │
    └───────┬────────┘           └──────────┬──────────┘
            │                               │
            │ owns                          │ calls
            ▼                              ▼
    ┌──────────────┐            ┌─────────────────────┐
    │CaptureThread │            │     McpTools         │
    │(background)  │            │  (stateless fns)    │
    └──────┬───────┘            └──────────┬──────────┘
           │                               │
           │ calls                         │ calls
           ▼                              ▼
    ┌──────────────────────────────────────────────────┐
    │                Platform interface                 │
    │  create_platform() → PlatformWin / Mac / Linux   │
    └──────────────────────────────────────────────────┘
```

---

## Threading Model

| Thread | Responsibilities |
|--------|-----------------|
| **Main / Render** | GLFW events, ImGui rendering, GL texture upload, connecting/disconnecting |
| **Capture** | Calls `Platform::capture()` in a loop; stores changed frames in `LatestFrame` |
| **Worker** | One-shot async tasks: `launch_app`, `maximize_window`, `key_press` |

The only shared mutable state between threads is `LatestFrame`, which is protected
by a `std::mutex`. The mutex is held only during the brief frame copy — never
during the slow capture call itself.

In **MCP mode** there is only one thread (main). The capture thread is not started
because `McpServer` calls `Platform::screenshot()` on demand instead.

---

## Data Flow — GUI Mode

```
Platform::capture()       (capture thread, ~20 fps)
    │  raw BGRA pixels at CAPTURE_W (1280px) resolution
    ▼
LatestFrame::store()      (mutex lock, brief)
    │  dirty = true
    ▼
AgentDesktopApp::upload_frame()   (main thread, each render frame)
    │  glTexSubImage2D (GL_BGRA)
    ▼
ImGui::Image()            (renders GL texture in the preview panel)
```

---

## Data Flow — MCP Mode

```
AI client (stdin)
    │  {"method":"tools/call", "params":{"name":"screenshot",...}}
    ▼
McpServer::dispatch()
    ▼
McpTools::tool_screenshot()
    ▼
Platform::screenshot()    (main thread, native resolution)
    │  BGRA pixels
    ▼
ImageEncoder::screenshot_to_png_base64()
    │  scale to ≤1280px → PNG encode → base64
    ▼
AI client (stdout)
    │  {"result":{"content":[{"type":"image","data":"...","mimeType":"image/png"}]}}
```

---

## Platform Abstraction

`Platform` is a pure-virtual interface. The factory `create_platform()` is
defined once per platform file and returns a heap-allocated concrete instance.

| Platform | Virtual desktop | Capture | Mouse | Keyboard |
|----------|----------------|---------|-------|----------|
| Windows  | `CreateDesktopW` | GDI `PrintWindow` + `GetDIBits` | `PostMessage WM_*BUTTON*` | `SendInput` |
| macOS    | `CGVirtualDisplay` (SPI) | `SCScreenshotManager` | `CGEventCreateMouseEvent` | `CGEventCreateKeyboardEvent` |
| Linux    | Xvfb subprocess | `XGetImage` | `XTestFakeButtonEvent` | `XTestFakeKeyEvent` |

---

## Dependency Strategy

All dependencies are fetched at CMake configure time — no system packages required
(except the OpenGL and X11 system libraries on Linux).

| Dependency | Version | Purpose |
|-----------|---------|---------|
| GLFW | 3.4 | Window creation, OpenGL context, input events |
| Dear ImGui | 1.91.5 | Immediate-mode UI |
| nlohmann/json | 3.11.3 | JSON-RPC parsing/serialisation |
| stb_image | master | JPEG/PNG decode for loading assets |
| stb_image_write | master | In-memory PNG encoding for screenshots |
| GoogleTest | 1.14.0 | Unit test framework (tests only) |
