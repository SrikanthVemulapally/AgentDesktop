/**
 * @file ImageEncoder.h
 * @brief BGRA-to-PNG encoder and Base64 utilities for MCP image responses.
 *
 * The MCP protocol returns screenshots as base64-encoded PNG images inside
 * a JSON content block.  This module encapsulates:
 *
 *   1. BGRA → RGBA channel swap (OpenGL / Win32 native format → PNG RGB order).
 *   2. PNG encoding via stb_image_write (in-memory, no file I/O).
 *   3. Base64 encoding of the resulting PNG bytes.
 *   4. Optional nearest-neighbour downscale to a maximum width so that
 *      screenshots fit within typical LLM vision context windows (≤ 1 280 px).
 *
 * All functions are stateless and thread-safe.
 *
 * @copyright AgentDesktop Project
 */

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace agentdesktop {
namespace mcp {

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

/** Maximum image width sent to an LLM via MCP (pixels). */
static constexpr int LLM_MAX_WIDTH = 1280;

// ---------------------------------------------------------------------------
// API
// ---------------------------------------------------------------------------

/**
 * @brief Base64-encode arbitrary binary data.
 *
 * Uses the standard alphabet (A-Za-z0-9+/) with '=' padding.
 *
 * @param data  Pointer to the source bytes. May be null when @p len == 0.
 * @param len   Number of bytes to encode.
 * @return Base64-encoded string.
 */
[[nodiscard]] std::string base64_encode(const uint8_t* data, size_t len);

/**
 * @brief Encode a BGRA pixel buffer as a PNG, then Base64-encode the result.
 *
 * @param bgra    Packed BGRA pixels, top-down, stride = width * 4.
 * @param width   Image width  in pixels.
 * @param height  Image height in pixels.
 * @return Base64-encoded PNG string, empty on encoding failure.
 */
[[nodiscard]] std::string encode_png_base64(const uint8_t* bgra,
                                             int width, int height);

/**
 * @brief Downscale a BGRA image using nearest-neighbour interpolation.
 *
 * @param src     Source pixel buffer (BGRA, top-down).
 * @param src_w   Source width  in pixels.
 * @param src_h   Source height in pixels.
 * @param dst_w   Target width  in pixels.
 * @param dst_h   Target height in pixels.
 * @return Resized pixel buffer (BGRA, top-down). Empty on invalid input.
 */
[[nodiscard]] std::vector<uint8_t> scale_nearest(const uint8_t* src,
                                                   int src_w, int src_h,
                                                   int dst_w, int dst_h);

/**
 * @brief Encode a BGRA frame as a Base64 PNG, downscaling to LLM_MAX_WIDTH
 *        if the image is wider than that threshold.
 *
 * This is the convenience function used by McpTools::screenshot().
 *
 * @param bgra    Source pixel buffer (BGRA, top-down).
 * @param width   Original image width  in pixels.
 * @param height  Original image height in pixels.
 * @return Base64-encoded PNG (possibly downscaled).
 */
[[nodiscard]] std::string screenshot_to_png_base64(const uint8_t* bgra,
                                                    int width, int height);

} // namespace mcp
} // namespace agentdesktop
