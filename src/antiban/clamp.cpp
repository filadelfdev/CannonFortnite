#include "antiban/clamp.h"
#include <algorithm>

namespace cf::ab::clamp {

FVector teleport(const FVector& from, const FVector& to, float maxDelta_cm){
    FVector d = to - from;
    float   l = d.Len();
    if(l <= maxDelta_cm) return to;
    FVector n = d.Norm();
    return from + n * maxDelta_cm;
}

float speedCap(float desired, float serverCap){
    // Stay 4% under a hard server echo — the client reconciles without
    // triggering the re-sync check that fires at hard ceiling.
    return std::min(desired, serverCap * 0.96f);
}

bool chestOpenRateOk(double now, double lastOpen){
    // ~3 chests per second ceiling. A real player at a loot pile hits 2-3/s;
    // 4+/s is the heuristic trigger.
    return (now - lastOpen) >= 0.33;
}

} // namespace cf::ab::clamp
