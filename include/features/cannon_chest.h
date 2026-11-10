#pragma once
#include "core/types.h"
#include <vector>
#include <atomic>

namespace cf::feat {

// Pirate Shop Token farm — real mechanic (Nov 2026, Treasure Hunters).
//
// The CORRECT model, verified against the reference recording:
//   1. Player enters a Pirate Cannon (vehicle).
//   2. Cannon is teleported next to a chest cluster (Busy Bay / Cursed
//      Cavern POIs are dense).
//   3. For EVERY chest inside interact radius, we invoke
//      AFortBuildingContainer::ServerInteract(this=chest, pawn=player).
//      Not fire. Not damage. Interact — the normal open path.
//   4. N parallel ServerInteract calls per tick → N chests open at the
//      same time. "chests_opened" counter ticks for each confirmed open.
//   5. Pirate Shop Token odds roll server-side per chest (1 in 135k).
//
// No cannon fire is ever invoked — the cannon is just the vehicle the
// player happens to be in; the server-side interact RPC is what opens
// chests. Riding the cannon lets the player hop fast between clusters.
struct CannonChestCfg {
    std::atomic<bool>  enabled{false};
    std::atomic<bool>  autoFire{true};
    std::atomic<bool>  teleportCannon{true};
    std::atomic<bool>  playerRidesCannon{true};
    std::atomic<bool>  preferHuntersHaven{true};
    // v6 — bulk-interact model (no fire).
    std::atomic<float> maxRange_m{2500.f};
    std::atomic<float> cooldownMeanMs{190.f};       // per interact gate
    std::atomic<float> cooldownSpreadMs{70.f};      // legacy, unused
    std::atomic<float> hoverHeight_cm{380.f};
    std::atomic<float> hoverJitter_cm{40.f};
    std::atomic<int>   chestsPerSession{0};
    std::atomic<float> serverConfirmGraceMs{180.f};
    std::atomic<bool>  skipLootedChests{true};

    // Interact-radius — distance from the cannon within which every
    // unlooted chest gets a ServerInteract call this tick.
    std::atomic<float> interactRadius_cm{900.f};
    // Max parallel interacts per tick (the "multiple chests at once"
    // look in the reference recording). A real player interacts
    // sequentially; parallel is one of the obvious detection surfaces.
    std::atomic<int>   maxInteractsPerTick{4};

    // Cannon placement — we still teleport the cannon between clusters
    // (fast hop), but never fire it.
    std::atomic<bool>  teleportCannon{true};
    std::atomic<bool>  playerRidesCannon{true};
    std::atomic<float> cannonTeleportCap_cm{12000.f};

    std::atomic<bool>  preferRpc{true};

    // Master speed preset — 1 (safest, ~1/s) to 10 (unsafe, ~10/s).
    std::atomic<int>   speedPreset{7};
};

// Rewrites cooldownMeanMs / maxFiresPerTick / fireRange / warp cap
// to the preset curve. Called when the user moves the Speed slider.
void ApplySpeedPreset(int level_1_to_10);

extern CannonChestCfg g_cfg;

// Called every overlay tick. Pulls the local player + actor list, picks the
// nearest live chest, snaps the nearest cannon's pitch/yaw at it, writes
// launch power, trips the fire RPC-equivalent proxy, and on landing
// writes the chest's SearchBounceData to simulate a search.
void Tick(const LocalPlayerCache& lp,
          const std::vector<Actor>& actors,
          float dt);

struct ChestVisual {
    Actor actor;
    float screenX{}, screenY{};
    bool  reachable{};
    bool  queuedAsTarget{};
};
std::vector<ChestVisual> GetVisualState();

// Pirate Token smash counter — one smash = one token = 137k xp.
uint32_t SmashedThisSession();
void     ResetSmashCounter();

} // namespace cf::feat
