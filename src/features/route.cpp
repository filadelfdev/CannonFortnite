#include "features/route.h"
#include "features/cannon_chest.h"
#include "sdk/offsets.h"
#include "core/process.h"
#include "antiban/clamp.h"
#include "util/log.h"
#include "util/report.h"

#include <chrono>
#include <algorithm>
#include <atomic>
#include <mutex>

namespace cf::feat {

RouteCfg g_routeCfg{};

namespace {

std::vector<uintptr_t> g_order;
size_t                 g_cursor = 0;
double                 g_lastHop = 0.0;
std::atomic<uint32_t>  g_looted{0};
std::mutex             g_lock;

double NowSec(){
    using namespace std::chrono;
    return duration_cast<duration<double>>(steady_clock::now().time_since_epoch()).count();
}

bool IsLooted(uintptr_t chest){
    return (cf::proc::R<uint8_t>(chest + cf::off::Chest_bAlreadySearched) & 1) != 0;
}

uint32_t ChestWeight(const Actor& a){
    // Weight the plan: closer first, with a rare-chest bonus so the
    // route prioritises tier > proximity when tier is high.
    uint32_t tier = cf::proc::R<uint32_t>(a.addr + cf::off::Chest_LootTier);
    uint32_t w = (uint32_t)a.distance;
    if(g_routeCfg.preferUnopenedTier.load() && tier >= 2) w = w / 2;
    return w;
}

void OpenChestRaw(uintptr_t chest){
    uint8_t flag = 1;
    // Guard: only count the open if the write actually landed. A failed
    // write (unmapped page, process exit) must not credit the counter.
    if(!cf::proc::Write(chest + cf::off::Chest_bAlreadySearched, &flag, 1)) return;
    float bounce[4] = { 1.f, 0.f, 0.f, 0.f };
    cf::proc::Write(chest + cf::off::Chest_SearchBounceData, bounce, sizeof(bounce));
    g_looted.fetch_add(1);
    cf::report::RecordEvent(cf::report::EventKind::RouteOpen);
}

} // namespace

uint32_t Looted(){ return g_looted.load(); }
void     ResetLooted(){ g_looted.store(0); }

void PlanRoute(const LocalPlayerCache& lp,
               const std::vector<Actor>& actors,
               std::vector<uintptr_t>& outOrder){
    outOrder.clear();
    float horizonCm = g_routeCfg.chainRange_m.load() * 100.f;

    // Collect reachable, unlooted chests.
    struct Node { uintptr_t addr; FVector pos; uint32_t weight; };
    std::vector<Node> pool;
    pool.reserve(actors.size());
    for(auto& a : actors){
        // Vault/Sprite chests never go in the planner — they don't
        // break and queueing them wastes hops.
        if(a.isVaultChest) continue;
        if(!(a.isChest || a.isLootDrop)) continue;
        float d = (a.pos - lp.pos).Len();
        if(d > horizonCm) continue;
        if(g_cfg.skipLootedChests.load() && IsLooted(a.addr)) continue;
        pool.push_back({a.addr, a.pos, ChestWeight(a)});
    }
    if(pool.empty()) return;

    // Greedy nearest-neighbour from the player.
    FVector cursor = lp.pos;
    std::vector<bool> used(pool.size(), false);
    outOrder.reserve(pool.size());
    for(size_t step=0; step<pool.size(); ++step){
        int bestIdx = -1;
        float bestD = 1e18f;
        for(size_t i=0;i<pool.size();++i){
            if(used[i]) continue;
            float d = (pool[i].pos - cursor).Len();
            // Weight scales up small distances for rare chests, so close
            // common beats far rare but far rare still beats far common.
            float score = d + float(pool[i].weight);
            if(score < bestD){ bestD = score; bestIdx = (int)i; }
        }
        if(bestIdx < 0) break;
        used[bestIdx] = true;
        outOrder.push_back(pool[bestIdx].addr);
        cursor = pool[bestIdx].pos;
    }
}

void TickRoute(const LocalPlayerCache& lp,
               const std::vector<Actor>& actors,
               float /*dt*/){
    if(!g_cfg.enabled.load()) return;
    // CannonSmash mode is handled by cannon_chest::Tick, not here. The
    // route planner only runs for TeleportChain + Bulk (gear-only paths).
    if(g_routeCfg.mode.load() == RouteMode::CannonSmash) return;
    if(!lp.pawn || !lp.rootComp) return;

    double now = NowSec();
    float  cadence = g_routeCfg.hopCadence_ms.load() / 1000.f;
    if((now - g_lastHop) < cadence) return;

    // Re-plan whenever the cursor runs off the end or the current target
    // disappears from the actor set.
    bool needPlan = false;
    {
        std::scoped_lock _(g_lock);
        if(g_cursor >= g_order.size()) needPlan = true;
        else {
            uintptr_t cur = g_order[g_cursor];
            bool stillThere = false;
            for(auto& a : actors) if(a.addr == cur){ stillThere = true; break; }
            if(!stillThere) needPlan = true;
        }
    }
    if(needPlan){
        std::scoped_lock _(g_lock);
        PlanRoute(lp, actors, g_order);
        g_cursor = 0;
        if(g_order.empty()) return;
    }

    uintptr_t target;
    { std::scoped_lock _(g_lock); target = g_order[g_cursor]; }

    uintptr_t root = cf::proc::R<uintptr_t>(target + cf::off::Pawn_RootComponent);
    if(!root){ std::scoped_lock _(g_lock); ++g_cursor; return; }

    FVector targetPos = cf::proc::R<FVector>(root + cf::off::Scene_RelativeLocation);

    // Hop: clamp the delta so a single warp never trips the anti-teleport
    // heuristic. If the distance is greater than the hop cap, we close
    // the gap over several ticks.
    FVector clamped = cf::ab::clamp::teleport(lp.pos, targetPos,
                                              g_routeCfg.hopDelta_cm.load());
    cf::proc::Write(lp.rootComp + cf::off::Scene_RelativeLocation, &clamped, sizeof(FVector));
    g_lastHop = now;

    // Face the target so the client's own anim doesn't desync the move.
    if(g_routeCfg.rotateToTarget.load()){
        FVector d = targetPos - lp.pos;
        FRotator rot{0.f, std::atan2(d.y, d.x) * 57.29578f, 0.f};
        cf::proc::Write(lp.rootComp + cf::off::Scene_RelativeRotation, &rot, sizeof(rot));
    }

    // Arrived? Bulk-open every chest in the sphere around our new pos.
    float arrived = (clamped - targetPos).Len();
    if(arrived < 160.f){
        if(g_routeCfg.bulkOpenOnArrive.load()){
            float r2 = g_routeCfg.bulkRadius_cm.load();
            r2 = r2 * r2;
            int opened = 0;
            int cap    = g_routeCfg.maxOpensPerTick.load();
            for(auto& a : actors){
                if(a.isVaultChest) continue;
                if(!(a.isChest || a.isLootDrop)) continue;
                float d2 = (a.pos - clamped).Len();
                if(d2*d2 > r2) continue;
                if(IsLooted(a.addr)) continue;
                if(!cf::ab::clamp::chestOpenRateOk(NowSec(), g_lastHop)) break;
                OpenChestRaw(a.addr);
                g_lastHop = NowSec();
                if(++opened >= cap) break;
            }
        } else {
            OpenChestRaw(target);
        }
        std::scoped_lock _(g_lock);
        ++g_cursor;
    }
}

} // namespace cf::feat
