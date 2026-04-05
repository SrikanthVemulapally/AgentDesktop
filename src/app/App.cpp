/**
 * @file App.cpp
 * @brief Implementation of AgentDesktopApp — UI rendering, platform control,
 *        OpenGL texture management, and input handling.
 *
 * UI layout (left→right, top→bottom)
 * ====================================
 *   ┌─────────────────────────────────────────────────────────┐
 *   │  Title bar  (status dot · FPS · resolution · buttons)   │
 *   ├───────────────────────────────┬─────────────────────────┤
 *   │  Desktop preview (GL texture) │  Sidebar                │
 *   │  Aspect-fit, click/scroll fwd │  LAUNCH / TEXT / KEYS / │
 *   │                               │  AI AGENT / WINDOW      │
 *   ├───────────────────────────────┴─────────────────────────┤
 *   │  Log panel  (timestamped, colour-coded)                  │
 *   ├──────────────────────────────────────────────────────────┤
 *   │  Status bar                                              │
 *   └──────────────────────────────────────────────────────────┘
 *
 * @copyright AgentDesktop Project
 */

#include "App.h"
#include "Theme.h"

// Dear ImGui
#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"

// OpenGL — use GLFW's bundled loader which pulls in the right headers
#include <GLFW/glfw3.h>
// Define GL constants missing from legacy GL/gl.h on Windows
#ifndef GL_CLAMP_TO_EDGE
#  define GL_CLAMP_TO_EDGE 0x812F
#endif
#ifndef GL_BGRA
#  define GL_BGRA 0x80E1
#endif
#ifndef GL_RGBA8
#  define GL_RGBA8 0x8058
#endif

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#endif

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <thread>

namespace agentdesktop {
namespace app {

// ---------------------------------------------------------------------------
// Colour helper — 0xRRGGBBAA → ImVec4
// ---------------------------------------------------------------------------
static ImVec4 hx(uint32_t c) {
    return {
        ((c >> 24) & 0xFF) / 255.f,
        ((c >> 16) & 0xFF) / 255.f,
        ((c >>  8) & 0xFF) / 255.f,
        ((c      ) & 0xFF) / 255.f
    };
}
// Overload for use with ImDrawList::AddRectFilled etc.
static ImU32 col(uint32_t c) { return IM_COL32(
    (c>>24)&0xFF, (c>>16)&0xFF, (c>>8)&0xFF, c&0xFF); }

using namespace Palette;

// ---------------------------------------------------------------------------
// Constructor / destructor
// ---------------------------------------------------------------------------

AgentDesktopApp::AgentDesktopApp(GLFWwindow* window)
    : m_win(window)
{
    apply_theme();

    // Allocate the preview GL texture (will be resized on first frame)
    glGenTextures(1, &m_tex);
    glBindTexture(GL_TEXTURE_2D, m_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S,     GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T,     GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);

    m_fps_t0 = now();
    log("AgentDesktop ready. Click Connect to start the virtual desktop.");
}

AgentDesktopApp::~AgentDesktopApp() {
    do_disconnect();
    if (m_tex) glDeleteTextures(1, &m_tex);
}

// ---------------------------------------------------------------------------
// Wall-clock helper
// ---------------------------------------------------------------------------

double AgentDesktopApp::now() const {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

// ---------------------------------------------------------------------------
// Log (thread-safe)
// ---------------------------------------------------------------------------

void AgentDesktopApp::log(const std::string& msg, LogEntry::Level lv) {
    std::lock_guard<std::mutex> lock(m_log_mtx);
    if (m_log.size() >= static_cast<size_t>(LOG_MAX)) m_log.pop_front();
    m_log.push_back({msg, lv, now()});
    m_log_scroll = true;
}

// ---------------------------------------------------------------------------
// Async worker
// ---------------------------------------------------------------------------

void AgentDesktopApp::async(std::function<void()> fn) {
    if (m_work_thread.joinable()) m_work_thread.join();
    m_busy = true;
    m_work_thread = std::thread([this, fn = std::move(fn)]() {
        fn();
        m_busy = false;
    });
}

// ---------------------------------------------------------------------------
// Platform connect / disconnect
// ---------------------------------------------------------------------------

void AgentDesktopApp::do_connect() {
    if (m_active || m_busy) return;

    log("Connecting — creating virtual desktop...");

    async([this]() {
        agentdesktop::Platform* plat = create_platform();
        std::string err;
        if (!plat->init(err)) {
            log("Connect failed: " + err, LogEntry::Level::Err);
            delete plat;
            return;
        }

        m_platform.reset(plat);
        m_active = true;

        log("Virtual desktop ready  "
            + std::to_string(m_platform->phys_width()) + "\xC3\x97"
            + std::to_string(m_platform->phys_height()),
            LogEntry::Level::Ok);

        m_capture.start(m_platform.get(), &m_latest_frame,
            [this](const std::string& e){
                log("Capture: " + e, LogEntry::Level::Err);
            });

        log("MCP ready — run with --mcp flag to expose AI tools.",
            LogEntry::Level::Ok);
    });
}

void AgentDesktopApp::do_disconnect() {
    if (!m_active) return;
    m_capture.stop();
    if (m_work_thread.joinable()) m_work_thread.join();
    m_platform.reset();
    m_active = false;
    log("Disconnected.");
}

// ---------------------------------------------------------------------------
// OpenGL texture upload (main thread only)
// ---------------------------------------------------------------------------

void AgentDesktopApp::upload_frame() {
    std::lock_guard<std::mutex> lk(m_latest_frame.mtx);
    if (!m_latest_frame.dirty) return;
    m_latest_frame.dirty = false;

    const int w = m_latest_frame.width;
    const int h = m_latest_frame.height;
    if (w <= 0 || h <= 0 || m_latest_frame.pixels.empty()) return;

    // GDI GetDIBits returns BGRA — swap to RGBA in-place because
    // GL_BGRA is an extension that may not be available through
    // GLFW's built-in OpenGL 3.3 Core loader.
    uint8_t* p = m_latest_frame.pixels.data();
    const size_t n = m_latest_frame.pixels.size();
    for (size_t i = 0; i < n; i += 4) std::swap(p[i], p[i + 2]);

    glBindTexture(GL_TEXTURE_2D, m_tex);
    if (w != m_tex_w || h != m_tex_h) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, m_latest_frame.pixels.data());
        m_tex_w = w; m_tex_h = h;
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h,
                        GL_RGBA, GL_UNSIGNED_BYTE, m_latest_frame.pixels.data());
    }
    glBindTexture(GL_TEXTURE_2D, 0);
}

