#pragma once
#include <cstdint>

// Offsets: C7S1 build 33.10 (Nov 2026 Hunter's Haven). FILL per-patch via
// scripts/dump_offsets.py — this header is a mapping surface, not a creed.
// Rewrites of these with a dumper must land here in one block so a bad
// patch is a one-file rebuild, never a scatter-hunt.

namespace cf::off {
    // --- engine globals ---
    constexpr uint64_t G_WORLD        = 0x14F8E990;
    constexpr uint64_t G_NAMES        = 0x14ECD380;
    constexpr uint64_t G_OBJECTS      = 0x14F1A6C0;
    constexpr uint64_t G_ENGINE       = 0x14F91108;

    // --- UWorld ---
    constexpr uint32_t UWorld_OwningGameInstance = 0x1D8;
    constexpr uint32_t UWorld_PersistentLevel    = 0x30;
    constexpr uint32_t UWorld_GameState          = 0x160;
    constexpr uint32_t UWorld_Levels             = 0x170;

    // --- ULevel ---
    constexpr uint32_t ULevel_Actors             = 0xA0;
    constexpr uint32_t ULevel_ActorsCount        = 0xA8;

    // --- UGameInstance ---
    constexpr uint32_t GameInstance_LocalPlayers = 0x38;

    // --- ULocalPlayer ---
    constexpr uint32_t LocalPlayer_PlayerController = 0x30;
    constexpr uint32_t LocalPlayer_ViewportClient   = 0x78;

    // --- APlayerController ---
    constexpr uint32_t PC_AcknowledgedPawn   = 0x338;
    constexpr uint32_t PC_PlayerCameraMgr    = 0x348;
    constexpr uint32_t PC_ControlRotation    = 0x2E8;

    // --- APawn / ACharacter ---
    constexpr uint32_t Pawn_RootComponent        = 0x198;
    constexpr uint32_t Pawn_Mesh                 = 0x340;
    constexpr uint32_t Character_CharacterMovement = 0x330;

    // --- UCharacterMovementComponent ---
    constexpr uint32_t CMC_MaxWalkSpeed      = 0x2B4;
    constexpr uint32_t CMC_MaxFlySpeed       = 0x2C8;
    constexpr uint32_t CMC_MaxAcceleration   = 0x298;
    constexpr uint32_t CMC_GravityScale      = 0x1E4;
    constexpr uint32_t CMC_AirControl        = 0x254;
    constexpr uint32_t CMC_JumpZVelocity     = 0x29C;
    constexpr uint32_t CMC_BrakingFriction   = 0x2A4;

    // --- USceneComponent ---
    constexpr uint32_t Scene_RelativeLocation = 0x128;
    constexpr uint32_t Scene_RelativeRotation = 0x134;
    constexpr uint32_t Scene_ComponentVelocity = 0x168;
    constexpr uint32_t Scene_ComponentToWorld  = 0x1E0;

    // --- APlayerCameraManager ---
    constexpr uint32_t Cam_CameraCachePrivate = 0x1F10;   // FCameraCacheEntry (POV FMinimalViewInfo at +0x10)
    constexpr uint32_t Cam_FOV_in_POV         = 0x30;

    // --- AFortPickup / AthenaLootChest (BuildingContainer base) ---
    constexpr uint32_t Chest_bAlreadySearched = 0x990;
    constexpr uint32_t Chest_LootTier         = 0x980;
    constexpr uint32_t Chest_SearchBounceData = 0x9A0;

    // --- Pirate_Cannon_VH (vehicle) ---
    constexpr uint32_t Cannon_SeatOccupant       = 0x10E0;
    constexpr uint32_t Cannon_LaunchPower        = 0x1130;
    constexpr uint32_t Cannon_FireAngle          = 0x1138;
    constexpr uint32_t Cannon_bCanFire           = 0x1150;
    constexpr uint32_t Cannon_LastFiredTime      = 0x1158;

    // --- UObject (base) ---
    constexpr uint32_t UObject_ClassPrivate = 0x10;
    constexpr uint32_t UObject_NamePrivate  = 0x18;
    constexpr uint32_t UObject_OuterPrivate = 0x20;

    // --- FName ---
    constexpr uint32_t FName_Index = 0x0;

    // --- GObjects (TUObjectArray) ---
    constexpr uint32_t TUObjectArray_Objects = 0x10;
    constexpr uint32_t TUObjectArray_Num     = 0x14;
    constexpr uint32_t TUObjectItem_Stride   = 0x18;
    constexpr uint32_t TUObjectItem_Object   = 0x00;
}
