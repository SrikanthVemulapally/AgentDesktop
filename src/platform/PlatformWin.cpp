/**
 * @file PlatformWin.cpp
 * @brief Windows implementation of the Platform interface.
 *
 * Virtual desktop
 * ---------------
 * Uses Win32 CreateDesktopW to create an isolated desktop invisible to the
 * user's current session.  Applications launched inside it cannot see the
 * user's windows and vice-versa.
 *
 * Screen capture
 * --------------
 * Each capture() call:
 *   1. Enumerates all visible windows on the virtual desktop.
 *   2. Paints each window off-screen via PrintWindow(PW_RENDERFULLCONTENT).
 *   3. Composites the result into a back-buffer bitmap.
 *   4. Scales down to Platform::CAPTURE_W with a fast GDI StretchBlt.
 *   5. Extracts raw BGRA pixels via GetDIBits.
 *
 * Input
 * -----
 * Mouse clicks are sent via PostMessage(WM_LBUTTONDOWN/UP …) on the deepest
 * child window at the click coordinate.  Keyboard input is sent via
 * SendInput (injected to the foreground window of the virtual desktop).
 *
 * @copyright AgentDesktop Project
 */

#ifndef UNICODE
#  define UNICODE
#endif
#ifndef _UNICODE
#  define _UNICODE
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>

#ifndef PW_RENDERFULLCONTENT
#  define PW_RENDERFULLCONTENT 2u
#endif

#include "Platform.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

// ---------------------------------------------------------------------------
// String helpers
// ---------------------------------------------------------------------------

static std::wstring to_wide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(),
                                      static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                        w.data(), n);
    return w;
}

static std::string from_wide(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(),
                                      static_cast<int>(w.size()),
                                      nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                        s.data(), n, nullptr, nullptr);
    return s;
}

// ---------------------------------------------------------------------------
// WindowsPlatform
// ---------------------------------------------------------------------------

namespace {

class WindowsPlatform : public agentdesktop::Platform {
public:
    WindowsPlatform()  = default;
    ~WindowsPlatform() override { cleanup(); }

    // Exposed for functional tests
    bool is_desktop_open() const { return m_desktop != nullptr; }

    bool init(std::string& error) override;
    bool prepare_capture_thread(std::string& error) override;
    agentdesktop::Frame capture() override;

    agentdesktop::LaunchResult  launch_app(const std::string& path,
                                            const std::string& args) override;
    agentdesktop::ActionResult  maximize_window(int pid) override;
    agentdesktop::ActionResult  click(int x, int y)        override;
    agentdesktop::ActionResult  double_click(int x, int y) override;
    agentdesktop::ActionResult  right_click(int x, int y)  override;
    agentdesktop::ActionResult  scroll(int x, int y, int delta) override;
    agentdesktop::ActionResult  type_text(const std::string& text) override;
    agentdesktop::ActionResult  key_press(const std::string& key) override;
    agentdesktop::ScreenshotResult screenshot(int rx = 0, int ry = 0,
                                               int rw = 0, int rh = 0) override;

    int phys_width()  const override { return m_phys_w; }
    int phys_height() const override { return m_phys_h; }

private:
    HDESK        m_desktop      = nullptr;
    std::wstring m_desk_name;              // e.g. AgentDesktop_1234
    std::wstring m_desk_full;              // e.g. WinSta0\AgentDesktop_1234
    HANDLE       m_shell_proc   = nullptr; // explorer.exe on the virtual desktop
    int          m_phys_w       = 1920;
    int          m_phys_h       = 1080;
    // Top-level window last focused by click()/double_click().
    // type_text() and key_press() use this instead of topmost_window() so
    // shell/taskbar windows created by Explorer cannot steal keystrokes.
    std::atomic<uintptr_t> m_last_clicked{0};

    void cleanup();
    bool launch_shell();  // start explorer.exe on m_desktop

    // GDI composite of all virtual-desktop windows into hMem
    void composite_to_dc(HDC hMem, HDC hScreenDC) const;

    // Find the deepest child HWND at screen coordinate (x,y)
    struct HitResult { HWND hwnd = nullptr; int cx = 0; int cy = 0; };
    HitResult hit_test(int x, int y) const;

    // Resolve short app names to full paths (notepad → notepad.exe etc.)
    static std::wstring resolve_app(const std::string& app);

    // Return the frontmost visible window on the virtual desktop
    HWND topmost_window() const;

    // Find the actual focused child control to send keyboard input to
    HWND find_typing_target(HWND main_wnd) const;

    // Bring a window into focus on the virtual desktop
    void activate_window(HWND hWnd) const;


