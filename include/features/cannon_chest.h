#pragma once
#include "core/types.h"
#include <vector>
#include <atomic>

namespace cf::feat {

// Pirate Token farm — the real mechanic. Spawn/take the nearest cannon,
// teleport the CANNON itself above each chest, aim straight down, fire.
// The cannonball smashes the chest, server registers it as a chest break
// via cannon damage, Pirate Token drops. Repeat for every chest in range.
struct CannonChestCfg {
    std::atomic<bool>  enabled{false};
    std::atomic<bool>  autoFire{true};
    std::atomic<bool>  teleportCannon{true};      // move the CANNON, not the player
    std::atomic<bool>  playerRidesCannon{true};   // keep local pawn parented so seat is held
    std::atomic<bool>  preferHuntersHaven{true};
    std::atomic<float> maxRange_m{2500.f};
    std::atomic<float> cooldownSec{0.20f};        // per-smash
    std::atomic<float> hoverHeight_cm{450.f};     // how high above the chest to park
    std::atomic<float> smashMuzzleSpeed{2400.f};  // low muzzle, straight-down shot
    std::atomic<float> aimPitchDeg{-89.f};        // nearly vertical down
    std::atomic<int>   chestsPerSession{0};       // 0 = unlimited
    std::atomic<bool>  openOnTouch{true};         // flag searched after the smash lands
    std::atomic<bool>  skipLootedChests{true};
    std::atomic<bool>  ignoreTerrain{true};       // do NOT re-solve arc, pure drop
};

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

} // namespace cf::feat
