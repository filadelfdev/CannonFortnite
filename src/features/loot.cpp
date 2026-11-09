#include "features/loot.h"
#include "features/route.h"
#include "sdk/engine.h"
#include "sdk/offsets.h"
#include "core/memory.h"
#include "antiban/clamp.h"
#include "antiban/hash.h"
#include "util/log.h"

#include <chrono>
#include <algorithm>

namespace cf::feat {

LootCfg g_lootCfg{};
MatsCfg g_matsCfg{};

namespace {

double NowSec(){
    using namespace std::chrono;
    return duration_cast<duration<double>>(steady_clock::now().time_since_epoch()).count();
}

bool IsLooted(uintptr_t chest){
    return (cf::mem::R<uint8_t>(chest + cf::off::Chest_bAlreadySearched) & 1) != 0;
}

void OpenChest(uintptr_t chest){
    uint8_t flag = 1;
    cf::mem::Write(chest + cf::off::Chest_bAlreadySearched, &flag, 1);
    float bounce[4] = { 1.f, 0.f, 0.f, 0.f };
    cf::mem::Write(chest + cf::off::Chest_SearchBounceData, bounce, sizeof(bounce));
}

// Pull an actor's root scene component toward the player by at most
// magnetSpeed_cm per call. Clamped so the server reconciliation sees a
// smooth drift, not a snap. Chests + floor loot both use this.
void MagnetToward(const FVector& playerPos, uintptr_t actor, float step){
    uintptr_t root = cf::mem::R<uintptr_t>(actor + cf::off::Pawn_RootComponent);
    if(!root) return;
    FVector cur = cf::mem::R<FVector>(root + cf::off::Scene_RelativeLocation);
    FVector to  = playerPos;
    to.z += 50.f;    // park at waist height so pickups don't fall through
    FVector d = to - cur;
    float   l = d.Len();
    if(l < 60.f) return;    // already close enough
    FVector n = d.Norm();
    FVector next = cur + n * std::min(step, l);
    cf::mem::Write(root + cf::off::Scene_RelativeLocation, &next, sizeof(FVector));
}

// Write the pawn pointer into the pickup's PickupTarget; the game's own
// pickup finisher then fires on its next tick, no server interact RPC.
void InstantPickupActor(uintptr_t pickup, uintptr_t pawn){
    cf::mem::Write(pickup + cf::off::Pickup_PickupTarget, &pawn, sizeof(uintptr_t));
    // Also mark the pickup replicated-net-count as already-grabbed so
    // the floating icon disappears client-side.
    uint8_t one = 1;
    cf::mem::Write(pickup + cf::off::Pickup_bServerStoppedSimulating, &one, 1);
}

} // namespace

void TickLoot(const LocalPlayerCache& lp,
              const std::vector<Actor>& actors,
              float /*dt*/){
    if(!g_lootCfg.enabled.load() || !lp.pawn) return;

    const float r  = g_lootCfg.magnetRadius_m.load() * 100.f;
    const float r2 = r * r;
    const float step = g_lootCfg.magnetSpeed_cm.load();
    const int   cap  = g_lootCfg.maxPullsPerTick.load();

    int pulls = 0;

    if(g_lootCfg.chestMagnet.load()){
        for(auto& a : actors){
            if(!a.isChest) continue;
            if(IsLooted(a.addr)) continue;
            FVector d = a.pos - lp.pos;
            if(d.x*d.x + d.y*d.y + d.z*d.z > r2) continue;
            MagnetToward(lp.pos, a.addr, step);
            // If close enough, auto-open.
            float dist = d.Len();
            if(dist < 220.f){
                static double lastOpen = 0.0;
                double now = NowSec();
                if(cf::ab::clamp::chestOpenRateOk(now, lastOpen)){
                    OpenChest(a.addr);
                    lastOpen = now;
                }
            }
            if(++pulls >= cap) return;
        }
    }

    if(g_lootCfg.pickupMagnet.load()){
        // Floor loot — iterate classified loot drops + any class
        // starting with "FortPickup" that classify() may have flagged
        // as a loot drop.
        for(auto& a : actors){
            if(!a.isLootDrop) continue;
            FVector d = a.pos - lp.pos;
            if(d.x*d.x + d.y*d.y + d.z*d.z > r2) continue;
            if(g_lootCfg.instantPickup.load()){
                InstantPickupActor(a.addr, lp.pawn);
            } else {
                MagnetToward(lp.pos, a.addr, step);
            }
            if(++pulls >= cap) return;
        }
    }
}

void TickMats(const LocalPlayerCache& lp){
    if(!g_matsCfg.enabled.load() || !lp.pawn) return;
    uintptr_t inv = cf::mem::R<uintptr_t>(lp.pawn + cf::off::Pawn_InventoryComponent);
    if(!inv) return;

    struct TArr { uintptr_t data; int32_t num; int32_t max; };
    TArr entries = cf::mem::R<TArr>(inv + cf::off::Inv_ReplicatedEntries);
    if(entries.num <= 0 || entries.num > 128) return;

    constexpr uint32_t stride = 0x50;   // FFortItemEntry stride on current UE5 build
    const int target = g_matsCfg.targetStack.load();
    const bool topUp = g_matsCfg.topUpOnly.load();

    for(int i=0;i<entries.num;++i){
        uintptr_t entry = entries.data + (uintptr_t)i * stride;
        uintptr_t def   = cf::mem::R<uintptr_t>(entry + cf::off::ItemEntry_ItemDefinition);
        if(!def) continue;
        uint32_t defName = cf::mem::R<uint32_t>(def + cf::off::UObject_NamePrivate);
        std::string name = cf::sdk::FNameToString(defName);
        uint32_t h = cf::ab::fnv1a_lower(name.c_str());
        bool isMat = (h == cf::off::Material_Wood_ItemDef_FnameHash  ||
                      h == cf::off::Material_Brick_ItemDef_FnameHash ||
                      h == cf::off::Material_Metal_ItemDef_FnameHash);
        if(!isMat) continue;
        int cur = cf::mem::R<int>(entry + cf::off::ItemEntry_Count);
        if(topUp && cur >= target) continue;
        cf::mem::Write(entry + cf::off::ItemEntry_Count, &target, sizeof(int));
    }
}

} // namespace cf::feat
