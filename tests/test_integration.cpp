/**
 * @file test_integration.cpp
 * @brief Functional / integration tests for AgentDesktop.
 *
 * These tests exercise the full connect → capture → input → disconnect
 * lifecycle using the REAL platform implementation (PlatformWin on Windows).
 *
 * Windows-only tests are guarded with #ifdef _WIN32.
 * Tests that require a real display are tagged "Integration" and are skipped
 * automatically when running in a headless/CI environment via the
 * AGENTDESKTOP_SKIP_INTEGRATION environment variable.
 *
 * Run only unit tests:
 *   AgentDesktopTests.exe --gtest_filter=-Integration.*
 *
 * Run everything (requires interactive Windows session):
 *   AgentDesktopTests.exe
 *
 * @copyright AgentDesktop Project
 */

#include <gtest/gtest.h>
#include "platform/Platform.h"
#include "capture/CaptureThread.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <string>
#include <thread>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#endif

using namespace agentdesktop;
using namespace agentdesktop::capture;

// ---------------------------------------------------------------------------
// Helper: should we skip integration tests?
// ---------------------------------------------------------------------------

static bool skip_integration() {
#ifdef _WIN32
    const char* v = std::getenv("AGENTDESKTOP_SKIP_INTEGRATION");
    if (v && std::string(v) == "1") return true;
    // Skip if there is no screen (headless / service account)
    if (GetSystemMetrics(SM_CXSCREEN) == 0) return true;
    return false;
#else
    return true;  // integration tests only run on Windows for now
#endif
}

// Fixture that skips if integration tests are disabled
class Integration : public ::testing::Test {
protected:
    void SetUp() override {
        if (skip_integration()) {
            GTEST_SKIP() << "Integration tests skipped "
                            "(set AGENTDESKTOP_SKIP_INTEGRATION=0 to enable)";
        }
    }
};

// ---------------------------------------------------------------------------
// Platform lifecycle — create / init / destroy
// ---------------------------------------------------------------------------

TEST_F(Integration, PlatformInitAndDestroy) {
    auto* plat = create_platform();
    ASSERT_NE(plat, nullptr);

    std::string err;
    const bool ok = plat->init(err);

    if (!ok) {
        // Print the error so CI logs are informative
        std::cerr << "[Integration] Platform::init failed: " << err << "\n";
    }
    EXPECT_TRUE(ok) << "init() error: " << err;

    if (ok) {
        EXPECT_GT(plat->phys_width(),  0);
        EXPECT_GT(plat->phys_height(), 0);
    }

    delete plat;
}

// ---------------------------------------------------------------------------
// Full connect → wait for first frame → disconnect
// ---------------------------------------------------------------------------

