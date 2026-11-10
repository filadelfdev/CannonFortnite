#include "features/cannon_chest.h"
#include "sdk/engine.h"
#include "sdk/offsets.h"
#include "core/memory.h"
#include "antiban/clamp.h"
#include "antiban/jitter.h"
#include "antiban/rpc_path.h"
#include "util/log.h"

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
    uintptr_t targetChest{};
    double    lastFireTime{};
    std::atomic<uint32_t> smashed{0};
    std::vector<ChestVisual> visualSnap;
    std::vector<uintptr_t>   prequeue;
    std::deque<PendingConfirm> pending;   // waiting for server-break confirm
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

// Prefer RPC. Fallback writes the fields directly — noisier, works.
void MoveCannon(const LocalPlayerCache& lp, uintptr_t cannon, const FVector& chestPos){
    FVector target = chestPos;
    target.z += g_cfg.hoverHeight_cm.load()
              + cf::ab::jitter::wobble(0.f, g_cfg.hoverJitter_cm.load());

    FRotator aim{
        g_cfg.aimPitchDeg.load() + cf::ab::jitter::wobble(0.f, g_cfg.aimJitterDeg.load()),
        cf::ab::jitter::frange(-5.f, 5.f),       // tiny yaw wobble
        cf::ab::jitter::frange(-0.5f, 0.5f)      // roll noise
    };

    if(g_cfg.preferRpc.load() && cf::ab::rpc::Ready()){
        // Clamp per-call delta to the vehicle envelope so even the RPC
        // path doesn't send server-rejected moves.
        uintptr_t root = cf::mem::R<uintptr_t>(cannon + cf::off::Pawn_RootComponent);
        FVector cur = cf::mem::R<FVector>(root + cf::off::Scene_RelativeLocation);
        FVector clamped = cf::ab::clamp::teleport(cur, target,
                                                   g_cfg.cannonTeleportCap_cm.load());
        cf::ab::rpc::CannonMove(cannon, clamped, aim);
    } else {
        // Direct-write fallback.
        uintptr_t root = cf::mem::R<uintptr_t>(cannon + cf::off::Pawn_RootComponent);
        if(!root) return;
        FVector cur = cf::mem::R<FVector>(root + cf::off::Scene_RelativeLocation);
        FVector clamped = cf::ab::clamp::teleport(cur, target,
                                                   g_cfg.cannonTeleportCap_cm.load());
        cf::mem::Write(root + cf::off::Scene_RelativeLocation, &clamped, sizeof(FVector));
        cf::mem::Write(root + cf::off::Scene_RelativeRotation, &aim, sizeof(aim));
        FVector zero{};
        cf::mem::Write(root + cf::off::Scene_ComponentVelocity, &zero, sizeof(FVector));

        if(g_cfg.playerRidesCannon.load() && lp.rootComp){
            FVector seat = clamped; seat.z += 80.f;
            cf::mem::Write(lp.rootComp + cf::off::Scene_RelativeLocation, &seat, sizeof(FVector));
            cf::mem::Write(lp.rootComp + cf::off::Scene_ComponentVelocity, &zero, sizeof(FVector));
        }
    }
}

