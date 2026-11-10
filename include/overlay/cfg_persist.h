#pragma once
#include "overlay/overlay.h"

namespace cf::ov::persist {

// Hotkey persistence. Writes 5 int32 scancodes + a 4-byte magic to a
// small binary file under %LOCALAPPDATA%\Microsoft\Windows\INetCache
// (same place the HWID ledger lives — scanner-unfriendly path).
//
// Loaded once at overlay Run() start, re-written every time the rebind
// UI commits a new key. Idempotent; missing/corrupt file → silently
// fall back to the defaults.

void LoadHotkeys(OverlayCfg& cfg);
void SaveHotkeys(const OverlayCfg& cfg);

} // namespace cf::ov::persist
