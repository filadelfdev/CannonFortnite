#pragma once
#include "core/cf_api.h"
#include "core/types.h"
#include <vector>
#include <string>

namespace cf::sdk {

// Resolve engine globals (GWorld, GObjects, GNames) relative to a known
// base; prefer dumped offsets (sdk/offsets.h) first, pattern as fallback.
CF_CORE_API bool ResolveGlobals();

// FName → string via GNames chunked array. Cached by index per tick.
CF_CORE_API std::string FNameToString(uint32_t fnameIndex);

// Walk the UWorld's persistent level + streaming levels, pushing every
// live AActor pointer into `out`. Caller filters by class.
CF_CORE_API bool EnumerateActors(std::vector<uintptr_t>& out);

// Fast class-name lookup for one actor. Cached by UClass*.
CF_CORE_API std::string ActorClassName(uintptr_t actor);

// Pull the local player's whole cache in one scatter. Hot path.
CF_CORE_API bool ReadLocalPlayer(LocalPlayerCache& lp);

// World-to-screen using the camera POV read inside ReadLocalPlayer.
CF_CORE_API bool WorldToScreen(const LocalPlayerCache& lp,
                               const FVector& w, float& sx, float& sy,
                               float screenW, float screenH);

// Classify an actor into chest / cannon / loot-drop / player buckets from
// its UClass name (hash-matched so the SDK layer has no raw strings).
CF_CORE_API void ClassifyActor(Actor& a);

// Invalidate the per-actor class-name cache. Call this on every full
// re-enum so a reused actor address (post-GC) isn't served the previous
// occupant's class. The FName-string cache (keyed by stable FName index)
// does NOT need invalidation.
CF_CORE_API void InvalidateClassCache();

// Invoke a UFunction on a UObject by name. Walks obj->Class->Functions,
// hashes names, and jumps through UObject::ProcessEvent (resolved by
// pattern at ResolveGlobals time). `params` is the parameter struct
// laid out exactly as the UFunction's property chain expects; pass
// nullptr for a parameterless function. Returns false if ProcessEvent
// is unresolved or the name does not match any UFunction on the class.
CF_CORE_API bool CallUFunction(uintptr_t object, const char* funcName,
                               void* params = nullptr);

} // namespace cf::sdk
