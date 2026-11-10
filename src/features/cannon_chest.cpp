#include "features/cannon_chest.h"
#include "sdk/engine.h"
#include "sdk/offsets.h"
#include "core/memory.h"
#include "antiban/clamp.h"
#include "antiban/jitter.h"
#include "antiban/rpc_path.h"
#include "util/log.h"
#include "util/report.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <mutex>
#include <atomic>
#include <deque>

namespace cf::feat {

CannonChestCfg g_cfg{};

namespace {

struct PendingConfirm {
    uintptr_t chest;
    double    fireTime;
};

struct Pipeline {
    uintptr_t activeCannon{};
    double    lastInteractTime{};
    std::atomic<uint32_t> smashed{0};
    std::vector<ChestVisual> visualSnap;
    std::vector<uintptr_t>   prequeue;
    std::deque<PendingConfirm> pending;
    std::mutex visLock;
} p;

double NowSec(){
    using namespace std::chrono;
    return duration_cast<duration<double>>(steady_clock::now().time_since_epoch()).count();
}

bool IsChestLooted(uintptr_t chest){
    if(!chest) return true;
    uint8_t v = cf::mem::R<uint8_t>(chest + cf::off::Chest_bAlreadySearched);
    return (v & 1) != 0;
}

void FlagChestSearched(uintptr_t chest){
    if(!chest) return;
    uint8_t flag = 1;
    cf::mem::Write(chest + cf::off::Chest_bAlreadySearched, &flag, 1);
    float bounce[4] = { 1.f, 0.f, 0.f, 0.f };
    cf::mem::Write(chest + cf::off::Chest_SearchBounceData, bounce, sizeof(bounce));
}

// Move the cannon toward a cluster of chests. Uses vehicle envelope.
void WarpCannonToCluster(const LocalPlayerCache& lp,
                         uintptr_t cannon,
                         const FVector& clusterCenter){
    uintptr_t cannonRoot = cf::mem::R<uintptr_t>(cannon + cf::off::Pawn_RootComponent);
    if(!cannonRoot) return;

    FVector cur = cf::mem::R<FVector>(cannonRoot + cf::off::Scene_RelativeLocation);
    FVector target = clusterCenter;
    target.z += g_cfg.hoverHeight_cm.load()
              + cf::ab::jitter::wobble(0.f, g_cfg.hoverJitter_cm.load());

    FVector next = cf::ab::clamp::teleport(cur, target,
                                           g_cfg.cannonTeleportCap_cm.load());
    cf::mem::Write(cannonRoot + cf::off::Scene_RelativeLocation, &next, sizeof(FVector));

    FVector zero{};
    cf::mem::Write(cannonRoot + cf::off::Scene_ComponentVelocity, &zero, sizeof(FVector));

    if(g_cfg.playerRidesCannon.load() && lp.rootComp){
        FVector seat = next;
        seat.z += 80.f;
        cf::mem::Write(lp.rootComp + cf::off::Scene_RelativeLocation, &seat, sizeof(FVector));
        cf::mem::Write(lp.rootComp + cf::off::Scene_ComponentVelocity, &zero, sizeof(FVector));
    }
}

// Invoke ServerInteract on one chest via RPC path. Falls through to
// direct write of the search flag if RPC isn't resolved.
void InteractChest(uintptr_t chest, uintptr_t pawn){
    if(g_cfg.preferRpc.load() && cf::ab::rpc::Ready()){
        cf::ab::rpc::ChestServerInteract(chest, pawn);
    } else {
        FlagChestSearched(chest);
    }
}

uintptr_t PickNearestCannon(const LocalPlayerCache& lp,
                            const std::vector<Actor>& actors,
                            float maxRange_m){
    uintptr_t best = 0;
    float     bestD = maxRange_m * 100.f;
    for(auto& a : actors){
        if(!a.isCannon) continue;
        float d = (a.pos - lp.pos).Len();
        if(d < bestD){ bestD = d; best = a.addr; }
    }
    return best;
}

// Find the chest cluster with the most unlooted chests inside
// interactRadius_cm. Returns the geometric center + list of chest
// addrs for this tick's bulk interact.
FVector PickBestCluster(const std::vector<Actor>& actors,
                        float searchRadius_cm,
                        std::vector<uintptr_t>& outChests){
    outChests.clear();
    const float r2 = g_cfg.interactRadius_cm.load() * g_cfg.interactRadius_cm.load();

    // Candidate centers: every unlooted chest's own position.
    int bestCount = 0;
    FVector bestCenter{};
    std::vector<uintptr_t> bestList;
    bestList.reserve(16);

    for(auto& candidate : actors){
        if(candidate.isVaultChest) continue;
        if(!candidate.isChest) continue;
        if(g_cfg.skipLootedChests.load() && IsChestLooted(candidate.addr)) continue;

        // Count other unlooted chests within interactRadius of this one.
        int count = 0;
        std::vector<uintptr_t> here;
        here.reserve(16);
        for(auto& other : actors){
            if(other.isVaultChest) continue;
            if(!other.isChest) continue;
            if(g_cfg.skipLootedChests.load() && IsChestLooted(other.addr)) continue;
            FVector d = other.pos - candidate.pos;
            if(d.x*d.x + d.y*d.y + d.z*d.z <= r2){
                here.push_back(other.addr);
                count++;
            }
        }

        if(count > bestCount){
            bestCount  = count;
            bestCenter = candidate.pos;
            bestList   = std::move(here);
        }

        (void)searchRadius_cm;
    }

    outChests = std::move(bestList);
    return bestCenter;
}

void ProcessPending(){
    double now = NowSec();
    float  graceSec = g_cfg.serverConfirmGraceMs.load() * 0.001f;
    while(!p.pending.empty()){
        auto& front = p.pending.front();
        if(IsChestLooted(front.chest)){
            p.smashed.fetch_add(1);
            cf::report::RecordEvent(cf::report::EventKind::CannonSmash);
            p.pending.pop_front();
            continue;
        }
        if((now - front.fireTime) > graceSec){
            p.pending.pop_front();
            continue;
        }
        break;
    }
}

} // namespace

std::vector<ChestVisual> GetVisualState(){
    std::scoped_lock _(p.visLock);
    return p.visualSnap;
}

uint32_t SmashedThisSession(){ return p.smashed.load(); }
void     ResetSmashCounter(){ p.smashed.store(0); }

void ApplySpeedPreset(int level){
    if(level < 1) level = 1;
    if(level > 10) level = 10;

    struct Preset {
        float cooldownMs;   int perTick;   float warpCap;   float interactRadius;
    };
    static const Preset table[10] = {
        //  cooldown   per-tick   warp cap     interact radius
        { 900.f,       1,         4000.f,      500.f   },  // 1 — safest, ~1/s
        { 650.f,       1,         6000.f,      600.f   },  // 2
        { 480.f,       2,         7500.f,      700.f   },  // 3
        { 360.f,       2,         9000.f,      800.f   },  // 4
        { 280.f,       3,        10500.f,      850.f   },  // 5
        { 230.f,       3,        11500.f,      900.f   },  // 6
        { 190.f,       4,        12000.f,      900.f   },  // 7 — v6 default
        { 150.f,       5,        14000.f,     1000.f   },  // 8
        { 120.f,       6,        17000.f,     1100.f   },  // 9
        {  95.f,       8,        22000.f,     1300.f   },  // 10 — unsafe
    };
    const auto& pr = table[level-1];
    g_cfg.cooldownMeanMs.store(pr.cooldownMs);
    g_cfg.maxInteractsPerTick.store(pr.perTick);
    g_cfg.cannonTeleportCap_cm.store(pr.warpCap);
    g_cfg.interactRadius_cm.store(pr.interactRadius);
    g_cfg.speedPreset.store(level);
}

void Tick(const LocalPlayerCache& lp,
          const std::vector<Actor>& actors,
          float /*dt*/){
    if(!g_cfg.enabled.load()) return;
    if(!lp.pawn || !lp.rootComp) return;

    ProcessPending();

    // Active cannon lock.
    if(!p.activeCannon){
        p.activeCannon = PickNearestCannon(lp, actors, g_cfg.maxRange_m.load());
        if(!p.activeCannon) return;
    } else {
        bool still = false;
        for(auto& a : actors) if(a.addr == p.activeCannon && a.isCannon){ still = true; break; }
        if(!still){ p.activeCannon = 0; return; }
    }

    // Find the richest chest cluster in range.
    std::vector<uintptr_t> cluster;
    FVector clusterCenter = PickBestCluster(actors, g_cfg.maxRange_m.load()*100.f, cluster);
    if(cluster.empty()) return;

    // Visual snapshot (first N chests of the cluster become "queued").
    {
        std::vector<ChestVisual> snap;
        snap.reserve(actors.size());
        for(auto& a : actors){
            if(!(a.isChest || a.isLootDrop)) continue;
            if(a.isVaultChest) continue;
            ChestVisual v{};
            v.actor = a;
            v.queuedAsTarget = std::find(cluster.begin(), cluster.end(), a.addr) != cluster.end();
            v.reachable      = (a.pos - lp.pos).Len() < g_cfg.maxRange_m.load()*100.f;
            snap.push_back(v);
        }
        std::scoped_lock _(p.visLock);
        p.visualSnap = std::move(snap);
    }

    // Move the cannon above the cluster center (brings the player too
    // via playerRidesCannon). No fire — the cannon is just the vehicle.
    if(g_cfg.teleportCannon.load()){
        WarpCannonToCluster(lp, p.activeCannon, clusterCenter);
    }

    // Jittered inter-arrival. Each interact call still passes through
    // the gate so parallel-per-tick doesn't collapse to a clean burst.
    double now = NowSec();
    const double sigma = 0.30;
    int interactsThisTick = 0;
    const int cap = g_cfg.maxInteractsPerTick.load();

    for(uintptr_t chest : cluster){
        if(interactsThisTick >= cap) break;
        if(!cf::ab::jitter::shouldFireLognormal(now, p.lastInteractTime,
                                                g_cfg.cooldownMeanMs.load(),
                                                sigma)){
            break;
        }

        // Fire the interact RPC.
        InteractChest(chest, lp.pawn);
        p.pending.push_back({chest, now});
        ++interactsThisTick;

        int lim = g_cfg.chestsPerSession.load();
        if(lim > 0 && (int)p.smashed.load() >= lim){
            g_cfg.enabled.store(false);
            return;
        }

        now = NowSec();
    }
}

} // namespace cf::feat
