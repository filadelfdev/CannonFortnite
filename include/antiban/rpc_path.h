#pragma once
#include "core/types.h"
#include <cstdint>

// RPC-path writers — the critical shift for v4.
//
// The old direct-write model (set bCanFire=1, bump LastFiredTime,
// write RelativeLocation) fails against modern Fortnite because every
// one of those fields is independently validated against the last RPC
// the server received from the client. The server sees "client is
// displaying state it never sent" and ejects.
//
// This layer calls the SAME game functions the real client calls:
//   APirate_Cannon::ServerFireCannon
//   APirate_Cannon::ServerUpdateVehicleLocation
//   AFortBuildingContainer::ServerTakeCannonDamage
//   AFortPlayerPawnAthena::ServerMove
//   AFortPickup::ServerPickup
//
// The invocation routes through the game's own reliable channel, so
// the server sees the exact trace a real player produces — because
// it IS the real-player call site.
//
// Implementation: patterns resolve on attach; call dispatch goes
// through driver/kread.c extension that schedules a kernel work-item
// to invoke the function with the game's own render-thread context.
// When the driver is unavailable the RPC layer returns false and the
// caller falls back to direct writes (noisier, documented).

namespace cf::ab::rpc {

bool Resolve();
bool Ready();

// Cannon.
bool CannonFire(uintptr_t cannon, float launchPower, const FRotator& aim);
bool CannonMove(uintptr_t cannon, const FVector& newPos, const FRotator& aim);
bool CannonSeat(uintptr_t cannon, uintptr_t pawn);

// Container damage — legacy (patched mechanic).
bool ChestTakeCannonDamage(uintptr_t chest, uintptr_t cannon, float dmg);

// Container interact — the CURRENT mechanic (Nov 2026). Called from
// within the cannon with the player as instigator. Server opens the
// chest, drops loot, rolls the Pirate Shop Token odds. Multiple
// interacts per tick is how the "many chests at once" look is
// produced — not a single AoE call, but N parallel invokes.
bool ChestServerInteract(uintptr_t chest, uintptr_t pawn);

// Pawn movement (used sparingly — the biggest ban vector).
bool PawnServerMove(uintptr_t pawn,
                    const FVector& newPos,
                    const FVector& newVel,
                    uint16_t packedRot);

// Pickup grab.
bool PickupServerGrab(uintptr_t pickup, uintptr_t pawn);

} // namespace cf::ab::rpc
