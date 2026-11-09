#include "antiban/clamp.h"
#include <algorithm>

namespace cf::ab::clamp {

FVector teleport(const FVector& from, const FVector& to, float maxDelta_cm){
    FVector d = to - from;
    float   l = d.Len();
    if(l <= maxDelta_cm) return to;
    // Biased short of the cap by 4% — Fortnite's anti-warp fires on
    // equal-to-max deltas across three consecutive ticks because a legit
    // movement burst always undershoots the ceiling by physics noise.
    FVector n = d.Norm();
    return from + n * (maxDelta_cm * 0.96f);
}

float speedCap(float desired, float serverCap){
    // Stay 4% under a hard server echo — the client reconciles without
    // triggering the re-sync check that fires at hard ceiling.
    return std::min(desired, serverCap * 0.96f);
}

bool chestOpenRateOk(double now, double lastOpen){
    // Jitter the gate between 90ms and 160ms. A real player at a rare-chest
    // pile hits 6-10/s, so sitting at 7/s average with jitter lives in the
    // same distribution a real sweat does, not a clean monotonic flood.
    // The heuristic trigger is "N opens with inter-arrival stddev under 10ms
    // over a 500ms window" — jitter here kills that signature.
    static thread_local uint32_t s = 0x9E3779B9u;
    s = s*1664525u + 1013904223u;
    double gate = 0.090 + ((s & 0xFFFF)/65535.0) * 0.070;
    return (now - lastOpen) >= gate;
}

} // namespace cf::ab::clamp
