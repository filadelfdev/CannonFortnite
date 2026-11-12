#include "features/loot.h"
#include "features/route.h"
#include "sdk/engine.h"
#include "sdk/offsets.h"
#include "core/process.h"
#include "antiban/clamp.h"
#include "antiban/hash.h"
#include "util/log.h"
#include "util/report.h"

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
    return (cf::proc::R<uint8_t>(chest + cf::off::Chest_bAlreadySearched) & 1) != 0;
}

void OpenChest(uintptr_t chest){
    uint8_t flag = 1;
    cf::proc::Write(chest + cf::off::Chest_bAlreadySearched, &flag, 1);
    float bounce[4] = { 1.f, 0.f, 0.f, 0.f };
    cf::proc::Write(chest + cf::off::Chest_SearchBounceData, bounce, sizeof(bounce));
}

// Pull an actor's root scene component toward the player by at most
// magnetSpeed_cm per call. Clamped so the server reconciliation sees a
// smooth drift, not a snap. Chests + floor loot both use this.
void MagnetToward(const FVector& playerPos, uintptr_t actor, float step){
    uintptr_t root = cf::proc::R<uintptr_t>(actor + cf::off::Pawn_RootComponent);
    if(!root) return;
    FVector cur = cf::proc::R<FVector>(root + cf::off::Scene_RelativeLocation);
    FVector to  = playerPos;
    to.z += 50.f;    // park at waist height so pickups don't fall through
    FVector d = to - cur;
    float   l = d.Len();
    if(l < 60.f) return;    // already close enough
    FVector n = d.Norm();
    FVector next = cur + n * std::min(step, l);
    cf::proc::Write(root + cf::off::Scene_RelativeLocation, &next, sizeof(FVector));
}

// Write the pawn pointer into the pickup's PickupTarget; the game's own
// pickup finisher then fires on its next tick, no server interact RPC.
void InstantPickupActor(uintptr_t pickup, uintptr_t pawn){
    cf::proc::Write(pickup + cf::off::Pickup_PickupTarget, &pawn, sizeof(uintptr_t));
    uint8_t one = 1;
    cf::proc::Write(pickup + cf::off::Pickup_bServerStoppedSimulating, &one, 1);
    cf::report::RecordEvent(cf::report::EventKind::PickupGrab);
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

    static double lastPulse = 0.0;
    double now = NowSec();
    if((now - lastPulse) < 0.9) return;
    lastPulse = now;
    cf::report::RecordEvent(cf::report::EventKind::MatsFlood);

    uintptr_t inv = cf::proc::R<uintptr_t>(lp.pawn + cf::off::Pawn_InventoryComponent);
    if(!inv) return;

    struct TArr { uintptr_t data; int32_t num; int32_t max; };
    TArr entries = cf::proc::R<TArr>(inv + cf::off::Inv_ReplicatedEntries);
    if(entries.num <= 0 || entries.num > 128) return;

    // FFortItemEntry stride — real size on current UE5 shipping build.
    // Measured 0x58 (header: NetSerializeItem replicated struct with
    // ItemDefinition* + Count + LoadedAmmo + Guid + reserved slack).
    constexpr uint32_t stride = 0x58;

    // Hashes computed once — fnv1a_lower returns uint64_t, the uint32_t
    // constants in offsets.h always mismatched the width.
    static const uint64_t H_Wood  = cf::ab::fnv1a_lower("BuildingItemData_Wood");
    static const uint64_t H_Brick = cf::ab::fnv1a_lower("BuildingItemData_Stone");
    static const uint64_t H_Metal = cf::ab::fnv1a_lower("BuildingItemData_Metal");

    const int target = g_matsCfg.targetStack.load();
    const bool topUp = g_matsCfg.topUpOnly.load();

    for(int i=0;i<entries.num;++i){
        uintptr_t entry = entries.data + (uintptr_t)i * stride;
        uintptr_t def   = cf::proc::R<uintptr_t>(entry + cf::off::ItemEntry_ItemDefinition);
        if(!def) continue;
        uint32_t defName = cf::proc::R<uint32_t>(def + cf::off::UObject_NamePrivate);
        std::string name = cf::sdk::FNameToString(defName);
        uint64_t h = cf::ab::fnv1a_lower(name.c_str());
        bool isMat = (h == H_Wood || h == H_Brick || h == H_Metal);
        if(!isMat) continue;
        int cur = cf::proc::R<int>(entry + cf::off::ItemEntry_Count);
        if(topUp && cur >= target) continue;
        cf::proc::Write(entry + cf::off::ItemEntry_Count, &target, sizeof(int));
    }
}

} // namespace cf::feat
