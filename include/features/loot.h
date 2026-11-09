#pragma once
#include "core/types.h"
#include <vector>
#include <atomic>

namespace cf::feat {

struct LootCfg {
    std::atomic<bool>  enabled{true};
    std::atomic<bool>  chestMagnet{true};        // pull chests TO the player
    std::atomic<bool>  pickupMagnet{true};       // pull floor loot to the player
    std::atomic<bool>  wallIgnore{true};         // no LOS check
    std::atomic<float> magnetRadius_m{80.f};     // pull radius
    std::atomic<float> magnetSpeed_cm{450.f};    // per-tick step per item
    std::atomic<bool>  instantPickup{true};      // write PickupTarget directly
    std::atomic<int>   maxPullsPerTick{6};
    std::atomic<bool>  autoLootDroppedWeapons{true};
    std::atomic<bool>  prioritiseMythic{true};
};
extern LootCfg g_lootCfg;

void TickLoot(const LocalPlayerCache& lp, const std::vector<Actor>& actors, float dt);

// Mats flood — writes full stacks to the inventory wood/brick/metal slots.
struct MatsCfg {
    std::atomic<bool> enabled{false};
    std::atomic<int>  targetStack{999};
    std::atomic<bool> topUpOnly{true};        // never decrease a higher real count
};
extern MatsCfg g_matsCfg;

void TickMats(const LocalPlayerCache& lp);

} // namespace cf::feat
