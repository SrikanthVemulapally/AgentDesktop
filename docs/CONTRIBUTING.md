# Contributing to AgentDesktop

Thank you for considering a contribution.  This document explains how to set up
a development environment, run the tests, and submit changes.

---

## Prerequisites

| Tool | Version | Notes |
|------|---------|-------|
| CMake | ≥ 3.20 | `cmake --version` |
| C++ compiler | C++17 capable | MSVC 19.14+, Clang 7+, GCC 8+ |
| Git | any | For FetchContent cloning |
| **Windows only** | Visual Studio 2019+ or Build Tools | x64 toolchain |
| **macOS only** | Xcode 14+ | macOS 12.3+ for CGVirtualDisplay |
| **Linux only** | `libX11-dev`, `libXtst-dev`, `Xvfb`, `openbox` | apt/dnf |

---

## Building

```bash
# Clone
git clone https://github.com/yourorg/AgentDesktop.git
cd AgentDesktop

# Windows (PowerShell)
.\scripts\build.ps1

# macOS / Linux
./scripts/build.sh
```

Or manually:

```bash
cmake -S . -B build/Release -DCMAKE_BUILD_TYPE=Release
cmake --build build/Release --config Release --parallel
```

Binaries are placed in `bin/`.

---

## Running Tests

```bash
# Build with tests (default — tests are always included)
cmake -S . -B build/Release -DCMAKE_BUILD_TYPE=Release
cmake --build build/Release --config Release --parallel

# Run all tests
ctest --test-dir build/Release --output-on-failure -C Release

# Or run the test binary directly
./bin/AgentDesktopTests          # Linux / macOS
.\bin\AgentDesktopTests.exe      # Windows
```

### Adding a new test

1. Add a `.cpp` file in `tests/`.
2. Add it to `TEST_SOURCES` in `tests/CMakeLists.txt`.
3. Use the `MockPlatform` in `test_platform_mock.cpp` as a model for
   tests that need a Platform without a real virtual desktop.

---

## Code Style

The project follows the `.clang-format` file at the repository root.
Run `clang-format -i` on any file you touch:

```bash
clang-format -i src/**/*.cpp src/**/*.h
```

Key conventions:
- **Namespace everything** under `agentdesktop::` (except `create_platform()`
  which is extern "C" linkage by convention).
- **Every file** starts with a Doxygen `@file` block and a `@brief` summary.
- **Every public method** has a Doxygen comment in the header.
- **No `using namespace std`** at file scope.
- Prefer `[[nodiscard]]` on functions returning values the caller must not ignore.

---

## Project Structure Rules

| Layer | Allowed dependencies |
|-------|---------------------|
| `src/platform/` | Standard library only — no ImGui, no nlohmann |
| `src/capture/` | `platform/Platform.h`, standard library only |
| `src/mcp/` | `platform/Platform.h`, nlohmann/json, stb_image_write |
| `src/app/` | All of the above + ImGui + GLFW |
| `tests/` | GoogleTest + the units under test; **no** GLFW or OpenGL |

The separation ensures tests compile without any system-level GUI dependencies.

---

## Pull Request Checklist

- [ ] All existing tests pass (`ctest --output-on-failure`).
- [ ] New functionality has corresponding unit tests.
- [ ] Every new source file has a Doxygen `@file` / `@brief` block.
- [ ] Every new public API has Doxygen comments on all parameters.
- [ ] `clang-format` has been applied.
- [ ] `CHANGELOG.md` has an entry under `[Unreleased]`.
- [ ] The PR description explains *what* changed and *why*.

---

## Reporting Issues

Please include:
- OS version and compiler version.
- CMake version (`cmake --version`).
- Full build output (copy from terminal).
- Steps to reproduce.