    // LPARAM encoding for WM_*BUTTON* messages
    static LPARAM lp_xy(int x, int y) {
        return (LPARAM)((((WORD)y) << 16) | ((WORD)x));
    }
};

// ---------------------------------------------------------------------------
// init
// ---------------------------------------------------------------------------

bool WindowsPlatform::init(std::string& error) {
    SetProcessDPIAware();

    // Query the primary display resolution
    m_phys_w = GetSystemMetrics(SM_CXSCREEN);
    m_phys_h = GetSystemMetrics(SM_CYSCREEN);

    // Unique desktop name — use tick count to avoid collisions on rapid restart
    m_desk_name = L"AgentDesktop_" + std::to_wstring(GetTickCount());
    // Station-qualified name required by STARTUPINFOW.lpDesktop
    m_desk_full = L"WinSta0\\" + m_desk_name;

    m_desktop = CreateDesktopW(
        m_desk_name.c_str(),
        nullptr, nullptr, 0,
        GENERIC_ALL,
        nullptr);

    if (!m_desktop) {
        const DWORD err = GetLastError();
        error = "CreateDesktopW failed (error " + std::to_string(err) + ")";
        return false;
    }

    // Start explorer.exe as shell on the virtual desktop (non-fatal if absent)
    launch_shell();
    return true;
}

bool WindowsPlatform::launch_shell() {
    // Use the station-qualified name so explorer attaches to the right desktop
    STARTUPINFOW si{};
    si.cb        = sizeof(si);
    si.lpDesktop = m_desk_full.data();

    PROCESS_INFORMATION pi{};
    wchar_t exePath[] = L"C:\\Windows\\explorer.exe";
    if (!CreateProcessW(exePath, nullptr, nullptr, nullptr,
                        FALSE, 0, nullptr, nullptr, &si, &pi)) {
        return false;
    }

    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    m_shell_proc = nullptr;
    return true;
}

bool WindowsPlatform::prepare_capture_thread(std::string& error) {
    if (!SetThreadDesktop(m_desktop)) {
        error = "SetThreadDesktop failed (error=" + std::to_string(GetLastError()) + ")";
        return false;
    }
    return true;
}

void WindowsPlatform::cleanup() {
    // Terminate the shell process we spawned
    if (m_shell_proc) {
        TerminateProcess(m_shell_proc, 0);
        WaitForSingleObject(m_shell_proc, 2000);
        CloseHandle(m_shell_proc);
        m_shell_proc = nullptr;
    }
    if (m_desktop) {
        CloseDesktop(m_desktop);
        m_desktop = nullptr;
    }
}

// ---------------------------------------------------------------------------
// composite_to_dc — used by screenshot(); capture() inlines this logic.
// ---------------------------------------------------------------------------

void WindowsPlatform::composite_to_dc(HDC hMem, HDC hScreenDC) const {
    HBRUSH hBrush = CreateSolidBrush(RGB(30, 30, 30));
    RECT   bgRect = {0, 0, m_phys_w, m_phys_h};
    FillRect(hMem, &bgRect, hBrush);
    DeleteObject(hBrush);

    struct EnumCtx { std::vector<HWND> windows; };
    EnumCtx ctx;
    EnumDesktopWindows(m_desktop, [](HWND h, LPARAM lp) -> BOOL {
        auto* c = reinterpret_cast<EnumCtx*>(lp);
        if (IsWindowVisible(h)) c->windows.push_back(h);
        return TRUE;
    }, reinterpret_cast<LPARAM>(&ctx));

    std::reverse(ctx.windows.begin(), ctx.windows.end());

    for (HWND hWnd : ctx.windows) {
        RECT r;
        if (!GetWindowRect(hWnd, &r)) continue;
        int ww = r.right - r.left, wh = r.bottom - r.top;
        if (ww <= 0 || wh <= 0) continue;
        if (r.right <= 0 || r.bottom <= 0 || r.left >= m_phys_w || r.top >= m_phys_h) continue;

        HDC     hWDC  = CreateCompatibleDC(hScreenDC);
        HBITMAP hWBmp = CreateCompatibleBitmap(hScreenDC, ww, wh);
        SelectObject(hWDC, hWBmp);
        PrintWindow(hWnd, hWDC, PW_RENDERFULLCONTENT);
        BitBlt(hMem, r.left, r.top, ww, wh, hWDC, 0, 0, SRCCOPY);
        DeleteObject(hWBmp);
        DeleteDC(hWDC);
    }
}

// ---------------------------------------------------------------------------
// capture — inline composite + scale + extract, single hScreenDC
// ---------------------------------------------------------------------------

agentdesktop::Frame WindowsPlatform::capture() {
    agentdesktop::Frame result;

    HDC hScreenDC = GetDC(nullptr);
    HDC hMemDC    = CreateCompatibleDC(hScreenDC);
    HBITMAP hBmp  = CreateCompatibleBitmap(hScreenDC, m_phys_w, m_phys_h);
    HGDIOBJ hOld  = SelectObject(hMemDC, hBmp);

    // Fill background
    HBRUSH hBrush = CreateSolidBrush(RGB(30, 30, 30));
    RECT   bgRect = {0, 0, m_phys_w, m_phys_h};
    FillRect(hMemDC, &bgRect, hBrush);
    DeleteObject(hBrush);

    // Enumerate visible windows on the virtual desktop
    struct EnumCtx { std::vector<HWND> windows; };
    EnumCtx ctx;
    EnumDesktopWindows(m_desktop, [](HWND h, LPARAM lp) -> BOOL {
        auto* c = reinterpret_cast<EnumCtx*>(lp);
        if (IsWindowVisible(h)) c->windows.push_back(h);
        return TRUE;
    }, reinterpret_cast<LPARAM>(&ctx));

    std::reverse(ctx.windows.begin(), ctx.windows.end());

    for (HWND hWnd : ctx.windows) {
        RECT r;
        if (!GetWindowRect(hWnd, &r)) continue;
        int ww = r.right - r.left, wh = r.bottom - r.top;
        if (ww <= 0 || wh <= 0) continue;
        if (r.right <= 0 || r.bottom <= 0 || r.left >= m_phys_w || r.top >= m_phys_h) continue;

        HDC     hWDC  = CreateCompatibleDC(hScreenDC);
        HBITMAP hWBmp = CreateCompatibleBitmap(hScreenDC, ww, wh);
        SelectObject(hWDC, hWBmp);
        PrintWindow(hWnd, hWDC, PW_RENDERFULLCONTENT);
        BitBlt(hMemDC, r.left, r.top, ww, wh, hWDC, 0, 0, SRCCOPY);
        DeleteObject(hWBmp);
        DeleteDC(hWDC);
    }

    // Scale down to CAPTURE_W
    int targetH = static_cast<int>(static_cast<double>(m_phys_h) * CAPTURE_W / m_phys_w);
    HDC     hScaleDC  = CreateCompatibleDC(hScreenDC);
    HBITMAP hScaleBmp = CreateCompatibleBitmap(hScreenDC, CAPTURE_W, targetH);
    SelectObject(hScaleDC, hScaleBmp);
    SetStretchBltMode(hScaleDC, HALFTONE);
    SetBrushOrgEx(hScaleDC, 0, 0, nullptr);
    StretchBlt(hScaleDC, 0, 0, CAPTURE_W, targetH,
               hMemDC,   0, 0, m_phys_w, m_phys_h, SRCCOPY);

    // Extract BGRA pixels
    BITMAPINFOHEADER bih{};
    bih.biSize        = sizeof(bih);
    bih.biWidth       = CAPTURE_W;
    bih.biHeight      = -targetH;
    bih.biPlanes      = 1;
    bih.biBitCount    = 32;
    bih.biCompression = BI_RGB;

    result.pixels.resize(static_cast<size_t>(CAPTURE_W) * targetH * 4);
    GetDIBits(hScaleDC, hScaleBmp, 0, targetH,
              result.pixels.data(),
              reinterpret_cast<BITMAPINFO*>(&bih), DIB_RGB_COLORS);

    result.width       = CAPTURE_W;
    result.height      = targetH;
    result.phys_width  = m_phys_w;
    result.phys_height = m_phys_h;

    // Cleanup
    SelectObject(hMemDC, hOld);
    DeleteObject(hScaleBmp);
    DeleteDC(hScaleDC);
    DeleteObject(hBmp);
    DeleteDC(hMemDC);
    ReleaseDC(nullptr, hScreenDC);
    return result;
}

// ---------------------------------------------------------------------------
// screenshot (native resolution, optional region)
// ---------------------------------------------------------------------------

agentdesktop::ScreenshotResult WindowsPlatform::screenshot(
        int rx, int ry, int rw, int rh) {
    agentdesktop::ScreenshotResult result;

    if (rw <= 0 || rh <= 0) { rx = 0; ry = 0; rw = m_phys_w; rh = m_phys_h; }
    if (rx < 0) rx = 0; if (ry < 0) ry = 0;
    if (rx + rw > m_phys_w) rw = m_phys_w - rx;
    if (ry + rh > m_phys_h) rh = m_phys_h - ry;
    if (rw <= 0 || rh <= 0) { result.error = "Region out of bounds"; return result; }

    HDC     hScreen = GetDC(nullptr);
    HDC     hMem    = CreateCompatibleDC(hScreen);
    HBITMAP hBmp    = CreateCompatibleBitmap(hScreen, m_phys_w, m_phys_h);
    HGDIOBJ hOld    = SelectObject(hMem, hBmp);

    composite_to_dc(hMem, hScreen);

    // Crop to requested region
    HDC     hReg  = CreateCompatibleDC(hScreen);
    HBITMAP hRBmp = CreateCompatibleBitmap(hScreen, rw, rh);
    SelectObject(hReg, hRBmp);
    BitBlt(hReg, 0, 0, rw, rh, hMem, rx, ry, SRCCOPY);

    BITMAPINFOHEADER bih{};
    bih.biSize = sizeof(bih); bih.biWidth = rw; bih.biHeight = -rh;
    bih.biPlanes = 1; bih.biBitCount = 32; bih.biCompression = BI_RGB;

    result.pixels.resize(static_cast<size_t>(rw) * rh * 4);
    GetDIBits(hReg, hRBmp, 0, rh, result.pixels.data(),
              reinterpret_cast<BITMAPINFO*>(&bih), DIB_RGB_COLORS);
    result.width = rw; result.height = rh; result.ok = true;

    DeleteObject(hRBmp); DeleteDC(hReg);
    SelectObject(hMem, hOld);
    DeleteObject(hBmp); DeleteDC(hMem);
    ReleaseDC(nullptr, hScreen);
    return result;
}

// ---------------------------------------------------------------------------
// launch_app
// ---------------------------------------------------------------------------

std::wstring WindowsPlatform::resolve_app(const std::string& app) {
    const std::wstring wapp = to_wide(app);

    // Already an absolute path — use as-is
    if (wapp.find(L'\\') != std::wstring::npos || wapp.find(L'/') != std::wstring::npos)
        return wapp;

    // Lower-case for map lookup
    std::wstring lower = wapp;
    for (auto& c : lower) c = towlower(c);

    // Browsers first with absolute install paths (not in PATH or System32)
    static const std::pair<const wchar_t*, const wchar_t*> kAbsMap[] = {
        {L"chrome",        L"C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe"},
        {L"google chrome", L"C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe"},
        {L"edge",          L"C:\\Program Files (x86)\\Microsoft\\Edge\\Application\\msedge.exe"},
        {L"microsoft edge",L"C:\\Program Files (x86)\\Microsoft\\Edge\\Application\\msedge.exe"},
    };
    for (auto& [key, val] : kAbsMap) {
        if (lower == key) {
            if (GetFileAttributesW(val) != INVALID_FILE_ATTRIBUTES)
                return val;
            break; // not at expected location — fall through to PATH search
        }
    }

    const std::wstring wexe = wapp.find(L'.') == std::wstring::npos
                              ? wapp + L".exe" : wapp;

    // Search PATH
    wchar_t found[MAX_PATH]{};
    if (SearchPathW(nullptr, wexe.c_str(), nullptr, MAX_PATH, found, nullptr))
        return found;

    // Common system apps
    wchar_t sys[MAX_PATH]{};
    GetSystemDirectoryW(sys, MAX_PATH);
    const std::wstring sysd(sys);

    static const std::pair<const wchar_t*, const wchar_t*> kSysMap[] = {
        {L"notepad",    L"notepad.exe"},
        {L"calc",       L"calc.exe"},
        {L"paint",      L"mspaint.exe"},
        {L"cmd",        L"cmd.exe"},
        {L"powershell", L"WindowsPowerShell\\v1.0\\powershell.exe"},
        {L"explorer",   L"explorer.exe"},
        {L"wordpad",    L"wordpad.exe"},
        {L"taskmgr",    L"taskmgr.exe"},
    };
    for (auto& [key, val] : kSysMap) {
        if (lower == key) {
            const std::wstring full = sysd + L"\\" + val;
            if (GetFileAttributesW(full.c_str()) != INVALID_FILE_ATTRIBUTES)
                return full;
            return val;
        }
    }
    return wexe;
}

agentdesktop::LaunchResult WindowsPlatform::launch_app(
        const std::string& path, const std::string& args) {
    agentdesktop::LaunchResult result;

    const std::wstring wpath = resolve_app(path);
    std::wstring wargs = to_wide(args);

    // Chrome / Edge: inject --disable-gpu and a per-desktop user-data-dir so
    // the browser renders correctly on the virtual display.
    const bool is_chrome = wpath.find(L"chrome.exe") != std::wstring::npos;
    const bool is_edge   = wpath.find(L"msedge.exe") != std::wstring::npos;
    if (is_chrome || is_edge) {
        if (wargs.find(L"--disable-gpu") == std::wstring::npos) {
            wchar_t tmp[MAX_PATH]{};
            GetTempPathW(MAX_PATH, tmp);
            const std::wstring udd = std::wstring(tmp) + L"ad-" + m_desk_name;
            wargs += L" --disable-gpu --no-first-run --user-data-dir=\"" + udd + L"\"";
        }
    }

    std::wstring cmdline = L"\"" + wpath + L"\"";
    if (!wargs.empty()) cmdline += L" " + wargs;

    STARTUPINFOW si{};
    si.cb        = sizeof(si);
    // Must use station-qualified name so the process attaches to the right desktop
    si.lpDesktop = m_desk_full.data();

    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr,
                        const_cast<LPWSTR>(cmdline.c_str()),
                        nullptr, nullptr, FALSE,
                        NORMAL_PRIORITY_CLASS,
                        nullptr, nullptr, &si, &pi)) {
        result.error = "CreateProcessW failed: " + std::to_string(GetLastError());
        return result;
    }

    result.ok  = true;
    result.pid = static_cast<int>(pi.dwProcessId);
    const DWORD launched_pid = pi.dwProcessId;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    // Wait for the app's main window to appear (up to 3 s) so that subsequent
    // type_text / key_press calls target it directly instead of relying on
    // topmost_window() which may pick up Explorer shell windows.
    struct WaitCtx {
        HDESK               desktop;
        DWORD               pid;
        HWND                found   = nullptr;
        std::atomic<uintptr_t>* last_clicked;
    };
    WaitCtx wctx{ m_desktop, launched_pid, nullptr, &m_last_clicked };

    HANDLE hWait = CreateThread(nullptr, 0, [](void* arg) -> DWORD {
        auto* c = reinterpret_cast<WaitCtx*>(arg);
        // Must bind this thread to the virtual desktop before any User32 call
        SetThreadDesktop(c->desktop);

        auto enumFn = [](HWND h, LPARAM lp) -> BOOL {
            auto* ctx = reinterpret_cast<WaitCtx*>(lp);
            if (!IsWindowVisible(h)) return TRUE;
            if (GetWindow(h, GW_OWNER) != nullptr) return TRUE; // skip owned windows
            wchar_t title[256]{};
            GetWindowTextW(h, title, 256);
            if (title[0] == L'\0') return TRUE;
            DWORD wpid = 0;
            GetWindowThreadProcessId(h, &wpid);
            if (wpid == ctx->pid) { ctx->found = h; return FALSE; }
            return TRUE;
        };

        for (int i = 0; i < 10 && !c->found; ++i) {
            Sleep(300);
            EnumDesktopWindows(c->desktop, enumFn, reinterpret_cast<LPARAM>(c));
        }

        if (c->found)
            c->last_clicked->store(reinterpret_cast<uintptr_t>(c->found));
        return 0;
    }, &wctx, 0, nullptr);

    if (hWait) {
        WaitForSingleObject(hWait, 4000);
        CloseHandle(hWait);
    }

    return result;
}

