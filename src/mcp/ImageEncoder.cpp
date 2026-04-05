/**
 * @file ImageEncoder.cpp
 * @brief Implementation of BGRA-to-PNG encoding and Base64 utilities.
 *
 * Uses stb_image_write (single-header, in-memory callback) for PNG encoding.
 * No file I/O is performed. stb_image_write is compiled only in this
 * translation unit via STB_IMAGE_WRITE_IMPLEMENTATION to avoid ODR violations.
 *
 * @copyright AgentDesktop Project
 */

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include "stb_image_write.h"

#include "ImageEncoder.h"

#include <cstring>
#include <vector>

namespace agentdesktop {
namespace mcp {

// ---------------------------------------------------------------------------
// Base64
// ---------------------------------------------------------------------------

std::string base64_encode(const uint8_t* data, size_t len) {
    static constexpr char kTable[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    std::string out;
    out.reserve(((len + 2) / 3) * 4);

    for (size_t i = 0; i < len; i += 3) {
        const uint32_t b = (static_cast<uint32_t>(data[i]) << 16)
                         | (i + 1 < len ? static_cast<uint32_t>(data[i + 1]) << 8 : 0u)
                         | (i + 2 < len ? static_cast<uint32_t>(data[i + 2])       : 0u);

        out += kTable[(b >> 18) & 0x3Fu];
        out += kTable[(b >> 12) & 0x3Fu];
        out += (i + 1 < len) ? kTable[(b >> 6) & 0x3Fu] : '=';
        out += (i + 2 < len) ? kTable[(b     ) & 0x3Fu] : '=';
    }
    return out;
}

// ---------------------------------------------------------------------------
// PNG encoding
// ---------------------------------------------------------------------------

std::string encode_png_base64(const uint8_t* bgra, int width, int height) {
    if (!bgra || width <= 0 || height <= 0) return {};

    // Convert BGRA → RGBA (PNG expects RGBA).
    const size_t pixel_count = static_cast<size_t>(width) * static_cast<size_t>(height);
    std::vector<uint8_t> rgba(pixel_count * 4);
    for (size_t i = 0; i < pixel_count; ++i) {
        rgba[i * 4 + 0] = bgra[i * 4 + 2]; // R ← B source index
        rgba[i * 4 + 1] = bgra[i * 4 + 1]; // G
        rgba[i * 4 + 2] = bgra[i * 4 + 0]; // B ← R source index
        rgba[i * 4 + 3] = 0xFF;             // A — always opaque
    }

    // Accumulate PNG bytes via stb callback.
    struct Buffer { std::vector<uint8_t> data; };
    Buffer buf;
    buf.data.reserve(pixel_count);           // rough pre-allocation

    stbi_write_png_to_func(
        [](void* ctx, void* bytes, int n) {
            auto* b = static_cast<Buffer*>(ctx);
            const auto* p = static_cast<const uint8_t*>(bytes);
            b->data.insert(b->data.end(), p, p + n);
        },
        &buf, width, height, 4, rgba.data(), width * 4);

    if (buf.data.empty()) return {};
    return base64_encode(buf.data.data(), buf.data.size());
}

// ---------------------------------------------------------------------------
// Nearest-neighbour scale
// ---------------------------------------------------------------------------

std::vector<uint8_t> scale_nearest(const uint8_t* src,
                                    int src_w, int src_h,
                                    int dst_w, int dst_h) {
    if (!src || src_w <= 0 || src_h <= 0 || dst_w <= 0 || dst_h <= 0) return {};

    std::vector<uint8_t> dst(static_cast<size_t>(dst_w) * dst_h * 4);
    for (int dy = 0; dy < dst_h; ++dy) {
        const int sy = dy * src_h / dst_h;
        for (int dx = 0; dx < dst_w; ++dx) {
            const int sx = dx * src_w / dst_w;
            const uint8_t* s = src  + (static_cast<size_t>(sy) * src_w + sx) * 4;
            uint8_t*       d = dst.data() + (static_cast<size_t>(dy) * dst_w + dx) * 4;
            d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = s[3];
        }
    }
    return dst;
}

// ---------------------------------------------------------------------------
// Convenience wrapper
// ---------------------------------------------------------------------------

std::string screenshot_to_png_base64(const uint8_t* bgra, int width, int height) {
    if (!bgra || width <= 0 || height <= 0) return {};

    if (width > LLM_MAX_WIDTH) {
        const int scaled_h = height * LLM_MAX_WIDTH / width;
        const std::vector<uint8_t> scaled =
            scale_nearest(bgra, width, height, LLM_MAX_WIDTH, scaled_h);
        return encode_png_base64(scaled.data(), LLM_MAX_WIDTH, scaled_h);
    }
    return encode_png_base64(bgra, width, height);
}

} // namespace mcp
} // namespace agentdesktop
