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

    // --- AFortPickup (floor loot) ---
    constexpr uint32_t Pickup_PickupTarget       = 0x870;   // target pawn
    constexpr uint32_t Pickup_PrimaryPickupItemEntry = 0x820;
    constexpr uint32_t Pickup_ItemDefinition     = 0x820 + 0x18;
    constexpr uint32_t Pickup_Count              = 0x820 + 0x28;
    constexpr uint32_t Pickup_bServerStoppedSimulating = 0x8A0;
    constexpr uint32_t Pickup_PickupNetData_Count= 0x898;

    // --- AFortPlayerPawnAthena / AFortPlayerControllerAthena ---
    constexpr uint32_t Pawn_WeaponList           = 0xA18;   // TArray<AFortWeapon*>
    constexpr uint32_t Pawn_CurrentWeapon        = 0xA40;
    constexpr uint32_t Pawn_InventoryComponent   = 0x920;
    constexpr uint32_t Pawn_bIsDying             = 0x6F0;
    constexpr uint32_t Pawn_TeamIndex            = 0xB20;

    // --- FortInventory ---
    constexpr uint32_t Inv_ReplicatedEntries     = 0x90;    // TArray<FFortItemEntry>
    constexpr uint32_t Inv_LocalInventoryItems   = 0xA0;

    // --- FFortItemEntry ---
    constexpr uint32_t ItemEntry_ItemDefinition  = 0x18;
    constexpr uint32_t ItemEntry_Count           = 0x20;
    constexpr uint32_t ItemEntry_LoadedAmmo      = 0x28;
    constexpr uint32_t ItemEntry_Guid            = 0x30;

    // --- AFortWeapon ---
    constexpr uint32_t Weapon_WeaponData         = 0x538;
    constexpr uint32_t Weapon_AmmoCount          = 0xA40;
    constexpr uint32_t Weapon_bHasInfiniteAmmo   = 0xA50;
    constexpr uint32_t Weapon_LastFireTime       = 0x7B8;
    constexpr uint32_t Weapon_FireCooldown       = 0x7C0;

    // --- material item defs (BP paths — resolved by fname hash) ---
    // Computed at startup via cf::sdk::MaterialHashWood() etc so they
    // stay in sync with the fnv1a_lower return type (uint64_t). Previous
    // revisions declared these as uint32_t constants which silently
    // mismatched the hash width and killed the mats-flood compare.

    // --- bone indices on FortPlayerPawn mesh ---
    constexpr int Bone_Head   = 66;
    constexpr int Bone_Neck   = 65;
    constexpr int Bone_Chest  = 7;
    constexpr int Bone_Pelvis = 1;

    // --- UObject (base) ---
    constexpr uint32_t UObject_ClassPrivate = 0x10;
    constexpr uint32_t UObject_NamePrivate  = 0x18;
    constexpr uint32_t UObject_OuterPrivate = 0x20;
    // UObject's vtable[ProcessEvent] — the virtual dispatch slot for
    // the universal RPC entry. Index varies per UE build; dumped by
    // scripts/dump_offsets.py pattern-sig "40 55 56 57 41 54 41 55 41 56 41 57 48 8D AC 24".
    constexpr uint32_t UObject_VTable_ProcessEvent = 0x44 * 8;    // slot 0x44 → offset in bytes

    // --- UStruct / UClass (reflection walk) ---
    // UStruct is the base of UClass and UFunction; it holds a linked
    // list of its child UFields (functions, properties) via Children.
    // UClass additionally owns a FuncMap (TMap<FName, UFunction*>) for
    // O(1) UFunction lookup by name — much faster than walking Children.
    constexpr uint32_t UStruct_SuperStruct   = 0x40;
    constexpr uint32_t UStruct_Children      = 0x48;   // UField* linked list head
    constexpr uint32_t UField_Next           = 0x28;   // UField's next sibling
    constexpr uint32_t UClass_FuncMap        = 0x98;   // TMap<FName, UFunction*>

    // --- FName ---
    constexpr uint32_t FName_Index = 0x0;

    // --- GObjects (TUObjectArray) ---
    constexpr uint32_t TUObjectArray_Objects = 0x10;
    constexpr uint32_t TUObjectArray_Num     = 0x14;
    constexpr uint32_t TUObjectItem_Stride   = 0x18;
    constexpr uint32_t TUObjectItem_Object   = 0x00;
}
