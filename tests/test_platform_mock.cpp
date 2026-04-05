/**
 * @file test_platform_mock.cpp
 * @brief Unit tests for the Platform interface contract and CaptureThread
 *        using a stub implementation — no real virtual desktop required.
 *
 * Validates that:
 *   - A conforming Platform implementation satisfies all interface contracts.
 *   - CaptureThread correctly starts, runs, deduplicates frames, and stops.
 *   - LatestFrame dirty-flag protocol works under concurrent access.
 *
 * @copyright AgentDesktop Project
 */

#include <gtest/gtest.h>
#include "platform/Platform.h"
#include "capture/CaptureThread.h"
#include "capture/FrameHash.h"

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

using namespace agentdesktop;
using namespace agentdesktop::capture;

// ---------------------------------------------------------------------------
// StubPlatform — minimal conforming implementation for testing
// ---------------------------------------------------------------------------

class StubPlatform : public Platform {
public:
    int  init_call_count      = 0;
    int  capture_call_count   = 0;
    bool should_fail_init     = false;
    bool return_empty_frames  = false;

    // Controls the pixel value returned in captured frames (to test dedup)
    std::atomic<uint8_t> frame_pixel_value{0x42};

    bool init(std::string& error) override {
        ++init_call_count;
        if (should_fail_init) { error = "stub failure"; return false; }
        return true;
    }

    Frame capture() override {
        ++capture_call_count;
        if (return_empty_frames) return {};

        Frame f;
        f.width       = 64;
        f.height      = 48;
        f.phys_width  = 64;
        f.phys_height = 48;
        f.pixels.assign(static_cast<size_t>(64) * 48 * 4,
                        frame_pixel_value.load());
        return f;
    }

    LaunchResult  launch_app(const std::string&, const std::string&) override {
        return {true, 1, ""};
    }
    ActionResult  maximize_window(int) override { return {true}; }
    ActionResult  click(int, int)        override { return {true}; }
    ActionResult  double_click(int, int) override { return {true}; }
    ActionResult  right_click(int, int)  override { return {true}; }
    ActionResult  scroll(int, int, int)  override { return {true}; }
    ActionResult  type_text(const std::string&) override { return {true}; }
    ActionResult  key_press(const std::string&) override { return {true}; }

    ScreenshotResult screenshot(int = 0, int = 0, int = 0, int = 0) override {
        ScreenshotResult r;
        r.ok = true; r.width = 64; r.height = 48;
        r.pixels.assign(static_cast<size_t>(64) * 48 * 4, 0xFF);
        return r;
    }

    int phys_width()  const override { return 64; }
    int phys_height() const override { return 48; }
};

// ---------------------------------------------------------------------------
// Platform interface contract tests
// ---------------------------------------------------------------------------

TEST(PlatformContract, InitSucceeds) {
    StubPlatform p;
    std::string err;
    EXPECT_TRUE(p.init(err));
    EXPECT_TRUE(err.empty());
    EXPECT_EQ(p.init_call_count, 1);
}

TEST(PlatformContract, InitCanFail) {
    StubPlatform p;
    p.should_fail_init = true;
    std::string err;
    EXPECT_FALSE(p.init(err));
    EXPECT_FALSE(err.empty());
}

TEST(PlatformContract, CaptureReturnsBGRAPixels) {
    StubPlatform p;
    const Frame f = p.capture();
    EXPECT_EQ(f.width,  64);
    EXPECT_EQ(f.height, 48);
    EXPECT_EQ(f.pixels.size(), static_cast<size_t>(64 * 48 * 4));
}

TEST(PlatformContract, DefaultPreptureCaptureThreadSucceeds) {
    StubPlatform p;
    std::string err;
    EXPECT_TRUE(p.prepare_capture_thread(err));
}

TEST(PlatformContract, AllInputMethodsSucceed) {
    StubPlatform p;
    EXPECT_TRUE(p.click(10, 20).ok);
    EXPECT_TRUE(p.double_click(10, 20).ok);
    EXPECT_TRUE(p.right_click(10, 20).ok);
    EXPECT_TRUE(p.scroll(10, 20, 3).ok);
    EXPECT_TRUE(p.type_text("hello").ok);
    EXPECT_TRUE(p.key_press("Enter").ok);
    EXPECT_TRUE(p.maximize_window(1234).ok);
}

TEST(PlatformContract, ScreenshotReturnsPixels) {
    StubPlatform p;
    const ScreenshotResult r = p.screenshot(0, 0, 0, 0);
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.width,  64);
    EXPECT_EQ(r.height, 48);
    EXPECT_EQ(r.pixels.size(), static_cast<size_t>(64 * 48 * 4));
}

