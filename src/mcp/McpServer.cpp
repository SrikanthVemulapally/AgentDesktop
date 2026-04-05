/**
 * @file McpServer.cpp
 * @brief Implementation of the MCP stdio JSON-RPC 2.0 server.
 *
 * @copyright AgentDesktop Project
 */

#include "McpServer.h"
#include "McpTools.h"

#include <cstdio>
#include <iostream>
#include <string>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <fcntl.h>
#  include <io.h>
#endif

namespace agentdesktop {
namespace mcp {

// ---------------------------------------------------------------------------
// Wire helpers
// ---------------------------------------------------------------------------

void McpServer::send(const json& response) {
    const std::string line = response.dump() + "\n";
    fwrite(line.c_str(), 1, line.size(), stdout);
    fflush(stdout);
}

json McpServer::make_error(const json& id, int code, const std::string& message) {
    return {
        {"jsonrpc", "2.0"},
        {"id",      id},
        {"error",   {{"code", code}, {"message", message}}}
    };
}

// ---------------------------------------------------------------------------
// tools/list schema builder
// ---------------------------------------------------------------------------

json McpServer::build_tools_list() {
    // Helper: build a simple {x,y} coordinate schema
    auto xy_schema = []() -> json {
        return {
            {"type", "object"},
            {"required", {"x", "y"}},
            {"properties", {
                {"x", {{"type","integer"},{"description","X coordinate in virtual-desktop pixels"}}},
                {"y", {{"type","integer"},{"description","Y coordinate in virtual-desktop pixels"}}}
            }}
        };
    };

    return json::array({
        {
            {"name",        "get_desktop_info"},
            {"description", "Return the virtual desktop dimensions, OS platform, and active state."},
            {"inputSchema", {{"type","object"},{"properties",json::object()}}}
        },
        {
            {"name",        "screenshot"},
            {"description",
             "Capture a PNG screenshot of the virtual desktop or a specific region. "
             "Always take a screenshot first to understand the current UI state. "
             "Omit x/y/w/h (or set to 0) for a full-desktop capture."},
            {"inputSchema", {
                {"type","object"},
                {"properties", {
                    {"x",{{"type","integer"},{"description","Left edge of region (default 0)"}}},
                    {"y",{{"type","integer"},{"description","Top  edge of region (default 0)"}}},
                    {"w",{{"type","integer"},{"description","Width  of region, 0=full desktop"}}},
                    {"h",{{"type","integer"},{"description","Height of region, 0=full desktop"}}}
                }}
            }}
        },
        {
            {"name",        "launch_app"},
            {"description",
             "Launch an application inside the virtual desktop. "
             "Use short names (notepad, chrome, firefox, cmd, code) "
             "or supply the full executable path."},
            {"inputSchema", {
                {"type","object"},
                {"required",{"path"}},
                {"properties", {
                    {"path",{{"type","string"},{"description","Application name or full exe path"}}},
                    {"args",{{"type","string"},{"description","Command-line arguments (optional)"}}}
                }}
            }}
        },
        {
            {"name",        "maximize_window"},
            {"description", "Maximise the primary window of a process by its PID."},
            {"inputSchema", {
                {"type","object"},
                {"required",{"pid"}},
                {"properties", {
                    {"pid",{{"type","integer"},{"description","Process ID returned by launch_app"}}}
                }}
            }}
        },
        {
            {"name",        "click"},
            {"description", "Send a single left-click at the specified coordinates."},
            {"inputSchema", xy_schema()}
        },
        {
            {"name",        "double_click"},
            {"description", "Send a double left-click at the specified coordinates."},
            {"inputSchema", xy_schema()}
        },
        {
            {"name",        "right_click"},
            {"description", "Send a right-click (context menu) at the specified coordinates."},
            {"inputSchema", xy_schema()}
        },
        {
            {"name",        "scroll"},
            {"description", "Scroll at the given position. Positive delta = up, negative = down."},
            {"inputSchema", {
                {"type","object"},
                {"required",{"x","y","delta"}},
                {"properties", {
                    {"x",    {{"type","integer"},{"description","X coordinate"}}},
                    {"y",    {{"type","integer"},{"description","Y coordinate"}}},
                    {"delta",{{"type","integer"},{"description","Lines to scroll: positive=up, negative=down"}}}
                }}
            }}
        },
        {
            {"name",        "type_text"},
            {"description", "Type a UTF-8 string into the currently focused window."},
            {"inputSchema", {
                {"type","object"},
                {"required",{"text"}},
                {"properties", {
                    {"text",{{"type","string"},{"description","Text to type"}}}
                }}
            }}
        },
        {
            {"name",        "key_press"},
            {"description",
             "Send a named key press. Supported keys: Enter, Escape, Tab, Backspace, Delete, "
             "Space, ArrowLeft, ArrowRight, ArrowUp, ArrowDown, Home, End, PageUp, PageDown, "
             "F1-F12. Modifier prefix: ctrl+, alt+, shift+, cmd+ (e.g. ctrl+c, alt+F4)."},
            {"inputSchema", {
                {"type","object"},
                {"required",{"key"}},
                {"properties", {
                    {"key",{{"type","string"},{"description","Key name, e.g. Enter, ctrl+c"}}}
                }}
            }}
        }
    });
}

// ---------------------------------------------------------------------------
// tools/call dispatcher
// ---------------------------------------------------------------------------

json McpServer::handle_tools_call(const json& params, const json& id) {
    if (!params.contains("name")) {
        return make_error(id, -32602, "Missing 'name' in tools/call params");
    }

    const std::string name = params["name"].get<std::string>();
    const json args = params.value("arguments", json::object());
    agentdesktop::Platform* plat = m_platform.get();

    json tool_result;

    if      (name == "get_desktop_info")  tool_result = tool_get_desktop_info(args, plat);
    else if (name == "screenshot")        tool_result = tool_screenshot(args, plat);
    else if (name == "launch_app")        tool_result = tool_launch_app(args, plat);
    else if (name == "maximize_window")   tool_result = tool_maximize_window(args, plat);
    else if (name == "click")             tool_result = tool_click(args, plat);
    else if (name == "double_click")      tool_result = tool_double_click(args, plat);
    else if (name == "right_click")       tool_result = tool_right_click(args, plat);
    else if (name == "scroll")            tool_result = tool_scroll(args, plat);
    else if (name == "type_text")         tool_result = tool_type_text(args, plat);
    else if (name == "key_press")         tool_result = tool_key_press(args, plat);
    else {
        return make_error(id, -32601, "Tool not found: " + name);
    }

    return {{"jsonrpc","2.0"}, {"id",id}, {"result", tool_result}};
}

// ---------------------------------------------------------------------------
// Main dispatch
// ---------------------------------------------------------------------------

json McpServer::dispatch(const json& req) {
    // Notifications have no id field — spec says no response.
    if (!req.contains("id") || req["id"].is_null()) {
        return nullptr;
    }

    const json id = req["id"];
    const std::string method = req.value("method", "");

    // ── initialize ──────────────────────────────────────────────────────────
    if (method == "initialize") {
        return {
            {"jsonrpc","2.0"}, {"id", id},
            {"result", {
                {"protocolVersion", "2024-11-05"},
                {"capabilities",    {{"tools", json::object()}}},
                {"serverInfo",      {{"name","AgentDesktop"},{"version","1.0.0"}}},
                {"instructions",
                 "You control an isolated virtual desktop via AgentDesktop.\n\n"
                 "RECOMMENDED WORKFLOW:\n"
                 "  1. screenshot — always start by taking a screenshot to see the current state.\n"
                 "  2. launch_app — start an app by name (notepad, chrome, etc.) or full path.\n"
                 "  3. screenshot — confirm the app launched and is visible.\n"
                 "  4. click / double_click / right_click — interact at coordinates you see in the screenshot.\n"
                 "  5. type_text / key_press — send keyboard input to the focused window.\n"
                 "  6. screenshot — verify the result of every significant action.\n\n"
                 "Coordinates are absolute virtual-desktop pixels. "
                 "Use get_desktop_info to get the desktop dimensions."}
            }}
        };
    }

    // ── notifications/initialized — no response ──────────────────────────────
    if (method.rfind("notifications/", 0) == 0) {
        return nullptr;
    }

    // ── ping ──────────────────────────────────────────────────────────────────
    if (method == "ping") {
        return {{"jsonrpc","2.0"}, {"id",id}, {"result", json::object()}};
    }

    // ── tools/list ────────────────────────────────────────────────────────────
    if (method == "tools/list") {
        return {
            {"jsonrpc","2.0"}, {"id",id},
            {"result", {{"tools", build_tools_list()}}}
        };
    }

    // ── tools/call ────────────────────────────────────────────────────────────
    if (method == "tools/call") {
        const json params = req.value("params", json::object());
        return handle_tools_call(params, id);
    }

    return make_error(id, -32601, "Method not found: " + method);
}

// ---------------------------------------------------------------------------
// run()
// ---------------------------------------------------------------------------

int McpServer::run() {
#ifdef _WIN32
    // Binary mode — prevents Windows CRLF translation corrupting the JSON stream.
    _setmode(_fileno(stdin),  _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif

    // Redirect stderr to a log file so diagnostics never pollute stdout.
#ifdef _WIN32
    freopen("agentdesktop_mcp.log", "a", stderr);
#else
    freopen("/tmp/agentdesktop_mcp.log", "a", stderr);
#endif

    fprintf(stderr, "[mcp] AgentDesktop MCP server starting (spec 2024-11-05)\n");

    // Initialise the virtual desktop platform.
    m_platform.reset(create_platform());
    std::string init_error;
    if (!m_platform->init(init_error)) {
        fprintf(stderr, "[mcp] Platform init failed: %s\n", init_error.c_str());
        // Keep running — tools will return a clear error message to the LLM.
        m_platform.reset();
    } else {
        fprintf(stderr, "[mcp] Platform ready: %dx%d\n",
                m_platform->phys_width(), m_platform->phys_height());
    }

    // Main JSON-RPC loop — reads one JSON object per line from stdin.
    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty()) continue;

        json response;
        try {
            const json request = json::parse(line);
            response = dispatch(request);
        } catch (const json::exception& ex) {
            response = make_error(nullptr, -32700,
                                   std::string("Parse error: ") + ex.what());
        } catch (const std::exception& ex) {
            response = make_error(nullptr, -32603,
                                   std::string("Internal error: ") + ex.what());
        } catch (...) {
            response = make_error(nullptr, -32603, "Unknown internal error");
        }

        // null sentinel = notification, send nothing.
        if (!response.is_null()) {
            send(response);
        }
    }

    fprintf(stderr, "[mcp] AgentDesktop MCP server exiting\n");
    return 0;
}

} // namespace mcp
} // namespace agentdesktop

// ---------------------------------------------------------------------------
// C-linkage entry point called from main()
// ---------------------------------------------------------------------------

int run_mcp_mode() {
    agentdesktop::mcp::McpServer server;
    return server.run();
}
