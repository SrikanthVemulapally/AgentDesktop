/**
 * @file McpTools.cpp
 * @brief Implementation of all MCP tool functions.
 *
 * Each function corresponds to one tool exposed in the MCP tools/list
 * response.  All functions are stateless and thread-safe.
 *
 * @copyright AgentDesktop Project
 */

#include "McpTools.h"
#include "ImageEncoder.h"

#include <cstring>
#include <string>

namespace agentdesktop {
namespace mcp {

// ---------------------------------------------------------------------------
// Result builders
// ---------------------------------------------------------------------------

json make_text_result(const std::string& text, bool is_error) {
    return {
        {"content", json::array({{
            {"type",    "text"},
            {"text",    text},
            {"isError", is_error}
        }})}
    };
}

json make_image_result(const std::string& base64_png) {
    return {
        {"content", json::array({{
            {"type",     "image"},
            {"data",     base64_png},
            {"mimeType", "image/png"}
        }})}
    };
}

// ---------------------------------------------------------------------------
// Guard helper — returns an error result when platform is unavailable
// ---------------------------------------------------------------------------

static bool require_platform(agentdesktop::Platform* plat, json& out) {
    if (!plat) {
        out = make_text_result(
            "Virtual desktop is not active. "
            "Start AgentDesktop in GUI mode and click Connect first, "
            "or launch with --mcp which initialises the platform automatically.",
            /*is_error=*/true);
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// get_desktop_info
// ---------------------------------------------------------------------------

json tool_get_desktop_info(const json& /*args*/, agentdesktop::Platform* plat) {
    if (!plat) {
        return make_text_result(
            "active=false  Virtual desktop not initialised.", false);
    }

    const std::string info =
        std::string("active=true")
        + "  width="    + std::to_string(plat->phys_width())
        + "  height="   + std::to_string(plat->phys_height())
#ifdef _WIN32
        + "  platform=windows"
#elif defined(__APPLE__)
        + "  platform=macos"
#else
        + "  platform=linux"
#endif
        ;
    return make_text_result(info, false);
}

// ---------------------------------------------------------------------------
// screenshot
// ---------------------------------------------------------------------------

json tool_screenshot(const json& args, agentdesktop::Platform* plat) {
    json guard;
    if (!require_platform(plat, guard)) return guard;

    const json& a = args.is_object() ? args : json::object();
    const int x = a.value("x", 0);
    const int y = a.value("y", 0);
    const int w = a.value("w", 0);
    const int h = a.value("h", 0);

    const ScreenshotResult sr = plat->screenshot(x, y, w, h);
    if (!sr.ok) {
        return make_text_result("Screenshot failed: " + sr.error, true);
    }

    const std::string b64 =
        screenshot_to_png_base64(sr.pixels.data(), sr.width, sr.height);
    if (b64.empty()) {
        return make_text_result("Screenshot encoding failed.", true);
    }
    return make_image_result(b64);
}

// ---------------------------------------------------------------------------
// launch_app
// ---------------------------------------------------------------------------

json tool_launch_app(const json& args, agentdesktop::Platform* plat) {
    json guard;
    if (!require_platform(plat, guard)) return guard;

    const json& a = args.is_object() ? args : json::object();
    const std::string path = a.value("path", "");
    if (path.empty()) {
        return make_text_result("Error: 'path' parameter is required.", true);
    }
    const std::string app_args = a.value("args", "");

    const LaunchResult lr = plat->launch_app(path, app_args);
    if (!lr.ok) {
        return make_text_result("Launch failed: " + lr.error, true);
    }
    return make_text_result(
        "Launched successfully. PID=" + std::to_string(lr.pid), false);
}

// ---------------------------------------------------------------------------
// maximize_window
// ---------------------------------------------------------------------------

json tool_maximize_window(const json& args, agentdesktop::Platform* plat) {
    json guard;
    if (!require_platform(plat, guard)) return guard;

    const int pid = args.value("pid", 0);
    if (pid <= 0) {
        return make_text_result("Error: 'pid' parameter is required and must be > 0.", true);
    }

    const ActionResult ar = plat->maximize_window(pid);
    return make_text_result(
        ar.ok ? "Window maximized." : "Error: " + ar.error, !ar.ok);
}

// ---------------------------------------------------------------------------
// click / double_click / right_click
// ---------------------------------------------------------------------------

json tool_click(const json& args, agentdesktop::Platform* plat) {
    json guard;
    if (!require_platform(plat, guard)) return guard;
    const ActionResult ar = plat->click(args.value("x", 0), args.value("y", 0));
    return make_text_result(ar.ok ? "Click sent." : "Error: " + ar.error, !ar.ok);
}

json tool_double_click(const json& args, agentdesktop::Platform* plat) {
    json guard;
    if (!require_platform(plat, guard)) return guard;
    const ActionResult ar = plat->double_click(args.value("x", 0), args.value("y", 0));
    return make_text_result(ar.ok ? "Double-click sent." : "Error: " + ar.error, !ar.ok);
}

json tool_right_click(const json& args, agentdesktop::Platform* plat) {
    json guard;
    if (!require_platform(plat, guard)) return guard;
    const ActionResult ar = plat->right_click(args.value("x", 0), args.value("y", 0));
    return make_text_result(ar.ok ? "Right-click sent." : "Error: " + ar.error, !ar.ok);
}

// ---------------------------------------------------------------------------
// scroll
// ---------------------------------------------------------------------------

json tool_scroll(const json& args, agentdesktop::Platform* plat) {
    json guard;
    if (!require_platform(plat, guard)) return guard;
    const ActionResult ar = plat->scroll(
        args.value("x", 0), args.value("y", 0), args.value("delta", 3));
    return make_text_result(ar.ok ? "Scroll sent." : "Error: " + ar.error, !ar.ok);
}

// ---------------------------------------------------------------------------
// type_text
// ---------------------------------------------------------------------------

json tool_type_text(const json& args, agentdesktop::Platform* plat) {
    json guard;
    if (!require_platform(plat, guard)) return guard;

    const json& a = args.is_object() ? args : json::object();
    const std::string text = a.value("text", "");
    if (text.empty()) {
        return make_text_result("Error: 'text' parameter is required.", true);
    }
    const ActionResult ar = plat->type_text(text);
    return make_text_result(
        ar.ok ? "Typed " + std::to_string(text.size()) + " character(s)."
              : "Error: " + ar.error,
        !ar.ok);
}

// ---------------------------------------------------------------------------
// key_press
// ---------------------------------------------------------------------------

json tool_key_press(const json& args, agentdesktop::Platform* plat) {
    json guard;
    if (!require_platform(plat, guard)) return guard;

    const std::string key = args.value("key", "");
    if (key.empty()) {
        return make_text_result("Error: 'key' parameter is required.", true);
    }
    const ActionResult ar = plat->key_press(key);
    return make_text_result(ar.ok ? "Key sent." : "Error: " + ar.error, !ar.ok);
}

} // namespace mcp
} // namespace agentdesktop