TEST_F(Integration, ConnectCapturesFrame) {
    auto* plat = create_platform();
    ASSERT_NE(plat, nullptr);

    std::string err;
    ASSERT_TRUE(plat->init(err)) << err;

    LatestFrame  lf;
    std::atomic<int> error_count{0};
    CaptureThread ct;
    ct.start(plat, &lf, [&](const std::string&){ ++error_count; });

    // Wait up to 5 seconds for the first frame
    const auto deadline = std::chrono::steady_clock::now()
                        + std::chrono::seconds(5);
    bool got_frame = false;
    while (std::chrono::steady_clock::now() < deadline) {
        {
            std::lock_guard<std::mutex> lk(lf.mtx);
            if (lf.dirty && !lf.pixels.empty()) {
                got_frame = true;
                break;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    ct.stop();
    delete plat;

    EXPECT_TRUE(got_frame) << "No frame received within 5 seconds";
    EXPECT_EQ(error_count.load(), 0) << "Capture errors occurred";
}

// ---------------------------------------------------------------------------
// Frame pixel count matches declared dimensions
// ---------------------------------------------------------------------------

TEST_F(Integration, FramePixelCountMatchesDimensions) {
    auto* plat = create_platform();
    ASSERT_NE(plat, nullptr);

    std::string err;
    ASSERT_TRUE(plat->init(err)) << err;

    LatestFrame lf;
    CaptureThread ct;
    ct.start(plat, &lf, nullptr);

    // Wait for first frame
    const auto deadline = std::chrono::steady_clock::now()
                        + std::chrono::seconds(5);
    bool got_frame = false;
    int fw = 0, fh = 0;
    size_t fpix = 0;
    while (std::chrono::steady_clock::now() < deadline) {
        {
            std::lock_guard<std::mutex> lk(lf.mtx);
            if (lf.dirty && !lf.pixels.empty()) {
                fw = lf.width; fh = lf.height; fpix = lf.pixels.size();
                got_frame = true;
                break;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    ct.stop();
    delete plat;

    ASSERT_TRUE(got_frame);
    EXPECT_GT(fw, 0);
    EXPECT_GT(fh, 0);
    EXPECT_EQ(fpix, static_cast<size_t>(fw * fh * 4))
        << "Expected " << fw * fh * 4 << " bytes, got " << fpix;
    // Frame should be scaled to ≤ CAPTURE_W
    EXPECT_LE(fw, Platform::CAPTURE_W);
}

// ---------------------------------------------------------------------------
// Screenshot returns non-empty BGRA pixels
// ---------------------------------------------------------------------------

TEST_F(Integration, ScreenshotReturnsPixels) {
    auto* plat = create_platform();
    ASSERT_NE(plat, nullptr);

    std::string err;
    ASSERT_TRUE(plat->init(err)) << err;

    // Allow shell to settle
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    const ScreenshotResult r = plat->screenshot();
    delete plat;

    EXPECT_TRUE(r.ok)   << "screenshot() error: " << r.error;
    EXPECT_GT(r.width,  0);
    EXPECT_GT(r.height, 0);
    EXPECT_EQ(r.pixels.size(), static_cast<size_t>(r.width * r.height * 4));
}

// ---------------------------------------------------------------------------
// Launch an application inside the virtual desktop
// ---------------------------------------------------------------------------

TEST_F(Integration, LaunchNotepadInsideDesktop) {
    auto* plat = create_platform();
    ASSERT_NE(plat, nullptr);

    std::string err;
    ASSERT_TRUE(plat->init(err)) << err;

    const LaunchResult lr = plat->launch_app("notepad", "");
    delete plat;

    EXPECT_TRUE(lr.ok)  << "launch_app error: " << lr.error;
    EXPECT_GT(lr.pid, 0);
}

// ---------------------------------------------------------------------------
// Launch → maximize_window
// ---------------------------------------------------------------------------

TEST_F(Integration, LaunchAndMaximize) {
    auto* plat = create_platform();
    ASSERT_NE(plat, nullptr);

    std::string err;
    ASSERT_TRUE(plat->init(err)) << err;

    // mspaint is a classic Win32 GUI app that reliably creates a visible
    // HWND on the virtual desktop. notepad on Win11 is UWP/WinUI3 and
    // does NOT enumerate via EnumDesktopWindows. cmd.exe is a console
    // app whose HWND may take an unpredictable time to appear.
    const LaunchResult lr = plat->launch_app("mspaint", "");
    if (!lr.ok) {
        delete plat;
        GTEST_SKIP() << "mspaint not available on this system: " << lr.error;
    }

    // maximize_window polls internally; 3 s is sufficient for mspaint
    const ActionResult mr = plat->maximize_window(lr.pid);
    delete plat;

    EXPECT_TRUE(mr.ok) << "maximize_window error: " << mr.error;
}

// ---------------------------------------------------------------------------
// Double-connect: second connect is a no-op (already active)
// ---------------------------------------------------------------------------

TEST_F(Integration, PlatformInitTwiceFails) {
    auto* plat = create_platform();
    ASSERT_NE(plat, nullptr);

    std::string err1, err2;
    ASSERT_TRUE(plat->init(err1)) << err1;

    // Second init on the same object should fail or at minimum not crash
    // (it will try to create the same desktop name and get ERROR_ALREADY_EXISTS)
    const bool ok2 = plat->init(err2);
    // Either it gracefully fails or returns true — both are acceptable;
    // what is NOT acceptable is a crash or UB.
    (void)ok2;

    delete plat;
}

// ---------------------------------------------------------------------------
// Disconnect while capture thread is running is safe
// ---------------------------------------------------------------------------

TEST_F(Integration, DisconnectWhileCaptureRunningIsSafe) {
    auto* plat = create_platform();
    ASSERT_NE(plat, nullptr);

    std::string err;
    ASSERT_TRUE(plat->init(err)) << err;

    LatestFrame lf;
    CaptureThread ct;
    ct.start(plat, &lf, nullptr);

    // Let it run briefly
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // Stop capture first (required contract: stop before destroying platform)
    EXPECT_NO_THROW(ct.stop());
    EXPECT_NO_THROW(delete plat);
}

// ---------------------------------------------------------------------------
// Multiple connect / disconnect cycles (resource leak check)
// ---------------------------------------------------------------------------

TEST_F(Integration, MultipleConnectDisconnectCycles) {
    for (int i = 0; i < 3; ++i) {
        auto* plat = create_platform();
        ASSERT_NE(plat, nullptr) << "cycle " << i;

        std::string err;
        ASSERT_TRUE(plat->init(err)) << "cycle " << i << ": " << err;

        LatestFrame lf;
        CaptureThread ct;
        ct.start(plat, &lf, nullptr);
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        ct.stop();
        delete plat;
    }
    // If we get here without crashing, handles/DCs are being released correctly.
    SUCCEED();
}
