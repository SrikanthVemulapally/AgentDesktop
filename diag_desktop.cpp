// Standalone diagnostic: create virtual desktop, launch explorer, enumerate windows
// Build: cl /EHsc diag_desktop.cpp /link user32.lib gdi32.lib
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <windows.h>
#include <cstdio>
#include <vector>
#include <string>

int main() {
    SetProcessDPIAware();

    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);
    printf("Screen: %dx%d\n", sw, sh);

    // Create desktop
    DWORD tick = GetTickCount();
    std::wstring deskName = L"DiagDesktop_" + std::to_wstring(tick);
    std::wstring deskFull = L"WinSta0\\" + deskName;

    HDESK hDesk = CreateDesktopW(deskName.c_str(), nullptr, nullptr, 0, GENERIC_ALL, nullptr);
    if (!hDesk) { printf("CreateDesktopW FAILED: %lu\n", GetLastError()); return 1; }
    printf("CreateDesktopW OK: %p\n", hDesk);

    // Launch explorer
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.lpDesktop = deskFull.data();
    PROCESS_INFORMATION pi{};
    wchar_t exePath[] = L"C:\\Windows\\explorer.exe";
    if (!CreateProcessW(exePath, nullptr, nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        printf("CreateProcessW(explorer) FAILED: %lu\n", GetLastError());
    } else {
        printf("Explorer launched, PID=%lu\n", pi.dwProcessId);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }

    // Wait for explorer to create windows
    printf("Waiting 3 seconds for explorer...\n");
    Sleep(3000);

    // Bind this thread to the virtual desktop
    if (!SetThreadDesktop(hDesk)) {
        printf("SetThreadDesktop FAILED: %lu\n", GetLastError());
        CloseDesktop(hDesk);
        return 1;
    }
    printf("SetThreadDesktop OK\n");

    // Enumerate ALL windows (visible + invisible)
    struct EnumCtx { int total; int visible; std::vector<HWND> vis_wins; };
    EnumCtx ctx{0, 0, {}};
    EnumDesktopWindows(hDesk, [](HWND h, LPARAM lp) -> BOOL {
        auto* c = reinterpret_cast<EnumCtx*>(lp);
        c->total++;
        if (IsWindowVisible(h)) {
            c->visible++;
            c->vis_wins.push_back(h);
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&ctx));

    printf("EnumDesktopWindows: total=%d  visible=%d\n", ctx.total, ctx.visible);

    for (HWND hw : ctx.vis_wins) {
        wchar_t cls[256] = {}, title[256] = {};
        GetClassNameW(hw, cls, 256);
        GetWindowTextW(hw, title, 256);
        RECT r{};
        GetWindowRect(hw, &r);
        printf("  HWND=%p  class='%ls'  title='%ls'  rect=(%ld,%ld,%ld,%ld)\n",
               hw, cls, title, r.left, r.top, r.right, r.bottom);
    }

    // Also try EnumWindows (enumerates current thread's desktop)
    printf("\nEnumWindows (thread desktop):\n");
    struct ECtx2 { int total; int visible; };
    ECtx2 ctx2{0, 0};
    EnumWindows([](HWND h, LPARAM lp) -> BOOL {
        auto* c = reinterpret_cast<ECtx2*>(lp);
        c->total++;
        if (IsWindowVisible(h)) c->visible++;
        return TRUE;
    }, reinterpret_cast<LPARAM>(&ctx2));
    printf("EnumWindows: total=%d  visible=%d\n", ctx2.total, ctx2.visible);

    CloseDesktop(hDesk);
    printf("Done.\n");
    return 0;
}