// ---------------------------------------------------------------------------
// Click / scroll forwarding
// ---------------------------------------------------------------------------

void AgentDesktopApp::send_click(float rx, float ry, int type) {
    if (!m_platform) return;
    const int x = static_cast<int>(rx * m_platform->phys_width());
    const int y = static_cast<int>(ry * m_platform->phys_height());
    async([this, x, y, type]() {
        ActionResult r = (type == 1) ? m_platform->double_click(x, y)
                       : (type == 2) ? m_platform->right_click(x, y)
                       :               m_platform->click(x, y);
        if (!r.ok) log("Click error: " + r.error, LogEntry::Level::Warn);
    });
}

void AgentDesktopApp::send_scroll(float rx, float ry, int delta) {
    if (!m_platform) return;
    const int x = static_cast<int>(rx * m_platform->phys_width());
    const int y = static_cast<int>(ry * m_platform->phys_height());
    async([this, x, y, delta]() {
        m_platform->scroll(x, y, delta);
    });
}

// ---------------------------------------------------------------------------
// Master render()
// ---------------------------------------------------------------------------

void AgentDesktopApp::render() {
    // --- FPS tracking --------------------------------------------------------
    ++m_fps_frames;
    const double t = now();
    if (t - m_fps_t0 >= 0.5) {
        m_fps = static_cast<float>(m_fps_frames / (t - m_fps_t0));
        m_fps_frames = 0;
        m_fps_t0 = t;
    }

    // --- Upload any new captured frame ---------------------------------------
    if (m_active) upload_frame();

    // --- Full-window dockspace -----------------------------------------------
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->Pos);
    ImGui::SetNextWindowSize(vp->Size);
    ImGui::SetNextWindowBgAlpha(1.f);

    constexpr ImGuiWindowFlags ROOT_FLAGS =
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoBringToFrontOnFocus;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0, 0});
    ImGui::Begin("##root", nullptr, ROOT_FLAGS);
    ImGui::PopStyleVar();

    const float total_w = vp->Size.x;
    const float total_h = vp->Size.y;

    constexpr float TITLE_H   = 44.f;
    constexpr float STATUS_H  = 26.f;
    constexpr float LOG_H     = 160.f;
    constexpr float SIDEBAR_W = 270.f;

    const float preview_h = total_h - TITLE_H - STATUS_H - LOG_H;
    const float preview_w = total_w - SIDEBAR_W;

    draw_titlebar();

    // Main row
    ImGui::SetCursorPos({0, TITLE_H});
    draw_preview(preview_w, preview_h);
    ImGui::SetCursorPos({preview_w, TITLE_H});
    draw_sidebar(SIDEBAR_W, preview_h);

    // Log + status
    ImGui::SetCursorPos({0, TITLE_H + preview_h});
    draw_log(total_w, LOG_H);
    ImGui::SetCursorPos({0, TITLE_H + preview_h + LOG_H});
    draw_statusbar(total_w, STATUS_H);

    ImGui::End();

    // Modals
    draw_about();
    draw_help();
}