// ---------------------------------------------------------------------------
// maximize_window
// ---------------------------------------------------------------------------

agentdesktop::ActionResult WindowsPlatform::maximize_window(int pid) {
    // EnumDesktopWindows only works from a thread bound to the target desktop.
    // Spawn a fresh thread (no message queue yet), bind it to m_desktop first,
    // then enumerate.
    struct Ctx {
        HDESK desktop;
        DWORD pid;
        HWND  found = nullptr;
    };
    Ctx ctx{ m_desktop, static_cast<DWORD>(pid) };

    auto enumFn = [](HWND h, LPARAM lp) -> BOOL {
        auto* c = reinterpret_cast<Ctx*>(lp);
        DWORD wpid = 0;
        GetWindowThreadProcessId(h, &wpid);
        if (wpid == c->pid && IsWindowVisible(h) && GetWindow(h, GW_OWNER) == nullptr) {
            c->found = h; return FALSE;
        }
        return TRUE;
    };

    auto worker = [](void* arg) -> DWORD {
        auto* c = reinterpret_cast<Ctx*>(arg);
        // Bind this fresh thread to the virtual desktop BEFORE any User32 call
        SetThreadDesktop(c->desktop);

        BOOL(*fn)(HWND, LPARAM) = [](HWND h, LPARAM lp) -> BOOL {
            auto* cx = reinterpret_cast<Ctx*>(lp);
            DWORD wpid = 0;
            GetWindowThreadProcessId(h, &wpid);
            if (wpid == cx->pid && IsWindowVisible(h) && GetWindow(h, GW_OWNER) == nullptr) {
                cx->found = h; return FALSE;
            }
            return TRUE;
        };

        // Poll up to 5 retries × 500 ms = 2.5 s
        EnumDesktopWindows(c->desktop, fn, reinterpret_cast<LPARAM>(c));
        for (int i = 0; i < 5 && !c->found; ++i) {
            Sleep(500);
            EnumDesktopWindows(c->desktop, fn, reinterpret_cast<LPARAM>(c));
        }

        if (c->found) {
            ShowWindow(c->found, SW_MAXIMIZE);
            SetForegroundWindow(c->found);
        }
        return 0;
    };
    (void)enumFn; // used by worker lambda via capture

    HANDLE hThread = CreateThread(nullptr, 0, worker, &ctx, 0, nullptr);
    if (!hThread)
        return {false, "CreateThread failed: " + std::to_string(GetLastError())};

    // Wait up to 4 s (2.5 s poll + 1.5 s headroom)
    WaitForSingleObject(hThread, 4000);
    CloseHandle(hThread);

    if (!ctx.found)
        return {false, "No window found for PID=" + std::to_string(pid)};
    return {true, ""};
}

