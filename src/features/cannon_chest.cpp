#include "features/cannon_chest.h"
#include "sdk/engine.h"
#include "sdk/offsets.h"
#include "core/process.h"
#include "antiban/clamp.h"
#include "antiban/jitter.h"

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
    double    lastFireTime{};
    std::atomic<uint32_t> smashed{0};
    std::vector<ChestVisual> visualSnap;
    std::deque<PendingConfirm> pending;
    std::mutex visLock;
} p;

double NowSec(){
    using namespace std::chrono;
    return duration_cast<duration<double>>(steady_clock::now().time_since_epoch()).count();
}

bool IsChestLooted(uintptr_t chest){
    if(!chest) return true;
    uint8_t v = cf::proc::R<uint8_t>(chest + cf::off::Chest_bAlreadySearched);
    return (v & 1) != 0;
}

FRotator LookAt(const FVector& from, const FVector& to){
    FVector d = to - from;
    float dist2D = std::sqrt(d.x*d.x + d.y*d.y);
    FRotator r;
    r.pitch = std::atan2(d.z, dist2D) * 57.29578f;
    r.yaw   = std::atan2(d.y, d.x)    * 57.29578f;
    r.roll  = 0.f;
    return r;
}

// Fire the currently-equipped weapon (Pirate Cannon) at a chest.
//
// Model: the ref cheat holds a Pirate Cannon item in hand — not a
// vehicle. Each shot:
//   1. Rotate PC_ControlRotation toward the chest (pawn aim, silent).
//   2. Zero Weapon_LastFireTime → bypasses client-side cooldown.
//   3. Write Weapon_AmmoCount high enough / write bHasInfiniteAmmo.
//   4. Flip the weapon's fire-state / FireCount++ so the engine ticks
//      a shot on the next frame.
void FireCannonWeaponAt(const LocalPlayerCache& lp,
                        uintptr_t weapon,
                        const FVector& chestPos){
    if(!weapon || !lp.controller || !lp.pawn) return;

    // 1. Silent aim — only the weapon's fire-rotation if silent-aim is
    //    toggled, else the controller view.
    FVector shootOrigin = lp.camPos;
    FRotator aim = LookAt(shootOrigin, chestPos);
    aim.pitch += cf::ab::jitter::wobble(0.f, g_cfg.aimJitterDeg.load());
    aim.yaw   += cf::ab::jitter::wobble(0.f, g_cfg.aimJitterDeg.load());
    cf::proc::Write(lp.controller + cf::off::PC_ControlRotation, &aim, sizeof(aim));
    // Also stash into weapon's own fire-trace rotation (bullet magnet).
    cf::proc::Write(weapon + 0x820, &aim, sizeof(FRotator));

    // 2. Zero the client-side cooldown — the "no cooldown" trick.
    float zero = 0.f;
    cf::proc::Write(weapon + cf::off::Weapon_LastFireTime, &zero, sizeof(float));
    cf::proc::Write(weapon + cf::off::Weapon_FireCooldown, &zero, sizeof(float));

    // 3. Keep ammo topped up so shots never run dry mid-burst.
    uint8_t infAmmo = 1;
    cf::proc::Write(weapon + cf::off::Weapon_bHasInfiniteAmmo, &infAmmo, 1);
    int ammo = 9999;
    cf::proc::Write(weapon + cf::off::Weapon_AmmoCount, &ammo, sizeof(int));

    // 4. RPC path (if resolved) sends the real ServerFire; otherwise
    //    the direct writes above are the "fire" signal.
    // TODO: direct UFunction call via engine reflection once sigs are
    // resolved (Phase 2 RE). The writes above (cooldown zero + ammo
    // top-up + aim write) are the cheat-side signal; a real ServerFire
    // RPC invocation on the current weapon's UFunction table lives in
    // this slot and will land here once resolved.
}

