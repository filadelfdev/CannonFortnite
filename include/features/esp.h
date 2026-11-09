#pragma once
#include "core/types.h"
#include <vector>
#include <atomic>

namespace cf::feat {

struct EspCfg {
    std::atomic<bool> enabled{true};
    std::atomic<bool> drawChests{true};
    std::atomic<bool> drawCannons{true};
    std::atomic<bool> drawPlayers{true};
    std::atomic<bool> drawLines{true};
    std::atomic<bool> drawDistance{true};
    std::atomic<float> maxDraw_m{600.f};
    std::atomic<int>   chestColor{0xFFFFD700};   // ARGB — gold
    std::atomic<int>   cannonColor{0xFFFF6B35};
    std::atomic<int>   playerColor{0xFFE74C3C};
    std::atomic<int>   lootDropColor{0xFF2ECC71};
};
extern EspCfg g_espCfg;

struct DrawItem {
    float x, y;
    float dist_m;
    int   color;
    char  label[32];
    bool  drawLineFromBottom;
};
std::vector<DrawItem> BuildDrawList(const LocalPlayerCache& lp,
                                    const std::vector<Actor>& actors,
                                    float screenW, float screenH);

} // namespace cf::feat
