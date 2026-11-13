#pragma once
#include "core/cf_api.h"
#include <cstdint>
#include <string>

namespace cf::report {

// Session report — counts, rates, min/max intervals, token drops.
// Each cannon smash / chest open / token notify calls one of the
// RecordX() functions. The overlay Report page renders live Summary();
// panic exit auto-saves to %LOCALAPPDATA%\INetCache\.cf_session.txt.

enum class EventKind {
    CannonSmash,
    RouteOpen,
    BulkOpen,
    PickupGrab,
    MatsFlood,
    TokenDrop,       // only known if we see the on-screen notification
};

CF_CORE_API void RecordEvent(EventKind k);
CF_CORE_API void MarkTokenDrop();    // called when the client-side token notification fires

struct Summary {
    double   sessionSeconds;
    uint32_t cannonSmashes;
    uint32_t routeOpens;
    uint32_t bulkOpens;
    uint32_t pickupGrabs;
    uint32_t matsPulses;
    uint32_t tokenDrops;
    double   avgRatePerSec;
    double   peakRatePerSec;
    double   minIntervalMs;
    double   maxIntervalMs;
    uint32_t minChestsPerMin;
    uint32_t maxChestsPerMin;
    uint32_t totalChests() const {
        return cannonSmashes + routeOpens + bulkOpens;
    }
    // Each server-side chest open = +600 Battle Pass XP (Treasure Hunters).
    uint32_t totalXp() const { return totalChests() * 600; }
    // Rough BP-level estimate — assumes 80k XP per level (typical).
    double   approxBpLevels() const { return totalXp() / 80000.0; }
};

CF_CORE_API Summary     Current();
CF_CORE_API void        Reset();
CF_CORE_API bool        SaveToFile();            // writes to %LOCALAPPDATA%\INetCache
CF_CORE_API std::string SavePath();              // human-readable path

} // namespace cf::report
