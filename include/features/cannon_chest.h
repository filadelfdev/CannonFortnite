#pragma once
#include "core/types.h"
#include <vector>
#include <atomic>

namespace cf::feat {

struct CannonChestCfg {
    std::atomic<bool>  enabled{false};
    std::atomic<bool>  autoFire{true};         // fires when the arc clears
    std::atomic<bool>  teleportIntoCannon{true};
    std::atomic<bool>  preferHuntersHaven{true};
    std::atomic<float> maxRange_m{1200.f};     // skip chests beyond this
    std::atomic<float> cooldownSec{0.75f};     // per-fire cooldown
    std::atomic<float> arcGravity{-980.f};     // UE cm/s^2 * scale
    std::atomic<float> cannonMuzzleSpeed{4200.f};
    std::atomic<int>   chestsPerBurst{3};      // ride cannon → open → repeat
    std::atomic<bool>  openOnTouch{true};
    std::atomic<bool>  skipLootedChests{true};
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
