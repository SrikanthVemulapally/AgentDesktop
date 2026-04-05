/**
 * @file Platform.h
 * @brief Abstract platform interface for AgentDesktop's virtual desktop layer.
 *
 * This header defines the pure-virtual Platform interface that every
 * platform-specific implementation must satisfy. The three concrete
 * implementations are:
 *
 *   - PlatformWin.cpp   — Windows: CreateDesktopW / GDI PrintWindow / PostMessage
 *   - PlatformMac.mm    — macOS:   CGVirtualDisplay / ScreenCaptureKit / CGEvent
 *   - PlatformLinux.cpp — Linux:   Xvfb / XGetImage / XTest
 *
 * The factory function create_platform() is defined in each platform file and
 * returns the appropriate concrete instance. The caller owns the returned
 * pointer and is responsible for deletion (use std::unique_ptr<Platform>).
 *
 * Thread-safety contract:
 *   - init()                   called once on the main thread before any other method.
 *   - prepare_capture_thread() called once on the capture thread before the first capture().
 *   - capture()                called repeatedly only from the capture thread.
 *   - All other methods        may be called from any thread after init() returns true.
 *
 * @copyright AgentDesktop Project
 */

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace agentdesktop {

// ---------------------------------------------------------------------------
// Data types
// ---------------------------------------------------------------------------

/**
 * @brief A single captured frame from the virtual desktop.
 *
 * Pixels are stored as packed BGRA (4 bytes per pixel), top-down, with
 * stride = width * 4. The platform scales the raw capture down to
 * Platform::CAPTURE_W pixels wide so that GPU uploads are inexpensive.
 *
 * phys_width / phys_height report the native virtual-desktop resolution
 * before any scaling, which is needed when converting preview click
 * coordinates back to virtual-desktop coordinates.
 */
struct Frame {
    std::vector<uint8_t> pixels;     ///< BGRA pixel data, top-down
    int width       = 0;             ///< Scaled pixel width  (≤ CAPTURE_W)
    int height      = 0;             ///< Scaled pixel height
    int phys_width  = 0;             ///< Native virtual-desktop width
    int phys_height = 0;             ///< Native virtual-desktop height
};

/**
 * @brief Result of a Platform::screenshot() call.
 *
 * Pixels are BGRA, top-down, at native (unscaled) resolution unless the
 * caller explicitly requests a region smaller than the full desktop.
 */
struct ScreenshotResult {
    std::vector<uint8_t> pixels; ///< BGRA pixel data
    int         width  = 0;
    int         height = 0;
    bool        ok     = false;
    std::string error;
};

/**
 * @brief Result of a Platform::launch_app() call.
 */
struct LaunchResult {
    bool        ok    = false;
    int         pid   = 0;      ///< OS process ID, valid only when ok == true
    std::string error;
};

/**
 * @brief Generic result for input / window-management operations.
 */
struct ActionResult {
    bool        ok = false;
    std::string error;
};

// ---------------------------------------------------------------------------
// Platform interface
// ---------------------------------------------------------------------------

/**
 * @brief Abstract interface for all platform-specific virtual-desktop operations.
 *
 * Concrete implementations live in:
 *   src/platform/PlatformWin.cpp
 *   src/platform/PlatformMac.mm
 *   src/platform/PlatformLinux.cpp
 */
class Platform {
public:
    virtual ~Platform() = default;

    // Non-copyable, non-movable — platform resources are exclusive.
    Platform(const Platform&)            = delete;
    Platform& operator=(const Platform&) = delete;
    Platform(Platform&&)                 = delete;
    Platform& operator=(Platform&&)      = delete;

    // -----------------------------------------------------------------------
    // Lifecycle
    // -----------------------------------------------------------------------

    /**
     * @brief Create and initialise the virtual desktop.
     *
     * Must be called exactly once on the main thread before any other method.
     * On success returns true. On failure returns false and writes a
     * human-readable description into @p error.
     */
    virtual bool init(std::string& error) = 0;

    /**
     * @brief Prepare the calling thread for capture.
     *
     * Called once on the capture thread immediately before the first
     * capture() invocation. The Windows implementation uses this to bind
     * the thread to the virtual desktop (SetThreadDesktop). Other
     * platforms return true immediately.
     *
     * @param error  Receives an error message when returning false.
     * @return true  on success.
     */
    virtual bool prepare_capture_thread(std::string& /*error*/) { return true; }

    // -----------------------------------------------------------------------
    // Capture
    // -----------------------------------------------------------------------

    /**
     * @brief Capture one frame from the virtual desktop.
     *
     * Called repeatedly from the dedicated capture thread. Returns a Frame
     * with pixels scaled to CAPTURE_W. Returns an empty Frame (pixels.empty())
     * on transient failures; the capture loop will retry.
     */
    virtual Frame capture() = 0;

    // -----------------------------------------------------------------------
    // Application management
    // -----------------------------------------------------------------------

    /**
     * @brief Launch an application inside the virtual desktop.
     *
     * @p path may be a short name ("notepad", "chrome") or a full absolute
     * path. @p args is an optional space-separated argument string.
     */
    virtual LaunchResult launch_app(const std::string& path,
                                    const std::string& args) = 0;

    /**
     * @brief Maximise the primary window of the given process.
     *
     * Waits up to ~2 s for the window to appear before giving up.
     * @p pid is the value returned by launch_app().pid.
     */
    virtual ActionResult maximize_window(int pid) = 0;

    // -----------------------------------------------------------------------
    // Input — all coordinates in physical virtual-desktop pixels
    // -----------------------------------------------------------------------

    /** @brief Send a single left-click at (@p x, @p y). */
    virtual ActionResult click(int x, int y) = 0;

    /** @brief Send a double left-click at (@p x, @p y). */
    virtual ActionResult double_click(int x, int y) = 0;

    /** @brief Send a right-click (context-menu) at (@p x, @p y). */
    virtual ActionResult right_click(int x, int y) = 0;

    /**
     * @brief Scroll at (@p x, @p y).
     * @p delta positive = scroll up, negative = scroll down. Magnitude is
     * in units of "lines" (each platform maps this to its native unit).
     */
    virtual ActionResult scroll(int x, int y, int delta) = 0;

    /** @brief Type a UTF-8 string into the currently focused window. */
    virtual ActionResult type_text(const std::string& text) = 0;

    /**
     * @brief Send a named key press.
     *
     * Supported key names: Enter, Return, Escape, Tab, Backspace, Delete,
     * Space, Insert, ArrowLeft, ArrowRight, ArrowUp, ArrowDown,
     * Home, End, PageUp, PageDown, F1–F12.
     * Modifier prefix: ctrl+, alt+, shift+, cmd+  (e.g. "ctrl+c", "alt+F4").
     */
    virtual ActionResult key_press(const std::string& key_name) = 0;

    // -----------------------------------------------------------------------
    // Screenshot
    // -----------------------------------------------------------------------

    /**
     * @brief Capture a high-quality screenshot at native resolution.
     *
     * When all four region parameters are 0 (or omitted) the full virtual
     * desktop is captured. Otherwise the specified rectangle is captured.
     * Coordinates and dimensions are clamped to the desktop bounds.
     *
     * Pixels are BGRA, top-down. Unlike capture(), this method is not
     * performance-critical and may be called from any thread.
     *
     * @param x  Left edge of the region (0 = full desktop).
     * @param y  Top edge of the region  (0 = full desktop).
     * @param w  Width  of the region    (0 = full desktop).
     * @param h  Height of the region    (0 = full desktop).
     */
    virtual ScreenshotResult screenshot(int x = 0, int y = 0,
                                        int w = 0, int h = 0) = 0;

    // -----------------------------------------------------------------------
    // Accessors
    // -----------------------------------------------------------------------

    /** @brief Native width  of the virtual desktop in pixels. */
    virtual int phys_width()  const = 0;

    /** @brief Native height of the virtual desktop in pixels. */
    virtual int phys_height() const = 0;

    // -----------------------------------------------------------------------
    // Constants
    // -----------------------------------------------------------------------

    /**
     * @brief Target width for real-time capture frames.
     *
     * Platform implementations scale captured frames to this width before
     * returning them from capture(). The capture thread then stores them
     * into a shared LatestFrame buffer for direct GL upload.
     */
    static constexpr int CAPTURE_W = 1280;

protected:
    Platform() = default;
};

} // namespace agentdesktop

/**
 * @brief Factory function — returns a heap-allocated Platform instance
 *        appropriate for the current OS.
 *
 * Defined in PlatformWin.cpp / PlatformMac.mm / PlatformLinux.cpp.
 * The caller owns the returned pointer.
 */
agentdesktop::Platform* create_platform();