void FireCannonAt(uintptr_t cannon, uintptr_t chest){
    FRotator aim{ g_cfg.aimPitchDeg.load()
                  + cf::ab::jitter::wobble(0.f, g_cfg.aimJitterDeg.load()),
                  0.f, 0.f };
    float muzzle = g_cfg.smashMuzzleSpeed.load()
                 + cf::ab::jitter::wobble(0.f, 60.f);

    if(g_cfg.preferRpc.load() && cf::ab::rpc::Ready()){
        cf::ab::rpc::CannonFire(cannon, muzzle, aim);
        // Container damage RPC — the real token trigger.
        cf::ab::rpc::ChestTakeCannonDamage(chest, cannon, /*dmg*/ 400.f);
    } else {
        // Direct-write — v3 behaviour.
        uintptr_t root = cf::mem::R<uintptr_t>(cannon + cf::off::Pawn_RootComponent);
        if(root){
            cf::mem::Write(root + cf::off::Scene_RelativeRotation, &aim, sizeof(aim));
            cf::mem::Write(cannon + cf::off::Cannon_FireAngle,   &aim.pitch, sizeof(float));
        }
        cf::mem::Write(cannon + cf::off::Cannon_LaunchPower,   &muzzle, sizeof(float));
        uint8_t canFire = 1;
        cf::mem::Write(cannon + cf::off::Cannon_bCanFire,      &canFire, 1);
        float ts = (float)NowSec();
        cf::mem::Write(cannon + cf::off::Cannon_LastFiredTime, &ts, sizeof(float));
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

void BuildQueue(const FVector& from,
                const std::vector<Actor>& actors,
                float maxRange_m,
                int lookahead,
                std::vector<uintptr_t>& out){
    out.clear();
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

// Drain pending confirms: a chest that the server actually broke will
// come back with bAlreadySearched set by the game itself. We only
// count it as smashed when the server confirms. This is the fix for
// the v3 "mark client-side before server processed" ban vector.
void ProcessPending(){
    double now = NowSec();
    float  graceSec = g_cfg.serverConfirmGraceMs.load() * 0.001f;
    while(!p.pending.empty()){
        auto& front = p.pending.front();
        if(IsChestLooted(front.chest)){
            p.smashed.fetch_add(1);
            p.pending.pop_front();
            continue;
        }
        if((now - front.fireTime) > graceSec){
            // Server didn't confirm — the smash missed. Don't count it,
            // don't flag it. The chest returns to the queue next tick.
            p.pending.pop_front();
            continue;
        }
        break;   // front still within grace window, nothing older either
    }
}

} // namespace

std::vector<ChestVisual> GetVisualState(){
    std::scoped_lock _(p.visLock);
    return p.visualSnap;
}

uint32_t SmashedThisSession(){ return p.smashed.load(); }
void     ResetSmashCounter(){ p.smashed.store(0); }

void Tick(const LocalPlayerCache& lp,
          const std::vector<Actor>& actors,
          float /*dt*/){
    if(!g_cfg.enabled.load()) return;
    if(!lp.pawn || !lp.rootComp) return;

    ProcessPending();

    // 1. Active cannon.
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

    BuildQueue(cannonPos, actors, g_cfg.maxRange_m.load(),
               g_cfg.lookahead.load(), p.prequeue);

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

    // v5: lognormal inter-arrival — a human's chest-open cadence follows
    // lognormal(mode=cooldownMs, sigma=0.45). Spreading uniform noise
    // keeps stddev up but EAC's classifier sniffs the kurtosis — lognormal
    // matches both moments.
    double now = NowSec();
    const double sigma = 0.45;
    if(!cf::ab::jitter::shouldFireLognormal(now, p.lastFireTime,
                                            g_cfg.cooldownMeanMs.load(),
                                            sigma)){
        // Still move toward the next chest even if we can't fire yet —
        // the actual fire RPC is cheap and gated, the move is the slow
        // part. One move write per tick is below the vehicle envelope.
        p.targetChest = p.prequeue.front();
        uintptr_t chestRoot = cf::mem::R<uintptr_t>(p.targetChest + cf::off::Pawn_RootComponent);
        if(!chestRoot){ p.prequeue.erase(p.prequeue.begin()); return; }
        FVector chestPos = cf::mem::R<FVector>(chestRoot + cf::off::Scene_RelativeLocation);
        if(g_cfg.teleportCannon.load()) MoveCannon(lp, p.activeCannon, chestPos);
        return;
    }

    // Fire path — only one chest per tick, ever (v4).
    p.targetChest = p.prequeue.front();
    uintptr_t chestRoot = cf::mem::R<uintptr_t>(p.targetChest + cf::off::Pawn_RootComponent);
    if(!chestRoot){ p.prequeue.erase(p.prequeue.begin()); return; }
    FVector chestPos = cf::mem::R<FVector>(chestRoot + cf::off::Scene_RelativeLocation);

    if(g_cfg.teleportCannon.load()) MoveCannon(lp, p.activeCannon, chestPos);

    // Alignment check (default ON in v4 — safer).
    cannonPos = cf::mem::R<FVector>(cannonRoot + cf::off::Scene_RelativeLocation);
    if(!g_cfg.skipAlignmentCheck.load()){
        float horiz = std::sqrt((cannonPos.x-chestPos.x)*(cannonPos.x-chestPos.x) +
                                (cannonPos.y-chestPos.y)*(cannonPos.y-chestPos.y));
        float dz    = cannonPos.z - chestPos.z;
        if(horiz > 220.f || dz < g_cfg.hoverHeight_cm.load()*0.5f) return;
    }

    FireCannonAt(p.activeCannon, p.targetChest);
    p.pending.push_back({p.targetChest, now});

    // Do NOT flag searched yet — wait for server. Grace-window drain
    // in ProcessPending() counts the smash only when the server
    // confirms. Legacy behaviour (immediate flag) available via the
    // openOnTouch toggle for debugging.
    if(g_cfg.openOnTouch.load()){
        FlagChestSearched(p.targetChest);
    }

    p.prequeue.erase(p.prequeue.begin());
    p.targetChest = 0;

    int lim = g_cfg.chestsPerSession.load();
    if(lim > 0 && (int)p.smashed.load() >= lim){
        g_cfg.enabled.store(false);
    }
}

} // namespace cf::feat
