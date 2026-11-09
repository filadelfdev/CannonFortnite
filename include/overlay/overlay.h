#pragma once
#include "core/types.h"
#include "features/esp.h"
#include <functional>
#include <atomic>

namespace cf::ov {

struct OverlayCfg {
    std::atomic<bool> showMenu{true};
    std::atomic<int>  menuKey{0x2D};         // VK_INSERT
    std::atomic<int>  panicKey{0x23};        // VK_END — kill everything
    std::atomic<int>  toggleCannonKey{0x79}; // F10
    std::atomic<int>  toggleSpeedKey{0x78};  // F9
    std::atomic<int>  toggleEspKey{0x7A};    // F11
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
