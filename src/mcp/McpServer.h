/**
 * @file McpServer.h
 * @brief MCP stdio JSON-RPC 2.0 server for AgentDesktop.
 *
 * McpServer implements the Model Context Protocol (spec 2024-11-05) using
 * the stdio transport.  When AgentDesktop is launched with the --mcp flag,
 * main() calls McpServer::run(), which:
 *
 *   1. Initialises the platform (creates the virtual desktop).
 *   2. Reads newline-delimited JSON-RPC requests from stdin.
 *   3. Dispatches each request to the appropriate McpTools function.
 *   4. Writes the JSON-RPC response to stdout.
 *
 * stderr is redirected to a log file so that diagnostic output never
 * contaminates the JSON stream.
 *
 * Supported JSON-RPC methods
 * --------------------------
 *   initialize              — MCP handshake; returns server capabilities.
 *   notifications/*         — No-op (notifications require no response).
 *   ping                    — Returns an empty result object.
 *   tools/list              — Returns all available tool definitions with schemas.
 *   tools/call              — Dispatches to the named McpTools function.
 *
 * Registration example (claude_desktop_config.json / mcp.json)
 * -------------------------------------------------------------
 * @code{.json}
 * {
 *   "mcpServers": {
 *     "agentdesktop": {
 *       "command": "C:/path/to/AgentDesktop.exe",
 *       "args":    ["--mcp"]
 *     }
 *   }
 * }
 * @endcode
 *
 * @copyright AgentDesktop Project
 */

#pragma once

#include "../platform/Platform.h"
#include "nlohmann/json.hpp"

#include <memory>
#include <string>

namespace agentdesktop {
namespace mcp {

using json = nlohmann::json;

/**
 * @brief Stdio MCP server.
 *
 * Owns its Platform instance.  Call run() once; it blocks until stdin
 * is closed (i.e. the AI client disconnects or the process is terminated).
 */
class McpServer {
public:
    McpServer()  = default;
    ~McpServer() = default;

    McpServer(const McpServer&)            = delete;
    McpServer& operator=(const McpServer&) = delete;

    /**
     * @brief Run the MCP server loop.
     *
     * Blocks until stdin reaches EOF.  Should be called from the main
     * thread when the binary is invoked with --mcp.
     *
     * @return Process exit code (0 = success).
     */
    int run();

private:
    std::unique_ptr<agentdesktop::Platform> m_platform;

    // -----------------------------------------------------------------------
    // Internal dispatch
    // -----------------------------------------------------------------------

    /** @brief Handle one parsed JSON-RPC request; returns the response or null
     *         for notifications. */
    json dispatch(const json& request);

    /** @brief Build the tools/list result array. */
    static json build_tools_list();

    /** @brief Dispatch a tools/call request to the matching McpTools function. */
    json handle_tools_call(const json& params, const json& id);

    // -----------------------------------------------------------------------
    // Wire helpers
    // -----------------------------------------------------------------------

    static void send(const json& response);
    static json make_error(const json& id, int code, const std::string& message);
};

} // namespace mcp
} // namespace agentdesktop

/**
 * @brief Entry point called from main() when --mcp flag is present.
 * @return Process exit code.
 */
int run_mcp_mode();