// ---------------------------------------------------------------------------
// draw_titlebar
// ---------------------------------------------------------------------------

void AgentDesktopApp::draw_titlebar() {
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 0.f);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, hx(0x0A0B12FF));
    ImGui::BeginChild("##title", {0, 44}, false, ImGuiWindowFlags_NoScrollbar);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 wp = ImGui::GetWindowPos();
    const float  ww = ImGui::GetWindowWidth();

    // Bottom border line
    dl->AddLine({wp.x, wp.y+43}, {wp.x+ww, wp.y+43}, col(BORDER));

    // Status dot (pulsing when active)
    const float dot_x = wp.x + 18.f;
    const float dot_y = wp.y + 22.f;
    if (m_active) {
        const float pulse = 0.7f + 0.3f * sinf(static_cast<float>(now()) * 3.f);
        dl->AddCircleFilled({dot_x, dot_y}, 6.f, IM_COL32(61, 219, 130,
            static_cast<int>(255 * pulse)));
        dl->AddCircle({dot_x, dot_y}, 7.f, col(0x3DDB8260), 16, 1.5f);
    } else {
        dl->AddCircleFilled({dot_x, dot_y}, 5.f, col(0x444870FF));
    }

    // App title
    ImGui::SetCursorPos({32, 12});
    ImGui::PushStyleColor(ImGuiCol_Text, hx(TXT_HI));
    ImGui::Text("AgentDesktop");
    ImGui::PopStyleColor();

    // Right-side info
    ImGui::SameLine(0, 12);
    if (m_active && m_platform) {
        char info[64];
        snprintf(info, sizeof(info), "%.0f fps  %d×%d",
                 m_fps,
                 m_platform->phys_width(),
                 m_platform->phys_height());
        ImGui::PushStyleColor(ImGuiCol_Text, hx(BLUE_DIM));
        ImGui::Text("%s", info);
        ImGui::PopStyleColor();
    }

    // Buttons on the right
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.f);
    const float btn_y  = 8.f;
    const float btn_h  = 28.f;

    // Help button
    ImGui::SetCursorPos({ww - 280, btn_y});
    ImGui::PushStyleColor(ImGuiCol_Button,        hx(BG3));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, hx(0x2E3356FF));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  hx(BG3));
    if (ImGui::Button("Help", {54, btn_h}))  m_show_help  = true;
    ImGui::PopStyleColor(3);

    ImGui::SameLine(0, 4);
    if (ImGui::Button("About", {54, btn_h})) m_show_about = true;
    ImGui::SameLine(0, 12);

    // Connect / Disconnect
    if (!m_active) {
        ImGui::PushStyleColor(ImGuiCol_Button,        hx(ACE_DIM));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, hx(ACE));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  hx(ACE_ACT));
        if (ImGui::Button("Connect", {90, btn_h})) do_connect();
        ImGui::PopStyleColor(3);
    } else {
        ImGui::PushStyleColor(ImGuiCol_Button,        hx(0x7A1A1AFF));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, hx(0xAA2222FF));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  hx(0x661111FF));
        if (ImGui::Button("Disconnect", {90, btn_h})) do_disconnect();
        ImGui::PopStyleColor(3);
    }

    ImGui::PopStyleVar(); // FrameRounding

    ImGui::EndChild();
    ImGui::PopStyleColor(); // ChildBg
    ImGui::PopStyleVar();   // ChildRounding
}

