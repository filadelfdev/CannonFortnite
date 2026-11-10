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
    // v5.1 — user-requested 5/s target. Antidetect layers (RPC path,
    // env_lie, dxgi cloak, lognormal jitter) stay active. This is the
    // greedy end of the safe envelope; expect shorter session survival
    // than v4 defaults. Burn-and-rotate HWID via the Security panel if
    // a session ends in a kick.
    std::atomic<float> maxRange_m{2500.f};
    std::atomic<float> cooldownMeanMs{190.f};       // lognormal mode
    std::atomic<float> cooldownSpreadMs{70.f};      // legacy field (unused with lognormal)
    std::atomic<float> hoverHeight_cm{380.f};
    std::atomic<float> hoverJitter_cm{40.f};
    std::atomic<float> smashMuzzleSpeed{2400.f};
    std::atomic<float> aimPitchDeg{-89.f};
    std::atomic<float> aimJitterDeg{1.5f};
    std::atomic<int>   chestsPerSession{0};
    std::atomic<bool>  openOnTouch{false};           // wait for server confirm
    std::atomic<float> serverConfirmGraceMs{180.f};  // tighter for 5/s cadence
    std::atomic<bool>  skipLootedChests{true};

    // Vehicle warp cap — 12000cm sits between v4 safe (7500) and v3
    // greed (50000). Server-side vehicle envelope tolerates 10-15k
    // per tick if not three consecutive maxes; jitter keeps us off that.
    std::atomic<float> cannonTeleportCap_cm{12000.f};
    std::atomic<bool>  skipAlignmentCheck{true};     // write-fire same tick
    std::atomic<int>   lookahead{10};
    std::atomic<bool>  fireOnApproach{true};
    std::atomic<float> fireRange_cm{360.f};
    std::atomic<int>   maxFiresPerTick{3};

    std::atomic<bool>  preferRpc{true};
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
