/**
 * @file CaptureThread.cpp
 * @brief Implementation of the background capture pipeline.
 *
 * The capture loop:
 *   1. Calls Platform::prepare_capture_thread() once (Windows: SetThreadDesktop).
 *   2. Calls Platform::capture() in a tight loop.
 *   3. Hashes each frame with FNV-1a; skips identical frames.
 *   4. Stores changed frames into the LatestFrame shared buffer.
 *   5. On persistent empty-frame failures, invokes the on_error callback.
 *
 * @copyright AgentDesktop Project
 */

#include "CaptureThread.h"

#include <chrono>
#include <cstdio>
#include <thread>

namespace agentdesktop {
namespace capture {

// ---------------------------------------------------------------------------
// CaptureThread public API
// ---------------------------------------------------------------------------

void CaptureThread::start(agentdesktop::Platform*                  platform,
                           LatestFrame*                             out,
                           std::function<void(const std::string&)> on_error) {
    m_running = true;
    m_thread  = std::thread(&CaptureThread::loop, this, platform, out,
                             std::move(on_error));
}

void CaptureThread::stop() {
    m_running = false;
    if (m_thread.joinable()) {
        m_thread.join();
    }
}

// ---------------------------------------------------------------------------
// Capture loop (runs on the capture thread)
// ---------------------------------------------------------------------------

void CaptureThread::loop(agentdesktop::Platform*                  platform,
                          LatestFrame*                             out,
                          std::function<void(const std::string&)> on_error) {
    // --- One-time per-thread setup (Windows: bind to virtual desktop) --------
    {
        std::string err;
        if (!platform->prepare_capture_thread(err)) {
            if (on_error) on_error("prepare_capture_thread failed: " + err);
            m_running = false;
            return;
        }
    }

    uint32_t last_hash   = 0;
    int      empty_count = 0;
    constexpr int MAX_EMPTY_STREAK = 10; // report error after this many empty frames

    while (m_running) {
        agentdesktop::Frame f = platform->capture();

        // Empty frame — platform is temporarily unavailable; back off and retry.
        if (f.pixels.empty()) {
            ++empty_count;
            if (empty_count >= MAX_EMPTY_STREAK && on_error) {
                on_error("Capture returned " + std::to_string(MAX_EMPTY_STREAK)
                         + " empty frames in a row. Platform may be unavailable.");
                // Reset counter so we don't spam the log on every iteration.
                empty_count = 0;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            continue;
        }
        empty_count = 0;

        // Deduplicate: only upload when the frame content changes.
        const uint32_t h = frame_hash(f.pixels.data(), f.width, f.height);
        if (h == last_hash) {
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
            continue;
        }
        last_hash = h;

        out->store(std::move(f));
    }
}

} // namespace capture
} // namespace agentdesktop
