#include "antiban/heartbeat.h"
#include "antiban/hash.h"
#include "antiban/antiban.h"
#include "util/log.h"

#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <chrono>
#include <thread>
#include <atomic>
#include <vector>
#include <mutex>
#include <cstring>

namespace cf::ab {

namespace {

struct PinnedModule {
    uintptr_t base{};
    size_t    size{};
    std::vector<uint8_t> pristine;   // snapshot of the AC module's .text
};

std::atomic<bool> g_run{false};
std::atomic<bool> g_armed{false};
std::mutex        g_lock;
std::vector<PinnedModule> g_pins;
std::thread       g_worker;

bool PinOne(HMODULE m){
    MODULEINFO mi{};
    if(!GetModuleInformation(GetCurrentProcess(), m, &mi, sizeof(mi))) return false;

    // Walk PE headers → find .text; snapshot its bytes so the heartbeat
    // worker can re-write any corruption before the AC enumerator sees it.
    auto base = (uintptr_t)m;
    auto dos  = (PIMAGE_DOS_HEADER)base;
    auto nt   = (PIMAGE_NT_HEADERS)(base + dos->e_lfanew);
    auto sec  = IMAGE_FIRST_SECTION(nt);

    for(int i=0;i<nt->FileHeader.NumberOfSections;++i, ++sec){
        if(memcmp(sec->Name, ".text", 5) != 0) continue;
        PinnedModule p{};
        p.base = base + sec->VirtualAddress;
        p.size = sec->Misc.VirtualSize;
        p.pristine.assign(p.size, 0);
        memcpy(p.pristine.data(), (void*)p.base, p.size);
        std::scoped_lock _(g_lock);
        g_pins.push_back(std::move(p));
        return true;
    }
    return false;
}

void Rearm(){
    std::scoped_lock _(g_lock);
    for(auto& p : g_pins){
        DWORD old=0;
        VirtualProtect((void*)p.base, p.size, PAGE_EXECUTE_READWRITE, &old);
        // Only restore bytes that actually drifted — a full memcpy would
        // show up as a .text self-write to the integrity sweep.
        auto* src = (const uint8_t*)p.base;
        for(size_t i=0;i<p.size;++i){
            if(src[i] != p.pristine[i]){
                ((uint8_t*)p.base)[i] = p.pristine[i];
            }
        }
        VirtualProtect((void*)p.base, p.size, old, &old);
    }
}

void MaskThreadTrace(){
    // Short pass: flip the thread impersonation token off on every one
    // of our worker threads. Nothing imitates a legit user that way, so
    // the thread's stable ID becomes noise to the heartbeat snapshot.
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if(snap == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 te{sizeof(te)};
    uint32_t pid = GetCurrentProcessId();
    for(BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)){
        if(te.th32OwnerProcessID != pid) continue;
        HANDLE t = OpenThread(THREAD_SET_INFORMATION, FALSE, te.th32ThreadID);
        if(!t) continue;
        // Reset impersonation — token nulled out.
        HANDLE nullTok = nullptr;
        SetThreadToken(&t, nullTok);
        CloseHandle(t);
    }
    CloseHandle(snap);
}

void DecoyHandles(){
    // Create three throwaway named-object handles so the handle table
    // snapshot looks indistinguishable from a clean host. The OS
    // garbage-collects them when we exit.
    static std::atomic<bool> seeded{false};
    if(seeded.exchange(true)) return;
    CreateEventW   (nullptr, FALSE, FALSE, nullptr);
    CreateMutexW   (nullptr, FALSE, nullptr);
    CreateSemaphoreW(nullptr, 0, 1, nullptr);
}

void Worker(){
    HMODULE mods[1024]; DWORD need=0;
    if(!EnumProcessModulesEx(GetCurrentProcess(), mods, sizeof(mods), &need, LIST_MODULES_64BIT))
        return;

    wchar_t name[MAX_PATH];
    for(DWORD i=0;i<need/sizeof(HMODULE);++i){
        if(!GetModuleBaseNameW(GetCurrentProcess(), mods[i], name, MAX_PATH)) continue;
        uint64_t h = fnv1a_w(name);
        if(h == fnv1a_w(L"easyanticheat_x64.dll") ||
           h == fnv1a_w(L"easyanticheat.dll")      ||
           h == fnv1a_w(L"beclient_x64.dll")){
            if(PinOne(mods[i])){ CF_INFO("heartbeat: pinned AC module"); g_armed.store(true); }
        }
    }

    DecoyHandles();

    // Heartbeat loop — rearm every 200ms with a ±80ms jitter so it
    // never lines up with any single AC poll interval.
    uint32_t seed = 0xA5A5A5A5;
    while(g_run.load()){
        Rearm();
        MaskThreadTrace();
        seed = seed * 1664525u + 1013904223u;
        int ms = 120 + (seed & 0xFF);
        std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    }
}

} // namespace

void InstallHeartbeat(){
    if(g_run.exchange(true)) return;
    g_worker = std::thread(Worker);
    HideSelfThreads();   // hide the heartbeat thread immediately
}

void StopHeartbeat(){
    g_run.store(false);
    if(g_worker.joinable()) g_worker.join();
    g_armed.store(false);
}

bool HeartbeatArmed(){ return g_armed.load(); }

} // namespace cf::ab
