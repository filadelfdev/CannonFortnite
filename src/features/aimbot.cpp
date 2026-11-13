#include "features/aimbot.h"
#include "sdk/engine.h"
#include "sdk/offsets.h"
#include "core/process.h"
#include "antiban/clamp.h"

#include "util/log.h"

#include <cmath>
#include <algorithm>
#include <limits>

namespace cf::feat {

AimCfg g_aimCfg{};

namespace {

// Pawn mesh is a USkeletalMeshComponent; bone transforms live in
// FBoneTransforms[bone] at ComponentSpaceTransformsArray on the mesh.
// Layout: TArray<FTransform> at mesh + 0x5B0 (varies per build; FILL).
constexpr uint32_t Mesh_ComponentSpaceTransforms = 0x5B0;

// Full component-space bone position:
//   bone_world = Mesh.ComponentToWorld * BoneTransforms[idx].Translation
// Previously we naively added `componentOrigin + bone.translation`
// which is WRONG when the mesh is rotated (shooting breaks the moment
// the target turns). Now we read the mesh's ComponentToWorld matrix
// and rotate the bone offset by the mesh rotation before adding.
FVector BonePos(uintptr_t pawn, int boneIdx){
    uintptr_t mesh = cf::proc::R<uintptr_t>(pawn + cf::off::Pawn_Mesh);
    if(!mesh) return {};
    struct TArr { uintptr_t data; int32_t num; int32_t max; };
    TArr arr = cf::proc::R<TArr>(mesh + Mesh_ComponentSpaceTransforms);
    if(boneIdx < 0 || boneIdx >= arr.num) return {};
    FTransform bone = cf::proc::R<FTransform>(arr.data + (uintptr_t)boneIdx * sizeof(FTransform));
    FTransform ctow = cf::proc::R<FTransform>(mesh + cf::off::Scene_ComponentToWorld);

    // Rotate bone.translation by ctow.rot (quat) and add ctow.translation.
    const float qx = ctow.rot[0], qy = ctow.rot[1], qz = ctow.rot[2], qw = ctow.rot[3];
    const FVector& v = bone.translation;
    // v' = v + 2 * cross(q.xyz, cross(q.xyz, v) + q.w * v)
    FVector cross1 {
        qy*v.z - qz*v.y + qw*v.x,
        qz*v.x - qx*v.z + qw*v.y,
        qx*v.y - qy*v.x + qw*v.z
    };
    FVector cross2 {
        qy*cross1.z - qz*cross1.y,
        qz*cross1.x - qx*cross1.z,
        qx*cross1.y - qy*cross1.x
    };
    FVector rotated {
        v.x + 2.f*cross2.x,
        v.y + 2.f*cross2.y,
        v.z + 2.f*cross2.z,
    };
    return rotated + ctow.translation;
}

float AngleBetweenDeg(const FVector& a, const FVector& b){
    FVector an = a.Norm(); FVector bn = b.Norm();
    float d = an.x*bn.x + an.y*bn.y + an.z*bn.z;
    d = std::max(-1.f, std::min(1.f, d));
    return std::acos(d) * 57.29578f;
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

FRotator Lerp(const FRotator& a, const FRotator& b, float t){
    auto wrap = [](float d){
        while(d >  180.f) d -= 360.f;
        while(d < -180.f) d += 360.f;
        return d;
    };
    return { a.pitch + wrap(b.pitch - a.pitch)*t,
             a.yaw   + wrap(b.yaw   - a.yaw)  *t,
             0.f };
}

} // namespace

void TickAim(const LocalPlayerCache& lp,
             const std::vector<Actor>& actors,
             float dt){
    if(!g_aimCfg.enabled.load() || !lp.pawn || !lp.controller) return;

    // No-recoil / no-spread: write the current weapon's recoil + spread
    // scalars to zero. Cheap and does not hit movement replication.
    if(g_aimCfg.noRecoil.load() || g_aimCfg.noSpread.load()){
        uintptr_t weapon = cf::proc::R<uintptr_t>(lp.pawn + cf::off::Pawn_CurrentWeapon);
        if(weapon){
            if(g_aimCfg.noRecoil.load()){
                float zero = 0.f;
                cf::proc::Write(weapon + 0x7E0, &zero, sizeof(float));   // recoil scalar
            }
            if(g_aimCfg.noSpread.load()){
                float zero = 0.f;
                cf::proc::Write(weapon + 0x7F8, &zero, sizeof(float));   // spread scalar
            }
        }
    }

    // Pick a target inside the FOV cone.
    const float maxCm = g_aimCfg.maxRange_m.load() * 100.f;
    const float fov   = g_aimCfg.fov_deg.load();
    int   boneIdx = cf::off::Bone_Head;
    switch((AimBone)g_aimCfg.bone.load()){
        case AimBone::Head:   boneIdx = cf::off::Bone_Head; break;
        case AimBone::Neck:   boneIdx = cf::off::Bone_Neck; break;
        case AimBone::Chest:  boneIdx = cf::off::Bone_Chest; break;
        case AimBone::Pelvis: boneIdx = cf::off::Bone_Pelvis; break;
    }

    uint32_t myTeam = cf::proc::R<uint32_t>(lp.pawn + cf::off::Pawn_TeamIndex);

    // Our forward from camera rotation.
    float cy = std::cos(lp.camRot.yaw * 0.017453292f);
    float sy = std::sin(lp.camRot.yaw * 0.017453292f);
    float cp = std::cos(lp.camRot.pitch * 0.017453292f);
    float sp = std::sin(lp.camRot.pitch * 0.017453292f);
    FVector camFwd{cp*cy, cp*sy, sp};

    uintptr_t bestPawn = 0;
    FVector   bestBone{};
    float     bestScore = std::numeric_limits<float>::max();

    for(auto& a : actors){
        if(!a.isPlayer) continue;
        if(a.addr == lp.pawn) continue;
        if(g_aimCfg.teamCheck.load()){
            uint32_t t = cf::proc::R<uint32_t>(a.addr + cf::off::Pawn_TeamIndex);
            if(t == myTeam) continue;
        }
        uint8_t dying = cf::proc::R<uint8_t>(a.addr + cf::off::Pawn_bIsDying);
        if(dying) continue;

        FVector bone = BonePos(a.addr, boneIdx);
        if(bone.Len() < 1.f) bone = a.pos;

        // Velocity lead.
        if(g_aimCfg.predictVelocity.load()){
            uintptr_t root = cf::proc::R<uintptr_t>(a.addr + cf::off::Pawn_RootComponent);
            if(root){
                FVector vel = cf::proc::R<FVector>(root + cf::off::Scene_ComponentVelocity);
                float dist  = (bone - lp.camPos).Len();
                float t     = dist / g_aimCfg.projectileSpeed.load();
                bone = bone + vel * t;
            }
        }

        FVector toTarget = bone - lp.camPos;
        float dist = toTarget.Len();
        if(dist > maxCm) continue;
        float ang = AngleBetweenDeg(camFwd, toTarget);
        if(ang > fov) continue;

        // Score: angle weighted more than distance so a closer target
        // right of crosshair beats a farther one square on.
        float score = ang * 100.f + dist;
        if(score < bestScore){ bestScore = score; bestPawn = a.addr; bestBone = bone; }
    }

    if(!bestPawn) return;

    FRotator desired = LookAt(lp.camPos, bestBone);

    if(g_aimCfg.silent.load()){
        // Silent / bullet-magnet: write the target rotation ONLY into
        // the weapon's fire-trace rotation slot, not into the controller
        // view rotation. The crosshair stays where the player aims.
        uintptr_t weapon = cf::proc::R<uintptr_t>(lp.pawn + cf::off::Pawn_CurrentWeapon);
        if(weapon){
            // Bullet-origin rotation lives at weapon + 0x820 on current UE5 build.
            cf::proc::Write(weapon + 0x820, &desired, sizeof(FRotator));
        }
    } else {
        float t = 1.f - std::exp(-dt * (20.f - std::min(19.f, g_aimCfg.smooth.load())));
        FRotator cur  = cf::proc::R<FRotator>(lp.controller + cf::off::PC_ControlRotation);
        FRotator next = Lerp(cur, desired, t);
        cf::proc::Write(lp.controller + cf::off::PC_ControlRotation, &next, sizeof(FRotator));
    }

    if(g_aimCfg.autoFireOnLock.load()){
        uintptr_t weapon = cf::proc::R<uintptr_t>(lp.pawn + cf::off::Pawn_CurrentWeapon);
        if(weapon){
            // Zeroing cooldown is necessary but not sufficient — the
            // engine still needs the fire RPC to kick a bullet out.
            // Invoke AFortWeapon::ServerFire with the aim rotation we
            // just computed; ProcessEvent takes care of the FName →
            // UFunction dispatch. Returns silently on a UFunction miss.
            float zero = 0.f;
            cf::proc::Write(weapon + cf::off::Weapon_FireCooldown, &zero, sizeof(float));
            cf::proc::Write(weapon + cf::off::Weapon_LastFireTime,  &zero, sizeof(float));

            struct FireParams {
                FRotator fireRotation;
                FVector  fireStart;
            } params;
            params.fireRotation = desired;
            params.fireStart    = lp.camPos;
            cf::sdk::CallUFunction(weapon, "ServerFire", &params);
        }
    }
}

} // namespace cf::feat