// ---------------------------------------------------------------------------
// draw_preview
// ---------------------------------------------------------------------------

void AgentDesktopApp::draw_preview(float w, float h) {
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 0.f);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, hx(0x080910FF));
    ImGui::BeginChild("##preview", {w, h}, false,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

    ImDrawList* dl  = ImGui::GetWindowDrawList();
    const ImVec2 wp = ImGui::GetWindowPos();

    m_preview_hover = false;

    if (!m_active || m_tex_w == 0) {
        // Placeholder when not connected
        const char* msg = m_active ? "Waiting for first frame…" : "Not connected";
        const ImVec2 ts = ImGui::CalcTextSize(msg);
        dl->AddText({wp.x + (w - ts.x) * 0.5f, wp.y + (h - ts.y) * 0.5f},
                    col(BLUE_DIM), msg);
    } else {
        // Aspect-fit the texture inside the available area
        const float aspect = static_cast<float>(m_tex_w) / static_cast<float>(m_tex_h);
        float iw = w - 4, ih = iw / aspect;
        if (ih > h - 4) { ih = h - 4; iw = ih * aspect; }
        const float ox = wp.x + (w - iw) * 0.5f;
        const float oy = wp.y + (h - ih) * 0.5f;
        const ImVec2 tl{ox, oy}, br{ox + iw, oy + ih};

        // Subtle dark border behind the image
        dl->AddRect({tl.x - 1.f, tl.y - 1.f}, {br.x + 1.f, br.y + 1.f},
                    IM_COL32(0,0,0,120), 0.f, 0, 1.f);
        dl->AddImage((ImTextureID)(uintptr_t)m_tex, tl, br);

        // FPS overlay (top-right corner of image)
        char buf[32];
        snprintf(buf, sizeof(buf), "%.0f fps", m_fps);
        const ImVec2 ts2 = ImGui::CalcTextSize(buf);
        dl->AddRectFilled({br.x - ts2.x - 12, tl.y + 4},
                          {br.x - 4,          tl.y + 4 + ts2.y + 4},
                          col(0x0A0B14CC), 3.f);
        dl->AddText({br.x - ts2.x - 8, tl.y + 6}, col(BLUE_DIM), buf);

        // Invisible button to capture mouse events over the image
        ImGui::SetCursorScreenPos(tl);
        ImGui::InvisibleButton("##preview_btn", {iw, ih});

        if (ImGui::IsItemHovered()) {
            m_preview_hover = true;
            const ImVec2 mp = ImGui::GetMousePos();
            float rx = (mp.x - tl.x) / iw;
            float ry = (mp.y - tl.y) / ih;
            if (rx < 0.f) rx = 0.f; if (rx > 1.f) rx = 1.f;
            if (ry < 0.f) ry = 0.f; if (ry > 1.f) ry = 1.f;
            m_hover_rx = rx; m_hover_ry = ry;

            const bool dbl   = ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
            const bool sgl   = !dbl && ImGui::IsMouseClicked(ImGuiMouseButton_Left);
            const bool right = ImGui::IsMouseClicked(ImGuiMouseButton_Right);
            if (dbl)   send_click(rx, ry, 1);
            if (sgl)   send_click(rx, ry, 0);
            if (right) send_click(rx, ry, 2);

            // Crosshair cursor hint
            ImGui::SetMouseCursor(ImGuiMouseCursor_None);
            const float cx = mp.x, cy = mp.y;
            dl->AddLine({cx - 10, cy}, {cx + 10, cy}, col(0xFFFFFF60), 1.f);
            dl->AddLine({cx, cy - 10}, {cx, cy + 10}, col(0xFFFFFF60), 1.f);
            dl->AddCircle({cx, cy}, 4.f, col(0xFFFFFF90), 12, 1.f);
        }
    }

    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
}

// ---------------------------------------------------------------------------
// draw_sidebar
// ---------------------------------------------------------------------------

