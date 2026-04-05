/**
 * @file Theme.h
 * @brief Dear ImGui colour palette and style configuration for AgentDesktop.
 *
 * All colours are defined as 0xRRGGBBAA constants in the Palette namespace
 * so that the entire visual theme can be changed from one place.
 *
 * @copyright AgentDesktop Project
 */

#pragma once

#include <cstdint>

namespace agentdesktop {
namespace app {

// ---------------------------------------------------------------------------
// Colour palette  (0xRRGGBBAA)
// ---------------------------------------------------------------------------

namespace Palette {
    // Backgrounds
    constexpr uint32_t BG0     = 0x0D0E14FF; ///< Deepest background (window fill)
    constexpr uint32_t BG1     = 0x13141DFF; ///< Panel background
    constexpr uint32_t BG2     = 0x1A1B27FF; ///< Child-panel background
    constexpr uint32_t BG3     = 0x22243AFF; ///< Input / button surface

    // Accent
    constexpr uint32_t ACE     = 0x4A90E2FF; ///< Primary blue accent
    constexpr uint32_t ACE_DIM = 0x2B5592FF; ///< Dimmed accent (button default)
    constexpr uint32_t ACE_HOV = 0x5CA8FFFF; ///< Accent hover
    constexpr uint32_t ACE_ACT = 0x3370C0FF; ///< Accent active/pressed

    // Status
    constexpr uint32_t GREEN   = 0x3DDB82FF;
    constexpr uint32_t AMBER   = 0xF5A623FF;
    constexpr uint32_t RED     = 0xFF5555FF;
    constexpr uint32_t BLUE_DIM= 0x6B7DB3FF;

    // Text
    constexpr uint32_t TXT_HI  = 0xE8ECF8FF; ///< High-emphasis text
    constexpr uint32_t TXT_MID = 0x9AA4CCFF; ///< Mid-emphasis text
    constexpr uint32_t TXT_LO  = 0x4C5480FF; ///< Low-emphasis / disabled text

    // Borders
    constexpr uint32_t BORDER  = 0x2A2D44FF;
} // namespace Palette

// ---------------------------------------------------------------------------
// API
// ---------------------------------------------------------------------------

/**
 * @brief Apply the AgentDesktop visual theme to the current ImGui context.
 *
 * Must be called after ImGui::CreateContext() and before the first frame.
 * Safe to call again at runtime to hot-reload the theme.
 */
void apply_theme();

/**
 * @brief Convert a packed 0xRRGGBBAA colour to an ImVec4.
 */
void hex_to_imvec4(uint32_t rgba, float out[4]);

} // namespace app
} // namespace agentdesktop
