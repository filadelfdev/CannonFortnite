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
    // v4 defaults — tuned for sustainable rate, not peak throughput.
    // ~2 smashes/sec with ±30% jitter = ~110-130/min sustained.
    // Still ~15-18M xp/min per the token math, well above a human farm.
    std::atomic<float> maxRange_m{2000.f};
    std::atomic<float> cooldownMeanMs{480.f};   // 480ms ± 180ms
    std::atomic<float> cooldownSpreadMs{180.f};
    std::atomic<float> hoverHeight_cm{380.f};
    std::atomic<float> hoverJitter_cm{40.f};
    std::atomic<float> smashMuzzleSpeed{2400.f};
    std::atomic<float> aimPitchDeg{-89.f};
    std::atomic<float> aimJitterDeg{1.5f};      // ±1.5° on each aim write
    std::atomic<int>   chestsPerSession{0};
    std::atomic<bool>  openOnTouch{false};      // v3 flagged client-side; v4 waits for server confirm
    std::atomic<float> serverConfirmGraceMs{220.f}; // delay before marking
    std::atomic<bool>  skipLootedChests{true};

    // Vehicle warp cap — server-side envelope is ~8000cm/tick for
    // cannons. v3 used 50000, which is what got the account burned.
    std::atomic<float> cannonTeleportCap_cm{7500.f};
    std::atomic<bool>  skipAlignmentCheck{false};   // v4 waits for align
    std::atomic<int>   lookahead{4};
    std::atomic<bool>  fireOnApproach{false};       // single, confirmed fire
    std::atomic<float> fireRange_cm{250.f};
    std::atomic<int>   maxFiresPerTick{1};          // one smash per tick

    // v4 mandatory: prefer the RPC call path over direct writes. Falls
    // through to writes only if rpc::Ready() is false.
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
