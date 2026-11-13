#pragma once
#include "core/cf_api.h"
#include "core/types.h"
#include <vector>
#include <atomic>

namespace cf::feat {

enum class AimBone { Head, Neck, Chest, Pelvis };

struct AimCfg {
    std::atomic<bool>  enabled{false};
    std::atomic<bool>  silent{true};         // bullet-magnet; no visible crosshair swing
    std::atomic<bool>  visibleOnly{false};   // false = ignore LOS (hit-through-walls)
    std::atomic<bool>  teamCheck{true};
    std::atomic<float> fov_deg{14.f};        // selection cone around crosshair
    std::atomic<float> maxRange_m{280.f};
    std::atomic<float> smooth{6.5f};         // only used when !silent; 1=snap, 20=glacial
    std::atomic<bool>  predictVelocity{true};
    std::atomic<float> projectileSpeed{36000.f}; // for lead; AR ~2500 m/s → 250000 cm/s (hitscan ignores)
    std::atomic<int>   bone{(int)AimBone::Head};
    std::atomic<bool>  autoFireOnLock{false};
    std::atomic<bool>  noRecoil{true};
    std::atomic<bool>  noSpread{true};
};
CF_FEAT_API extern AimCfg g_aimCfg;

CF_FEAT_API void TickAim(const LocalPlayerCache& lp,
                         const std::vector<Actor>& actors, float dt);

} // namespace cf::feat
