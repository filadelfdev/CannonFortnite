#include "features/esp.h"
#include "sdk/engine.h"
#include <cstdio>
#include <algorithm>

namespace cf::feat {

EspCfg g_espCfg{};

std::vector<DrawItem> BuildDrawList(const LocalPlayerCache& lp,
                                    const std::vector<Actor>& actors,
                                    float screenW, float screenH){
    std::vector<DrawItem> out;
    if(!g_espCfg.enabled.load()) return out;
    out.reserve(actors.size());

    float maxCm = g_espCfg.maxDraw_m.load() * 100.f;

    for(auto& a : actors){
        bool drawIt = (a.isChest    && g_espCfg.drawChests.load())
                   || (a.isCannon   && g_espCfg.drawCannons.load())
                   || (a.isPlayer   && g_espCfg.drawPlayers.load())
                   || (a.isLootDrop && g_espCfg.drawChests.load());
        if(!drawIt) continue;

        float d = (a.pos - lp.pos).Len();
        if(d > maxCm) continue;

        float sx=0, sy=0;
        if(!cf::sdk::WorldToScreen(lp, a.pos, sx, sy, screenW, screenH)) continue;

        DrawItem item{};
        item.x = sx; item.y = sy;
        item.dist_m = d/100.f;
        item.drawLineFromBottom = g_espCfg.drawLines.load();

        if(a.isChest)         { item.color = g_espCfg.chestColor.load();   std::snprintf(item.label, 32, "chest %.0fm", item.dist_m); }
        else if(a.isCannon)   { item.color = g_espCfg.cannonColor.load();  std::snprintf(item.label, 32, "cannon %.0fm", item.dist_m); }
        else if(a.isPlayer)   { item.color = g_espCfg.playerColor.load();  std::snprintf(item.label, 32, "player %.0fm", item.dist_m); }
        else                  { item.color = g_espCfg.lootDropColor.load();std::snprintf(item.label, 32, "drop %.0fm", item.dist_m); }

        out.push_back(item);
    }

    std::sort(out.begin(), out.end(), [](const DrawItem& a, const DrawItem& b){
        return a.dist_m > b.dist_m;   // far first, so near draws on top
    });
    return out;
}

} // namespace cf::feat
