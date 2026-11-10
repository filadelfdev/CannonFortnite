#pragma once
#include <windows.h>

namespace cf::ab::wincloak {

// EAC enumerates top-level windows and grades them. The flag combo
// WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW
// is a published overlay-cheat fingerprint. Two mitigations:
//
// 1. BuildFlags() returns a flag set that gives equivalent behaviour
//    but with a different combination (no TOPMOST on creation, we
//    SetWindowPos it later; no TOOLWINDOW flag; use a child-style
//    layered window parented to a decoy).
// 2. SetWindowDisplayAffinity(WDA_EXCLUDEFROMCAPTURE) hides us from
//    screenshot / capture APIs including PrintWindow and GDI blits.
// 3. Register a regionless class so the window reports class name
//    hash matching common dev tools, not a bespoke name.

DWORD BuildExFlags();
DWORD BuildStyleFlags();
const wchar_t* BuildClassName();

void Harden(HWND h);

} // namespace cf::ab::wincloak
