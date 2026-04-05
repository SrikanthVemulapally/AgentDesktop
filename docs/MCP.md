# MCP Integration Guide

AgentDesktop implements the [Model Context Protocol](https://modelcontextprotocol.io/)
(spec **2024-11-05**) using the **stdio transport**.  When registered with an AI
client, the same `AgentDesktop` binary is launched with `--mcp` and communicates
via standard input/output using newline-delimited JSON-RPC 2.0.

---

## Quick Start

### 1. Register with your AI client

Paste the following into your AI client's MCP configuration file.  You can also
click **Copy MCP Config** in the AgentDesktop sidebar to get the snippet with the
exact path to your installed binary.

**Claude Desktop** (`~/Library/Application Support/Claude/claude_desktop_config.json`
on macOS, `%APPDATA%\Claude\claude_desktop_config.json` on Windows):

```json
{
  "mcpServers": {
    "agentdesktop": {
      "command": "C:/path/to/AgentDesktop.exe",
      "args":    ["--mcp"]
    }
  }
}
```

**Cursor / Windsurf / Continue / Cline** (`mcp.json` or `.cursor/mcp.json`):

```json
{
  "agentdesktop": {
    "command": "C:/path/to/AgentDesktop.exe",
    "args":    ["--mcp"]
  }
}
```

### 2. Restart your AI client

The client will launch `AgentDesktop.exe --mcp` automatically when it needs the
tools.  A log file `agentdesktop_mcp.log` (same directory as the binary) records
diagnostic output.

---

## Available Tools

| Tool | Description |
|------|-------------|
| `get_desktop_info` | Virtual desktop dimensions, OS platform, active state |
| `screenshot` | Capture PNG of full desktop or a region |
| `launch_app` | Start an application by name or path |
| `maximize_window` | Maximise the primary window of a process (by PID) |
| `click` | Single left-click at (x, y) |
| `double_click` | Double left-click at (x, y) |
| `right_click` | Right-click / context menu at (x, y) |
| `scroll` | Scroll wheel at (x, y); positive delta = up |
| `type_text` | Type a UTF-8 string into the focused window |
| `key_press` | Send a named key (Enter, ctrl+c, F5, …) |

---

## Tool Reference

### `get_desktop_info`

No parameters.  Returns a text string like:

```
active=true  width=1920  height=1080  platform=windows
```

---

### `screenshot`

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `x` | integer | 0 | Left edge of region |
| `y` | integer | 0 | Top edge of region |
| `w` | integer | 0 | Width (0 = full desktop) |
| `h` | integer | 0 | Height (0 = full desktop) |

Returns an MCP **image** content block (PNG, base64-encoded).
Wide screenshots (> 1280 px) are automatically downscaled to 1280 px wide to
fit within typical LLM vision context windows.

---

### `launch_app`

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `path` | string | ✓ | App name (`notepad`, `chrome`) or full path |
| `args` | string | — | Command-line arguments |

Returns: `Launched successfully. PID=<pid>`

Use the returned PID with `maximize_window`.

---

### `maximize_window`

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `pid` | integer | ✓ | Process ID returned by `launch_app` |

---

### `click` / `double_click` / `right_click`

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `x` | integer | ✓ | X coordinate in virtual-desktop pixels |
| `y` | integer | ✓ | Y coordinate in virtual-desktop pixels |

---

### `scroll`

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `x` | integer | ✓ | X coordinate |
| `y` | integer | ✓ | Y coordinate |
| `delta` | integer | ✓ | Lines to scroll: positive = up, negative = down |

---

### `type_text`

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `text` | string | ✓ | UTF-8 text to type into the focused window |

---

### `key_press`

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `key` | string | ✓ | Key name (see table below) |

**Supported key names:**

| Category | Keys |
|----------|------|
| Navigation | `ArrowLeft`, `ArrowRight`, `ArrowUp`, `ArrowDown`, `Home`, `End`, `PageUp`, `PageDown` |
| Editing | `Enter`, `Return`, `Escape`, `Tab`, `Backspace`, `Delete`, `Insert`, `Space` |
| Function | `F1` – `F12` |
| Modifier combos | Prefix with `ctrl+`, `alt+`, `shift+`, `cmd+`  e.g. `ctrl+c`, `ctrl+v`, `alt+F4` |

---

## Recommended Workflow

The AI server instructions embedded in the `initialize` response guide the model
to follow this workflow:

```
1. screenshot               → see the current state
2. launch_app (name/path)   → start the target application
3. screenshot               → confirm app launched
4. click at coordinates     → interact with UI elements
5. type_text / key_press    → enter data / trigger actions
6. screenshot               → verify result
```

**Always take a screenshot before clicking** — coordinates are absolute pixels
in the virtual desktop, so you need to see where things are first.

---

## Architecture Notes

- **No pipe, no subprocess** — in `--mcp` mode the binary initialises the
  platform directly (same code path as GUI mode) and runs the JSON-RPC loop
  on the main thread.  There is no named-pipe or IPC layer.

- **Screenshot encoding** — `Platform::screenshot()` returns raw BGRA pixels;
  `ImageEncoder::screenshot_to_png_base64()` converts them to RGBA, PNG-encodes
  via `stb_image_write`, and base64-encodes the result in-memory.

- **stderr** is redirected to `agentdesktop_mcp.log` so that no diagnostic
  output ever appears on stdout (which would corrupt the JSON stream).

- **stdin binary mode** is set on Windows via `_setmode` to prevent CRLF
  translation from corrupting multi-byte JSON sequences.
