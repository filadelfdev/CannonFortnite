#pragma once
#include "core/types.h"
#include "features/esp.h"
#include <functional>
#include <atomic>

namespace cf::ov {

struct OverlayCfg {
    std::atomic<bool> showMenu{true};
    // Defaults picked for 60% keyboards (Akko Fun 60 Pro etc) — no
    // F-row / INSERT / END physical keys. Every one of these fires
    // without Fn held. Rebind UI lives on the Security page.
    std::atomic<int>  menuKey{0xA5};         // VK_RMENU       (Right Alt)
    std::atomic<int>  panicKey{0x14};        // VK_CAPITAL     (Caps Lock)
    std::atomic<int>  toggleCannonKey{0x05}; // VK_XBUTTON1    (mouse side 1)
    std::atomic<int>  toggleSpeedKey{0x06};  // VK_XBUTTON2    (mouse side 2)
    std::atomic<int>  toggleEspKey{0xA3};    // VK_RCONTROL    (Right Ctrl)

    // Rebind state — the overlay sets this to the id of the key being
    // rebound, the input loop watches for the next keydown and stores it.
    std::atomic<int>  rebindTarget{0};       // 0=none, 1..5 = one of the five
};
extern OverlayCfg g_ovCfg;

using TickFn = std::function<void(float dt, float screenW, float screenH)>;

// Create transparent, click-through overlay window, attach a D3D11 swap
// chain to it, run ImGui. Blocks on its own message loop; caller owns the
// thread. Returns on panic or WM_QUIT.
int Run(TickFn perFrame);

// Called from the main thread's actor pipeline to publish the latest
// draw list to the overlay renderer.
void PublishDrawList(std::vector<cf::feat::DrawItem> items);

} // namespace cf::ov
