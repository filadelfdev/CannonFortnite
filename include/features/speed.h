#pragma once
#include "core/types.h"
#include <atomic>

namespace cf::feat {

struct SpeedCfg {
    std::atomic<bool>  enabled{false};
    std::atomic<float> walkSpeed{2800.f};        // vanilla ~600
    std::atomic<float> flySpeed{4800.f};         // vanilla ~1200
    std::atomic<float> acceleration{16000.f};    // vanilla ~2048
    std::atomic<float> airControl{1.0f};         // vanilla 0.35
    std::atomic<float> gravityScale{0.4f};       // vanilla 1.0 — moonjumps
    std::atomic<float> jumpZ{1100.f};            // vanilla 620
    std::atomic<float> brakingFriction{0.0f};
    std::atomic<bool>  pulseWrite{true};         // re-write every N ticks (snapback defence)
    std::atomic<int>   pulseEveryTicks{3};
};

extern SpeedCfg g_speedCfg;

void TickSpeed(const LocalPlayerCache& lp, int tickIndex);

} // namespace cf::feat
