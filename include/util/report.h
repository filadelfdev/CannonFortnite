#pragma once
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

void RecordEvent(EventKind k);
void MarkTokenDrop();    // called when the client-side token notification fires

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
    double   minIntervalMs;      // tightest gap between two smashes
    double   maxIntervalMs;      // longest pause
    uint32_t minChestsPerMin;    // worst minute
    uint32_t maxChestsPerMin;    // best minute
    uint32_t totalChests() const {
        return cannonSmashes + routeOpens + bulkOpens;
    }
};

Summary Current();
void    Reset();
bool    SaveToFile();            // writes to %LOCALAPPDATA%\INetCache
std::string SavePath();          // human-readable path

} // namespace cf::report
