#include "sdk/engine.h"
#include "sdk/offsets.h"
#include "core/process.h"
#include "antiban/hash.h"

#include <unordered_map>
#include <mutex>
#include <vector>
#include <cmath>

namespace cf::sdk {
namespace {

struct Globals {
    uintptr_t gWorld{};
    uintptr_t gNames{};
    uintptr_t gObjects{};
    // ProcessEvent dispatcher — the engine's universal function-call
    // entry. Resolved by the vtable slot on the local player's UObject
    // once (any UObject's vtable has the same ProcessEvent slot). The
    // helper falls back to a pattern scan if the vtable read fails.
    uintptr_t fnProcessEvent{};
    bool      resolved{};
    std::mutex lock;
} g;

// --- UFunction lookup cache ----------------------------------------
//
// Walking UClass->FuncMap every call would be O(buckets) per invoke;
// instead each (uclass, funcName) pair is memoised. Keyed by uclass to
// survive instance changes of the same class.
struct FuncKey {
    uintptr_t uclass;
    uint64_t  namehash;
    bool operator==(const FuncKey& o) const {
        return uclass==o.uclass && namehash==o.namehash;
    }
};
struct FuncKeyHash {
    size_t operator()(const FuncKey& k) const noexcept {
        return std::hash<uintptr_t>{}(k.uclass) ^ (k.namehash * 0x9E3779B97F4A7C15ull);
    }
};
std::unordered_map<FuncKey, uintptr_t, FuncKeyHash> g_funcCache;
std::mutex g_funcCacheLock;

std::unordered_map<uint32_t, std::string> g_nameCache;
std::unordered_map<uintptr_t, std::string> g_classCache;
std::mutex g_cacheLock;

// FName-to-string: UE5 chunked GNames, two-level pointer array of 0x4000
// entries each, each entry a header + ANSI string.
// Reads happen WITHOUT the cache lock held — a syscall under lock
// blocks every other sdk reader.
std::string ReadFNameChunked(uint32_t index){
    // 1. Cache hit?
    {
        std::scoped_lock _(g_cacheLock);
        auto it = g_nameCache.find(index);
        if(it != g_nameCache.end()) return it->second;
    }

    // 2. Cold read — outside the lock.
    const uint32_t chunk    = index >> 16;
    const uint32_t offset   = (index & 0xFFFF) << 1;
    uintptr_t chunkPtrAddr  = g.gNames + 0x10 + (uintptr_t)chunk * 8;
    uintptr_t chunkBase     = cf::proc::R<uintptr_t>(chunkPtrAddr);
    std::string out;
    if(chunkBase){
        uintptr_t entryAddr = chunkBase + offset;
        uint16_t header = cf::proc::R<uint16_t>(entryAddr);
        uint16_t len    = header >> 6;
        if(len > 0 && len <= 256){
            char buf[257]{};
            cf::proc::Read(entryAddr + 2, buf, len);
            buf[len] = 0;
            out.assign(buf, len);
        }
    }

    // 3. Store — even empty entries so we don't re-syscall every tick.
    std::scoped_lock _(g_cacheLock);
    g_nameCache[index] = out;
    return out;
}

} // namespace

bool ResolveGlobals(){
    std::scoped_lock _(g.lock);
    if(g.resolved) return true;
    uintptr_t base = cf::proc::Base();
    if(!base) return false;
    g.gWorld   = base + cf::off::G_WORLD;
    g.gNames   = base + cf::off::G_NAMES;
    g.gObjects = base + cf::off::G_OBJECTS;

    // ProcessEvent discovery. Two strategies, tried in order:
    //   1. Read the vtable slot off any resolvable UObject (local
    //      player's controller is the earliest stable one; done lazily
    //      the first time CallUFunction is invoked, since world isn't
    //      populated yet on the first ResolveGlobals call).
    //   2. Pattern-scan the main module for the ProcessEvent prologue.
    //      Signature covers the common UE5 build.
    g.fnProcessEvent = cf::proc::Pattern(
        "40 55 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 ?? ?? ?? ?? 48 81 EC");
    // fnProcessEvent may legitimately be 0 here — the lazy vtable
    // read in CallUFunction will fill it on the first real call.

    g.resolved = true;
    return true;
}

std::string FNameToString(uint32_t fnameIndex){
    return ReadFNameChunked(fnameIndex);
}

std::string ActorClassName(uintptr_t actor){
    if(!actor) return {};
    // Note: the UAF guard here is InvalidateClassCache() being called on
    // every full re-enum — if an actor address gets GC'd and reused we
    // flush the cache before new addresses can collide.
    {
        std::scoped_lock _(g_cacheLock);
        auto it = g_classCache.find(actor);
        if(it!=g_classCache.end()) return it->second;
    }
    uintptr_t uclass = cf::proc::R<uintptr_t>(actor + cf::off::UObject_ClassPrivate);
    if(!uclass) return {};
    uint32_t nameIdx = cf::proc::R<uint32_t>(uclass + cf::off::UObject_NamePrivate);
    std::string n = FNameToString(nameIdx);
    std::scoped_lock _(g_cacheLock);
    g_classCache[actor] = n;
    return n;
}

void InvalidateClassCache(){
    std::scoped_lock _(g_cacheLock);
    g_classCache.clear();
}

bool EnumerateActors(std::vector<uintptr_t>& out){
    out.clear();
    if(!g.resolved && !ResolveGlobals()) return false;

    uintptr_t world = cf::proc::R<uintptr_t>(g.gWorld);
    if(!world) return false;

    // Persistent + streaming levels.
    struct TArr { uintptr_t data; int32_t num; int32_t max; };
    TArr levels = cf::proc::R<TArr>(world + cf::off::UWorld_Levels);
    if(levels.num<=0 || levels.num>256) return false;

    out.reserve(4096);
    for(int i=0;i<levels.num;++i){
        uintptr_t level = cf::proc::R<uintptr_t>(levels.data + i*8);
        if(!level) continue;

        uintptr_t actorsArr = level + cf::off::ULevel_Actors;
        TArr arr = cf::proc::R<TArr>(actorsArr);
        if(arr.num<=0 || arr.num>65536) continue;

        std::vector<uintptr_t> page(arr.num);
        if(!cf::proc::Read(arr.data, page.data(), arr.num*sizeof(uintptr_t))) continue;
        for(auto a : page) if(a) out.push_back(a);
    }
    return true;
}

void ClassifyActor(Actor& a){
    uint64_t h = cf::ab::fnv1a_lower(a.className.c_str());

    // Normal breakable chests — ServerInteract target, +600 XP per open.
    constexpr uint64_t H_Tiered_Chest   = cf::ab::fnv1a_lower("Tiered_Chest_Athena_C");
    constexpr uint64_t H_Tiered_Ammo    = cf::ab::fnv1a_lower("Tiered_Ammo_Athena_C");
    constexpr uint64_t H_BuildContainer = cf::ab::fnv1a_lower("BuildingContainer_C");
    constexpr uint64_t H_RareChest      = cf::ab::fnv1a_lower("Tiered_Chest_Rare_Athena_C");
    constexpr uint64_t H_PirateChest    = cf::ab::fnv1a_lower("Tiered_Chest_Pirate_Athena_C");
    // Reference comp (Nov 2026 Treasure Hunters) also shows the golden
    // skull "Sprite Chest" and "Pirate Chest" opening via proximity —
    // treat both as normal chests, NOT vaults. Only the vault chests
    // behind a locked door need a key.
    constexpr uint64_t H_Sprite_Chest   = cf::ab::fnv1a_lower("Tiered_Sprite_Chest_Athena_C");
    constexpr uint64_t H_Pirate_Golden  = cf::ab::fnv1a_lower("Tiered_PirateChest_Athena_C");

    // Vault chests — only openable via Vault Key, not interact.
    constexpr uint64_t H_VaultChest1    = cf::ab::fnv1a_lower("Tiered_Vault_Chest_Pirate_Athena_C");
    constexpr uint64_t H_VaultChest2    = cf::ab::fnv1a_lower("Vault_Chest_Pirate_C");
    constexpr uint64_t H_VaultChest3    = cf::ab::fnv1a_lower("Pirate_Vault_Chest_C");
    constexpr uint64_t H_VaultDoor      = cf::ab::fnv1a_lower("Pirate_Vault_Door_C");

    constexpr uint64_t H_SupplyDrop     = cf::ab::fnv1a_lower("AthenaSupplyDrop_C");
    constexpr uint64_t H_Cannon         = cf::ab::fnv1a_lower("Pirate_Cannon_VH_C");
    constexpr uint64_t H_CannonAlt      = cf::ab::fnv1a_lower("Vehicle_Pirate_Cannon_C");
    constexpr uint64_t H_FortPawn       = cf::ab::fnv1a_lower("FortPlayerPawnAthena_C");

    a.isVaultChest = (h==H_VaultChest1 || h==H_VaultChest2 ||
                      h==H_VaultChest3 || h==H_VaultDoor);
    a.isChest    = !a.isVaultChest &&
                   (h==H_Tiered_Chest || h==H_Tiered_Ammo ||
                    h==H_BuildContainer || h==H_RareChest ||
                    h==H_PirateChest || h==H_Sprite_Chest ||
                    h==H_Pirate_Golden);
    a.isLootDrop = (h==H_SupplyDrop);
    a.isCannon   = (h==H_Cannon || h==H_CannonAlt);
    a.isPlayer   = (h==H_FortPawn);
}

bool ReadLocalPlayer(LocalPlayerCache& lp){
    if(!g.resolved && !ResolveGlobals()) return false;

    uintptr_t world = cf::proc::R<uintptr_t>(g.gWorld);
    if(!world) return false;

    uintptr_t gi = cf::proc::R<uintptr_t>(world + cf::off::UWorld_OwningGameInstance);
    if(!gi) return false;

    struct TArr { uintptr_t data; int32_t num; int32_t max; };
    TArr players = cf::proc::R<TArr>(gi + cf::off::GameInstance_LocalPlayers);
    if(players.num<=0) return false;

    uintptr_t localPlayer = cf::proc::R<uintptr_t>(players.data);
    if(!localPlayer) return false;

    lp.controller  = cf::proc::R<uintptr_t>(localPlayer + cf::off::LocalPlayer_PlayerController);
    if(!lp.controller) return false;

    lp.pawn       = cf::proc::R<uintptr_t>(lp.controller + cf::off::PC_AcknowledgedPawn);
    uintptr_t cam = cf::proc::R<uintptr_t>(lp.controller + cf::off::PC_PlayerCameraMgr);

    if(lp.pawn){
        lp.rootComp     = cf::proc::R<uintptr_t>(lp.pawn + cf::off::Pawn_RootComponent);
        lp.movementComp = cf::proc::R<uintptr_t>(lp.pawn + cf::off::Character_CharacterMovement);
        if(lp.rootComp){
            lp.pos      = cf::proc::R<FVector>(lp.rootComp + cf::off::Scene_RelativeLocation);
            lp.velocity = cf::proc::R<FVector>(lp.rootComp + cf::off::Scene_ComponentVelocity);
        }
    }

    if(cam){
        FVector  camPos  = cf::proc::R<FVector> (cam + cf::off::Cam_CameraCachePrivate + 0x10);
        FRotator camRot  = cf::proc::R<FRotator>(cam + cf::off::Cam_CameraCachePrivate + 0x10 + 0x0C);
        float    fov     = cf::proc::R<float>   (cam + cf::off::Cam_CameraCachePrivate + 0x10 + 0x30);
        lp.camPos = camPos; lp.camRot = camRot; lp.fov = fov;

        // Build view matrix from rotation (yaw, pitch, roll in degrees).
        float cy=std::cos(camRot.yaw  *0.017453292f), sy=std::sin(camRot.yaw  *0.017453292f);
        float cp=std::cos(camRot.pitch*0.017453292f), sp=std::sin(camRot.pitch*0.017453292f);
        float cr=std::cos(camRot.roll *0.017453292f), sr=std::sin(camRot.roll *0.017453292f);

        FMatrix& m = lp.view;
        m.m[0][0]=cp*cy;                 m.m[0][1]=cp*sy;                 m.m[0][2]=sp;
        m.m[1][0]=sr*sp*cy - cr*sy;      m.m[1][1]=sr*sp*sy + cr*cy;      m.m[1][2]=-sr*cp;
        m.m[2][0]=-(cr*sp*cy + sr*sy);   m.m[2][1]=cy*sr - cr*sp*sy;      m.m[2][2]=cr*cp;
        m.m[3][3]=1;
    }
    return lp.pawn!=0 && cam!=0;
}

bool WorldToScreen(const LocalPlayerCache& lp,
                   const FVector& w, float& sx, float& sy,
                   float screenW, float screenH){
    FVector d = w - lp.camPos;
    FVector ax{lp.view.m[0][0], lp.view.m[0][1], lp.view.m[0][2]};
    FVector ay{lp.view.m[1][0], lp.view.m[1][1], lp.view.m[1][2]};
    FVector az{lp.view.m[2][0], lp.view.m[2][1], lp.view.m[2][2]};
    float zx = d.x*ax.x + d.y*ax.y + d.z*ax.z;
    if(zx < 1.0f) return false;
    float zy = d.x*ay.x + d.y*ay.y + d.z*ay.z;
    float zz = d.x*az.x + d.y*az.y + d.z*az.z;
    float tanH = std::tan(lp.fov*0.5f*0.017453292f);
    sx = (screenW*0.5f) + zy/(zx*tanH) * (screenW*0.5f);
    sy = (screenH*0.5f) - zz/(zx*tanH*(screenH/screenW)) * (screenH*0.5f);
    return sx>=0 && sx<=screenW && sy>=0 && sy<=screenH;
}

namespace {

// Walk a UClass's Children list looking for a UFunction whose UObject
// name matches namehash (lowercased fnv1a). Follows SuperStruct up the
// inheritance chain so a function declared on APawn is reachable from
// AFortPlayerPawnAthena. Returns the UFunction* or 0.
uintptr_t FindUFunctionByHash(uintptr_t uclass, uint64_t namehash){
    while(uclass){
        uintptr_t child = cf::proc::R<uintptr_t>(uclass + cf::off::UStruct_Children);
        while(child){
            uint32_t nameIdx = cf::proc::R<uint32_t>(child + cf::off::UObject_NamePrivate);
            std::string n = FNameToString(nameIdx);
            if(!n.empty() && cf::ab::fnv1a_lower(n.c_str()) == namehash){
                return child;
            }
            child = cf::proc::R<uintptr_t>(child + cf::off::UField_Next);
        }
        uclass = cf::proc::R<uintptr_t>(uclass + cf::off::UStruct_SuperStruct);
    }
    return 0;
}

// Resolve ProcessEvent lazily off an object's vtable the first time
// we need it. The slot index is the dumped UObject_VTable_ProcessEvent
// offset; falls back to the pattern-scan result if the slot read 0.
uintptr_t ResolveProcessEvent(uintptr_t obj){
    if(g.fnProcessEvent) return g.fnProcessEvent;
    if(!obj) return 0;
    uintptr_t vtbl = cf::proc::R<uintptr_t>(obj);
    if(!vtbl) return 0;
    uintptr_t fn = cf::proc::R<uintptr_t>(vtbl + cf::off::UObject_VTable_ProcessEvent);
    if(fn){
        std::scoped_lock _(g.lock);
        g.fnProcessEvent = fn;
    }
    return fn;
}

} // namespace

bool CallUFunction(uintptr_t object, const char* funcName, void* params){
    if(!object || !funcName || !*funcName) return false;

    const uint64_t h = cf::ab::fnv1a_lower(funcName);
    uintptr_t uclass = cf::proc::R<uintptr_t>(object + cf::off::UObject_ClassPrivate);
    if(!uclass) return false;

    // 1. Resolve the UFunction* (cached per class+name).
    uintptr_t ufunc = 0;
    {
        std::scoped_lock _(g_funcCacheLock);
        auto it = g_funcCache.find(FuncKey{uclass, h});
        if(it != g_funcCache.end()) ufunc = it->second;
    }
    if(!ufunc){
        ufunc = FindUFunctionByHash(uclass, h);
        std::scoped_lock _(g_funcCacheLock);
        g_funcCache[FuncKey{uclass, h}] = ufunc;
    }
    if(!ufunc) return false;

    // 2. Resolve ProcessEvent — vtable off the object we're calling on.
    uintptr_t pe = ResolveProcessEvent(object);
    if(!pe) return false;

    // 3. Thunk: ProcessEvent is a non-static member — Microsoft x64 ABI
    //    passes `this` in RCX, so a plain C call with (object, ufunc,
    //    params) in RCX/RDX/R8 maps directly onto the member signature.
    using PE = void (__fastcall*)(uintptr_t self, uintptr_t fn, void* p);
    reinterpret_cast<PE>(pe)(object, ufunc, params);
    return true;
}

} // namespace cf::sdk
