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

namespace cf::feat {

CannonChestCfg g_cfg{};

namespace {

struct Pipeline {
    uintptr_t activeCannon{};
    uintptr_t targetChest{};
    double    lastFireTime{};
    uint32_t  smashed{};
    std::vector<ChestVisual> visualSnap;
    std::mutex visLock;
} p;

double NowSec(){
    using namespace std::chrono;
    return duration_cast<duration<double>>(steady_clock::now().time_since_epoch()).count();
}

bool IsChestLooted(uintptr_t chest){
    uint8_t v = cf::mem::R<uint8_t>(chest + cf::off::Chest_bAlreadySearched);
    return (v & 1) != 0;
}

void FlagChestSearched(uintptr_t chest){
    uint8_t flag = 1;
    cf::mem::Write(chest + cf::off::Chest_bAlreadySearched, &flag, 1);
    float bounce[4] = { 1.f, 0.f, 0.f, 0.f };
    cf::mem::Write(chest + cf::off::Chest_SearchBounceData, bounce, sizeof(bounce));
}

// Teleport the CANNON to a parking spot directly above the chest.
// This is the mechanic that drops Pirate Tokens — cannons registered
// as damage sources against world containers.
void ParkCannonAboveChest(const LocalPlayerCache& lp,
                          uintptr_t cannon,
                          const FVector& chestPos){
    uintptr_t cannonRoot = cf::mem::R<uintptr_t>(cannon + cf::off::Pawn_RootComponent);
    if(!cannonRoot) return;

    FVector cur = cf::mem::R<FVector>(cannonRoot + cf::off::Scene_RelativeLocation);
    FVector target = chestPos;
    target.z += g_cfg.hoverHeight_cm.load();

    // Clamp the cannon's per-tick delta the same way we clamp player
    // teleports — cannons replicate position too, and a snap over the
    // anti-warp cap burns the vehicle.
    FVector next = cf::ab::clamp::teleport(cur, target, /*maxDelta_cm*/ 3100.f);
    cf::mem::Write(cannonRoot + cf::off::Scene_RelativeLocation, &next, sizeof(FVector));

    // Pin velocity to zero so the vehicle doesn't physics-slide off.
    FVector zero{};
    cf::mem::Write(cannonRoot + cf::off::Scene_ComponentVelocity, &zero, sizeof(FVector));

    // Also drag the player with the cannon so the seat occupancy stays
    // bound — Fortnite yeets the driver when the vehicle moves faster
    // than its own replicated speed envelope.
    if(g_cfg.playerRidesCannon.load() && lp.rootComp){
        FVector seat = next;
        seat.z += 80.f;
        cf::mem::Write(lp.rootComp + cf::off::Scene_RelativeLocation, &seat, sizeof(FVector));
        cf::mem::Write(lp.rootComp + cf::off::Scene_ComponentVelocity, &zero, sizeof(FVector));
    }
}

// Aim the cannon straight down and fire. The ball spawns with the
// cannon's muzzle rotation; low muzzle speed keeps the ball inside the
// chest's hit volume instead of punching through.
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

uintptr_t PickNextChest(const LocalPlayerCache& lp,
                        const std::vector<Actor>& actors,
                        float maxRange_m,
                        uintptr_t skip){
    uintptr_t best = 0;
    float     bestD = maxRange_m * 100.f;
    for(auto& a : actors){
        if(!(a.isChest || a.isLootDrop)) continue;
        if(a.addr == skip) continue;
        if(g_cfg.skipLootedChests.load() && IsChestLooted(a.addr)) continue;
        float d = (a.pos - lp.pos).Len();
        if(d < bestD){ bestD = d; best = a.addr; }
    }
    return best;
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

    // 1. Lock the active cannon.
    if(!p.activeCannon){
        p.activeCannon = PickNearestCannon(lp, actors, g_cfg.maxRange_m.load());
        if(!p.activeCannon) return;
    } else {
        // Verify it's still in the actor list (game may despawn it).
        bool stillThere = false;
        for(auto& a : actors) if(a.addr == p.activeCannon && a.isCannon){ stillThere = true; break; }
        if(!stillThere){ p.activeCannon = 0; return; }
    }

    // 2. Pick the next unlooted chest.
    if(!p.targetChest || IsChestLooted(p.targetChest)){
        p.targetChest = PickNextChest(lp, actors, g_cfg.maxRange_m.load(), 0);
        if(!p.targetChest) return;
    }

    // Visual snapshot.
    {
        std::vector<ChestVisual> snap;
        snap.reserve(actors.size());
        for(auto& a : actors){
            if(!(a.isChest || a.isLootDrop)) continue;
            ChestVisual v{};
            v.actor = a;
            v.queuedAsTarget = (a.addr == p.targetChest);
            v.reachable      = (a.pos - lp.pos).Len() < g_cfg.maxRange_m.load()*100.f;
            snap.push_back(v);
        }
        std::scoped_lock _(p.visLock);
        p.visualSnap = std::move(snap);
    }

    // 3. Read the chest pos, park the cannon directly above it.
    uintptr_t chestRoot = cf::mem::R<uintptr_t>(p.targetChest + cf::off::Pawn_RootComponent);
    if(!chestRoot){ p.targetChest = 0; return; }
    FVector chestPos = cf::mem::R<FVector>(chestRoot + cf::off::Scene_RelativeLocation);

    if(g_cfg.teleportCannon.load()){
        ParkCannonAboveChest(lp, p.activeCannon, chestPos);
    }

    // 4. Verify we're actually above the chest before firing — otherwise
    //    a mid-warp shot wastes the cooldown.
    uintptr_t cannonRoot = cf::mem::R<uintptr_t>(p.activeCannon + cf::off::Pawn_RootComponent);
    FVector   cannonPos  = cf::mem::R<FVector>(cannonRoot + cf::off::Scene_RelativeLocation);
    float dx = cannonPos.x - chestPos.x;
    float dy = cannonPos.y - chestPos.y;
    float dz = cannonPos.z - chestPos.z;
    float horiz = std::sqrt(dx*dx + dy*dy);
    bool aligned = horiz < 180.f && dz > g_cfg.hoverHeight_cm.load()*0.6f;

    if(!aligned) return;

    // 5. Smash.
    double now = NowSec();
    if(g_cfg.autoFire.load() && (now - p.lastFireTime) >= g_cfg.cooldownSec.load()){
        SmashDown(p.activeCannon);
        p.lastFireTime = now;

        // The ball spawns and hits the chest within ~1 frame at this
        // drop height — mark the chest searched so the next planner
        // tick doesn't re-queue it. Pirate Token drop fires from the
        // server-side cannon-damage-container path, independent of
        // this flag.
        if(g_cfg.openOnTouch.load()){
            FlagChestSearched(p.targetChest);
            ++p.smashed;
            if(g_cfg.chestsPerSession.load() > 0 &&
               (int)p.smashed >= g_cfg.chestsPerSession.load()){
                g_cfg.enabled.store(false);
                return;
            }
        }

        // Advance to the next chest. Keep the same cannon.
        p.targetChest = PickNextChest(lp, actors, g_cfg.maxRange_m.load(), p.targetChest);
    }
}

} // namespace cf::feat
