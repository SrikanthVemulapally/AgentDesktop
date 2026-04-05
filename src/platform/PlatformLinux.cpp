/**
 * @file PlatformLinux.cpp
 * @brief Linux implementation of the Platform interface.
 *
 * Virtual desktop
 * ---------------
 * Starts an Xvfb (X virtual framebuffer) process on a free display number.
 * A lightweight window manager (openbox or fluxbox, whichever is available)
 * is also launched to give applications proper window decoration.
 *
 * Screen capture
 * --------------
 * Uses XGetImage to capture the root window of the Xvfb display. Pixels are
 * extracted per-pixel via XGetPixel to handle arbitrary bit depths robustly.
 *
 * Input
 * -----
 * Mouse and keyboard events are injected via the XTest extension
 * (XTestFakeButtonEvent, XTestFakeKeyEvent, XTestFakeMotionEvent).
 *
 * Dependencies
 * ------------
 *   Runtime: Xvfb, openbox (or fluxbox), libX11, libXtst
 *   Build:   libX11-devel, libXtst-devel, libXext-devel
 *
 * @copyright AgentDesktop Project
 */

#include "Platform.h"

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <X11/extensions/XTest.h>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

/** @brief Find an unused X display number (scans :10 … :99). */
static int find_free_display() {
    for (int n = 10; n < 100; ++n) {
        const std::string lock = "/tmp/.X" + std::to_string(n) + "-lock";
        if (access(lock.c_str(), F_OK) != 0) return n;
    }
    return 10;
}

/** @brief Fork + exec a background process, returning its PID. */
static pid_t spawn(const char* prog, char* const argv[]) {
    pid_t pid = fork();
    if (pid == 0) {
        // Child: redirect stdout/stderr to /dev/null
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) { dup2(devnull, 1); dup2(devnull, 2); close(devnull); }
        execvp(prog, argv);
        _exit(1);
    }
    return pid;
}

// ---------------------------------------------------------------------------
// LinuxPlatform
// ---------------------------------------------------------------------------

class LinuxPlatform : public agentdesktop::Platform {
public:
    LinuxPlatform()  = default;
    ~LinuxPlatform() override { cleanup(); }

    bool init(std::string& error) override;
    bool prepare_capture_thread(std::string& /*error*/) override { return true; }

    agentdesktop::Frame            capture() override;
    agentdesktop::LaunchResult     launch_app(const std::string& path,
                                               const std::string& args) override;
    agentdesktop::ActionResult     maximize_window(int pid) override;
    agentdesktop::ActionResult     click(int x, int y) override;
    agentdesktop::ActionResult     double_click(int x, int y) override;
    agentdesktop::ActionResult     right_click(int x, int y) override;
    agentdesktop::ActionResult     scroll(int x, int y, int delta) override;
    agentdesktop::ActionResult     type_text(const std::string& text) override;
    agentdesktop::ActionResult     key_press(const std::string& key) override;
    agentdesktop::ScreenshotResult screenshot(int rx = 0, int ry = 0,
                                               int rw = 0, int rh = 0) override;

    int phys_width()  const override { return m_w; }
    int phys_height() const override { return m_h; }

private:
    Display*    m_dpy      = nullptr;
    std::string m_disp_str;
    pid_t       m_xvfb_pid = -1;
    pid_t       m_wm_pid   = -1;
    int         m_w        = 1920;
    int         m_h        = 1080;

    void cleanup();

    // Extract one frame's BGRA pixels from a root-window XImage
    static std::vector<uint8_t> image_to_bgra(XImage* img, int w, int h);

    // Resolve key name to X KeySym
    static KeySym name_to_sym(const std::string& name);
};

// ---------------------------------------------------------------------------
// init
// ---------------------------------------------------------------------------

bool LinuxPlatform::init(std::string& error) {
    const int disp_num = find_free_display();
    m_disp_str = ":" + std::to_string(disp_num);

    // Launch Xvfb
    {
        const std::string geom  = std::to_string(m_w) + "x"
                                + std::to_string(m_h) + "x24";
        char* argv[] = {
            const_cast<char*>("Xvfb"),
            const_cast<char*>(m_disp_str.c_str()),
            const_cast<char*>("-screen"), const_cast<char*>("0"),
            const_cast<char*>(geom.c_str()),
            const_cast<char*>("-ac"),
            nullptr
        };
        m_xvfb_pid = spawn("Xvfb", argv);
        if (m_xvfb_pid < 0) {
            error = "Failed to launch Xvfb"; return false;
        }
    }

    // Give Xvfb a moment to start
    std::this_thread::sleep_for(std::chrono::milliseconds(600));

    // Connect to the new display
    m_dpy = XOpenDisplay(m_disp_str.c_str());
    if (!m_dpy) {
        error = "XOpenDisplay failed for " + m_disp_str;
        cleanup();
        return false;
    }

    // Set the background colour on the root window
    const Window root = DefaultRootWindow(m_dpy);
    XSetWindowBackground(m_dpy, root, 0x0D0E14);
    XClearWindow(m_dpy, root);
    XFlush(m_dpy);

    // Launch a lightweight window manager (optional — failures are non-fatal)
    setenv("DISPLAY", m_disp_str.c_str(), 1);
    for (const char* wm : {"openbox", "fluxbox", "icewm"}) {
        char* argv[] = { const_cast<char*>(wm), nullptr };
        m_wm_pid = spawn(wm, argv);
        if (m_wm_pid > 0) break;
    }

    return true;
}

