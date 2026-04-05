/**
 * @file App.h
 * @brief Main application controller — owns the Platform, CaptureThread,
 *        OpenGL texture, and all ImGui UI state.
 *
 * AgentDesktopApp is instantiated once in main() after GLFW/OpenGL/ImGui
 * are initialised.  render() is called on every frame; GLFW input callbacks
 * forward events via on_key(), on_char(), and on_scroll().
 *
 * Threading model
 * ---------------
 *   Main thread   : render(), GL calls, Platform control operations (async-dispatched)
 *   Capture thread: CaptureThread::loop() — polls Platform::capture()
 *   Worker thread : one-shot async tasks (launch_app, maximize_window, key_press)
 *
 * The shared LatestFrame buffer is the only data crossing thread boundaries;
 * all other state is main-thread-only.
 *
 * @copyright AgentDesktop Project
 */

#pragma once

#include "LogEntry.h"
#include "../platform/Platform.h"
#include "../capture/CaptureThread.h"

#include <atomic>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct GLFWwindow; // forward declaration — avoids pulling in GLFW headers

namespace agentdesktop {
namespace app {

/**
 * @brief Top-level application class.
 *
 * Owns the Platform, CaptureThread, OpenGL texture, and all UI state.
 * One instance per process; not copyable or movable.
 */
class AgentDesktopApp {
public:
    /**
     * @brief Construct the application.
     * @param window  The GLFW window created by main(). Not owned; must
     *                remain valid for the lifetime of this object.
     */
    explicit AgentDesktopApp(GLFWwindow* window);

    /** @brief Destructor — disconnects the virtual desktop if connected. */
    ~AgentDesktopApp();

    AgentDesktopApp(const AgentDesktopApp&)            = delete;
    AgentDesktopApp& operator=(const AgentDesktopApp&) = delete;

    // -----------------------------------------------------------------------
    // Per-frame entry points (called from main render loop)
    // -----------------------------------------------------------------------

    /** @brief Render one frame of the entire UI. Call once per GLFW frame. */
    void render();

    // -----------------------------------------------------------------------
    // GLFW input callbacks (forwarded from glfwSetKeyCallback etc.)
    // -----------------------------------------------------------------------

    /** @brief Handle a GLFW key event. Forwards key presses to the virtual desktop. */
    void on_key(int glfw_key, int action, int mods);

    /** @brief Handle a Unicode character input event. */
    void on_char(unsigned int codepoint);

    /** @brief Handle a scroll wheel event. */
    void on_scroll(double xoff, double yoff);

private:
    // -----------------------------------------------------------------------
    // GLFW window (non-owning)
    // -----------------------------------------------------------------------
    GLFWwindow* m_win = nullptr;

    // -----------------------------------------------------------------------
    // Platform + capture pipeline
    // -----------------------------------------------------------------------
    std::unique_ptr<agentdesktop::Platform>   m_platform;
    capture::LatestFrame                      m_latest_frame;
    capture::CaptureThread                    m_capture;
    bool                                      m_active     = false;
    std::string                               m_init_error;

    // -----------------------------------------------------------------------
    // OpenGL texture for the desktop preview
    // -----------------------------------------------------------------------
    unsigned int m_tex   = 0;   ///< GL texture name (0 = not allocated)
    int          m_tex_w = 0;   ///< Current texture width
    int          m_tex_h = 0;   ///< Current texture height

    // -----------------------------------------------------------------------
    // Frame-rate tracking
    // -----------------------------------------------------------------------
    float  m_fps        = 0.f;
    int    m_fps_frames = 0;
    double m_fps_t0     = 0.0;

    // -----------------------------------------------------------------------
    // Async worker thread (one-shot, non-blocking UI operations)
    // -----------------------------------------------------------------------
    std::thread       m_work_thread;
    std::atomic<bool> m_busy{false};

    // -----------------------------------------------------------------------
    // Preview panel mouse state
    // -----------------------------------------------------------------------
    bool  m_preview_hover = false;
    float m_hover_rx = 0.f;  ///< Relative X in [0,1] within the preview image
    float m_hover_ry = 0.f;  ///< Relative Y in [0,1] within the preview image

    // -----------------------------------------------------------------------
    // UI input buffers
    // -----------------------------------------------------------------------
    char m_app_path[512] = "";
    char m_app_args[256] = "";
    char m_type_buf[512] = "";
    int  m_pid_input     = 0;
    bool m_show_about    = false;
    bool m_show_help     = false;

    // -----------------------------------------------------------------------
    // In-app log
    // -----------------------------------------------------------------------
    std::deque<LogEntry> m_log;
    std::mutex           m_log_mtx;
    bool                 m_log_scroll = true;
    static constexpr int LOG_MAX      = 500; ///< Maximum retained log entries

    // -----------------------------------------------------------------------
    // Private methods — connection lifecycle
    // -----------------------------------------------------------------------

    /** @brief Create the platform and start the capture thread. */
    void do_connect();

    /** @brief Stop capture, destroy the platform. */
    void do_disconnect();

    // -----------------------------------------------------------------------
    // Private methods — rendering
    // -----------------------------------------------------------------------

    /** @brief Upload a dirty LatestFrame to the OpenGL texture (main thread only). */
    void upload_frame();

    /**
     * @brief Dispatch a lambda to the worker thread.
     *
     * Waits for any previous work to finish before starting the new task
     * so that at most one worker is running at a time.
     */
    void async(std::function<void()> fn);

    /**
     * @brief Translate a normalised preview coordinate to a virtual-desktop
     *        coordinate and send a click/double-click/right-click.
     * @param rx   Normalised X in [0, 1].
     * @param ry   Normalised Y in [0, 1].
     * @param type 0=single left, 1=double left, 2=right.
     */
    void send_click(float rx, float ry, int type);

    /** @brief Forward a scroll event from the preview panel. */
    void send_scroll(float rx, float ry, int delta);

    // -----------------------------------------------------------------------
    // Private methods — UI sections
    // -----------------------------------------------------------------------
    void draw_titlebar();
    void draw_preview(float w, float h);
    void draw_sidebar(float w, float h);
    void draw_log(float w, float h);
    void draw_statusbar(float w, float h);
    void draw_about();
    void draw_help();

    // -----------------------------------------------------------------------
    // Private utilities
    // -----------------------------------------------------------------------

    /** @brief Append a message to the in-app log (thread-safe). */
    void log(const std::string& msg,
             LogEntry::Level lv = LogEntry::Level::Info);

    /** @brief Return current wall-clock time in seconds (steady_clock). */
    double now() const;
};

} // namespace app
} // namespace agentdesktop
