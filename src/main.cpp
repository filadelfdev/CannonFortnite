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
#include "util/report.h"
#include "antiban/antiban.h"
#include "antiban/hash.h"
#include "antiban/heartbeat.h"
#include "antiban/jitter.h"
#include "antiban/rpc_path.h"
#include "antiban/env_lie.h"
#include "antiban/dxgi_cloak.h"
#include "antiban/dxgi_kernel.h"
#include "util/log.h"
#include <fstream>
#include <ctime>
#include <string>
#include <shlobj.h>

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
const cf::ab::dxgi_k::Fingerprint* g_kernelCloakFp = nullptr;

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
        // Class-name cache flushes every ~500 enums (~2s) so a reused
        // actor address can't serve a stale class-name indefinitely.
        if((frame % (6 * 500)) == 0) cf::sdk::InvalidateClassCache();
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
    CF_INFO("cf :: startup v4");

    // v4 antiban init order matters:
    //   1. seed the jitter RNG  (used by every subsequent step)
    //   2. baseline .text       (SelfIntegrityOk needs this BEFORE zero)
    //   3. zero the PE header   (noise reduction, not integrity)
    //   4. install VEH          (eats BP/SS from any poke)
    //   5. hide worker threads
    cf::ab::jitter::Seed();
    cf::ab::InstallVEH();
    // v5: environment lies BEFORE anything else enumerates the box.
    // test-signing bit hidden, CfRead/CfSpoof invisible to NtEnumerateKey.
    cf::ab::envlie::InstallAll();
    // v5: DXGI adapter identity spoofed (rotator seeds it below).
    cf::ab::dxgi::Install();
    cf::ab::ZeroOwnHeader();
    cf::ab::HideSelfThreads();

    // HWID spoofer was split into a standalone binary (CannonSpoofer).
    // We ONLY read its marker here; the cheat never applies HWID itself.
    // If no marker is present, warn loud — running without a spoof is
    // the single fastest way to burn your real HWID.
    //
    // Marker path: %LOCALAPPDATA%\Microsoft\Windows\INetCache\.cf_spoof.marker
    //              (written by spoof-apply.exe from the standalone repo)
    {
        std::ifstream mf([]{
            wchar_t p[MAX_PATH]{};
            SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, p);
            std::wstring s = p;
            s += L"\\Microsoft\\Windows\\INetCache\\.cf_spoof.marker";
            return s;
        }().c_str(), std::ios::binary);
        if(!mf){
            CF_ERR("==========================================================");
            CF_ERR(" SPOOFER NOT DETECTED");
            CF_ERR(" Run CannonSpoofer\\spoofer.exe → [1] apply, as admin, reboot,");
            CF_ERR(" THEN launch this cheat. Running without it = HWID burn risk.");
            CF_ERR("==========================================================");
        } else {
            struct { uint32_t magic; uint32_t ver; int32_t slot; int32_t pad;
                     int64_t appliedUnix; uint64_t fpHash; } b{};
            mf.read((char*)&b, sizeof(b));
            if(b.magic == 0xCF50F000){
                int64_t ageH = (std::time(nullptr) - b.appliedUnix) / 3600;
                CF_INFO("spoofer marker: slot %d, applied %lld h ago", b.slot, (long long)ageH);
            } else {
                CF_WARN("spoofer marker present but corrupt — rerun spoof-apply");
            }
        }
    }
    {
        // Keep the DXGI cloak fed. Pick the SAME slot the spoofer picked
        // (via marker) so GPU identity rotates with the HWID.
        // For now we re-derive from a hash of the marker's slot number.
        uint32_t seed = 0xDEADBEEF;
        {
            std::ifstream mf([]{
                wchar_t p[MAX_PATH]{};
                SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, p);
                std::wstring s = p;
                s += L"\\Microsoft\\Windows\\INetCache\\.cf_spoof.marker";
                return s;
            }().c_str(), std::ios::binary);
            if(mf){
                struct { uint32_t magic; uint32_t ver; int32_t slot; int32_t pad;
                         int64_t appliedUnix; uint64_t fpHash; } b{};
                mf.read((char*)&b, sizeof(b));
                if(b.magic == 0xCF50F000) seed = (uint32_t)(b.fpHash ^ (uint64_t)b.slot);
            }
        }
        // v5: feed DXGI cloak a deterministic adapter identity
        // derived from the slot so the GPU identity rotates with HWID.
        // GPU list tuned for a mid-low-tier host (RX 580 class). All
        // entries deliver 35-65fps range in Fortnite Chapter 7 UE5, so
        // the reported GPU matches the measured frame-time envelope
        // from the player's real hardware — no "high-end GPU on a
        // slow box" mismatch to flag.
        uint32_t seed = (uint32_t)cf::ab::fnv1a(fp.machineGuid.c_str());
        static const char* gpus[] = {
            "NVIDIA GeForce GTX 1060 6GB",
            "NVIDIA GeForce GTX 1650",
            "NVIDIA GeForce GTX 1660 SUPER",
            "NVIDIA GeForce RTX 3050",
            "AMD Radeon RX 580",
            "AMD Radeon RX 5500 XT",
            "AMD Radeon RX 6600",
        };
        static const unsigned ids[][4] = {
            {0x10DE, 0x1C03, 0x37161462, 0xA1},
            {0x10DE, 0x1F82, 0x12AD1043, 0xA1},
            {0x10DE, 0x21C4, 0x12AB1458, 0xA1},
            {0x10DE, 0x2507, 0x40901462, 0xA1},
            {0x1002, 0x67DF, 0xE366148C, 0xE7},
            {0x1002, 0x7340, 0x73403842, 0xCA},
            {0x1002, 0x73FF, 0xE4011DA2, 0xC1},
        };
        int pick = (int)(seed % (sizeof(gpus)/sizeof(gpus[0])));
        cf::ab::dxgi::SetReportedAdapter("GPU", gpus[pick],
            ids[pick][0], ids[pick][1], ids[pick][2], ids[pick][3]);
        CF_INFO("gpu cloak pick: %s (from spoofer marker seed)", gpus[pick]);

        static cf::ab::dxgi_k::Fingerprint s_kfp;
        size_t slen = 0;
        while(slen < 127 && gpus[pick][slen]) ++slen;
        for(size_t i=0;i<slen;++i) s_kfp.desc[i] = (wchar_t)gpus[pick][i];
        s_kfp.desc[slen] = 0;
        s_kfp.vendorId = ids[pick][0];
        s_kfp.deviceId = ids[pick][1];
        s_kfp.subsysId = ids[pick][2];
        s_kfp.revision = ids[pick][3];
        g_kernelCloakFp = &s_kfp;
    }

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
    // If it fails (driver not loaded), user-mode path stays live with
    // a loud warning — v4 defaults assume the kernel path so running
    // without it is a documented risk.
    bool kernelUp = false;
    if(cf::kmem::Open()){
        uint64_t targetHashNum = std::wcstoull(targetHash, nullptr, 16);
        if(cf::kmem::Arm(targetHashNum)){
            cf::mem::UseKernel(true);
            kernelUp = true;
            CF_INFO("kernel read path online");
        }
    }
    if(!kernelUp){
        CF_ERR("KERNEL DRIVER NOT LOADED — direct-write path only.");
        CF_ERR("v4 strongly recommends the driver; ban risk is significantly higher without it.");
        CF_ERR("Load with:  sc start CfRead  &&  sc start CfSpoof");
    }

    if(!cf::sdk::ResolveGlobals()){
        CF_ERR("engine globals unresolved — check offsets.h");
        cf::mem::Detach();
        return 3;
    }

    // Resolve the RPC call sites. Without these, cannon_chest falls
    // back to direct writes (v3 behaviour) — works, but ban risk up.
    if(!cf::ab::rpc::Resolve()){
        CF_WARN("RPC path offline — DIRECT WRITES ACTIVE — raise cooldown");
    }

    // Kernel-side DXGI cloak — the target-process hook that was bug #9.
    // Only runs if the driver is up AND we have the kernel fp staged by
    // the rotator above AND the vtable resolver returns something non-
    // stub. Falls through cleanly on any of those being absent.
    if(g_kernelCloakFp){
        cf::ab::dxgi_k::InstallInTarget(*g_kernelCloakFp);
    }

    // Spoofer — stash the real fingerprint so panic restores on exit.
    // (spoofer is a separate binary now — no StashReal here)

    std::thread worker(WorkerLoop);

    int rc = cf::ov::Run(PerFrame);

    g_run.store(false);
    if(worker.joinable()) worker.join();

    // Save session report before teardown.
    cf::report::SaveToFile();

    cf::ab::dxgi_k::RemoveFromTarget();
    cf::ab::StopHeartbeat();
    cf::kmem::Close();
    cf::mem::Detach();
    // (spoofer standalone handles RestoreReal via spoof-restore.exe)
    CF_INFO("cf :: shutdown rc=%d opened=%u", rc, cf::feat::Looted());
    return rc;
}