void LinuxPlatform::cleanup() {
    if (m_dpy) { XCloseDisplay(m_dpy); m_dpy = nullptr; }
    if (m_wm_pid   > 0) { kill(m_wm_pid,   SIGTERM); waitpid(m_wm_pid,   nullptr, 0); m_wm_pid   = -1; }
    if (m_xvfb_pid > 0) { kill(m_xvfb_pid, SIGTERM); waitpid(m_xvfb_pid, nullptr, 0); m_xvfb_pid = -1; }
}

// ---------------------------------------------------------------------------
// image_to_bgra
// ---------------------------------------------------------------------------

std::vector<uint8_t> LinuxPlatform::image_to_bgra(XImage* img, int w, int h) {
    std::vector<uint8_t> pixels(static_cast<size_t>(w) * h * 4);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const unsigned long pix = XGetPixel(img, x, y);
            uint8_t* d = pixels.data() + (static_cast<size_t>(y) * w + x) * 4;
            d[0] = static_cast<uint8_t>((pix      ) & 0xFF); // B
            d[1] = static_cast<uint8_t>((pix >>  8) & 0xFF); // G
            d[2] = static_cast<uint8_t>((pix >> 16) & 0xFF); // R
            d[3] = 0xFF;
        }
    }
    return pixels;
}

// ---------------------------------------------------------------------------
// capture
// ---------------------------------------------------------------------------

agentdesktop::Frame LinuxPlatform::capture() {
    agentdesktop::Frame frame;
    if (!m_dpy) return frame;

    const Window root = DefaultRootWindow(m_dpy);

    // Capture at native resolution then scale
    XImage* img = XGetImage(m_dpy, root, 0, 0, m_w, m_h, AllPlanes, ZPixmap);
    if (!img) return frame;

    // Scale down to CAPTURE_W using simple row/col sampling
    const int cap_w = CAPTURE_W;
    const int cap_h = m_h * CAPTURE_W / m_w;
    frame.pixels.resize(static_cast<size_t>(cap_w) * cap_h * 4);

    for (int dy = 0; dy < cap_h; ++dy) {
        const int sy = dy * m_h / cap_h;
        for (int dx = 0; dx < cap_w; ++dx) {
            const int sx = dx * m_w / cap_w;
            const unsigned long pix = XGetPixel(img, sx, sy);
            uint8_t* d = frame.pixels.data() +
                         (static_cast<size_t>(dy) * cap_w + dx) * 4;
            d[0] = static_cast<uint8_t>((pix      ) & 0xFF); // B
            d[1] = static_cast<uint8_t>((pix >>  8) & 0xFF); // G
            d[2] = static_cast<uint8_t>((pix >> 16) & 0xFF); // R
            d[3] = 0xFF;
        }
    }
    XDestroyImage(img);

    frame.width       = cap_w;
    frame.height      = cap_h;
    frame.phys_width  = m_w;
    frame.phys_height = m_h;
    return frame;
}

// ---------------------------------------------------------------------------
// screenshot
// ---------------------------------------------------------------------------

agentdesktop::ScreenshotResult LinuxPlatform::screenshot(
        int rx, int ry, int rw, int rh) {
    agentdesktop::ScreenshotResult result;
    if (!m_dpy) { result.error = "No display"; return result; }

    if (rw <= 0 || rh <= 0) { rx = 0; ry = 0; rw = m_w; rh = m_h; }
    if (rx < 0) rx = 0; if (ry < 0) ry = 0;
    if (rx + rw > m_w) rw = m_w - rx;
    if (ry + rh > m_h) rh = m_h - ry;
    if (rw <= 0 || rh <= 0) { result.error = "Invalid region"; return result; }

    const Window root = DefaultRootWindow(m_dpy);
    XImage* img = XGetImage(m_dpy, root, rx, ry,
                            static_cast<unsigned>(rw),
                            static_cast<unsigned>(rh),
                            AllPlanes, ZPixmap);
    if (!img) { result.error = "XGetImage failed"; return result; }

    result.pixels = image_to_bgra(img, rw, rh);
    XDestroyImage(img);
    result.width  = rw;
    result.height = rh;
    result.ok     = true;
    return result;
}

