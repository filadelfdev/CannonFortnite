#pragma once
#include "core/types.h"
#include <vector>
#include <atomic>

namespace cf::feat {

struct CannonChestCfg {
    std::atomic<bool>  enabled{false};
    std::atomic<bool>  autoFire{true};
    std::atomic<bool>  teleportIntoCannon{true};
    std::atomic<bool>  preferHuntersHaven{true};
    std::atomic<float> maxRange_m{2000.f};     // widened: the planner handles far ones
    std::atomic<float> cooldownSec{0.15f};     // cannon re-fires much faster now
    std::atomic<float> arcGravity{-980.f};
    std::atomic<float> cannonMuzzleSpeed{8800.f};  // ~2x stock; arc clears the whole POI
    std::atomic<int>   chestsPerBurst{6};
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
