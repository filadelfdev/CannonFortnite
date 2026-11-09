#pragma once
#include "core/types.h"
#include <vector>
#include <string>

namespace cf::sdk {

// Resolve engine globals (GWorld, GObjects, GNames) relative to a known
// base; prefer dumped offsets (sdk/offsets.h) first, pattern as fallback.
bool ResolveGlobals();

// FName → string via GNames chunked array. Cached by index per tick.
std::string FNameToString(uint32_t fnameIndex);

// Walk the UWorld's persistent level + streaming levels, pushing every
// live AActor pointer into `out`. Caller filters by class.
bool EnumerateActors(std::vector<uintptr_t>& out);

// Fast class-name lookup for one actor. Cached by UClass*.
std::string ActorClassName(uintptr_t actor);

// Pull the local player's whole cache in one scatter. Hot path.
bool ReadLocalPlayer(LocalPlayerCache& lp);

// World-to-screen using the camera POV read inside ReadLocalPlayer.
bool WorldToScreen(const LocalPlayerCache& lp,
                   const FVector& w, float& sx, float& sy,
                   float screenW, float screenH);

// Classify an actor into chest / cannon / loot-drop / player buckets from
// its UClass name (hash-matched so the SDK layer has no raw strings).
void ClassifyActor(Actor& a);

} // namespace cf::sdk