void AgentDesktopApp::draw_sidebar(float w, float h) {
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 0.f);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, hx(BG1));
    ImGui::BeginChild("##sidebar", {w, h}, false);

    ImDrawList* dl  = ImGui::GetWindowDrawList();
    const ImVec2 wp = ImGui::GetWindowPos();
    const float  sw = w;

    // Left border separator
    dl->AddLine(wp, {wp.x, wp.y + h}, col(BORDER));

    ImGui::SetCursorPos({8, 8});
    ImGui::BeginGroup();

    const bool conn = m_active;
    const bool busy = m_busy.load();

    // Section header helper
    auto section = [&](const char* label) {
        ImGui::Spacing();
        const ImVec2 p = ImGui::GetCursorScreenPos();
        dl->AddRectFilled({p.x, p.y + 2}, {p.x + 3, p.y + 14}, col(ACE), 2.f);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 8);
        ImGui::TextColored(hx(BLUE_DIM), "%s", label);
        const float lx = ImGui::GetCursorScreenPos().x;
        const float ly = ImGui::GetCursorScreenPos().y;
        dl->AddLine({lx, ly}, {lx + sw - 24, ly}, col(BORDER));
        ImGui::Spacing();
    };

    // Key button helper
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.f);
    const float kw = (sw - 28) / 4.f;
    auto key_btn = [&](const char* lbl, const char* key, float bw) {
        ImGui::PushStyleColor(ImGuiCol_Button,        hx(BG3));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, hx(0x20243600FF));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  hx(ACE_DIM));
        ImGui::BeginDisabled(busy || !conn);
        if (ImGui::Button(lbl, {bw, 22})) {
            const std::string k(key);
            async([this, k]() {
                const ActionResult r = m_platform->key_press(k);
                if (!r.ok) log("Key error: " + r.error, LogEntry::Level::Warn);
            });
        }
        ImGui::EndDisabled();
        ImGui::PopStyleColor(3);
    };

    // ── LAUNCH ───────────────────────────────────────────────────────────────
    section("LAUNCH APP");
    ImGui::SetNextItemWidth(sw - 24);
    ImGui::InputText("##path", m_app_path, sizeof(m_app_path),
                     ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SetNextItemWidth(sw - 24);
    ImGui::InputText("##args", m_app_args, sizeof(m_app_args));
    ImGui::PushStyleColor(ImGuiCol_Text, hx(TXT_LO));
    ImGui::TextUnformatted("path / args");
    ImGui::PopStyleColor();
    ImGui::Spacing();
    ImGui::BeginDisabled(busy || !conn || m_app_path[0] == '\0');
    if (ImGui::Button("Launch", {-1, 26})) {
        std::string p(m_app_path), a(m_app_args);
        async([this, p, a]() {
            const LaunchResult lr = m_platform->launch_app(p, a);
            if (lr.ok)
                log("Launched  PID=" + std::to_string(lr.pid), LogEntry::Level::Ok);
            else
                log("Launch failed: " + lr.error, LogEntry::Level::Err);
        });
    }
    ImGui::EndDisabled();

    // ── SEND TEXT ─────────────────────────────────────────────────────────────
    section("SEND TEXT");
    ImGui::SetNextItemWidth(sw - 24);
    ImGui::InputText("##type", m_type_buf, sizeof(m_type_buf));
    ImGui::Spacing();
    ImGui::BeginDisabled(busy || !conn || m_type_buf[0] == '\0');
    if (ImGui::Button("Type Text", {-1, 26})) {
        std::string t(m_type_buf);
        async([this, t]() {
            const ActionResult r = m_platform->type_text(t);
            if (!r.ok) log("Type error: " + r.error, LogEntry::Level::Warn);
        });
    }
    ImGui::EndDisabled();

    // ── KEYBOARD ─────────────────────────────────────────────────────────────
    section("KEYBOARD");
    key_btn("Esc",  "Escape",    kw); ImGui::SameLine();
    key_btn("Tab",  "Tab",       kw); ImGui::SameLine();
    key_btn("Bksp", "Backspace", kw); ImGui::SameLine();
    key_btn("Del",  "Delete",    kw);
    key_btn("Home", "Home",      kw); ImGui::SameLine();
    key_btn("End",  "End",       kw); ImGui::SameLine();
    key_btn("PgUp", "PageUp",    kw); ImGui::SameLine();
    key_btn("PgDn", "PageDown",  kw);
    key_btn("F1",   "F1",        kw); ImGui::SameLine();
    key_btn("F2",   "F2",        kw); ImGui::SameLine();
    key_btn("F3",   "F3",        kw); ImGui::SameLine();
    key_btn("F5",   "F5",        kw);
    ImGui::Spacing();
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + kw + 4);
    key_btn(" \xe2\x96\xb2 ", "ArrowUp",    kw);
    key_btn(" \xe2\x97\x80 ", "ArrowLeft",  kw); ImGui::SameLine();
    key_btn(" \xe2\x96\xbc ", "ArrowDown",  kw); ImGui::SameLine();
    key_btn(" \xe2\x96\xb6 ", "ArrowRight", kw);
    ImGui::PopStyleVar(); // FrameRounding

    // ── AI AGENT (MCP) ────────────────────────────────────────────────────────
    section("AI AGENT (MCP)");
    {
        const ImVec2 cp = ImGui::GetCursorScreenPos();
        dl->AddRectFilled({cp.x - 2, cp.y - 2}, {cp.x + sw - 22, cp.y + 54},
                          col(0x0D1220FF), 5.f);
        dl->AddRect({cp.x - 2, cp.y - 2}, {cp.x + sw - 22, cp.y + 54},
                    col(BORDER), 5.f, 0, 1.f);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 6);
        ImGui::TextColored({.32f,.48f,.70f,1.f}, "Protocol");
        ImGui::SameLine(0, 6);
        ImGui::TextColored(hx(TXT_MID), "MCP stdio  JSON-RPC 2.0");
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 6);
        ImGui::TextColored({.32f,.48f,.70f,1.f}, "Tools   ");
        ImGui::SameLine(0, 6);
        ImGui::TextColored(hx(TXT_MID), "screenshot launch click type keys");
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 6);
        ImGui::TextColored({.32f,.48f,.70f,1.f}, "Flag    ");
        ImGui::SameLine(0, 6);
        ImGui::TextColored(hx(ACE_HOV), "AgentDesktop --mcp");
    }
    ImGui::Dummy({0, 58});
    ImGui::Spacing();

    ImGui::PushStyleColor(ImGuiCol_Button,        hx(0x13182800FF));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, hx(0x182642FF));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  hx(BG3));
    if (ImGui::Button("Copy MCP Config", {-1, 26})) {
        std::string snippet;
#ifdef _WIN32
        {
            wchar_t exe_path[260] = {};
            GetModuleFileNameW(nullptr, exe_path, 260);
            char utf8[260 * 3] = {};
            WideCharToMultiByte(CP_UTF8, 0, exe_path, -1, utf8, (int)sizeof(utf8), nullptr, nullptr);
            snippet =
                "\"agentdesktop\": {\n"
                "  \"command\": \"" + std::string(utf8) + "\",\n"
                "  \"args\": [\"--mcp\"]\n"
                "}";
        }
#else
        snippet =
            "\"agentdesktop\": {\n"
            "  \"command\": \"AgentDesktop\",\n"
            "  \"args\": [\"--mcp\"]\n"
            "}";
#endif
        ImGui::SetClipboardText(snippet.c_str());
        log("MCP config snippet copied to clipboard.", LogEntry::Level::Ok);
    }
    ImGui::PopStyleColor(3);
    ImGui::Spacing();

    // ── WINDOW ───────────────────────────────────────────────────────────────
    section("WINDOW");
    ImGui::SetNextItemWidth(sw - 80);
    ImGui::InputInt("##pid", &m_pid_input);
    ImGui::SameLine();
    ImGui::TextColored(hx(TXT_LO), "PID");
    ImGui::Spacing();
    ImGui::BeginDisabled(busy || !conn || m_pid_input <= 0);
    if (ImGui::Button("Maximize Window", {-1, 26})) {
        const int pid = m_pid_input;
        async([this, pid]() {
            const ActionResult r = m_platform->maximize_window(pid);
            if (!r.ok) log("Maximize failed: " + r.error, LogEntry::Level::Warn);
            else       log("Window maximized (PID=" + std::to_string(pid) + ")",
                           LogEntry::Level::Ok);
        });
    }
    ImGui::EndDisabled();

    ImGui::EndGroup();
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
}