// ---------------------------------------------------------------------------
// launch_app
// ---------------------------------------------------------------------------

agentdesktop::LaunchResult LinuxPlatform::launch_app(
        const std::string& path, const std::string& args) {
    agentdesktop::LaunchResult result;
    if (!m_dpy) { result.error = "No display"; return result; }

    setenv("DISPLAY", m_disp_str.c_str(), 1);

    // Build argv
    std::vector<std::string> parts;
    parts.push_back(path);
    if (!args.empty()) {
        // Split on spaces (simple tokenise — not shell-level quoting)
        std::istringstream iss(args);
        std::string tok;
        while (iss >> tok) parts.push_back(tok);
    }
    std::vector<char*> argv;
    for (auto& s : parts) argv.push_back(const_cast<char*>(s.c_str()));
    argv.push_back(nullptr);

    const pid_t pid = spawn(path.c_str(), argv.data());
    if (pid < 0) { result.error = "fork failed"; return result; }

    result.ok  = true;
    result.pid = static_cast<int>(pid);
    return result;
}

// ---------------------------------------------------------------------------
// maximize_window
// ---------------------------------------------------------------------------

agentdesktop::ActionResult LinuxPlatform::maximize_window(int pid) {
    if (!m_dpy) return {false, "No display"};
    // Send _NET_WM_STATE maximize to all windows belonging to pid
    const Atom state   = XInternAtom(m_dpy, "_NET_WM_STATE", False);
    const Atom maxH    = XInternAtom(m_dpy, "_NET_WM_STATE_MAXIMIZED_HORZ", False);
    const Atom maxV    = XInternAtom(m_dpy, "_NET_WM_STATE_MAXIMIZED_VERT", False);
    const Window root  = DefaultRootWindow(m_dpy);

    // Walk the window tree to find windows owned by pid
    Window root2, parent2;
    Window* children = nullptr;
    unsigned int nchildren = 0;
    XQueryTree(m_dpy, root, &root2, &parent2, &children, &nchildren);
    for (unsigned int i = 0; i < nchildren; ++i) {
        Atom type; int fmt; unsigned long n, after;
        unsigned char* prop = nullptr;
        Atom pidAtom = XInternAtom(m_dpy, "_NET_WM_PID", True);
        if (pidAtom != None &&
            XGetWindowProperty(m_dpy, children[i], pidAtom,
                               0, 1, False, XA_CARDINAL,
                               &type, &fmt, &n, &after, &prop) == Success
            && prop) {
            const unsigned long wpid = *reinterpret_cast<unsigned long*>(prop);
            XFree(prop);
            if (static_cast<int>(wpid) == pid) {
                XEvent ev{};
                ev.type                 = ClientMessage;
                ev.xclient.window       = children[i];
                ev.xclient.message_type = state;
                ev.xclient.format       = 32;
                ev.xclient.data.l[0]    = 1; // _NET_WM_STATE_ADD
                ev.xclient.data.l[1]    = static_cast<long>(maxH);
                ev.xclient.data.l[2]    = static_cast<long>(maxV);
                XSendEvent(m_dpy, root, False,
                           SubstructureRedirectMask | SubstructureNotifyMask, &ev);
                XFlush(m_dpy);
            }
        }
    }
    if (children) XFree(children);
    return {true};
}

// ---------------------------------------------------------------------------
// click / double_click / right_click / scroll
// ---------------------------------------------------------------------------

agentdesktop::ActionResult LinuxPlatform::click(int x, int y) {
    if (!m_dpy) return {false, "No display"};
    XTestFakeMotionEvent(m_dpy, -1, x, y, 0);
    XTestFakeButtonEvent(m_dpy, 1, True,  0);
    XTestFakeButtonEvent(m_dpy, 1, False, 30);
    XFlush(m_dpy);
    return {true};
}

agentdesktop::ActionResult LinuxPlatform::double_click(int x, int y) {
    click(x, y);
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    click(x, y);
    return {true};
}

agentdesktop::ActionResult LinuxPlatform::right_click(int x, int y) {
    if (!m_dpy) return {false, "No display"};
    XTestFakeMotionEvent(m_dpy, -1, x, y, 0);
    XTestFakeButtonEvent(m_dpy, 3, True,  0);
    XTestFakeButtonEvent(m_dpy, 3, False, 30);
    XFlush(m_dpy);
    return {true};
}

agentdesktop::ActionResult LinuxPlatform::scroll(int x, int y, int delta) {
    if (!m_dpy) return {false, "No display"};
    XTestFakeMotionEvent(m_dpy, -1, x, y, 0);
    const unsigned int btn = delta > 0 ? 4u : 5u;
    const int count = delta < 0 ? -delta : delta;
    for (int i = 0; i < count; ++i) {
        XTestFakeButtonEvent(m_dpy, btn, True,  0);
        XTestFakeButtonEvent(m_dpy, btn, False, 10);
    }
    XFlush(m_dpy);
    return {true};
}

