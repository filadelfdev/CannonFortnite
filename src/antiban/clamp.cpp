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
    // Cannon-smash path: the server's own "chest break via cannon damage"
    // sequence spawns, flies, lands, breaks and drops the token in roughly
    // 60-110ms. We ride under that envelope with ~35-80ms jitter, which
    // lands at a 12-25/s ceiling — well over the 300/min (5/s) target.
    // Jitter kills the stddev heuristic the same way; just at a higher floor.
    static thread_local uint32_t s = 0x9E3779B9u;
    s = s*1664525u + 1013904223u;
    double gate = 0.035 + ((s & 0xFFFF)/65535.0) * 0.045;
    return (now - lastOpen) >= gate;
}

} // namespace cf::ab::clamp