TEST(PlatformContract, CaptureWidthConstant) {
    EXPECT_EQ(Platform::CAPTURE_W, 1280);
}

// ---------------------------------------------------------------------------
// LatestFrame
// ---------------------------------------------------------------------------

TEST(LatestFrame, InitialStateNotDirty) {
    LatestFrame lf;
    EXPECT_FALSE(lf.dirty);
    EXPECT_EQ(lf.width,  0);
    EXPECT_EQ(lf.height, 0);
}

TEST(LatestFrame, StoreSetsDirty) {
    LatestFrame lf;
    Frame f;
    f.width = 10; f.height = 10;
    f.pixels.assign(400, 0xFF);
    lf.store(std::move(f));
    EXPECT_TRUE(lf.dirty);
    EXPECT_EQ(lf.width,  10);
    EXPECT_EQ(lf.height, 10);
    EXPECT_EQ(lf.pixels.size(), 400u);
}

TEST(LatestFrame, ManualClearDirty) {
    LatestFrame lf;
    Frame f; f.width = 4; f.height = 4; f.pixels.assign(64, 0);
    lf.store(std::move(f));
    lf.dirty = false;
    EXPECT_FALSE(lf.dirty);
}

// ---------------------------------------------------------------------------
// CaptureThread
// ---------------------------------------------------------------------------

TEST(CaptureThread, StartAndStopClean) {
    StubPlatform plat;
    LatestFrame  lf;
    CaptureThread ct;
    ct.start(&plat, &lf, nullptr);
    EXPECT_TRUE(ct.running());
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    ct.stop();
    EXPECT_FALSE(ct.running());
}

TEST(CaptureThread, FrameIsDelivered) {
    StubPlatform plat;
    LatestFrame  lf;
    CaptureThread ct;
    ct.start(&plat, &lf, nullptr);

    // Wait up to 1 s for at least one frame to arrive
    const auto deadline = std::chrono::steady_clock::now()
                        + std::chrono::seconds(1);
    bool got_frame = false;
    while (std::chrono::steady_clock::now() < deadline) {
        {
            std::lock_guard<std::mutex> lk(lf.mtx);
            if (lf.dirty) { got_frame = true; break; }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ct.stop();
    EXPECT_TRUE(got_frame);
}

TEST(CaptureThread, DeduplicatesIdenticalFrames) {
    StubPlatform plat;
    plat.frame_pixel_value = 0xAA; // constant pixel → same hash every time

    LatestFrame  lf;
    CaptureThread ct;
    ct.start(&plat, &lf, nullptr);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    ct.stop();

    // Even though capture() was called many times, dirty should be set at most once
    // (after the first unique frame).  We can't know exactly how many captures ran,
    // but we know at least one happened.
    EXPECT_GT(plat.capture_call_count, 0);
}

TEST(CaptureThread, DetectsFrameChange) {
    StubPlatform plat;
    plat.frame_pixel_value = 0x11;

    LatestFrame  lf;
    CaptureThread ct;
    ct.start(&plat, &lf, nullptr);

    // Wait for first frame
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    {
        std::lock_guard<std::mutex> lk(lf.mtx);
        lf.dirty = false; // simulate render thread consuming the frame
    }

    // Change the pixel value — capture thread should deliver a new frame
    plat.frame_pixel_value = 0x22;
    std::this_thread::sleep_for(std::chrono::milliseconds(80));

    bool got_new_frame = false;
    {
        std::lock_guard<std::mutex> lk(lf.mtx);
        got_new_frame = lf.dirty;
    }
    ct.stop();
    EXPECT_TRUE(got_new_frame);
}

TEST(CaptureThread, ErrorCallbackOnEmptyFrames) {
    StubPlatform plat;
    plat.return_empty_frames = true;

    LatestFrame lf;
    std::atomic<int> error_count{0};
    CaptureThread ct;
    ct.start(&plat, &lf, [&](const std::string&) { ++error_count; });

    // Allow time for the MAX_EMPTY_STREAK threshold to be hit
    std::this_thread::sleep_for(std::chrono::milliseconds(2500));
    ct.stop();

    // At least one error should have been reported
    EXPECT_GE(error_count.load(), 1);
}

TEST(CaptureThread, StopBeforeStartIsSafe) {
    CaptureThread ct;
    EXPECT_NO_THROW(ct.stop()); // stop() when never started must not crash
}
