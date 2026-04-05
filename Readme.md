# AgentDesktop

**Single-binary, cross-platform AI agent virtual desktop.**  
Pure C++17 — no Electron, no Node.js, no subprocesses, no IPC overhead.

```
Windows ✓   macOS ✓   Linux ✓
```

---

## Features

- **Isolated virtual desktop** — hidden from the user's session; applications run inside it cannot see the user's windows
- **Live preview** — real-time GL texture at ~20 fps with click, double-click, right-click, scroll forwarding
- **Full keyboard control** — on-screen grid + physical keyboard passthrough
- **MCP integration** — register with Claude, Cursor, Windsurf, Cline or any MCP client; AI agents get 10 tools to control the desktop
- **Single binary, two modes** — GUI viewer or headless MCP server, same executable
- **Zero runtime dependencies** — MSVC static CRT, all deps bundled at build time

---

## Two Modes

| Mode | Invocation | Description |
|------|-----------|-------------|
| **GUI** | `AgentDesktop.exe` | Interactive viewer — GLFW + OpenGL + Dear ImGui |
| **MCP** | `AgentDesktop.exe --mcp` | Headless stdio JSON-RPC 2.0 MCP server |

---

## What it does

AgentDesktop creates a **hidden virtual desktop** that is invisible to the user.  
An AI agent (or you) can:
- Launch applications inside it
- See a live preview of the virtual desktop at ~20 fps
- Click, double-click, right-click, scroll at any position
- Type text and send key presses
- Maximize windows by PID

Everything runs **in the same process** — platform code is compiled directly in.  
Raw pixel data flows capture-thread → GPU texture with zero serialisation.

---

## Architecture

```
main.cpp  ─── GLFW + OpenGL 3.3 bootstrap
    │
    └── AgentDesktopApp (app.h / app.cpp)
          │
          ├── Platform interface (platform.h)
          │     ├── platform_win.cpp   — CreateDesktopW + GDI PrintWindow + PostMessage
          │     ├── platform_mac.mm    — CGVirtualDisplay + ScreenCaptureKit + CGEvent
          │     └── platform_linux.cpp — Xvfb + XGetImage + XTest
          │
          ├── CaptureThread (capture_thread.h / .cpp)
          │     └── calls Platform::capture() in a loop
          │           └── FNV-1a hash → skip identical frames
          │
          └── UI (Dear ImGui + OpenGL 3.3)
                ├── Title bar   — Connect / Disconnect / live FPS + resolution
                ├── Preview     — aspect-fit GL texture, click/dbl/right/scroll
                ├── Sidebar     — Launch App · Send Text · Keyboard · Window
                ├── Log panel   — timestamped, colour-coded, auto-scroll
                └── Status bar  — platform info + tech tags
```

**Why single-binary?**  
The original CoWorkspace approach needed a separate `capture-agent.exe` because  
Electron (a web runtime) cannot call Win32/CoreGraphics APIs directly.  
In pure C++ there is no such barrier — the platform layer compiles straight in,  
eliminating the pipe, JSON serialisation, base64 encoding, and subprocess management.

| Aspect | CoWorkspace-Native (2-binary) | AgentDesktop (1-binary) |
|---|---|---|
| Latency | Pipe + JSON + base64 decode | Direct memcpy to GPU |
| Complexity | IPC protocol, 2 CMake projects | One project, one exe |
| Startup | Agent spawn + ready handshake | `Platform::init()` inline |
| Debug | Cross-process | Single stack trace |

---

## Stack

| Layer | Technology |
|---|---|
| UI | [Dear ImGui](https://github.com/ocornut/imgui) v1.91.5 + OpenGL 3.3 Core |
| Window / input | [GLFW](https://github.com/glfw/glfw) 3.4 |
| Build | CMake 3.20+ with `FetchContent` (auto-downloads all deps) |
| Windows capture | Win32 `CreateDesktopW` + `GDI PrintWindow` |
| macOS capture | `CGVirtualDisplay` + `ScreenCaptureKit` |
| Linux capture | `Xvfb` + `XGetImage` (X11) |
| Windows input | `PostMessage` (WM_LBUTTONDOWN / WM_CHAR / WM_KEYDOWN) |
| macOS input | `CGEvent` (CoreGraphics) |
| Linux input | `XTest` extension |
| Frame dedup | FNV-1a 32-bit hash (header-only) |

---

## Quick Start

### Windows

**Prerequisites (one-time):**
```powershell
winget install Kitware.CMake
winget install Git.Git
# Visual Studio 2022 — "Desktop development with C++" workload
winget install Microsoft.VisualStudio.2022.Community
```

**Build and run:**
```powershell
git clone <repo> AgentDesktop
cd AgentDesktop
.\scripts\build.ps1
.\bin\AgentDesktop.exe
```

### macOS

**Prerequisites:**
```bash
xcode-select --install
brew install cmake
```

**Permissions required** (System Settings → Privacy & Security):
- **Screen Recording** — for ScreenCaptureKit capture
- **Accessibility** — for CGEvent mouse/keyboard input

**Build and run:**
```bash
chmod +x scripts/build.sh
./scripts/build.sh
./bin/AgentDesktop
```

### Linux

**Prerequisites:**
```bash
sudo apt install cmake build-essential git \
    libx11-dev libxtst-dev xvfb \
    openbox   # optional WM for app decorations
```

**Build and run:**
```bash
chmod +x scripts/build.sh
./scripts/build.sh
./bin/AgentDesktop
```

> Linux note: Xvfb is started automatically on `:99`–`:109`.  
> Install `openbox` (or any lightweight WM) for proper window decorations.

---

## Debug builds

```powershell
.\scripts\build.ps1 -Config Debug   # Windows
./scripts/build.sh Debug             # macOS / Linux
```

---

## Usage

1. **Click Connect** — creates the virtual desktop (Explorer/shell starts inside it).
2. **Launch App** — type an app name (`notepad`, `chrome`, `firefox`) or full path, click ▶ Launch.
3. **Interact** — click/double-click/right-click/scroll inside the preview window.
4. **Send Text** — type in the text box and press Enter or "Send Text".
5. **Keyboard** — use the on-screen keys or your physical keyboard (when preview is focused).
6. **Click Disconnect** — tears down the virtual desktop cleanly.

---

## Roadmap / Extending

| Feature | Where to add |
|---|---|
| HTTP/WebSocket API for AI agents | new `api_server.cpp` using `cpp-httplib` |
| Screenshot export (PNG/JPEG) | `stb_image_write` already available |
| Window listing / focus | extend `Platform` with `list_windows()` |
| Multi-monitor virtual desktop | platform-specific, Windows: multiple `HDESK` |
| Right-click menus | already wired — context menu from OS |
