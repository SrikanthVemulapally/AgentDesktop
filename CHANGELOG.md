# Changelog

All notable changes to AgentDesktop are documented here.
This project adheres to [Semantic Versioning](https://semver.org/).

---

## [Unreleased]

### Added
- Nothing yet.

---

## [1.0.0] — 2026-04-01

### Added
- **Single-binary architecture** — one executable, two modes (GUI + MCP `--mcp`).
- **Platform abstraction layer** (`src/platform/`) with implementations for:
  - Windows — `CreateDesktopW` / GDI `PrintWindow` / `SendInput`
  - macOS   — `CGVirtualDisplay` / `ScreenCaptureKit` / `CGEvent`
  - Linux   — Xvfb / `XGetImage` / XTest
- **CaptureThread** (`src/capture/`) — background frame capture with FNV-1a
  deduplication; only changed frames are uploaded to the GPU.
- **MCP server** (`src/mcp/`) — full Model Context Protocol 2024-11-05
  implementation over stdio JSON-RPC 2.0 with 10 tools:
  `get_desktop_info`, `screenshot`, `launch_app`, `maximize_window`,
  `click`, `double_click`, `right_click`, `scroll`, `type_text`, `key_press`.
- **ImageEncoder** — BGRA→RGBA→PNG (stb_image_write) + Base64; screenshots
  auto-downscaled to ≤ 1280 px for LLM vision context.
- **ImGui UI** (`src/app/`) — dark-navy theme, preview panel with click/scroll
  forwarding, sidebar (Launch / Text / Keyboard / MCP / Window), log panel,
  status bar.
- **"Copy MCP Config" button** — copies the exact `mcp.json` snippet with the
  full path to the binary.
- **Unit tests** (`tests/`) — GoogleTest suite covering FrameHash,
  ImageEncoder, MCP tool dispatch (with MockPlatform), and CaptureThread.
- **Documentation** — `docs/ARCHITECTURE.md`, `docs/MCP.md`,
  `docs/CONTRIBUTING.md`, `README.md`, `CHANGELOG.md`, `.clang-format`.
- **Build scripts** — `scripts/build.ps1` (Windows), `scripts/build.sh`
  (macOS / Linux).
- CMake FetchContent for all dependencies — no system packages required
  beyond OpenGL, X11 (Linux), and Xcode frameworks (macOS).

### Technical
- C++17 throughout; `CMAKE_CXX_EXTENSIONS OFF` (no GNU extensions).
- MSVC static CRT (`MultiThreaded`) — no runtime DLL dependencies.
- `NOMINMAX` + `WIN32_LEAN_AND_MEAN` applied consistently to avoid
  Windows header macro conflicts.