// ---------------------------------------------------------------------------
// draw_log
// ---------------------------------------------------------------------------

void AgentDesktopApp::draw_log(float w, float h) {
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 0.f);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, hx(0x0A0B12FF));
    ImGui::BeginChild("##log", {w, h}, false, ImGuiWindowFlags_NoScrollbar);

    ImDrawList* dl  = ImGui::GetWindowDrawList();
    const ImVec2 wp = ImGui::GetWindowPos();
    dl->AddLine(wp, {wp.x + w, wp.y}, col(BORDER));

    // Toolbar
    ImGui::SetCursorPos({8, 4});
    ImGui::TextColored(hx(BLUE_DIM), "LOG");
    ImGui::SameLine(0, 12);
    ImGui::PushStyleColor(ImGuiCol_Button,        hx(BG3));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, hx(0x2E335600));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  hx(BG3));
    if (ImGui::SmallButton("Clear")) {
        std::lock_guard<std::mutex> lk(m_log_mtx);
        m_log.clear();
    }
    ImGui::PopStyleColor(3);

    // Scrolling list
    ImGui::SetCursorPos({0, 22});
    ImGui::PushStyleColor(ImGuiCol_ChildBg, hx(0));
    ImGui::BeginChild("##loginner", {w, h - 22}, false);
    {
        std::lock_guard<std::mutex> lk(m_log_mtx);
        for (const auto& e : m_log) {
            // Timestamp
            char ts[16];
            const int s  = static_cast<int>(e.timestamp) % 3600;
            const int m_ = s / 60, ss = s % 60;
            snprintf(ts, sizeof(ts), "%02d:%02d", m_, ss);
            ImGui::TextColored(hx(TXT_LO), "%s", ts);
            ImGui::SameLine(0, 6);

            // Level tag
            const char*  tag;
            ImVec4 tc;
            switch (e.level) {
                case LogEntry::Level::Ok:   tag = "[OK]"; tc = hx(GREEN); break;
                case LogEntry::Level::Warn: tag = "[!!]"; tc = hx(AMBER); break;
                case LogEntry::Level::Err:  tag = "[XX]"; tc = hx(RED);   break;
                default:                    tag = "    "; tc = hx(TXT_MID); break;
            }
            ImGui::TextColored(tc, "%s", tag);
            ImGui::SameLine(0, 6);
            ImGui::TextColored(hx(TXT_HI), "%s", e.text.c_str());
        }
    }
    if (m_log_scroll) {
        ImGui::SetScrollHereY(1.f);
        m_log_scroll = false;
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();

    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
}

