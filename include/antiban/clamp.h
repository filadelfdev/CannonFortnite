#pragma once
#include "core/cf_api.h"
#include "core/types.h"

namespace cf::ab::clamp {

// Keep every written position/velocity/stat inside the server-side
// heuristic envelope. If a tick would write outside the envelope, we
// write the clamped value and re-queue the remainder for the next tick.

// Teleport delta cap — Fortnite's anti-warp fires on cumulative deltas
// above ~35m over a short window. 32m per tick with a 3-tick cooldown
// stays under it.
CF_AB_API FVector teleport(const FVector& from, const FVector& to, float maxDelta_cm);

// Speed caps — the server echoes a MaxWalkSpeed and will reject moves
// above that. We stay at or just under the next quantization step.
CF_AB_API float speedCap(float desired, float serverCap);

// Open-rate limiter for chest search.
CF_AB_API bool chestOpenRateOk(double now, double lastOpen);

} // namespace cf::ab::clamp
