#include "util/report.h"

#include <windows.h>
#include <shlobj.h>
#include <chrono>
#include <mutex>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <ctime>

namespace cf::report {

namespace {

std::mutex g_lock;

struct State {
    std::chrono::steady_clock::time_point started;
    std::chrono::steady_clock::time_point lastChestEvent;
    uint32_t cannonSmashes = 0;
    uint32_t routeOpens    = 0;
    uint32_t bulkOpens     = 0;
    uint32_t pickupGrabs   = 0;
    uint32_t matsPulses    = 0;
    uint32_t tokenDrops    = 0;
    double   minIntervalMs = 1e18;
    double   maxIntervalMs = 0.0;
    double   peakRate      = 0.0;

    // Rolling minute window.
    struct MinuteBucket {
        std::chrono::steady_clock::time_point mark;
        uint32_t count = 0;
    };
    MinuteBucket current{};
    uint32_t minChestsPerMin = ~0u;
    uint32_t maxChestsPerMin = 0;

    // Short peak window — last 5s, for peak rate.
    uint32_t last5sCount = 0;
    std::chrono::steady_clock::time_point last5sMark{};

    State(){
        started = std::chrono::steady_clock::now();
        lastChestEvent = started;
        current.mark = started;
        last5sMark = started;
    }
} g;

bool IsChestEvent(EventKind k){
    return k == EventKind::CannonSmash ||
           k == EventKind::RouteOpen   ||
           k == EventKind::BulkOpen;
}

void BumpChestAccounting(){
    auto now = std::chrono::steady_clock::now();
    double gap = std::chrono::duration<double, std::milli>(now - g.lastChestEvent).count();
    if(gap > 0 && gap < 60000.0){
        if(gap < g.minIntervalMs) g.minIntervalMs = gap;
        if(gap > g.maxIntervalMs) g.maxIntervalMs = gap;
    }
    g.lastChestEvent = now;

    // Minute bucket rollover.
    double sinceBucket = std::chrono::duration<double>(now - g.current.mark).count();
    if(sinceBucket >= 60.0){
        if(g.current.count < g.minChestsPerMin) g.minChestsPerMin = g.current.count;
        if(g.current.count > g.maxChestsPerMin) g.maxChestsPerMin = g.current.count;
        g.current.count = 0;
        g.current.mark = now;
    }
    g.current.count++;

    // 5-second peak window.
    double since5 = std::chrono::duration<double>(now - g.last5sMark).count();
    if(since5 >= 5.0){
        double rate = g.last5sCount / since5;
        if(rate > g.peakRate) g.peakRate = rate;
        g.last5sCount = 0;
        g.last5sMark = now;
    }
    g.last5sCount++;
}

} // namespace

void RecordEvent(EventKind k){
    std::scoped_lock _(g_lock);
    switch(k){
        case EventKind::CannonSmash: g.cannonSmashes++; BumpChestAccounting(); break;
        case EventKind::RouteOpen:   g.routeOpens++;    BumpChestAccounting(); break;
        case EventKind::BulkOpen:    g.bulkOpens++;     BumpChestAccounting(); break;
        case EventKind::PickupGrab:  g.pickupGrabs++; break;
        case EventKind::MatsFlood:   g.matsPulses++;  break;
        case EventKind::TokenDrop:   g.tokenDrops++;  break;
    }
}

void MarkTokenDrop(){
    std::scoped_lock _(g_lock);
    g.tokenDrops++;
}

Summary Current(){
    std::scoped_lock _(g_lock);
    Summary s{};
    auto now = std::chrono::steady_clock::now();
    s.sessionSeconds = std::chrono::duration<double>(now - g.started).count();
    s.cannonSmashes  = g.cannonSmashes;
    s.routeOpens     = g.routeOpens;
    s.bulkOpens      = g.bulkOpens;
    s.pickupGrabs    = g.pickupGrabs;
    s.matsPulses     = g.matsPulses;
    s.tokenDrops     = g.tokenDrops;
    uint32_t chests  = s.totalChests();
    s.avgRatePerSec  = s.sessionSeconds > 0.1 ? (double)chests / s.sessionSeconds : 0.0;
    s.peakRatePerSec = g.peakRate;
    s.minIntervalMs  = g.minIntervalMs >= 1e17 ? 0.0 : g.minIntervalMs;
    s.maxIntervalMs  = g.maxIntervalMs;
    s.minChestsPerMin = g.minChestsPerMin == ~0u ? 0 : g.minChestsPerMin;
    s.maxChestsPerMin = g.maxChestsPerMin;
    return s;
}

void Reset(){
    std::scoped_lock _(g_lock);
    g = State{};
}

std::string SavePath(){
    wchar_t p[MAX_PATH]{};
    SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, p);
    std::filesystem::path out = p;
    out /= L"Microsoft\\Windows\\INetCache";
    std::filesystem::create_directories(out);
    // Timestamped filename so multiple sessions don't overwrite.
    auto tt = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &tt);
    char ts[64];
    std::strftime(ts, 64, ".cf_session_%Y%m%d_%H%M%S.txt", &tm);
    out /= ts;
    return out.string();
}

bool SaveToFile(){
    auto s = Current();
    auto path = SavePath();
    std::ofstream f(path);
    if(!f) return false;
    f << "cannon ripper — session report\n";
    f << "===============================\n\n";
    f << "duration           : " << std::fixed << std::setprecision(1) << s.sessionSeconds << " s  (" << (s.sessionSeconds/60.0) << " min)\n";
    f << "chests total       : " << s.totalChests() << "\n";
    f << "  cannon smashes   : " << s.cannonSmashes << "\n";
    f << "  route opens      : " << s.routeOpens << "\n";
    f << "  bulk opens       : " << s.bulkOpens << "\n";
    f << "pickups grabbed    : " << s.pickupGrabs << "\n";
    f << "mats pulses        : " << s.matsPulses << "\n";
    f << "pirate tokens      : " << s.tokenDrops << "\n";
    f << "\n";
    f << "rate — avg         : " << std::setprecision(2) << s.avgRatePerSec << " chests/s\n";
    f << "rate — peak (5s)   : " << s.peakRatePerSec << " chests/s\n";
    f << "min interval       : " << std::setprecision(1) << s.minIntervalMs << " ms\n";
    f << "max interval       : " << s.maxIntervalMs << " ms\n";
    f << "worst minute       : " << s.minChestsPerMin << " chests\n";
    f << "best minute        : " << s.maxChestsPerMin << " chests\n";
    f << "\n";
    f << "projection (1 token = 135k chests)\n";
    if(s.avgRatePerSec > 0){
        double hoursPerToken = 135000.0 / (s.avgRatePerSec * 3600.0);
        f << "  hours per token  : " << std::setprecision(1) << hoursPerToken << " h\n";
        f << "  expected tokens  : " << std::setprecision(2) << (s.sessionSeconds * s.avgRatePerSec / 135000.0) << "\n";
    }
    f << "\n";
    f << "built by olivia — https://discord.gg/ojailbreaks\n";
    return true;
}

} // namespace cf::report
