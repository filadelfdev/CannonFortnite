#pragma once
#include "core/cf_api.h"
#include "core/types.h"
#include <atomic>
#include <vector>

namespace cf::feat {

// Route planner for the Pirate Token farm.
//   CannonSmash  — DEFAULT. Teleport the cannon above each chest in
//                  turn and smash straight down. Each smash credits
//                  one cannon-damage break, which is what drops the
//                  Pirate Token. This is the mechanic from the ref
//                  video; do not swap it out.
//   TeleportChain— player-teleport loot run. Opens chests but does NOT
//                  award the token (chest search path, not cannon
//                  damage path). Use only for gear.
//   Bulk         — same as TeleportChain but wider sphere; still no
//                  token. Debug mode.
enum class RouteMode { CannonSmash, TeleportChain, Bulk };

struct RouteCfg {
    std::atomic<RouteMode> mode{RouteMode::CannonSmash};   // Pirate Token mechanic
    std::atomic<float> hopDelta_cm{3100.f};
    std::atomic<float> hopCadence_ms{40.f};
    std::atomic<float> chainRange_m{2500.f};
    std::atomic<bool>  bulkOpenOnArrive{true};
    std::atomic<float> bulkRadius_cm{900.f};
    std::atomic<int>   maxOpensPerTick{4};
    std::atomic<bool>  rotateToTarget{true};
    std::atomic<bool>  preferUnopenedTier{true};
};
CF_FEAT_API extern RouteCfg g_routeCfg;

// Plan: greedy nearest-neighbor over chests; returns actor addresses in
// visit order. Cheap enough to run every ~6 ticks.
CF_FEAT_API void PlanRoute(const LocalPlayerCache& lp,
                           const std::vector<Actor>& actors,
                           std::vector<uintptr_t>& outOrder);

// One tick of the active route — advances the player, bulk-opens, picks
// the next hop. Called from the main pipeline when mode != CannonArc.
CF_FEAT_API void TickRoute(const LocalPlayerCache& lp,
                           const std::vector<Actor>& actors,
                           float dt);

// Count of chests the current session has opened (for the HUD).
CF_FEAT_API uint32_t Looted();
CF_FEAT_API void     ResetLooted();

} // namespace cf::feat
