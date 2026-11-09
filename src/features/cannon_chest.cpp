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
    FVector   lastFirePos{};
    double    lastFireTime{};
    int       burstCount{};
    std::vector<ChestVisual> visualSnap;
    std::mutex visLock;
} p;

double NowSec(){
    using namespace std::chrono;
    return duration_cast<duration<double>>(steady_clock::now().time_since_epoch()).count();
}

// Ballistic solver — classic projectile with gravity, picks the low-angle
// solution so the arc clears terrain in open ground; swaps to high-angle
// if the low one lands under the local player's foot plane.
bool SolveArc(const FVector& from, const FVector& to,
              float muzzle, float g, float& pitchDeg, float& yawDeg){
    FVector d = to - from;
    float dz  = d.z;
    float d2d = d.Len2D();
    if(d2d < 1.f) return false;

    float v2 = muzzle*muzzle;
    float g_ = -g;
    float disc = v2*v2 - g_*(g_*d2d*d2d + 2.f*dz*v2);
    if(disc < 0.f) return false;
    float sq = std::sqrt(disc);
    float lowAng  = std::atan((v2 - sq) / (g_*d2d));
    float highAng = std::atan((v2 + sq) / (g_*d2d));

    pitchDeg = lowAng * 57.29578f;
    if(pitchDeg < -20.f) pitchDeg = highAng * 57.29578f;
    yawDeg   = std::atan2(d.y, d.x) * 57.29578f;
    return true;
}

bool IsChestLooted(uintptr_t chest){
    uint8_t v = cf::mem::R<uint8_t>(chest + cf::off::Chest_bAlreadySearched);
    return (v & 1) != 0;
}

void OpenChestInPlace(uintptr_t chest){
    // Flip bAlreadySearched + drive SearchBounceData the way
    // ServerInteract would once a real interact fires. On a server-auth
    // slice this is cosmetic; on an offline/sandbox build it's a full
    // search. The antiban clamp blocks a flood of these so a missed
    // ServerInteract reply doesn't desync every chest at once.
    static double lastOpenTime = 0.0;
    double now = NowSec();
    if(!cf::ab::clamp::chestOpenRateOk(now, lastOpenTime)) return;
    lastOpenTime = now;

    uint8_t flag = 1;
    cf::mem::Write(chest + cf::off::Chest_bAlreadySearched, &flag, 1);
    float bounce[4] = { 1.f, 0.f, 0.f, 0.f };
    cf::mem::Write(chest + cf::off::Chest_SearchBounceData, bounce, sizeof(bounce));
}

void WriteCannonAim(uintptr_t cannon, float pitch, float yaw){
    // RelativeRotation lives on the cannon's root scene component.
    uintptr_t root = cf::mem::R<uintptr_t>(cannon + cf::off::Pawn_RootComponent);
    if(!root) return;
    FRotator r{pitch, yaw, 0.f};
    cf::mem::Write(root + cf::off::Scene_RelativeRotation, &r, sizeof(r));
    // Launch angle also lives duplicated in the cannon's own tuning block.
    cf::mem::Write(cannon + cf::off::Cannon_FireAngle, &pitch, sizeof(float));
}

void TripCannonFire(uintptr_t cannon, float muzzle){
    cf::mem::Write(cannon + cf::off::Cannon_LaunchPower, &muzzle, sizeof(float));
    uint8_t canFire = 1;
    cf::mem::Write(cannon + cf::off::Cannon_bCanFire, &canFire, 1);
    double t = NowSec();
    float  ts= (float)t;
    cf::mem::Write(cannon + cf::off::Cannon_LastFiredTime, &ts, sizeof(float));
}

