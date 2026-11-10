#include "sdk/engine.h"
#include "sdk/offsets.h"
#include "core/memory.h"
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
    bool      resolved{};
    std::mutex lock;
} g;

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
    uintptr_t chunkBase     = cf::mem::R<uintptr_t>(chunkPtrAddr);
    std::string out;
    if(chunkBase){
        uintptr_t entryAddr = chunkBase + offset;
        uint16_t header = cf::mem::R<uint16_t>(entryAddr);
        uint16_t len    = header >> 6;
        if(len > 0 && len <= 256){
            char buf[257]{};
            cf::mem::Read(entryAddr + 2, buf, len);
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
    uintptr_t base = cf::mem::Base();
    if(!base) return false;
    g.gWorld   = base + cf::off::G_WORLD;
    g.gNames   = base + cf::off::G_NAMES;
    g.gObjects = base + cf::off::G_OBJECTS;
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
    uintptr_t uclass = cf::mem::R<uintptr_t>(actor + cf::off::UObject_ClassPrivate);
    if(!uclass) return {};
    uint32_t nameIdx = cf::mem::R<uint32_t>(uclass + cf::off::UObject_NamePrivate);
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

    uintptr_t world = cf::mem::R<uintptr_t>(g.gWorld);
    if(!world) return false;

    // Persistent + streaming levels.
    struct TArr { uintptr_t data; int32_t num; int32_t max; };
    TArr levels = cf::mem::R<TArr>(world + cf::off::UWorld_Levels);
    if(levels.num<=0 || levels.num>256) return false;

    out.reserve(4096);
    for(int i=0;i<levels.num;++i){
        uintptr_t level = cf::mem::R<uintptr_t>(levels.data + i*8);
        if(!level) continue;

        uintptr_t actorsArr = level + cf::off::ULevel_Actors;
        TArr arr = cf::mem::R<TArr>(actorsArr);
        if(arr.num<=0 || arr.num>65536) continue;

        std::vector<uintptr_t> page(arr.num);
        if(!cf::mem::Read(arr.data, page.data(), arr.num*sizeof(uintptr_t))) continue;
        for(auto a : page) if(a) out.push_back(a);
    }
    return true;
}

void ClassifyActor(Actor& a){
    uint64_t h = cf::ab::fnv1a_lower(a.className.c_str());

    // Normal breakable chests — cannon-smashable, drops the token roll.
    constexpr uint64_t H_Tiered_Chest   = cf::ab::fnv1a_lower("Tiered_Chest_Athena_C");
    constexpr uint64_t H_Tiered_Ammo    = cf::ab::fnv1a_lower("Tiered_Ammo_Athena_C");
    constexpr uint64_t H_BuildContainer = cf::ab::fnv1a_lower("BuildingContainer_C");
    constexpr uint64_t H_RareChest      = cf::ab::fnv1a_lower("Tiered_Chest_Rare_Athena_C");
    constexpr uint64_t H_PirateChest    = cf::ab::fnv1a_lower("Tiered_Chest_Pirate_Athena_C");

    // Vault / Sprite chests — NEVER target. The cannon-break path no
    // longer applies to these (Nov 2026 patch); shooting one is wasted
    // ammo + a behaviour-classifier trigger.
    // FILL per-patch — class names below are best-known as of Nov 2026.
    constexpr uint64_t H_VaultChest1    = cf::ab::fnv1a_lower("Tiered_Vault_Chest_Pirate_Athena_C");
    constexpr uint64_t H_VaultChest2    = cf::ab::fnv1a_lower("Vault_Chest_Pirate_C");
    constexpr uint64_t H_VaultChest3    = cf::ab::fnv1a_lower("Pirate_Vault_Chest_C");
    constexpr uint64_t H_SpriteChest    = cf::ab::fnv1a_lower("Tiered_Sprite_Chest_Athena_C");
    constexpr uint64_t H_VaultDoor      = cf::ab::fnv1a_lower("Pirate_Vault_Door_C");

    constexpr uint64_t H_SupplyDrop     = cf::ab::fnv1a_lower("AthenaSupplyDrop_C");
    constexpr uint64_t H_Cannon         = cf::ab::fnv1a_lower("Pirate_Cannon_VH_C");
    constexpr uint64_t H_CannonAlt      = cf::ab::fnv1a_lower("Vehicle_Pirate_Cannon_C");
    constexpr uint64_t H_FortPawn       = cf::ab::fnv1a_lower("FortPlayerPawnAthena_C");

    a.isVaultChest = (h==H_VaultChest1 || h==H_VaultChest2 ||
                      h==H_VaultChest3 || h==H_SpriteChest ||
                      h==H_VaultDoor);
    a.isChest    = !a.isVaultChest &&
                   (h==H_Tiered_Chest || h==H_Tiered_Ammo ||
                    h==H_BuildContainer || h==H_RareChest ||
                    h==H_PirateChest);
    a.isLootDrop = (h==H_SupplyDrop);
    a.isCannon   = (h==H_Cannon || h==H_CannonAlt);
    a.isPlayer   = (h==H_FortPawn);
}

bool ReadLocalPlayer(LocalPlayerCache& lp){
    if(!g.resolved && !ResolveGlobals()) return false;

    uintptr_t world = cf::mem::R<uintptr_t>(g.gWorld);
    if(!world) return false;

    uintptr_t gi = cf::mem::R<uintptr_t>(world + cf::off::UWorld_OwningGameInstance);
    if(!gi) return false;

    struct TArr { uintptr_t data; int32_t num; int32_t max; };
    TArr players = cf::mem::R<TArr>(gi + cf::off::GameInstance_LocalPlayers);
    if(players.num<=0) return false;

    uintptr_t localPlayer = cf::mem::R<uintptr_t>(players.data);
    if(!localPlayer) return false;

    lp.controller  = cf::mem::R<uintptr_t>(localPlayer + cf::off::LocalPlayer_PlayerController);
    if(!lp.controller) return false;

    lp.pawn       = cf::mem::R<uintptr_t>(lp.controller + cf::off::PC_AcknowledgedPawn);
    uintptr_t cam = cf::mem::R<uintptr_t>(lp.controller + cf::off::PC_PlayerCameraMgr);

    if(lp.pawn){
        lp.rootComp     = cf::mem::R<uintptr_t>(lp.pawn + cf::off::Pawn_RootComponent);
        lp.movementComp = cf::mem::R<uintptr_t>(lp.pawn + cf::off::Character_CharacterMovement);
        if(lp.rootComp){
            lp.pos      = cf::mem::R<FVector>(lp.rootComp + cf::off::Scene_RelativeLocation);
            lp.velocity = cf::mem::R<FVector>(lp.rootComp + cf::off::Scene_ComponentVelocity);
        }
    }

    if(cam){
        FVector  camPos  = cf::mem::R<FVector> (cam + cf::off::Cam_CameraCachePrivate + 0x10);
        FRotator camRot  = cf::mem::R<FRotator>(cam + cf::off::Cam_CameraCachePrivate + 0x10 + 0x0C);
        float    fov     = cf::mem::R<float>   (cam + cf::off::Cam_CameraCachePrivate + 0x10 + 0x30);
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

} // namespace cf::sdk