// ---------------------------------------------------------------------------
// hit_test
// ---------------------------------------------------------------------------

static HWND deep_child(HWND parent, POINT pt) {
    HWND child = ChildWindowFromPointEx(parent, pt,
                    CWP_SKIPTRANSPARENT | CWP_SKIPINVISIBLE);
    if (!child || child == parent) return nullptr;
    POINT cp = pt;
    ScreenToClient(child, &cp);
    HWND deeper = deep_child(child, cp);
    return deeper ? deeper : child;
}

WindowsPlatform::HitResult WindowsPlatform::hit_test(int x, int y) const {
    struct Ctx { int x, y; std::vector<std::pair<HWND,RECT>> wins; };
    Ctx ctx{x, y};
    EnumDesktopWindows(m_desktop,
        [](HWND h, LPARAM lp) -> BOOL {
            if (!IsWindowVisible(h)) return TRUE;
            auto* c = reinterpret_cast<Ctx*>(lp);
            RECT r{};
            if (GetWindowRect(h, &r)) c->wins.push_back({h, r});
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&ctx));

    for (auto& [hw, r] : ctx.wins) {
        if (x < r.left || x >= r.right || y < r.top || y >= r.bottom) continue;
        POINT sp{x, y};
        HWND dc  = deep_child(hw, sp);
        HWND tgt = dc ? dc : hw;
        POINT cp{x, y};
        ScreenToClient(tgt, &cp);
        return {tgt, static_cast<int>(cp.x), static_cast<int>(cp.y)};
    }
    return {};
}

