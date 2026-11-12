#include "antiban/rpc_path.h"
#include "core/memory.h"
#include "core/kmem.h"
#include "sdk/offsets.h"
#include "util/log.h"

#include <windows.h>
#include <psapi.h>
#include <mutex>
#include <atomic>
#include <cstring>

namespace cf::ab::rpc {

namespace {

struct Fn {
    uintptr_t addr{};
    uint32_t  argBytes{};
    bool      ok() const { return addr != 0; }
};

struct Resolved {
    Fn cannonFire;
    Fn cannonMove;
    Fn cannonSeat;
    Fn chestDamage;
    Fn chestInteract;
    Fn pawnMove;
    Fn pickupGrab;
    std::atomic<bool> ready{false};
    std::mutex lock;
} r;

struct Sig { const char* pat; const char* mask; uint32_t argBytes; };

// Signatures — IDA-dumped against C7S1 build 33.10. Live in one block
// the same way sdk/offsets.h does so a patch is a one-file rebuild.
const Sig SIG_CannonFire   = { "\x48\x89\x5C\x24\x00\x57\x48\x83\xEC\x30\x8B\xDA\x48\x8B\xF9\xE8\x00\x00\x00\x00",
                               "xxxx?xxxxxxxxxxx????", 0x10 };
const Sig SIG_CannonMove   = { "\x48\x8B\xC4\x48\x89\x58\x18\x55\x56\x57\x48\x83\xEC\x40\x48\x8B\xF1",
                               "xxxxxxxxxxxxxxxxx", 0x1C };
const Sig SIG_CannonSeat   = { "\x40\x53\x48\x83\xEC\x30\x48\x8B\xDA\x48\x8B\xCA",
                               "xxxxxxxxxxxx", 0x10 };
const Sig SIG_ChestDamage  = { "\x48\x89\x5C\x24\x00\x48\x89\x74\x24\x00\x57\x48\x83\xEC\x20\x41\x8B\xD8\x48\x8B\xFA",
                               "xxxx?xxxx?xxxxxxxxxxx", 0x10 };
// ServerInteract on AFortBuildingContainer — the Nov 2026 chest
// opening path. Called with (this=chest, pawn=player) from inside
// cannon context.
const Sig SIG_ChestInteract= { "\x40\x53\x48\x83\xEC\x20\x48\x8B\xDA\x48\x8B\xCA\xE8",
                               "xxxxxxxxxxxxx", 0x10 };
const Sig SIG_PawnMove     = { "\x48\x8B\xC4\x55\x41\x55\x41\x56\x48\x8D\x68\xA1\x48\x81\xEC",
                               "xxxxxxxxxxxxxxx", 0x30 };
const Sig SIG_PickupGrab   = { "\x40\x53\x48\x83\xEC\x20\x48\x8B\xDA\x48\x8B\xCA\xE8\x00\x00\x00\x00",
                               "xxxxxxxxxxxxx????", 0x08 };

uintptr_t Scan(const Sig& s){
    uintptr_t base = cf::mem::Base();
    if(!base) return 0;
    HMODULE m = GetModuleHandleW(nullptr);
    MODULEINFO mi{};
    GetModuleInformation(GetCurrentProcess(), m, &mi, sizeof(mi));
    return cf::mem::Pattern(base, mi.SizeOfImage, s.pat, s.mask);
}

// Driver IOCTL — defined in driver/kread.c's v4 extension. Sends
// {fn, argBytes, bytes...} to the kernel, which schedules a work-item
// bound to the game's render-thread APC queue so the call executes
// inside the game's own tick.
#include "../../driver/cf_ioctl.h"
constexpr uint32_t IOCTL_CF_INVOKE = CF_INVOKE;

struct InvokeWire {
    uint64_t fn;
    uint32_t argBytes;
    uint32_t pad;
    uint8_t  args[256];
};

bool KernelInvoke(uintptr_t fn, const void* argsBlock, uint32_t argBytes){
    if(!fn || !cf::kmem::Online() || argBytes > sizeof(InvokeWire::args)) return false;
    InvokeWire w{};
    w.fn       = (uint64_t)fn;
    w.argBytes = argBytes;
    std::memcpy(w.args, argsBlock, argBytes);
    // Routed through the same \\.\CfRead device the kernel memory
    // bridge uses — the driver dispatches on IoControlCode.
    HANDLE h = CreateFileW(L"\\\\.\\CfRead", GENERIC_WRITE,
                           FILE_SHARE_READ|FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, 0, nullptr);
    if(h == INVALID_HANDLE_VALUE) return false;
    DWORD ret = 0;
    bool ok = DeviceIoControl(h, IOCTL_CF_INVOKE, &w, sizeof(w),
                              nullptr, 0, &ret, nullptr) != 0;
    CloseHandle(h);
    return ok;
}

} // namespace

bool Resolve(){
    std::scoped_lock _(r.lock);
    r.cannonFire.addr    = Scan(SIG_CannonFire);   r.cannonFire.argBytes    = SIG_CannonFire.argBytes;
    r.cannonMove.addr    = Scan(SIG_CannonMove);   r.cannonMove.argBytes    = SIG_CannonMove.argBytes;
    r.cannonSeat.addr    = Scan(SIG_CannonSeat);   r.cannonSeat.argBytes    = SIG_CannonSeat.argBytes;
    r.chestDamage.addr   = Scan(SIG_ChestDamage);  r.chestDamage.argBytes   = SIG_ChestDamage.argBytes;
    r.chestInteract.addr = Scan(SIG_ChestInteract);r.chestInteract.argBytes = SIG_ChestInteract.argBytes;
    r.pawnMove.addr      = Scan(SIG_PawnMove);     r.pawnMove.argBytes      = SIG_PawnMove.argBytes;
    r.pickupGrab.addr    = Scan(SIG_PickupGrab);   r.pickupGrab.argBytes    = SIG_PickupGrab.argBytes;

    bool core = r.cannonFire.ok() && r.cannonMove.ok() &&
                r.chestDamage.ok() && r.pawnMove.ok();
    r.ready.store(core && cf::kmem::Online());
    if(r.ready.load()) CF_INFO("rpc path armed (kernel dispatch)");
    else               CF_WARN("rpc path offline — direct-write fallback live");
    return r.ready.load();
}

bool Ready(){ return r.ready.load(); }

#pragma pack(push, 1)
struct ArgsCannonFire { uintptr_t self; float launch; FRotator aim; };
struct ArgsCannonMove { uintptr_t self; FVector pos;  FRotator aim; };
struct ArgsCannonSeat { uintptr_t self; uintptr_t pawn; };
struct ArgsChestDmg   { uintptr_t self; uintptr_t cannon; float dmg; };
struct ArgsPawnMove   { uintptr_t self; FVector pos; FVector vel; uint16_t rot; };
struct ArgsPickupGrab { uintptr_t self; uintptr_t pawn; };
#pragma pack(pop)

bool CannonFire(uintptr_t c, float launch, const FRotator& aim){
    ArgsCannonFire a{c, launch, aim};
    return KernelInvoke(r.cannonFire.addr, &a, sizeof(a));
}
bool CannonMove(uintptr_t c, const FVector& p, const FRotator& aim){
    ArgsCannonMove a{c, p, aim};
    return KernelInvoke(r.cannonMove.addr, &a, sizeof(a));
}
bool CannonSeat(uintptr_t c, uintptr_t pawn){
    ArgsCannonSeat a{c, pawn};
    return KernelInvoke(r.cannonSeat.addr, &a, sizeof(a));
}
bool ChestTakeCannonDamage(uintptr_t chest, uintptr_t cannon, float dmg){
    ArgsChestDmg a{chest, cannon, dmg};
    return KernelInvoke(r.chestDamage.addr, &a, sizeof(a));
}

#pragma pack(push, 1)
struct ArgsChestInteract { uintptr_t self; uintptr_t pawn; };
#pragma pack(pop)

bool ChestServerInteract(uintptr_t chest, uintptr_t pawn){
    ArgsChestInteract a{chest, pawn};
    return KernelInvoke(r.chestInteract.addr, &a, sizeof(a));
}
bool PawnServerMove(uintptr_t pawn, const FVector& p, const FVector& v, uint16_t rot){
    ArgsPawnMove a{pawn, p, v, rot};
    return KernelInvoke(r.pawnMove.addr, &a, sizeof(a));
}
bool PickupServerGrab(uintptr_t pickup, uintptr_t pawn){
    ArgsPickupGrab a{pickup, pawn};
    return KernelInvoke(r.pickupGrab.addr, &a, sizeof(a));
}

} // namespace cf::ab::rpc
