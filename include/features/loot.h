#pragma once
#include "core/cf_api.h"
#include "core/types.h"
#include <vector>
#include <atomic>

namespace cf::feat {

struct LootCfg {
    std::atomic<bool>  enabled{true};
    // Chest magnet DISABLED by default — moving chests breaks the
    // cannon-smash alignment and prevents the Pirate Token drop.
    // Only enable this when running TeleportChain (non-token mode).
    std::atomic<bool>  chestMagnet{false};
    std::atomic<bool>  pickupMagnet{true};        // post-smash floor loot sweep
    std::atomic<bool>  wallIgnore{true};
    std::atomic<float> magnetRadius_m{80.f};
    std::atomic<float> magnetSpeed_cm{450.f};
    std::atomic<bool>  instantPickup{true};
    std::atomic<int>   maxPullsPerTick{6};
    std::atomic<bool>  autoLootDroppedWeapons{true};
    std::atomic<bool>  prioritiseMythic{true};
};
CF_FEAT_API extern LootCfg g_lootCfg;

CF_FEAT_API void TickLoot(const LocalPlayerCache& lp,
                          const std::vector<Actor>& actors, float dt);

// Mats flood — writes full stacks to the inventory wood/brick/metal slots.
struct MatsCfg {
    std::atomic<bool> enabled{false};
    std::atomic<int>  targetStack{999};
    std::atomic<bool> topUpOnly{true};        // never decrease a higher real count
};
CF_FEAT_API extern MatsCfg g_matsCfg;

CF_FEAT_API void TickMats(const LocalPlayerCache& lp);

} // namespace cf::feat