// Find the densest chest cluster in range AROUND THE PLAYER. The
// player doesn't move toward it — the aim rotates TO each chest.
void BuildClusterQueue(const LocalPlayerCache& lp,
                       const std::vector<Actor>& actors,
                       std::vector<uintptr_t>& out){
    out.clear();
    const float r2 = g_cfg.interactRadius_cm.load() * g_cfg.interactRadius_cm.load();

    struct Entry { uintptr_t addr; float d; };
    std::vector<Entry> pool;
    for(auto& a : actors){
        if(a.isVaultChest) continue;
        if(!a.isChest) continue;
        if(g_cfg.skipLootedChests.load() && IsChestLooted(a.addr)) continue;
        FVector d = a.pos - lp.pos;
        if(d.x*d.x + d.y*d.y + d.z*d.z > r2) continue;
        pool.push_back({a.addr, d.Len()});
    }
    std::sort(pool.begin(), pool.end(),
              [](const Entry& a, const Entry& b){ return a.d < b.d; });
    int cap = g_cfg.maxInteractsPerTick.load() * 4;   // lookahead
    int n = std::min((int)pool.size(), cap);
    for(int i=0;i<n;++i) out.push_back(pool[i].addr);
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
    struct Preset { float cooldownMs; int perTick; float clusterRadius; };
    static const Preset table[10] = {
        //  cooldown   per-tick   radius (cm)
        { 900.f,       1,         500.f   },  // 1 — ~1/s safest
        { 650.f,       1,         600.f   },  // 2
        { 480.f,       2,         700.f   },  // 3 — ~2/s
        { 360.f,       2,         800.f   },  // 4
        { 280.f,       3,         900.f   },  // 5 — ~3/s
        { 230.f,       3,         1000.f  },  // 6 — ~4/s
        { 190.f,       4,         1100.f  },  // 7 — ~5/s default
        { 150.f,       5,         1300.f  },  // 8
        { 120.f,       6,         1500.f  },  // 9
        {  95.f,       8,         1800.f  },  // 10 — unsafe
    };
    const auto& pr = table[level-1];
    g_cfg.cooldownMeanMs.store(pr.cooldownMs);
    g_cfg.maxInteractsPerTick.store(pr.perTick);
    g_cfg.interactRadius_cm.store(pr.clusterRadius);
    g_cfg.speedPreset.store(level);
}

void Tick(const LocalPlayerCache& lp,
          const std::vector<Actor>& actors,
          float /*dt*/){
    if(!g_cfg.enabled.load()) return;
    if(!lp.pawn || !lp.rootComp || !lp.controller) return;

    ProcessPending();

    // Grab the currently equipped weapon — this is the Pirate Cannon
    // (or whatever the player has in hand). The cheat never swaps
    // weapon on its own; expects the user to have the right item.
    uintptr_t weapon = cf::proc::R<uintptr_t>(lp.pawn + cf::off::Pawn_CurrentWeapon);
    if(!weapon) return;

    // Build the near-cluster queue around the player.
    std::vector<uintptr_t> queue;
    BuildClusterQueue(lp, actors, queue);
    if(queue.empty()) return;

    // Visual snap.
    {
        std::vector<ChestVisual> snap;
        snap.reserve(actors.size());
        for(auto& a : actors){
            if(!(a.isChest || a.isLootDrop)) continue;
            if(a.isVaultChest) continue;
            ChestVisual v{};
            v.actor = a;
            v.queuedAsTarget = std::find(queue.begin(), queue.end(), a.addr) != queue.end();
            v.reachable      = (a.pos - lp.pos).Len() < g_cfg.interactRadius_cm.load();
            snap.push_back(v);
        }
        std::scoped_lock _(p.visLock);
        p.visualSnap = std::move(snap);
    }

    // BURST — aim rotates between chests, weapon fires rapidly.
    double now = NowSec();
    const double sigma = 0.30;
    int shotsThisTick = 0;
    const int cap = g_cfg.maxInteractsPerTick.load();

    for(uintptr_t chest : queue){
        if(shotsThisTick >= cap) break;
        if(IsChestLooted(chest)) continue;

        if(!cf::ab::jitter::shouldFireLognormal(now, p.lastFireTime,
                                                g_cfg.cooldownMeanMs.load(),
                                                sigma)){
            break;
        }

        uintptr_t chestRoot = cf::proc::R<uintptr_t>(chest + cf::off::Pawn_RootComponent);
        if(!chestRoot) continue;
        FVector chestPos = cf::proc::R<FVector>(chestRoot + cf::off::Scene_RelativeLocation);

        FireCannonWeaponAt(lp, weapon, chestPos);
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
