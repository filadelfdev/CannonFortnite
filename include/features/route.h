#pragma once
#include "core/types.h"
#include <atomic>
#include <vector>

namespace cf::feat {

// Route planner for the chest-collector. Two modes:
//   CannonArc   — legacy: ride cannon, arc one chest at a time.
//   TeleportChain — build a greedy TSP over every reachable chest in
//                   range, teleport along the chain, bulk-open at each
//                   stop. This is the fast path; collects dozens of
//                   chests in the time the cannon opens three.
enum class RouteMode { CannonArc, TeleportChain, Bulk };

struct RouteCfg {
    std::atomic<RouteMode> mode{RouteMode::TeleportChain};
    std::atomic<float> hopDelta_cm{3100.f};      // per-tick teleport cap (under warp)
    std::atomic<float> hopCadence_ms{40.f};      // ms between hops
    std::atomic<float> chainRange_m{1800.f};     // route planning horizon
    std::atomic<bool>  bulkOpenOnArrive{true};   // open every chest in 500cm sphere on hop land
    std::atomic<float> bulkRadius_cm{900.f};     // sphere radius
    std::atomic<int>   maxOpensPerTick{4};       // clamp.cpp also gates this; soft cap here
    std::atomic<bool>  rotateToTarget{true};
    std::atomic<bool>  preferUnopenedTier{true}; // weight rare chests higher
};
extern RouteCfg g_routeCfg;

// Plan: greedy nearest-neighbor over chests; returns actor addresses in
// visit order. Cheap enough to run every ~6 ticks.
void PlanRoute(const LocalPlayerCache& lp,
               const std::vector<Actor>& actors,
               std::vector<uintptr_t>& outOrder);

// One tick of the active route — advances the player, bulk-opens, picks
// the next hop. Called from the main pipeline when mode != CannonArc.
void TickRoute(const LocalPlayerCache& lp,
               const std::vector<Actor>& actors,
               float dt);

// Count of chests the current session has opened (for the HUD).
uint32_t Looted();
void     ResetLooted();

} // namespace cf::feat
