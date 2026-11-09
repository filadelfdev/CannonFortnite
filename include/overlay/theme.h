#pragma once

#if __has_include(<imgui.h>)
#  include <imgui.h>
#  define CF_HAS_IMGUI 1
#endif

namespace cf::ov::theme {

// Palette — dark navy card, cyan accent, muted mid for row surfaces.
// Kept to 7 colours so the UI reads as one system; tokens below.
namespace col {
    constexpr unsigned bg      = 0xFF0F1216;   // window
    constexpr unsigned bg2     = 0xFF161B22;   // header strip / sidebar
    constexpr unsigned surf    = 0xFF1C232D;   // row / card
    constexpr unsigned surfHi  = 0xFF242D39;   // hover
    constexpr unsigned stroke  = 0xFF2A3340;
    constexpr unsigned text    = 0xFFE6EDF3;
    constexpr unsigned textDim = 0xFF8792A2;
    constexpr unsigned accent  = 0xFF5BC0EB;   // cyan — ON states
    constexpr unsigned accent2 = 0xFF3A95C2;   // pressed
    constexpr unsigned ok      = 0xFF4ADE80;
    constexpr unsigned warn    = 0xFFF59E0B;
    constexpr unsigned bad     = 0xFFEF4444;
}

void Apply();     // installs the ImGui style; cheap, call once per device init.

} // namespace cf::ov::theme
