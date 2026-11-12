#pragma once

#if __has_include(<imgui.h>)
#  include <imgui.h>
#  define CF_HAS_IMGUI 1
#endif

namespace cf::ov::theme {

// Palette — dark navy card, cyan accent, muted mid for row surfaces.
// Kept to 7 colours so the UI reads as one system; tokens below.
namespace col {
    constexpr unsigned bg      = 0xFF0B0F14;   // window (deeper)
    constexpr unsigned bg2     = 0xFF13181F;   // header strip / sidebar
    constexpr unsigned surf    = 0xFF1A2029;   // row / card
    constexpr unsigned surfHi  = 0xFF242C38;   // hover
    constexpr unsigned stroke  = 0xFF2F3A4B;
    constexpr unsigned text    = 0xFFEEF2F7;
    constexpr unsigned textDim = 0xFF8B97A8;
    // Dual-accent: gold for brand / piracy, cyan for interactive state.
    constexpr unsigned gold    = 0xFFE8B938;   // brand / logo / highlights
    constexpr unsigned goldDim = 0xFFA07A20;
    constexpr unsigned accent  = 0xFF5BC0EB;   // cyan — ON states
    constexpr unsigned accent2 = 0xFF3A95C2;
    constexpr unsigned ok      = 0xFF4ADE80;
    constexpr unsigned warn    = 0xFFF59E0B;
    constexpr unsigned bad     = 0xFFEF4444;
}

void Apply();     // installs the ImGui style; cheap, call once per device init.

} // namespace cf::ov::theme