// ---------------------------------------------------------------------------
// draw_statusbar
// ---------------------------------------------------------------------------

void AgentDesktopApp::draw_statusbar(float w, float h) {
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 0.f);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, hx(0x090A10FF));
    ImGui::BeginChild("##status", {w, h}, false, ImGuiWindowFlags_NoScrollbar);

    ImDrawList* dl  = ImGui::GetWindowDrawList();
    const ImVec2 wp = ImGui::GetWindowPos();
    dl->AddLine(wp, {wp.x + w, wp.y}, col(BORDER));

    ImGui::SetCursorPos({12, 4});
    // Platform tag
    ImGui::TextColored(hx(BLUE_DIM),
#ifdef _WIN32
        "Windows"
#elif defined(__APPLE__)
        "macOS"
#else
        "Linux"
#endif
    );
    ImGui::SameLine(0, 12);

    // Tech tags
    auto tag = [&](const char* label, uint32_t bg_col) {
        const ImVec2 p  = ImGui::GetCursorScreenPos();
        const ImVec2 ts = ImGui::CalcTextSize(label);
        dl->AddRectFilled({p.x - 4, p.y - 1}, {p.x + ts.x + 4, p.y + ts.y + 1},
                          col(bg_col), 3.f);
        ImGui::TextColored(hx(TXT_HI), "%s", label);
        ImGui::SameLine(0, 6);
    };
    tag("ImGui",  0x1B2A4AFF);
    tag("OpenGL", 0x1B3A2AFF);
    tag("MCP",    0x2A1B3AFF);

    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
}

// ---------------------------------------------------------------------------
// draw_about / draw_help (modal popups)
// ---------------------------------------------------------------------------