// ---------------------------------------------------------------------------
// type_text
// ---------------------------------------------------------------------------

agentdesktop::ActionResult LinuxPlatform::type_text(const std::string& text) {
    if (!m_dpy) return {false, "No display"};
    for (unsigned char ch : text) {
        const KeySym ks = static_cast<KeySym>(ch < 0x80 ? ch : 0);
        if (!ks) continue;
        const KeyCode kc = XKeysymToKeycode(m_dpy, ks);
        if (!kc) continue;
        XTestFakeKeyEvent(m_dpy, kc, True,  0);
        XTestFakeKeyEvent(m_dpy, kc, False, 4);
    }
    XFlush(m_dpy);
    return {true};
}

// ---------------------------------------------------------------------------
// key_press
// ---------------------------------------------------------------------------

KeySym LinuxPlatform::name_to_sym(const std::string& name) {
    // Modifier-stripped name
    std::string k = name;
    auto eat = [&](const char* prefix) {
        if (k.size() > strlen(prefix) &&
            k.compare(0, strlen(prefix), prefix) == 0)
            k = k.substr(strlen(prefix));
    };
    eat("ctrl+"); eat("alt+"); eat("shift+"); eat("cmd+");

    static const std::pair<const char*, KeySym> kMap[] = {
        {"Enter",      XK_Return},  {"Return",     XK_Return},
        {"Escape",     XK_Escape},  {"Tab",        XK_Tab},
        {"Backspace",  XK_BackSpace},{"Delete",     XK_Delete},
        {"Insert",     XK_Insert},  {"Space",      XK_space},
        {"Home",       XK_Home},    {"End",        XK_End},
        {"PageUp",     XK_Page_Up}, {"PageDown",   XK_Page_Down},
        {"ArrowLeft",  XK_Left},    {"ArrowRight", XK_Right},
        {"ArrowUp",    XK_Up},      {"ArrowDown",  XK_Down},
        {"F1",XK_F1},{"F2",XK_F2},{"F3",XK_F3},{"F4",XK_F4},
        {"F5",XK_F5},{"F6",XK_F6},{"F7",XK_F7},{"F8",XK_F8},
        {"F9",XK_F9},{"F10",XK_F10},{"F11",XK_F11},{"F12",XK_F12},
    };
    for (auto& [nm, sym] : kMap)
        if (k == nm) return sym;
    if (k.size() == 1) return static_cast<KeySym>(k[0]);
    return NoSymbol;
}

agentdesktop::ActionResult LinuxPlatform::key_press(const std::string& key) {
    if (!m_dpy) return {false, "No display"};

    // Parse modifiers
    std::string k = key;
    bool ctrl = false, alt = false, shift = false;
    auto eat = [&](const char* prefix, bool& flag) {
        if (k.size() > strlen(prefix) &&
            k.compare(0, strlen(prefix), prefix) == 0) {
            flag = true; k = k.substr(strlen(prefix)); }
    };
    eat("ctrl+", ctrl); eat("alt+", alt); eat("shift+", shift);
    eat("cmd+",  ctrl); // cmd = ctrl on Linux

    const KeySym ks = name_to_sym(k);
    if (ks == NoSymbol) return {false, "Unknown key: " + key};
    const KeyCode kc = XKeysymToKeycode(m_dpy, ks);
    if (!kc) return {false, "No keycode for: " + key};

    if (ctrl)  XTestFakeKeyEvent(m_dpy, XKeysymToKeycode(m_dpy, XK_Control_L), True,  0);
    if (alt)   XTestFakeKeyEvent(m_dpy, XKeysymToKeycode(m_dpy, XK_Alt_L),     True,  0);
    if (shift) XTestFakeKeyEvent(m_dpy, XKeysymToKeycode(m_dpy, XK_Shift_L),   True,  0);
    XTestFakeKeyEvent(m_dpy, kc, True,  0);
    XTestFakeKeyEvent(m_dpy, kc, False, 0);
    if (shift) XTestFakeKeyEvent(m_dpy, XKeysymToKeycode(m_dpy, XK_Shift_L),   False, 0);
    if (alt)   XTestFakeKeyEvent(m_dpy, XKeysymToKeycode(m_dpy, XK_Alt_L),     False, 0);
    if (ctrl)  XTestFakeKeyEvent(m_dpy, XKeysymToKeycode(m_dpy, XK_Control_L), False, 0);
    XFlush(m_dpy);
    return {true};
}

} // anonymous namespace

// Needed for istringstream in launch_app
#include <sstream>

agentdesktop::Platform* create_platform() {
    return new LinuxPlatform();
}
