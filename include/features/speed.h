#pragma once
#include "core/types.h"
#include <atomic>

namespace cf::feat {

struct SpeedCfg {
    std::atomic<bool>  enabled{true};             // default on — user wants collection fast
    std::atomic<float> walkSpeed{6400.f};         // vanilla ~600, ~10x
    std::atomic<float> flySpeed{9200.f};          // vanilla ~1200
    std::atomic<float> acceleration{32000.f};     // instant stop/start
    std::atomic<float> airControl{1.0f};
    std::atomic<float> gravityScale{0.35f};       // moonjumps over roofs
    std::atomic<float> jumpZ{1400.f};
    std::atomic<float> brakingFriction{0.0f};
    std::atomic<bool>  pulseWrite{true};
    std::atomic<int>   pulseEveryTicks{1};        // write every tick now the kernel path is cheap
};

extern SpeedCfg g_speedCfg;

void TickSpeed(const LocalPlayerCache& lp, int tickIndex);

} // namespace cf::feat
