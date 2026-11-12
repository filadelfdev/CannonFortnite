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
    double    lastFireTime{};
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

// Teleport cannon + player (together, because playerRidesCannon) to a
// cluster center. Called ONLY when hopping between clusters — never
// during a burst.
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

    // Player rides the cannon → drag the pawn along with it.
    if(g_cfg.playerRidesCannon.load() && lp.rootComp){
        FVector seat = next;
        seat.z += 80.f;
        cf::mem::Write(lp.rootComp + cf::off::Scene_RelativeLocation, &seat, sizeof(FVector));
        cf::mem::Write(lp.rootComp + cf::off::Scene_ComponentVelocity, &zero, sizeof(FVector));
    }
}

// Compute aim to look at a chest from cannon position. The chest isn't
// usually below the cannon (we hover above, aim roughly down+toward).
FRotator LookAt(const FVector& from, const FVector& to){
    FVector d = to - from;
    float dist2D = std::sqrt(d.x*d.x + d.y*d.y);
    FRotator r;
    r.pitch = std::atan2(d.z, dist2D) * 57.29578f;
    r.yaw   = std::atan2(d.y, d.x)    * 57.29578f;
    r.roll  = 0.f;
    return r;
}

// Fire one shot at a specific chest. Writes aim + launch power + zeros
// the client-side cooldown so the next tick can fire again. This is
// the "no cooldown" trick the reference cheat uses.
void FireAtChest(uintptr_t cannon,
                 const FVector& cannonPos,
                 const FVector& chestPos){
    uintptr_t root = cf::mem::R<uintptr_t>(cannon + cf::off::Pawn_RootComponent);
    if(!root) return;

    FRotator aim = LookAt(cannonPos, chestPos);
    // tiny wobble on both axes so N consecutive aims aren't bit-identical.
    aim.pitch += cf::ab::jitter::wobble(0.f, g_cfg.aimJitterDeg.load());
    aim.yaw   += cf::ab::jitter::wobble(0.f, g_cfg.aimJitterDeg.load());

    cf::mem::Write(root + cf::off::Scene_RelativeRotation, &aim, sizeof(aim));
    cf::mem::Write(cannon + cf::off::Cannon_FireAngle, &aim.pitch, sizeof(float));

    // Launch power scaled to distance so the ball lands on the chest
    // in one tick (no wasted projectile travel time).
    float dist = (chestPos - cannonPos).Len();
    float muzzle = std::max(1200.f, std::min(9000.f, dist * 2.5f));
    cf::mem::Write(cannon + cf::off::Cannon_LaunchPower, &muzzle, sizeof(float));

    // Zero out LastFiredTime — the key to "no cooldown". The vehicle
    // tick reads this, sees it's 0, allows another fire instantly.
    float zero = 0.f;
    cf::mem::Write(cannon + cf::off::Cannon_LastFiredTime, &zero, sizeof(float));
    uint8_t canFire = 1;
    cf::mem::Write(cannon + cf::off::Cannon_bCanFire, &canFire, 1);
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

// Pick the densest cluster of unlooted chests in range. Returns center
// of cluster + the list of chests inside it.
FVector PickBestCluster(const std::vector<Actor>& actors,
                        std::vector<uintptr_t>& outChests){
    outChests.clear();
    const float r2 = g_cfg.interactRadius_cm.load() * g_cfg.interactRadius_cm.load();

    int bestCount = 0;
    FVector bestCenter{};
    std::vector<uintptr_t> bestList;

    for(auto& candidate : actors){
        if(candidate.isVaultChest) continue;
        if(!candidate.isChest) continue;
        if(g_cfg.skipLootedChests.load() && IsChestLooted(candidate.addr)) continue;

        int count = 0;
        std::vector<uintptr_t> here;
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
        float cooldownMs;   int perTick;   float warpCap;   float clusterRadius;
    };
    static const Preset table[10] = {
        //  cooldown   per-tick   warp cap     cluster radius
        { 900.f,       1,         4000.f,      500.f   },  // 1 — safest, ~1/s
        { 650.f,       1,         6000.f,      600.f   },  // 2
        { 480.f,       2,         7500.f,      700.f   },  // 3
        { 360.f,       2,         9000.f,      800.f   },  // 4
        { 280.f,       3,        10500.f,      850.f   },  // 5 — ~3/s
        { 230.f,       3,        11500.f,      900.f   },  // 6 — ~4/s
        { 190.f,       4,        12000.f,      900.f   },  // 7 — ~5/s default
        { 150.f,       5,        14000.f,     1000.f   },  // 8
        { 120.f,       6,        17000.f,     1100.f   },  // 9
        {  95.f,       8,        22000.f,     1300.f   },  // 10 — unsafe
    };
    const auto& pr = table[level-1];
    g_cfg.cooldownMeanMs.store(pr.cooldownMs);
    g_cfg.maxInteractsPerTick.store(pr.perTick);
    g_cfg.cannonTeleportCap_cm.store(pr.warpCap);
    g_cfg.interactRadius_cm.store(pr.clusterRadius);
    g_cfg.speedPreset.store(level);
}

void Tick(const LocalPlayerCache& lp,
          const std::vector<Actor>& actors,
          float /*dt*/){
    if(!g_cfg.enabled.load()) return;
    if(!lp.pawn || !lp.rootComp) return;

    ProcessPending();

    // Lock active cannon.
    if(!p.activeCannon){
        p.activeCannon = PickNearestCannon(lp, actors, g_cfg.maxRange_m.load());
        if(!p.activeCannon) return;
    } else {
        bool still = false;
        for(auto& a : actors) if(a.addr == p.activeCannon && a.isCannon){ still = true; break; }
        if(!still){ p.activeCannon = 0; return; }
    }

    uintptr_t cannonRoot = cf::mem::R<uintptr_t>(p.activeCannon + cf::off::Pawn_RootComponent);
    if(!cannonRoot){ p.activeCannon = 0; return; }
    FVector cannonPos = cf::mem::R<FVector>(cannonRoot + cf::off::Scene_RelativeLocation);

    // Find densest cluster of unlooted chests.
    std::vector<uintptr_t> cluster;
    FVector clusterCenter = PickBestCluster(actors, cluster);
    if(cluster.empty()) return;

    // Visual snapshot.
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

    // Hop or burst? If the cannon is far from the cluster center, HOP
    // (cannon + player teleport together, playerRidesCannon). If the
    // cannon is already at the cluster, start the BURST — cannon stays
    // put, aim rotates between chests, zero-cooldown fire each tick.
    float distToCluster = (cannonPos - clusterCenter).Len();
    constexpr float kBurstDistance_cm = 600.f;   // "we are here" threshold

    if(distToCluster > kBurstDistance_cm){
        if(g_cfg.teleportCannon.load()){
            WarpCannonToCluster(lp, p.activeCannon, clusterCenter);
        }
        return;   // let the warp settle, burst starts next tick
    }

    // BURST — cannon parked, rotate aim between chests in the cluster.
    double now = NowSec();
    const double sigma = 0.30;
    int shotsThisTick = 0;
    const int cap = g_cfg.maxInteractsPerTick.load();

    for(uintptr_t chest : cluster){
        if(shotsThisTick >= cap) break;
        if(IsChestLooted(chest)) continue;

        if(!cf::ab::jitter::shouldFireLognormal(now, p.lastFireTime,
                                                g_cfg.cooldownMeanMs.load(),
                                                sigma)){
            break;
        }

        uintptr_t chestRoot = cf::mem::R<uintptr_t>(chest + cf::off::Pawn_RootComponent);
        if(!chestRoot) continue;
        FVector chestPos = cf::mem::R<FVector>(chestRoot + cf::off::Scene_RelativeLocation);

        // Fire at THIS chest (aim rotation + launch write + zero cooldown).
        FireAtChest(p.activeCannon, cannonPos, chestPos);
        p.pending.push_back({chest, now});
        ++shotsThisTick;

        int lim = g_cfg.chestsPerSession.load();
        if(lim > 0 && (int)p.smashed.load() >= lim){
            g_cfg.enabled.store(false);
            return;
        }

        now = NowSec();
    }
}

} // namespace cf::feat
