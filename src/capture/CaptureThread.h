/**
 * @file CaptureThread.h
 * @brief Background capture pipeline — polls the virtual desktop and
 *        delivers frames to the rendering thread via a lock-protected buffer.
 *
 * Architecture
 * ============
 * The capture pipeline runs on a dedicated std::thread separate from the
 * main (render) thread to avoid blocking the UI at 60 Hz when capture
 * takes variable time (GDI PrintWindow can stall 5–50 ms per frame).
 *
 *   CaptureThread
 *     └─ calls Platform::capture() in a tight loop
 *          └─ computes FNV-1a hash of the returned frame
 *               └─ if hash changed → stores into LatestFrame (mutex-protected)
 *
 *   Render thread
 *     └─ checks LatestFrame::dirty each frame
 *          └─ if dirty → copies pixels under mutex → glTexSubImage2D → clears dirty
 *
 * The mutex is held only during the brief copy, not during the slow capture.
 *
 * @copyright AgentDesktop Project
 */

#pragma once

#include "FrameHash.h"
#include "../platform/Platform.h"

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace agentdesktop {
namespace capture {

// ---------------------------------------------------------------------------
// LatestFrame
// ---------------------------------------------------------------------------

/**
 * @brief Thread-safe single-slot frame buffer shared between the capture
 *        thread (writer) and the render thread (reader).
 *
 * The mutex is held only during the copy; capture and upload happen without
 * holding it.
 */
struct LatestFrame {
    std::vector<uint8_t> pixels; ///< BGRA pixel data (most recent frame)
    int  width  = 0;
    int  height = 0;
    bool dirty  = false;         ///< Set by capture thread; cleared by render thread after GL upload

    mutable std::mutex mtx;

    /**
     * @brief Store a newly captured frame. Acquires the mutex briefly.
     * @param f  Frame to store (moved into the buffer).
     */
    void store(agentdesktop::Frame&& f) {
        std::lock_guard<std::mutex> lk(mtx);
        pixels = std::move(f.pixels);
        width  = f.width;
        height = f.height;
        dirty  = true;
    }
};

// ---------------------------------------------------------------------------
// CaptureThread
// ---------------------------------------------------------------------------

/**
 * @brief Manages a dedicated thread that continuously captures frames from
 *        a Platform instance and stores changed frames into a LatestFrame.
 *
 * Usage
 * -----
 * @code
 *   LatestFrame shared_frame;
 *   CaptureThread ct;
 *   ct.start(platform.get(), &shared_frame,
 *       [](const std::string& e){ log_error(e); });
 *   // ... render loop ...
 *   ct.stop();
 * @endcode
 */
class CaptureThread {
public:
    CaptureThread()  = default;
    ~CaptureThread() { stop(); }

    CaptureThread(const CaptureThread&)            = delete;
    CaptureThread& operator=(const CaptureThread&) = delete;

    /**
     * @brief Start the capture thread.
     *
     * @param platform  Non-owning pointer to an initialised Platform.
     *                  Must remain valid until stop() returns.
     * @param out       Shared LatestFrame written to by the capture thread.
     *                  Must remain valid until stop() returns.
     * @param on_error  Callback invoked (from the capture thread) when
     *                  capture() returns empty frames persistently. Must
     *                  be thread-safe.
     */
    void start(agentdesktop::Platform*                     platform,
               LatestFrame*                                out,
               std::function<void(const std::string&)>    on_error);

    /**
     * @brief Signal the thread to stop and block until it exits.
     *
     * Safe to call even if start() was never called, or if already stopped.
     */
    void stop();

    /** @return true if the capture thread is currently running. */
    [[nodiscard]] bool running() const noexcept { return m_running.load(); }

private:
    std::thread       m_thread;
    std::atomic<bool> m_running{false};

    void loop(agentdesktop::Platform*                  platform,
              LatestFrame*                             out,
              std::function<void(const std::string&)> on_error);
};

} // namespace capture
} // namespace agentdesktop
