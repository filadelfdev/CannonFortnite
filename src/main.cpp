// CF — cannon-chest ripper entry point.
//
// Pipeline:
//   1. antiban init (VEH, header zero, thread hide, AC detect)
//   2. attach to the game via handle duplication (no OpenProcess)
//   3. resolve engine globals, start the actor-enumeration worker
//   4. run the overlay on this thread; worker feeds its draw list and
//      runs cannon_chest + speed each tick.
//   5. shutdown: detach memory, restore real HWID if the spoofer was armed.
//
// Attribution: Built by Olivia — https://discord.gg/ojailbreaks

#include "core/memory.h"
#include "core/kmem.h"
#include "sdk/engine.h"
#include "sdk/offsets.h"
#include "features/cannon_chest.h"
#include "features/route.h"
#include "features/speed.h"
#include "features/esp.h"
#include "features/loot.h"
#include "features/aimbot.h"
#include "overlay/overlay.h"
#include "antiban/antiban.h"
#include "antiban/hash.h"
#include "antiban/heartbeat.h"
#include "spoofer/spoofer.h"
#include "util/log.h"

#include <windows.h>
#include <thread>
#include <atomic>
#include <vector>
#include <mutex>
#include <chrono>

namespace {

std::atomic<bool> g_run{true};
std::atomic<int>  g_tick{0};
std::vector<cf::Actor> g_actors;
std::mutex g_actorsLock;

void WorkerLoop(){
    // Dual-cadence loop:
    //  * every 4ms  — refresh local player (hot path, feeds route hops)
    //  * every 50ms — re-enumerate the full actor list (expensive)
    // The route planner pulls a stable snapshot each tick and never
    // walks the live UWorld array itself.
    uint32_t frame = 0;
    std::vector<cf::Actor> cached;
    cached.reserve(2048);

    while(g_run.load()){
        cf::LocalPlayerCache lp{};
        if(!cf::sdk::ReadLocalPlayer(lp)){
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
            continue;
        }

        // Full re-enum every 6th tick (~24ms). Faster than before so
        // freshly-spawned chests enter the queue inside one cannon
        // cooldown window at 5+/s smash rate.
        if((frame++ % 6) == 0){
            std::vector<uintptr_t> ptrs;
            if(cf::sdk::EnumerateActors(ptrs)){
                cached.clear();
                for(auto p : ptrs){
                    cf::Actor a{};
                    a.addr      = p;
                    a.className = cf::sdk::ActorClassName(p);
                    cf::sdk::ClassifyActor(a);
                    if(!(a.isChest || a.isCannon || a.isPlayer || a.isLootDrop)) continue;

                    uintptr_t root = cf::mem::R<uintptr_t>(p + cf::off::Pawn_RootComponent);
                    if(!root) continue;
                    a.pos      = cf::mem::R<cf::FVector>(root + cf::off::Scene_RelativeLocation);
                    a.distance = (a.pos - lp.pos).Len()/100.f;
                    cached.push_back(a);
                }
            }
        } else {
            // Light refresh — update distance only, no class re-walk.
            for(auto& a : cached) a.distance = (a.pos - lp.pos).Len()/100.f;
        }

        {
            std::scoped_lock _(g_actorsLock);
            g_actors = cached;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(4));
    }
}

void PerFrame(float dt, float sw, float sh){
    std::vector<cf::Actor> snap;
    { std::scoped_lock _(g_actorsLock); snap = g_actors; }

    cf::LocalPlayerCache lp{};
    cf::sdk::ReadLocalPlayer(lp);

    int t = g_tick.fetch_add(1);

    cf::feat::TickSpeed(lp, t);
    cf::feat::TickLoot (lp, snap, dt);
    cf::feat::TickMats (lp);
    cf::feat::TickAim  (lp, snap, dt);

    auto mode = cf::feat::g_routeCfg.mode.load();
    if(mode == cf::feat::RouteMode::CannonSmash){
        // Primary path — teleport cannon above each chest, smash, token.
        cf::feat::Tick(lp, snap, dt);
    } else {
        // Secondary paths — player teleport, gear only, no token.
        cf::feat::TickRoute(lp, snap, dt);
    }

    auto dl = cf::feat::BuildDrawList(lp, snap, sw, sh);
    cf::ov::PublishDrawList(std::move(dl));
}

} // namespace

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int){
    cf::log::SetLevel(cf::log::Level::Info);
    CF_INFO("cf :: startup");

    // Antiban init first — zero header before anything else enumerates it.
    cf::ab::InstallVEH();
    cf::ab::ZeroOwnHeader();
    cf::ab::HideSelfThreads();

    auto ac = cf::ab::DetectAnticheat();
    if(ac != cf::ab::AcKind::None){
        CF_WARN("ac detected (%d) — heartbeat spoof engaging",(int)ac);
        cf::ab::InstallHeartbeat();
    }

    // Image-hash of the target executable, FILL per build. The caller
    // precomputes fnv1a_w(L"FortniteClient-Win64-Shipping.exe").
    // Shipped here as a literal hex so the string "Fortnite..." never
    // lands in .rdata.
    const wchar_t* targetHash = L"c6b2f4a9e1d87203";   // FILL per patch
    bool ok = false;
    for(int i=0;i<60 && !ok;++i){
        ok = cf::mem::Attach(targetHash);
        if(!ok) std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    if(!ok){
        CF_ERR("attach failed — target not running or hash mismatch");
        return 2;
    }

    // Try to open the kernel driver; if \\.\CfRead answers, arm it with
    // the target image hash and route every memory op through MmCopyVM.
    // If it fails (driver not loaded), user-mode path stays live.
    if(cf::kmem::Open()){
        uint64_t targetHashNum = std::wcstoull(targetHash, nullptr, 16);
        if(cf::kmem::Arm(targetHashNum)){
            cf::mem::UseKernel(true);
            CF_INFO("kernel read path online");
        }
    }

    if(!cf::sdk::ResolveGlobals()){
        CF_ERR("engine globals unresolved — check offsets.h");
        cf::mem::Detach();
        return 3;
    }

    // Spoofer — stash the real fingerprint so panic restores on exit.
    cf::spoof::StashReal();

    std::thread worker(WorkerLoop);

    int rc = cf::ov::Run(PerFrame);

    g_run.store(false);
    if(worker.joinable()) worker.join();

    cf::ab::StopHeartbeat();
    cf::kmem::Close();
    cf::mem::Detach();
    cf::spoof::RestoreReal();
    CF_INFO("cf :: shutdown rc=%d opened=%u", rc, cf::feat::Looted());
    return rc;
}