void TeleportPlayerToCannon(const LocalPlayerCache& lp, uintptr_t cannon){
    if(!lp.rootComp) return;
    uintptr_t cannonRoot = cf::mem::R<uintptr_t>(cannon + cf::off::Pawn_RootComponent);
    if(!cannonRoot) return;
    FVector cannonPos = cf::mem::R<FVector>(cannonRoot + cf::off::Scene_RelativeLocation);
    cannonPos.z += 80.f;   // seat height above the base of the vehicle
    // Clamp teleport distance per tick so the position delta never shows up
    // as a phase violation on the server-side anti-teleport heuristic.
    FVector clamped = cf::ab::clamp::teleport(lp.pos, cannonPos, /*maxDelta_cm*/ 3200.f);
    cf::mem::Write(lp.rootComp + cf::off::Scene_RelativeLocation, &clamped, sizeof(FVector));
}

uintptr_t PickNearestCannon(const LocalPlayerCache& lp,
                            const std::vector<Actor>& actors,
                            float maxRange_m){
    uintptr_t best = 0;
    float     bestD = maxRange_m*100.f;   // cm
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
    float     bestD = maxRange_m*100.f;
    for(auto& a : actors){
        if(!(a.isChest || a.isLootDrop)) continue;
        if(a.addr == skip) continue;
        if(g_cfg.skipLootedChests && IsChestLooted(a.addr)) continue;
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
          float dt){
    if(!g_cfg.enabled.load()) return;
    if(!lp.pawn || !lp.rootComp) return;

    uintptr_t cannon = PickNearestCannon(lp, actors, g_cfg.maxRange_m.load());
    p.activeCannon = cannon;

    // Target selection honours the current burst — once a chest is queued
    // we keep firing on it until the burst closes, then skip to the next.
    if(!p.targetChest || (g_cfg.skipLootedChests && IsChestLooted(p.targetChest))){
        p.targetChest = PickNextChest(lp, actors, g_cfg.maxRange_m.load(), 0);
        p.burstCount  = 0;
    }

    // Visual snapshot for the overlay — one write under lock, no partial state.
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
    {
        std::scoped_lock _(p.visLock);
        p.visualSnap = std::move(snap);
    }

    if(!cannon || !p.targetChest) return;

    // Teleport into the driver seat once per burst.
    if(g_cfg.teleportIntoCannon.load() && p.burstCount == 0){
        TeleportPlayerToCannon(lp, cannon);
    }

    // Solve the arc, write aim, fire if cooldown allows.
    uintptr_t cannonRoot = cf::mem::R<uintptr_t>(cannon + cf::off::Pawn_RootComponent);
    FVector   cannonPos  = cf::mem::R<FVector>(cannonRoot + cf::off::Scene_RelativeLocation);
    FVector   targetPos  = cf::mem::R<FVector>(
        cf::mem::R<uintptr_t>(p.targetChest + cf::off::Pawn_RootComponent)
        + cf::off::Scene_RelativeLocation);

    float pitch=0, yaw=0;
    if(!SolveArc(cannonPos, targetPos,
                 g_cfg.cannonMuzzleSpeed.load(),
                 g_cfg.arcGravity.load(),
                 pitch, yaw)) return;
    WriteCannonAim(cannon, pitch, yaw);

    double now = NowSec();
    if(g_cfg.autoFire.load() && (now - p.lastFireTime) >= g_cfg.cooldownSec.load()){
        TripCannonFire(cannon, g_cfg.cannonMuzzleSpeed.load());
        p.lastFireTime = now;
        p.lastFirePos  = cannonPos;

        if(g_cfg.openOnTouch.load()) OpenChestInPlace(p.targetChest);

        if(++p.burstCount >= g_cfg.chestsPerBurst.load()){
            // Close the burst, pick a fresh target next tick.
            p.targetChest = 0;
            p.burstCount  = 0;
        } else {
            // Mark looted so PickNextChest rolls forward.
            uintptr_t prev = p.targetChest;
            p.targetChest = PickNextChest(lp, actors, g_cfg.maxRange_m.load(), prev);
        }
    }
}

} // namespace cf::feat
