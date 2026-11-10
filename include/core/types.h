#pragma once
#include <cstdint>
#include <cmath>
#include <string>
#include <vector>

namespace cf {

struct FVector {
    float x{}, y{}, z{};
    constexpr FVector operator+(const FVector& r) const { return {x+r.x,y+r.y,z+r.z}; }
    constexpr FVector operator-(const FVector& r) const { return {x-r.x,y-r.y,z-r.z}; }
    constexpr FVector operator*(float s) const { return {x*s,y*s,z*s}; }
    float Len() const { return std::sqrt(x*x+y*y+z*z); }
    float Len2D() const { return std::sqrt(x*x+y*y); }
    FVector Norm() const { float l=Len(); return l>1e-5f? FVector{x/l,y/l,z/l}:FVector{}; }
};

struct FRotator {
    float pitch{}, yaw{}, roll{};
};

struct FTransform {
    float rot[4]{0,0,0,1};   // quat x,y,z,w
    FVector translation{};
    float pad{};
    FVector scale3d{1,1,1};
    float pad2{};
};

struct FMatrix { float m[4][4]{}; };

struct Actor {
    uintptr_t addr{};
    uint32_t classId{};
    std::string className;
    FVector pos{};
    float distance{};
    bool  isChest{};        // normal breakable chest (cannon-ok)
    bool  isVaultChest{};   // Vault/Sprite chest — do NOT target
    bool  isCannon{};
    bool  isLootDrop{};
    bool  isPlayer{};
};

struct LocalPlayerCache {
    uintptr_t pawn{};
    uintptr_t controller{};
    uintptr_t movementComp{};
    uintptr_t rootComp{};
    FVector   pos{};
    FVector   velocity{};
    FVector   camPos{};
    FRotator  camRot{};
    float     fov{90.f};
    FMatrix   view{};
};

} // namespace cf