HWND WindowsPlatform::topmost_window() const {
    HWND top = nullptr;
    EnumDesktopWindows(m_desktop,
        [](HWND h, LPARAM lp) -> BOOL {
            if (!IsWindowVisible(h)) return TRUE;
            wchar_t title[256]{};
            GetWindowTextW(h, title, 256);
            // Skip the desktop shell window and untitled windows
            if (wcscmp(title, L"Program Manager") != 0 && title[0] != L'\0') {
                *reinterpret_cast<HWND*>(lp) = h;
                return FALSE;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&top));
    return top;
}

// ---------------------------------------------------------------------------
// find_typing_target — resolve the focused child input control
// Uses AttachThreadInput to query focus across thread boundaries, then falls
// back to searching for an Edit/RichEdit child.
// ---------------------------------------------------------------------------

HWND WindowsPlatform::find_typing_target(HWND main_wnd) const {
    if (!main_wnd) return nullptr;

    DWORD tid_current = GetCurrentThreadId();
    DWORD tid_target  = GetWindowThreadProcessId(main_wnd, nullptr);
    HDESK hOrig       = GetThreadDesktop(tid_current);
    bool  switched    = (hOrig != m_desktop) && SetThreadDesktop(m_desktop);

    HWND focused = nullptr;
    if (tid_target && tid_target != tid_current) {
        if (AttachThreadInput(tid_current, tid_target, TRUE)) {
            focused = GetFocus();
            AttachThreadInput(tid_current, tid_target, FALSE);
        }
    }
    if (switched) SetThreadDesktop(hOrig);
    if (focused) return focused;

    // Fallback: find the first visible Edit / RichEdit child
    HWND editChild = nullptr;
    EnumChildWindows(main_wnd, [](HWND h, LPARAM lp) -> BOOL {
        wchar_t cls[64]{};
        GetClassNameW(h, cls, 64);
        if ((wcscmp(cls, L"Edit") == 0 || wcsncmp(cls, L"RichEdit", 8) == 0)
                && IsWindowVisible(h)) {
            *reinterpret_cast<HWND*>(lp) = h;
            return FALSE;
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&editChild));

    return editChild ? editChild : main_wnd;
}

// ---------------------------------------------------------------------------
// activate_window — bring target window to focus on the virtual desktop
// ---------------------------------------------------------------------------

void WindowsPlatform::activate_window(HWND hWnd) const {
    DWORD tid_current = GetCurrentThreadId();
    HDESK hOrig       = GetThreadDesktop(tid_current);
    bool  switched    = (hOrig != m_desktop) && SetThreadDesktop(m_desktop);

    BringWindowToTop(hWnd);
    DWORD tid_target = GetWindowThreadProcessId(hWnd, nullptr);
    if (tid_target && tid_target != tid_current) {
        if (AttachThreadInput(tid_current, tid_target, TRUE)) {
            SetForegroundWindow(hWnd);
            SetFocus(hWnd);
            AttachThreadInput(tid_current, tid_target, FALSE);
        }
    }
    if (switched) SetThreadDesktop(hOrig);
}


// ---------------------------------------------------------------------------
// click / double_click / right_click
// ---------------------------------------------------------------------------

// Store root window as click target only if it is a real app window
// (has a non-empty title that is not "Program Manager").
// This prevents clicking on the virtual desktop background — which is a
// shell window — from redirecting subsequent keystrokes to the shell.
static void try_set_last_clicked(std::atomic<uintptr_t>& out, HWND tgt) {
    HWND root = GetAncestor(tgt, GA_ROOT);
    HWND candidate = root ? root : tgt;
    wchar_t title[256]{};
    GetWindowTextW(candidate, title, 256);
    if (title[0] != L'\0' && wcscmp(title, L"Program Manager") != 0)
        out.store(reinterpret_cast<uintptr_t>(candidate));
}

agentdesktop::ActionResult WindowsPlatform::click(int x, int y) {
    auto [tgt, cx, cy] = hit_test(x, y);
    if (!tgt) return {false, "No window at coordinate"};
    activate_window(tgt);
    try_set_last_clicked(m_last_clicked, tgt);
    const LPARAM lp = lp_xy(cx, cy);
    PostMessage(tgt, WM_MOUSEMOVE,   0,          lp);
    PostMessage(tgt, WM_LBUTTONDOWN, MK_LBUTTON, lp);
    PostMessage(tgt, WM_LBUTTONUP,   0,          lp);
    return {true};
}

agentdesktop::ActionResult WindowsPlatform::double_click(int x, int y) {
    auto [tgt, cx, cy] = hit_test(x, y);
    if (!tgt) return {false, "No window at coordinate"};
    activate_window(tgt);
    try_set_last_clicked(m_last_clicked, tgt);
    const LPARAM lp = lp_xy(cx, cy);
    PostMessage(tgt, WM_MOUSEMOVE,     0,          lp);
    PostMessage(tgt, WM_LBUTTONDOWN,   MK_LBUTTON, lp);
    PostMessage(tgt, WM_LBUTTONUP,     0,          lp);
    PostMessage(tgt, WM_LBUTTONDBLCLK, MK_LBUTTON, lp);
    PostMessage(tgt, WM_LBUTTONUP,     0,          lp);
    return {true};
}

agentdesktop::ActionResult WindowsPlatform::right_click(int x, int y) {
    auto [tgt, cx, cy] = hit_test(x, y);
    if (!tgt) return {false, "No window at coordinate"};
    const LPARAM lp = lp_xy(cx, cy);
    PostMessage(tgt, WM_RBUTTONDOWN, MK_RBUTTON, lp);
    Sleep(30);
    PostMessage(tgt, WM_RBUTTONUP,   0,           lp);
    return {true};
}

// ---------------------------------------------------------------------------
// scroll
// ---------------------------------------------------------------------------

agentdesktop::ActionResult WindowsPlatform::scroll(int x, int y, int delta) {
    auto [tgt, cx, cy] = hit_test(x, y);
    if (!tgt) return {false, "No window at coordinate"};
    PostMessage(tgt, WM_MOUSEWHEEL,
                MAKEWPARAM(0, static_cast<SHORT>(delta * WHEEL_DELTA)),
                lp_xy(x, y));
    return {true};
}

// ---------------------------------------------------------------------------
// type_text
// ---------------------------------------------------------------------------

agentdesktop::ActionResult WindowsPlatform::type_text(const std::string& text) {
    // Use the window last focused by launch_app() or click().  Never fall back
    // to topmost_window(): it can return Explorer shell/search windows whose
    // titles are non-empty, causing keystrokes to open cmd or a search bar.
    HWND hMain = reinterpret_cast<HWND>(m_last_clicked.load());
    if (!hMain || !IsWindow(hMain))
        return {false, "No target window — launch an app or click in the preview first"};

    const HWND hTarget = find_typing_target(hMain);
    const std::wstring wide = to_wide(text);
    const HWND dst = hTarget ? hTarget : hMain;
    for (wchar_t ch : wide)
        PostMessage(dst, WM_CHAR, static_cast<WPARAM>(ch), 1);
    return {true};
}

// ---------------------------------------------------------------------------
// key_press
// ---------------------------------------------------------------------------

agentdesktop::ActionResult WindowsPlatform::key_press(const std::string& key) {
    // Parse optional modifier prefix
    UINT vk   = 0;
    bool ctrl  = false, alt = false, shift = false;

    std::string k = key;
    auto eat = [&](const char* prefix, bool& flag) {
        if (k.size() > strlen(prefix) &&
            k.compare(0, strlen(prefix), prefix) == 0) {
            flag = true; k = k.substr(strlen(prefix)); }
    };
    eat("ctrl+",  ctrl);
    eat("alt+",   alt);
    eat("shift+", shift);
    eat("cmd+",   ctrl); // treat cmd as ctrl on Windows

    static const std::pair<const char*, UINT> kMap[] = {
        {"Enter",      VK_RETURN}, {"Return",    VK_RETURN},
        {"Escape",     VK_ESCAPE}, {"Tab",       VK_TAB},
        {"Backspace",  VK_BACK},   {"Delete",    VK_DELETE},
        {"Insert",     VK_INSERT}, {"Space",     VK_SPACE},
        {"Home",       VK_HOME},   {"End",       VK_END},
        {"PageUp",     VK_PRIOR},  {"PageDown",  VK_NEXT},
        {"ArrowLeft",  VK_LEFT},   {"ArrowRight",VK_RIGHT},
        {"ArrowUp",    VK_UP},     {"ArrowDown", VK_DOWN},
        {"F1",VK_F1},{"F2",VK_F2},{"F3",VK_F3},{"F4",VK_F4},
        {"F5",VK_F5},{"F6",VK_F6},{"F7",VK_F7},{"F8",VK_F8},
        {"F9",VK_F9},{"F10",VK_F10},{"F11",VK_F11},{"F12",VK_F12},
    };
    for (auto& [name, code] : kMap)
        if (k == name) { vk = code; break; }

    // Single printable character
    if (!vk && k.size() == 1)
        vk = VkKeyScanA(k[0]) & 0xFF;

    if (!vk) return {false, "Unrecognised key: " + key};

    HWND hMain = reinterpret_cast<HWND>(m_last_clicked.load());
    if (!hMain || !IsWindow(hMain))
        return {false, "No target window — launch an app or click in the preview first"};
    HWND hTarget = find_typing_target(hMain);

    if (ctrl || alt || shift) {
        // PostMessage does not update GetKeyState, so modifier combos (e.g. Ctrl+C)
        // fail in apps that check GetKeyState(VK_CONTROL) in their WM_KEYDOWN handler.
        // Use SendInput instead — it feeds the hardware input queue and sets keyboard
        // state correctly. Switch to the virtual desktop thread first and bring the
        // target window into focus.
        DWORD tid_current = GetCurrentThreadId();
        HDESK hOrig       = GetThreadDesktop(tid_current);
        bool  switched    = (hOrig != m_desktop) && SetThreadDesktop(m_desktop);

        if (hTarget) activate_window(hTarget);

        std::vector<INPUT> inputs;
        auto push_key = [&](WORD code, bool down) {
            INPUT in{};
            in.type       = INPUT_KEYBOARD;
            in.ki.wVk     = code;
            in.ki.wScan   = static_cast<WORD>(MapVirtualKey(code, MAPVK_VK_TO_VSC));
            in.ki.dwFlags = down ? 0 : KEYEVENTF_KEYUP;
            inputs.push_back(in);
        };

        if (ctrl)  push_key(VK_CONTROL, true);
        if (alt)   push_key(VK_MENU,    true);
        if (shift) push_key(VK_SHIFT,   true);
        push_key(static_cast<WORD>(vk), true);
        push_key(static_cast<WORD>(vk), false);
        if (shift) push_key(VK_SHIFT,   false);
        if (alt)   push_key(VK_MENU,    false);
        if (ctrl)  push_key(VK_CONTROL, false);

        SendInput(static_cast<UINT>(inputs.size()), inputs.data(), sizeof(INPUT));
        if (switched) SetThreadDesktop(hOrig);
    } else {
        if (!hTarget) return {false, "No window on virtual desktop"};
        PostMessage(hTarget, WM_KEYDOWN, vk, 0);
        Sleep(10);
        PostMessage(hTarget, WM_KEYUP,   vk, 0);
    }

    return {true};
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Factory
// ---------------------------------------------------------------------------

agentdesktop::Platform* create_platform() {
    return new WindowsPlatform();
}
