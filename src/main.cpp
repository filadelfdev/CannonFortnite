// main.cpp — in-process boot sequence. Runs on the worker thread that
// DllMain spawned. By the time we get here the game is up (DllMain
// slept 1500 ms first), so GWorld + GNames are safe to read.
//
// Pipeline:
//   1. seed the behaviour jitter RNG
//   2. resolve engine globals (offsets.h FILL placeholders)
//   3. install the D3D11 Present hook so the overlay can render on
//      the game's own swapchain
//   4. start the worker loop that feeds the feature ticks
//
// Attribution: Built by Olivia — https://discord.gg/ojailbreaks

#include "core/process.h"
#include "sdk/engine.h"
#include "sdk/offsets.h"
#include "antiban/jitter.h"
#include "features/cannon_chest.h"
#include "features/speed.h"
#include "features/esp.h"
#include "features/loot.h"
#include "features/aimbot.h"
#include "overlay/overlay.h"
#include "util/log.h"
#include "util/report.h"

#include <windows.h>
#include <thread>
#include <atomic>
#include <vector>
#include <mutex>
#include <chrono>

namespace cf {

namespace {

std::atomic<bool> g_run{true};
std::atomic<int>  g_tick{0};
std::vector<cf::Actor> g_actors;
std::mutex g_actorsLock;

void WorkerLoop(){
    uint32_t frame = 0;
    std::vector<cf::Actor> cached;
    cached.reserve(2048);

    while(g_run.load()){
        cf::LocalPlayerCache lp{};
        if(!cf::sdk::ReadLocalPlayer(lp)){
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
            continue;
        }

        if((frame % 80) == 0) cf::sdk::InvalidateClassCache();
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

                    uintptr_t root = cf::proc::R<uintptr_t>(p + cf::off::Pawn_RootComponent);
                    if(!root) continue;
                    a.pos      = cf::proc::R<cf::FVector>(root + cf::off::Scene_RelativeLocation);
                    a.distance = (a.pos - lp.pos).Len() / 100.f;
                    cached.push_back(a);
                }
            }
        } else {
            for(auto& a : cached) a.distance = (a.pos - lp.pos).Len() / 100.f;
        }

        {
            std::scoped_lock _(g_actorsLock);
            g_actors = cached;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(4));
    }
}

void OverlayTick(float dt, float sw, float sh){
    std::vector<cf::Actor> snap;
    { std::scoped_lock _(g_actorsLock); snap = g_actors; }

    cf::LocalPlayerCache lp{};
    cf::sdk::ReadLocalPlayer(lp);

    int t = g_tick.fetch_add(1);
    cf::feat::TickSpeed(lp, t);
    cf::feat::TickLoot (lp, snap, dt);
    cf::feat::TickMats (lp);
    cf::feat::TickAim  (lp, snap, dt);
    cf::feat::Tick     (lp, snap, dt);

    auto dl = cf::feat::BuildDrawList(lp, snap, sw, sh);
    cf::ov::PublishDrawList(std::move(dl));
}

} // namespace

int Run(){
    cf::log::SetLevel(cf::log::Level::Info);
    CF_INFO("cheat :: boot (internal, PID=%lu)", GetCurrentProcessId());

    cf::ab::jitter::Seed();

    if(!cf::sdk::ResolveGlobals()){
        CF_ERR("engine globals unresolved — offsets.h still carries placeholders.");
        CF_ERR("overlay will boot, but features won't operate until offsets are real.");
    }

    std::thread worker(WorkerLoop);

    int rc = cf::ov::Run(OverlayTick);

    g_run.store(false);
    if(worker.joinable()) worker.join();

    cf::report::SaveToFile();
    CF_INFO("cheat :: shutdown rc=%d smashed=%u", rc, cf::feat::SmashedThisSession());
    return rc;
}

} // namespace cf
