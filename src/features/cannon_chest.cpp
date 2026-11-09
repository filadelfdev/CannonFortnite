#include "features/cannon_chest.h"
#include "sdk/engine.h"
#include "sdk/offsets.h"
#include "core/memory.h"
#include "antiban/clamp.h"
#include "util/log.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <mutex>
#include <atomic>

namespace cf::feat {

CannonChestCfg g_cfg{};

namespace {

struct Pipeline {
    uintptr_t activeCannon{};
    uintptr_t targetChest{};
    double    lastFireTime{};
    std::atomic<uint32_t> smashed{0};
    std::vector<ChestVisual> visualSnap;
    std::vector<uintptr_t> prequeue;   // lookahead: next N chests
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

// Teleport the CANNON with a wide clamp — vehicles live on a bigger
// replicated-move envelope than pawns, so we can snap over several
// hundred meters per tick instead of 31m.
void WarpCannon(const LocalPlayerCache& lp,
                uintptr_t cannon,
                const FVector& chestPos){
    uintptr_t cannonRoot = cf::mem::R<uintptr_t>(cannon + cf::off::Pawn_RootComponent);
    if(!cannonRoot) return;

    FVector cur = cf::mem::R<FVector>(cannonRoot + cf::off::Scene_RelativeLocation);
    FVector target = chestPos;
    target.z += g_cfg.hoverHeight_cm.load();

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

// One-shot aim + fire write. Called many times per second on the hot path.
void SmashDown(uintptr_t cannon){
    uintptr_t root = cf::mem::R<uintptr_t>(cannon + cf::off::Pawn_RootComponent);
    if(!root) return;

    float pitch = g_cfg.aimPitchDeg.load();
    FRotator r{pitch, 0.f, 0.f};
    cf::mem::Write(root  + cf::off::Scene_RelativeRotation, &r, sizeof(r));
    cf::mem::Write(cannon + cf::off::Cannon_FireAngle, &pitch, sizeof(float));

    float muzzle = g_cfg.smashMuzzleSpeed.load();
    cf::mem::Write(cannon + cf::off::Cannon_LaunchPower, &muzzle, sizeof(float));

    uint8_t canFire = 1;
    cf::mem::Write(cannon + cf::off::Cannon_bCanFire, &canFire, 1);
    float ts = (float)NowSec();
    cf::mem::Write(cannon + cf::off::Cannon_LastFiredTime, &ts, sizeof(float));
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

// Build a sorted queue of unlooted chests nearest to `from`. Capped at
// `lookahead` entries. This runs every tick but is cheap: an actor
// list of ~2000 items filters to a chest set of a few dozen.
void BuildQueue(const FVector& from,
                const std::vector<Actor>& actors,
                float maxRange_m,
                int lookahead,
                std::vector<uintptr_t>& out){
    out.clear();
    out.reserve(lookahead);

    struct Entry { uintptr_t addr; float d; };
    std::vector<Entry> pool;
    pool.reserve(64);
    float maxCm = maxRange_m * 100.f;
    for(auto& a : actors){
        if(!(a.isChest || a.isLootDrop)) continue;
        if(g_cfg.skipLootedChests.load() && IsChestLooted(a.addr)) continue;
        float d = (a.pos - from).Len();
        if(d > maxCm) continue;
        pool.push_back({a.addr, d});
    }
    std::sort(pool.begin(), pool.end(),
              [](const Entry& a, const Entry& b){ return a.d < b.d; });
    int n = std::min((int)pool.size(), lookahead);
    for(int i=0;i<n;++i) out.push_back(pool[i].addr);
}

} // namespace

std::vector<ChestVisual> GetVisualState(){
    std::scoped_lock _(p.visLock);
    return p.visualSnap;
}

void Tick(const LocalPlayerCache& lp,
          const std::vector<Actor>& actors,
          float /*dt*/){
    if(!g_cfg.enabled.load()) return;
    if(!lp.pawn || !lp.rootComp) return;

    // 1. Lock active cannon (keeps the cannon handle sticky across chests).
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

    // 2. Rebuild the near-queue every tick so we can multi-fire.
    BuildQueue(cannonPos, actors, g_cfg.maxRange_m.load(),
               g_cfg.lookahead.load(), p.prequeue);

    // Visual snapshot.
    {
        std::vector<ChestVisual> snap;
        snap.reserve(actors.size());
        for(auto& a : actors){
            if(!(a.isChest || a.isLootDrop)) continue;
            ChestVisual v{};
            v.actor = a;
            v.queuedAsTarget = (!p.prequeue.empty() && a.addr == p.prequeue.front());
            v.reachable      = (a.pos - lp.pos).Len() < g_cfg.maxRange_m.load()*100.f;
            snap.push_back(v);
        }
        std::scoped_lock _(p.visLock);
        p.visualSnap = std::move(snap);
    }

    if(p.prequeue.empty()){ p.targetChest = 0; return; }

    // 3. Fire-on-approach: scan the pre-queue, any chest already in
    // fireRange gets smashed IN THIS TICK (up to maxFiresPerTick).
    // Multi-fire is the key to 300+ chests/min when chests cluster.
    double now = NowSec();
    float  fr  = g_cfg.fireRange_cm.load();
    int    budget = g_cfg.maxFiresPerTick.load();

    if(g_cfg.fireOnApproach.load()){
        for(auto it = p.prequeue.begin(); it != p.prequeue.end() && budget > 0; ){
            uintptr_t chest = *it;
            uintptr_t chestRoot = cf::mem::R<uintptr_t>(chest + cf::off::Pawn_RootComponent);
            if(!chestRoot){ it = p.prequeue.erase(it); continue; }
            FVector chestPos = cf::mem::R<FVector>(chestRoot + cf::off::Scene_RelativeLocation);
            float horiz = std::sqrt((cannonPos.x-chestPos.x)*(cannonPos.x-chestPos.x) +
                                    (cannonPos.y-chestPos.y)*(cannonPos.y-chestPos.y));
            if(horiz < fr && (now - p.lastFireTime) >= g_cfg.cooldownSec.load()){
                SmashDown(p.activeCannon);
                p.lastFireTime = now;
                if(g_cfg.openOnTouch.load()){
                    FlagChestSearched(chest);
                    p.smashed.fetch_add(1);
                }
                it = p.prequeue.erase(it);
                --budget;
                int lim = g_cfg.chestsPerSession.load();
                if(lim > 0 && (int)p.smashed.load() >= lim){
                    g_cfg.enabled.store(false);
                    return;
                }
                continue;
            }
            ++it;
        }
    }

    if(p.prequeue.empty()){ p.targetChest = 0; return; }

    // 4. Warp the cannon to the first chest still in queue (closest).
    p.targetChest = p.prequeue.front();
    uintptr_t chestRoot = cf::mem::R<uintptr_t>(p.targetChest + cf::off::Pawn_RootComponent);
    if(!chestRoot){ p.prequeue.erase(p.prequeue.begin()); return; }
    FVector chestPos = cf::mem::R<FVector>(chestRoot + cf::off::Scene_RelativeLocation);

    if(g_cfg.teleportCannon.load()){
        WarpCannon(lp, p.activeCannon, chestPos);
    }

    // 5. Shot. Alignment skipped by default; raw write+fire same tick.
    cannonPos = cf::mem::R<FVector>(cannonRoot + cf::off::Scene_RelativeLocation);
    bool ok = true;
    if(!g_cfg.skipAlignmentCheck.load()){
        float horiz = std::sqrt((cannonPos.x-chestPos.x)*(cannonPos.x-chestPos.x) +
                                (cannonPos.y-chestPos.y)*(cannonPos.y-chestPos.y));
        float dz    = cannonPos.z - chestPos.z;
        ok = horiz < 180.f && dz > g_cfg.hoverHeight_cm.load()*0.6f;
    }
    if(!ok) return;

    if(g_cfg.autoFire.load() && (now - p.lastFireTime) >= g_cfg.cooldownSec.load()){
        SmashDown(p.activeCannon);
        p.lastFireTime = now;
        if(g_cfg.openOnTouch.load()){
            FlagChestSearched(p.targetChest);
            p.smashed.fetch_add(1);
            int lim = g_cfg.chestsPerSession.load();
            if(lim > 0 && (int)p.smashed.load() >= lim){
                g_cfg.enabled.store(false);
                return;
            }
        }
        p.prequeue.erase(p.prequeue.begin());
        p.targetChest = 0;
    }
}

// Exposed so the overlay HUD can show the live Pirate Token count.
uint32_t SmashedThisSession(){ return p.smashed.load(); }
void     ResetSmashCounter(){ p.smashed.store(0); }

} // namespace cf::feat
