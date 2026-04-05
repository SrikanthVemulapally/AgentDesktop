/**
 * @file Theme.cpp
 * @brief Implementation of the AgentDesktop ImGui visual theme.
 *
 * @copyright AgentDesktop Project
 */

#include "Theme.h"
#include "imgui.h"

namespace agentdesktop {
namespace app {

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

/** Convert 0xRRGGBBAA to ImVec4 */
static ImVec4 hx(uint32_t c) {
    return {
        ((c >> 24) & 0xFF) / 255.f,
        ((c >> 16) & 0xFF) / 255.f,
        ((c >>  8) & 0xFF) / 255.f,
        ((c      ) & 0xFF) / 255.f
    };
}

// ---------------------------------------------------------------------------
// apply_theme
// ---------------------------------------------------------------------------

void apply_theme() {
    using namespace Palette;

    ImGuiStyle& s = ImGui::GetStyle();

    // --- Layout ---
    s.WindowPadding    = {14, 14};
    s.FramePadding     = {10,  6};
    s.ItemSpacing      = { 8,  6};
    s.ItemInnerSpacing = { 6,  4};
    s.CellPadding      = { 6,  4};
    s.IndentSpacing    = 18;
    s.ScrollbarSize    =  9;
    s.GrabMinSize      = 10;

    // --- Rounding ---
    s.WindowRounding   =  0;
    s.ChildRounding    =  6;
    s.FrameRounding    =  5;
    s.PopupRounding    =  8;
    s.ScrollbarRounding=  5;
    s.GrabRounding     =  4;
    s.TabRounding      =  5;

    // --- Borders ---
    s.WindowBorderSize =  0;
    s.FrameBorderSize  =  0;
    s.ChildBorderSize  =  0;
    s.PopupBorderSize  =  1;
    s.TabBarBorderSize =  0;

    // --- Colours ---
    auto* c = s.Colors;
    c[ImGuiCol_WindowBg]             = hx(BG0);
    c[ImGuiCol_ChildBg]              = hx(BG1);
    c[ImGuiCol_PopupBg]              = hx(0x151621F5);
    c[ImGuiCol_FrameBg]              = hx(BG3);
    c[ImGuiCol_FrameBgHovered]       = hx(0x2E3356FF);
    c[ImGuiCol_FrameBgActive]        = hx(0x2A4A7AFF);
    c[ImGuiCol_Border]               = hx(BORDER);
    c[ImGuiCol_BorderShadow]         = {0,0,0,0};
    c[ImGuiCol_Text]                 = hx(TXT_HI);
    c[ImGuiCol_TextDisabled]         = hx(TXT_LO);
    c[ImGuiCol_TitleBg]              = hx(BG0);
    c[ImGuiCol_TitleBgActive]        = hx(0x0F1A30FF);
    c[ImGuiCol_TitleBgCollapsed]     = hx(0x0A0B12E0);
    c[ImGuiCol_MenuBarBg]            = hx(BG1);
    c[ImGuiCol_ScrollbarBg]          = hx(BG0);
    c[ImGuiCol_ScrollbarGrab]        = hx(BG3);
    c[ImGuiCol_ScrollbarGrabHovered] = hx(0x353860FF);
    c[ImGuiCol_ScrollbarGrabActive]  = hx(ACE_DIM);
    c[ImGuiCol_CheckMark]            = hx(ACE);
    c[ImGuiCol_SliderGrab]           = hx(ACE);
    c[ImGuiCol_SliderGrabActive]     = hx(ACE_HOV);
    c[ImGuiCol_Button]               = hx(ACE_DIM);
    c[ImGuiCol_ButtonHovered]        = hx(ACE);
    c[ImGuiCol_ButtonActive]         = hx(ACE_ACT);
    c[ImGuiCol_Header]               = hx(0x2B5592AA);
    c[ImGuiCol_HeaderHovered]        = hx(ACE_DIM);
    c[ImGuiCol_HeaderActive]         = hx(ACE);
    c[ImGuiCol_Separator]            = hx(BORDER);
    c[ImGuiCol_SeparatorHovered]     = hx(ACE_DIM);
    c[ImGuiCol_SeparatorActive]      = hx(ACE);
    c[ImGuiCol_ResizeGrip]           = hx(0x4A90E240);
    c[ImGuiCol_ResizeGripHovered]    = hx(0x4A90E2AA);
    c[ImGuiCol_ResizeGripActive]     = hx(ACE);
    c[ImGuiCol_Tab]                  = hx(0x1C2444FF);
    c[ImGuiCol_TabHovered]           = hx(ACE_DIM);
    c[ImGuiCol_TabActive]            = hx(0x2B5592FF);
    c[ImGuiCol_TabUnfocused]         = hx(0x111626FF);
    c[ImGuiCol_TabUnfocusedActive]   = hx(0x1E2F50FF);
    c[ImGuiCol_PlotLines]            = hx(GREEN);
    c[ImGuiCol_PlotHistogram]        = hx(ACE);
    c[ImGuiCol_TableHeaderBg]        = hx(0x161827FF);
    c[ImGuiCol_TableBorderStrong]    = hx(BORDER);
    c[ImGuiCol_TableBorderLight]     = hx(0x1E2030FF);
    c[ImGuiCol_ModalWindowDimBg]     = {0,0,0,.6f};
}

} // namespace app
} // namespace agentdesktop