void AgentDesktopApp::draw_about() {
    if (!m_show_about) return;
    ImGui::OpenPopup("About AgentDesktop");
    ImGui::SetNextWindowSize({400, 220}, ImGuiCond_Always);
    if (ImGui::BeginPopupModal("About AgentDesktop", &m_show_about,
                               ImGuiWindowFlags_NoResize)) {
        ImGui::Spacing();
        ImGui::TextColored(hx(Palette::ACE_HOV), "AgentDesktop v1.0.0");
        ImGui::Separator();
        ImGui::TextWrapped(
            "A cross-platform AI agent virtual desktop.\n\n"
            "Provides an isolated virtual desktop for AI agents to launch and "
            "interact with applications via the Model Context Protocol (MCP).\n\n"
            "Stack: C++17 · Dear ImGui · GLFW · OpenGL 3.3 · nlohmann/json");
        ImGui::Spacing();
        if (ImGui::Button("Close", {120, 0})) {
            m_show_about = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void AgentDesktopApp::draw_help() {
    if (!m_show_help) return;
    ImGui::OpenPopup("Help");
    ImGui::SetNextWindowSize({480, 320}, ImGuiCond_Always);
    if (ImGui::BeginPopupModal("Help", &m_show_help, ImGuiWindowFlags_NoResize)) {
        ImGui::Spacing();
        ImGui::TextColored(hx(Palette::ACE_HOV), "Quick Start");
        ImGui::Separator();
        ImGui::TextWrapped(
            "1. Click Connect to create the virtual desktop.\n"
            "2. Type an app name in 'LAUNCH APP' (e.g. notepad, chrome) and click Launch.\n"
            "3. Click inside the preview to send mouse clicks to the virtual desktop.\n"
            "4. Use 'SEND TEXT' to type into the focused window.\n\n"
            "MCP Integration\n"
            "---------------\n"
            "Run with --mcp to expose all controls as MCP tools:\n"
            "  AgentDesktop.exe --mcp\n\n"
            "Click 'Copy MCP Config' to get the JSON snippet for your AI client.");
        ImGui::Spacing();
        if (ImGui::Button("Close", {120, 0})) {
            m_show_help = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

// ---------------------------------------------------------------------------
// Input callbacks
// ---------------------------------------------------------------------------

void AgentDesktopApp::on_key(int glfw_key, int action, int mods) {
    ImGui_ImplGlfw_KeyCallback(m_win, glfw_key, 0, action, mods);
    if (!m_active || !m_platform) return;
    if (action == 0 /* GLFW_RELEASE */ ) return;
    if (ImGui::GetIO().WantCaptureKeyboard) return;

    // Simple key-name mapping (extend as needed)
    const char* key = nullptr;
    switch (glfw_key) {
        case 256: key = "Escape";    break;
        case 257: key = "Enter";     break;
        case 258: key = "Tab";       break;
        case 259: key = "Backspace"; break;
        case 261: key = "Delete";    break;
        case 262: key = "ArrowRight";break;
        case 263: key = "ArrowLeft"; break;
        case 264: key = "ArrowDown"; break;
        case 265: key = "ArrowUp";   break;
        case 266: key = "PageUp";    break;
        case 267: key = "PageDown";  break;
        case 268: key = "Home";      break;
        case 269: key = "End";       break;
        default: break;
    }
    if (key) {
        const std::string k(key);
        async([this, k]() { m_platform->key_press(k); });
    }
}

void AgentDesktopApp::on_char(unsigned int codepoint) {
    ImGui_ImplGlfw_CharCallback(m_win, codepoint);
    if (!m_active || !m_platform) return;
    if (ImGui::GetIO().WantCaptureKeyboard) return;
    // Convert single codepoint to UTF-8 and type it
    char utf8[5] = {};
    if (codepoint < 0x80u) {
        utf8[0] = static_cast<char>(codepoint);
    } else if (codepoint < 0x800u) {
        utf8[0] = static_cast<char>(0xC0 | (codepoint >> 6));
        utf8[1] = static_cast<char>(0x80 | (codepoint & 0x3F));
    } else if (codepoint < 0x10000u) {
        utf8[0] = static_cast<char>(0xE0 | (codepoint >> 12));
        utf8[1] = static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
        utf8[2] = static_cast<char>(0x80 | (codepoint & 0x3F));
    }
    if (utf8[0]) {
        const std::string s(utf8);
        async([this, s]() { m_platform->type_text(s); });
    }
}

void AgentDesktopApp::on_scroll(double /*xoff*/, double yoff) {
    if (!m_active || !m_preview_hover) return;
    send_scroll(m_hover_rx, m_hover_ry, static_cast<int>(yoff));
}

} // namespace app
} // namespace agentdesktop
