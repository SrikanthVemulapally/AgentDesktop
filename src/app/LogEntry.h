/**
 * @file LogEntry.h
 * @brief Log entry data type used by the application UI.
 *
 * LogEntry is a plain data struct with no methods.  The UI log panel
 * displays entries colour-coded by level in a scrollable list.
 *
 * @copyright AgentDesktop Project
 */

#pragma once

#include <string>

namespace agentdesktop {
namespace app {

/**
 * @brief A single entry in the in-application log panel.
 */
struct LogEntry {
    /** @brief Severity level for colour-coded rendering. */
    enum class Level {
        Info,  ///< Neutral informational message  (blue-grey)
        Ok,    ///< Successful operation            (green)
        Warn,  ///< Non-fatal warning               (amber)
        Err    ///< Error requiring attention        (red)
    };

    std::string text;                ///< Human-readable message
    Level       level     = Level::Info;
    double      timestamp = 0.0;    ///< wall-clock seconds (steady_clock epoch)
};

} // namespace app
} // namespace agentdesktop
