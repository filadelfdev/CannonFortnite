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
    std::atomic<bool>  teleportCannon{true};
    std::atomic<bool>  playerRidesCannon{true};
    std::atomic<bool>  preferHuntersHaven{true};
    std::atomic<float> maxRange_m{3000.f};
    // 60ms = ~16 smashes/sec theoretical, 300/min ≥ 100ms budget.
    // Default 0.07s leaves slack for the token drop RPC round-trip.
    std::atomic<float> cooldownSec{0.07f};
    std::atomic<float> hoverHeight_cm{380.f};
    std::atomic<float> smashMuzzleSpeed{2400.f};
    std::atomic<float> aimPitchDeg{-89.f};
    std::atomic<int>   chestsPerSession{0};
    std::atomic<bool>  openOnTouch{true};
    std::atomic<bool>  skipLootedChests{true};
    std::atomic<bool>  ignoreTerrain{true};

    // Burst tuning — needed for 300+ chests/min.
    // Teleport-cannon clamp ceiling: raised from 3100cm (player warp
    // cap) to 50000cm because cannons are treated by the server as
    // vehicles, which have a wider replicated-move envelope.
    std::atomic<float> cannonTeleportCap_cm{50000.f};
    // Skip the horizontal/vertical alignment check — write-fire-move
    // in the same tick. Loses ~2% of shots to mid-warp but doubles
    // throughput at range.
    std::atomic<bool>  skipAlignmentCheck{true};
    // Parallel pre-queue: how many chests ahead to pre-scan.
    std::atomic<int>   lookahead{8};
    // Fire-while-moving: issue the fire RPC-equivalent as soon as the
    // cannon is within 350cm horizontal of the target, don't wait
    // for the full park. Server-side break still registers because
    // the ball is spawned at the cannon's current pos with pitch -89
    // and the chest is directly below within one tick.
    std::atomic<bool>  fireOnApproach{true};
    std::atomic<float> fireRange_cm{400.f};
    // Multi-fire per tick: how many smashes to queue in a single tick
    // when we have multiple chests inside fireRange.
    std::atomic<int>   maxFiresPerTick{3};
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

// Pirate Token smash counter — one smash = one token = 137k xp.
uint32_t SmashedThisSession();
void     ResetSmashCounter();

} // namespace cf::feat
