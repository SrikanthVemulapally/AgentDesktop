/**
 * @file test_frame_hash.cpp
 * @brief Unit tests for the FNV-1a frame-deduplication hash.
 *
 * Verifies correctness, stability, and collision-resistance properties of
 * agentdesktop::capture::frame_hash().
 *
 * @copyright AgentDesktop Project
 */

#include <gtest/gtest.h>
#include "capture/FrameHash.h"

#include <cstring>
#include <vector>

using agentdesktop::capture::frame_hash;

// ---------------------------------------------------------------------------
// Determinism
// ---------------------------------------------------------------------------

TEST(FrameHash, SameInputSameHash) {
    const std::vector<uint8_t> pixels(1280 * 720 * 4, 0xAB);
    const uint32_t h1 = frame_hash(pixels.data(), 1280, 720);
    const uint32_t h2 = frame_hash(pixels.data(), 1280, 720);
    EXPECT_EQ(h1, h2);
}

// ---------------------------------------------------------------------------
// Empty frame returns FNV offset basis
// ---------------------------------------------------------------------------

TEST(FrameHash, EmptyFrameReturnsBasis) {
    // width == 0 → n == 0 → loop never runs → returns OFFSET_BASIS
    constexpr uint32_t FNV_OFFSET_BASIS = 2166136261u;
    const uint8_t dummy = 0;
    EXPECT_EQ(frame_hash(&dummy, 0, 0), FNV_OFFSET_BASIS);
    EXPECT_EQ(frame_hash(&dummy, 0, 100), FNV_OFFSET_BASIS);
    EXPECT_EQ(frame_hash(&dummy, 100, 0), FNV_OFFSET_BASIS);
}

// ---------------------------------------------------------------------------
// Single-pixel change changes hash
// ---------------------------------------------------------------------------

TEST(FrameHash, SingleBitChangeAltersHash) {
    std::vector<uint8_t> a(4 * 4 * 4, 0x00); // 4x4 black image
    std::vector<uint8_t> b = a;
    b[0] = 0x01; // flip one byte

    EXPECT_NE(frame_hash(a.data(), 4, 4), frame_hash(b.data(), 4, 4));
}

// ---------------------------------------------------------------------------
// Different sizes produce different hashes even for same pixel value
// ---------------------------------------------------------------------------

TEST(FrameHash, DifferentSizesProduceDifferentHashes) {
    const std::vector<uint8_t> a(10 * 10 * 4, 0x42);
    const std::vector<uint8_t> b(20 * 10 * 4, 0x42);
    // Different total byte counts → different hash (different n in the loop)
    EXPECT_NE(frame_hash(a.data(), 10, 10), frame_hash(b.data(), 20, 10));
}

// ---------------------------------------------------------------------------
// Known value — regression test (FNV-1a over 4 bytes = {0x01,0x02,0x03,0x04})
// ---------------------------------------------------------------------------

TEST(FrameHash, KnownValue_1x1Frame) {
    // 1×1 BGRA pixel: B=1 G=2 R=3 A=4
    const uint8_t px[4] = {0x01, 0x02, 0x03, 0x04};
    // Manually compute: h = 2166136261
    // i=0: h ^= 0x01 → h *= 16777619
    // i=1: h ^= 0x02 → h *= 16777619
    // i=2: h ^= 0x03 → h *= 16777619
    // i=3: h ^= 0x04 → h *= 16777619
    uint32_t expected = 2166136261u;
    for (int i = 0; i < 4; ++i) { expected ^= px[i]; expected *= 16777619u; }
    EXPECT_EQ(frame_hash(px, 1, 1), expected);
}

// ---------------------------------------------------------------------------
// All-zero vs all-one frame
// ---------------------------------------------------------------------------

TEST(FrameHash, AllZeroVsAllOne) {
    const std::vector<uint8_t> zeros(640 * 480 * 4, 0x00);
    const std::vector<uint8_t> ones( 640 * 480 * 4, 0xFF);
    EXPECT_NE(frame_hash(zeros.data(), 640, 480),
              frame_hash(ones.data(),  640, 480));
}
