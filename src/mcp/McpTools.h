/**
 * @file McpTools.h
 * @brief MCP tool implementations — each public function handles one MCP tool call.
 *
 * McpTools is a stateless namespace. Every function receives:
 *   - @p args    — the JSON arguments object from the tools/call request.
 *   - @p plat    — a pointer to the active Platform (may be null if not connected).
 *
 * Return values are nlohmann::json objects shaped as MCP tool-result content:
 *   Text:  { "content": [{ "type":"text",  "text":"...",  "isError":bool }] }
 *   Image: { "content": [{ "type":"image", "data":"...",  "mimeType":"image/png" }] }
 *
 * All functions are thread-safe as long as the Platform pointer remains
 * valid and its methods are themselves thread-safe (which they are per the
 * Platform contract).
 *
 * @copyright AgentDesktop Project
 */

#pragma once

#include "../platform/Platform.h"
#include "nlohmann/json.hpp"

#include <string>

namespace agentdesktop {
namespace mcp {

using json = nlohmann::json;

// ---------------------------------------------------------------------------
// Tool result builders (used internally and in tests)
// ---------------------------------------------------------------------------

/** @brief Build a text-content MCP tool result. */
[[nodiscard]] json make_text_result(const std::string& text,
                                    bool is_error = false);

/** @brief Build an image-content MCP tool result (PNG base64). */
[[nodiscard]] json make_image_result(const std::string& base64_png);

// ---------------------------------------------------------------------------
// Tool implementations
// ---------------------------------------------------------------------------

/**
 * @brief get_desktop_info — return dimensions and platform name.
 * @param args  Ignored (no parameters).
 * @param plat  May be null; in that case active=false is reported.
 */
[[nodiscard]] json tool_get_desktop_info(const json& args,
                                          agentdesktop::Platform* plat);

/**
 * @brief screenshot — capture the virtual desktop (or a region) as PNG.
 *
 * Optional args:
 *   x, y  — top-left corner of region (default 0).
 *   w, h  — size of region (default 0 = full desktop).
 *
 * The image is downscaled to ≤ LLM_MAX_WIDTH before encoding.
 */
[[nodiscard]] json tool_screenshot(const json& args,
                                    agentdesktop::Platform* plat);

/**
 * @brief launch_app — start an application in the virtual desktop.
 *
 * Required args: path (string).
 * Optional args: args (string).
 */
[[nodiscard]] json tool_launch_app(const json& args,
                                    agentdesktop::Platform* plat);

/**
 * @brief maximize_window — maximise the primary window of a process.
 *
 * Required args: pid (integer).
 */
[[nodiscard]] json tool_maximize_window(const json& args,
                                         agentdesktop::Platform* plat);

/**
 * @brief click — single left-click.
 * Required args: x, y (integers, virtual-desktop pixels).
 */
[[nodiscard]] json tool_click(const json& args, agentdesktop::Platform* plat);

/**
 * @brief double_click — double left-click.
 * Required args: x, y.
 */
[[nodiscard]] json tool_double_click(const json& args,
                                      agentdesktop::Platform* plat);

/**
 * @brief right_click — context-menu click.
 * Required args: x, y.
 */
[[nodiscard]] json tool_right_click(const json& args,
                                     agentdesktop::Platform* plat);

/**
 * @brief scroll — scroll wheel at a position.
 * Required args: x, y, delta (positive = up, negative = down).
 */
[[nodiscard]] json tool_scroll(const json& args, agentdesktop::Platform* plat);

/**
 * @brief type_text — type a UTF-8 string into the focused window.
 * Required args: text (string).
 */
[[nodiscard]] json tool_type_text(const json& args,
                                   agentdesktop::Platform* plat);

/**
 * @brief key_press — send a named key (Enter, Escape, ctrl+c, …).
 * Required args: key (string).
 */
[[nodiscard]] json tool_key_press(const json& args,
                                   agentdesktop::Platform* plat);

} // namespace mcp
} // namespace agentdesktop
