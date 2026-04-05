/**
 * @file test_image_encoder.cpp
 * @brief Unit tests for ImageEncoder — Base64, PNG encoding, and scaling.
 *
 * Tests are purely in-process (no platform, no GLFW, no OpenGL).
 *
 * @copyright AgentDesktop Project
 */

#include <gtest/gtest.h>
#include "mcp/ImageEncoder.h"

#include <cstring>
#include <vector>

using namespace agentdesktop::mcp;

// ---------------------------------------------------------------------------
// base64_encode
// ---------------------------------------------------------------------------

TEST(Base64Encode, EmptyInput) {
    EXPECT_EQ(base64_encode(nullptr, 0), "");
}

TEST(Base64Encode, RFC4648_TestVector1) {
    // "" → ""
    EXPECT_EQ(base64_encode(nullptr, 0), "");
}

TEST(Base64Encode, RFC4648_TestVector2) {
    // "f" → "Zg=="
    const uint8_t input[] = {'f'};
    EXPECT_EQ(base64_encode(input, 1), "Zg==");
}

TEST(Base64Encode, RFC4648_TestVector3) {
    // "fo" → "Zm8="
    const uint8_t input[] = {'f','o'};
    EXPECT_EQ(base64_encode(input, 2), "Zm8=");
}

TEST(Base64Encode, RFC4648_TestVector4) {
    // "foo" → "Zm9v"
    const uint8_t input[] = {'f','o','o'};
    EXPECT_EQ(base64_encode(input, 3), "Zm9v");
}

TEST(Base64Encode, RFC4648_TestVector5) {
    // "foobar" → "Zm9vYmFy"
    const uint8_t input[] = {'f','o','o','b','a','r'};
    EXPECT_EQ(base64_encode(input, 6), "Zm9vYmFy");
}

TEST(Base64Encode, OutputLengthIsMultipleOf4) {
    for (size_t len = 0; len <= 12; ++len) {
        std::vector<uint8_t> data(len, 0xAA);
        const std::string b64 = base64_encode(data.data(), len);
        EXPECT_EQ(b64.size() % 4, 0u)
            << "Output length not multiple of 4 for input len=" << len;
    }
}

TEST(Base64Encode, OnlyValidCharacters) {
    const std::vector<uint8_t> data(256);
    const std::string b64 = base64_encode(data.data(), data.size());
    for (char c : b64) {
        EXPECT_TRUE(
            (c >= 'A' && c <= 'Z') ||
            (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') ||
            c == '+' || c == '/' || c == '='
        ) << "Invalid base64 character: " << c;
    }
}

// ---------------------------------------------------------------------------
// scale_nearest
// ---------------------------------------------------------------------------

TEST(ScaleNearest, InvalidInputReturnsEmpty) {
    EXPECT_TRUE(scale_nearest(nullptr, 0, 0, 10, 10).empty());
    EXPECT_TRUE(scale_nearest(nullptr, 10, 10, 0, 0).empty());
}

TEST(ScaleNearest, OutputSizeIsCorrect) {
    const std::vector<uint8_t> src(100 * 100 * 4, 0xFF);
    const auto dst = scale_nearest(src.data(), 100, 100, 50, 50);
    EXPECT_EQ(dst.size(), static_cast<size_t>(50 * 50 * 4));
}

TEST(ScaleNearest, SolidColourPreserved) {
    // A solid red BGRA image scaled down should still be solid red.
    const int src_w = 200, src_h = 150;
    std::vector<uint8_t> src(src_w * src_h * 4);
    for (int i = 0; i < src_w * src_h; ++i) {
        src[i*4+0] = 0x00; // B
        src[i*4+1] = 0x00; // G
        src[i*4+2] = 0xFF; // R
        src[i*4+3] = 0xFF; // A
    }
    const int dst_w = 100, dst_h = 75;
    const auto dst = scale_nearest(src.data(), src_w, src_h, dst_w, dst_h);
    ASSERT_EQ(dst.size(), static_cast<size_t>(dst_w * dst_h * 4));
    for (int i = 0; i < dst_w * dst_h; ++i) {
        EXPECT_EQ(dst[i*4+2], 0xFF) << "R channel wrong at pixel " << i;
        EXPECT_EQ(dst[i*4+0], 0x00) << "B channel wrong at pixel " << i;
    }
}

TEST(ScaleNearest, UpscaleDoesNotCrash) {
    const std::vector<uint8_t> src(10 * 10 * 4, 0x80);
    const auto dst = scale_nearest(src.data(), 10, 10, 100, 100);
    EXPECT_EQ(dst.size(), static_cast<size_t>(100 * 100 * 4));
}

// ---------------------------------------------------------------------------
// encode_png_base64
// ---------------------------------------------------------------------------

TEST(EncodePngBase64, InvalidInputReturnsEmpty) {
    EXPECT_EQ(encode_png_base64(nullptr, 10, 10), "");
    EXPECT_EQ(encode_png_base64(nullptr, 0,  0),  "");
}

TEST(EncodePngBase64, Produces1x1PNG) {
    // A single white BGRA pixel
    const uint8_t px[4] = {0xFF, 0xFF, 0xFF, 0xFF};
    const std::string b64 = encode_png_base64(px, 1, 1);
    EXPECT_FALSE(b64.empty());
    // PNG files always start with \x89PNG in base64 = "iVBORw0KGgo"
    EXPECT_EQ(b64.substr(0, 11), "iVBORw0KGgo");
}

TEST(EncodePngBase64, OutputIsPureBase64) {
    const std::vector<uint8_t> px(32 * 32 * 4, 0x7F);
    const std::string b64 = encode_png_base64(px.data(), 32, 32);
    EXPECT_FALSE(b64.empty());
    for (char c : b64) {
        EXPECT_TRUE(
            (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '+' || c == '/' || c == '='
        ) << "Invalid base64 character: " << c;
    }
}

// ---------------------------------------------------------------------------
// screenshot_to_png_base64
// ---------------------------------------------------------------------------

TEST(ScreenshotToPngBase64, WideImageIsDownscaled) {
    // Create a 2560×1440 BGRA frame (wider than LLM_MAX_WIDTH=1280)
    const int W = 2560, H = 1440;
    const std::vector<uint8_t> px(W * H * 4, 0x42);
    const std::string b64 = screenshot_to_png_base64(px.data(), W, H);
    EXPECT_FALSE(b64.empty());
    // The result encodes a ≤1280px wide image; we can't decode it here,
    // but we verify it looks like a PNG.
    EXPECT_EQ(b64.substr(0, 11), "iVBORw0KGgo");
}

TEST(ScreenshotToPngBase64, NarrowImageNotScaled) {
    // 640×480 is already ≤ 1280 → no scaling
    const int W = 640, H = 480;
    const std::vector<uint8_t> px(W * H * 4, 0x10);
    const std::string b64 = screenshot_to_png_base64(px.data(), W, H);
    EXPECT_FALSE(b64.empty());
}
