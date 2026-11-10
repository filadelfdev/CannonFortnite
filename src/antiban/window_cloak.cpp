#include "antiban/window_cloak.h"

namespace cf::ab::wincloak {

DWORD BuildExFlags(){
    // Deliberately NOT: WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW combo.
    // Layered alone is common for legit input-method windows.
    return WS_EX_LAYERED | WS_EX_NOACTIVATE;
}

DWORD BuildStyleFlags(){
    // Popup child-style — no border, no caption, no system menu.
    return WS_POPUP;
}

const wchar_t* BuildClassName(){
    // Mimics a Windows Shell tooltip pane class name — benign to every
    // enumerator that filters by shell-owned classes.
    return L"tooltips_class32";
}

void Harden(HWND h){
    // Exclude ourselves from screen capture (Win10 2004+). PrintWindow,
    // GDI BitBlt, WGC — all return a black frame for our pixels.
    using SetWindowDisplayAffinity_t = BOOL (WINAPI*)(HWND, DWORD);
    auto p = (SetWindowDisplayAffinity_t)GetProcAddress(
                GetModuleHandleW(L"user32.dll"), "SetWindowDisplayAffinity");
    if(p){
        constexpr DWORD WDA_EXCLUDEFROMCAPTURE = 0x11;
        p(h, WDA_EXCLUDEFROMCAPTURE);
    }

    // Promote to topmost AFTER the window is created, not on create.
    // The flag-combo heuristic snapshots on WM_CREATE — late-flip
    // keeps us off that scan.
    SetWindowPos(h, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

} // namespace cf::ab::wincloak
