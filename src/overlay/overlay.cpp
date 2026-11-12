#include "overlay/overlay.h"
#include "overlay/theme.h"
#include "overlay/cfg_persist.h"
#include "antiban/window_cloak.h"
#include "features/cannon_chest.h"
#include "features/route.h"
#include "features/speed.h"
#include "features/loot.h"
#include "features/aimbot.h"
#include "core/memory.h"
#include "antiban/heartbeat.h"
#include "antiban/rpc_path.h"
#include "antiban/env_lie.h"
#include "antiban/dxgi_cloak.h"
#include "antiban/dxgi_kernel.h"
#include "util/report.h"
#include <fstream>
#include <ctime>
#include <string>
#include <shlobj.h>
#include "util/log.h"

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <mutex>
#include <vector>
#include <chrono>
#include <cstdio>

#if __has_include(<imgui.h>)
#  include <imgui.h>
#  include <backends/imgui_impl_win32.h>
#  include <backends/imgui_impl_dx11.h>
#  define CF_HAS_IMGUI 1
#else
#  define CF_HAS_IMGUI 0
#endif

#pragma comment(lib, "d3d11")
#pragma comment(lib, "dxgi")
#pragma comment(lib, "d3dcompiler")
#pragma comment(lib, "dwmapi")

#if CF_HAS_IMGUI
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);
#endif

namespace cf::ov {

OverlayCfg g_ovCfg{};

namespace {

HWND                g_hwnd{};
ID3D11Device*       g_dev{};
ID3D11DeviceContext*g_ctx{};
IDXGISwapChain*     g_swap{};
ID3D11RenderTargetView* g_rtv{};
UINT                g_w=1920, g_h=1080;
std::atomic<bool>   g_quit{false};

std::mutex              g_dlLock;
std::vector<cf::feat::DrawItem> g_dl;

enum class Tab { Route, Loot, Aim, Mats, Cannon, Speed, Esp, Report, Security, About };
Tab g_tab = Tab::Route;

// Top-left HUD toggle — the "Chests Searched: N" counter mirroring the
// in-game string the reference compilation shows (frame 20: "Chests
// Searched: 21"). Matches the server-side counter the game itself
// maintains when ServerInteract fires.
std::atomic<bool> g_showHud{true};

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp){
#if CF_HAS_IMGUI
    if(ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp)) return true;
#endif
    switch(msg){
        case WM_SIZE:
            if(g_dev && wp != SIZE_MINIMIZED){
                if(g_rtv){ g_rtv->Release(); g_rtv=nullptr; }
                g_swap->ResizeBuffers(0, (UINT)LOWORD(lp), (UINT)HIWORD(lp), DXGI_FORMAT_UNKNOWN, 0);
                ID3D11Texture2D* bb=nullptr;
                g_swap->GetBuffer(0, IID_PPV_ARGS(&bb));
                if(bb){ g_dev->CreateRenderTargetView(bb, nullptr, &g_rtv); bb->Release(); }
                g_w = LOWORD(lp); g_h = HIWORD(lp);
            }
            return 0;
        case WM_DESTROY: g_quit = true; PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

bool CreateDevice(){
    DXGI_SWAP_CHAIN_DESC sc{};
    sc.BufferCount=2;
    sc.BufferDesc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
    sc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sc.OutputWindow=g_hwnd;
    sc.SampleDesc.Count=1;
    sc.Windowed=TRUE;
    sc.SwapEffect=DXGI_SWAP_EFFECT_FLIP_DISCARD;

    D3D_FEATURE_LEVEL fl;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                                0, nullptr, 0, D3D11_SDK_VERSION,
                                                &sc, &g_swap, &g_dev, &fl, &g_ctx);
    if(FAILED(hr)) return false;
    ID3D11Texture2D* bb=nullptr;
    g_swap->GetBuffer(0, IID_PPV_ARGS(&bb));
    g_dev->CreateRenderTargetView(bb, nullptr, &g_rtv);
    bb->Release();
    return true;
}

bool CreateWindow_(){
    WNDCLASSEXW wc{sizeof(wc)};
    wc.style         = CS_HREDRAW|CS_VREDRAW;
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = GetModuleHandleW(nullptr);
    wc.lpszClassName = cf::ab::wincloak::BuildClassName();
    RegisterClassExW(&wc);

    RECT r{0,0,(LONG)GetSystemMetrics(SM_CXSCREEN),(LONG)GetSystemMetrics(SM_CYSCREEN)};
    g_w = r.right; g_h = r.bottom;

    g_hwnd = CreateWindowExW(
        cf::ab::wincloak::BuildExFlags(),
        wc.lpszClassName, L"", cf::ab::wincloak::BuildStyleFlags(),
        r.left, r.top, g_w, g_h,
        nullptr, nullptr, wc.hInstance, nullptr);

    SetLayeredWindowAttributes(g_hwnd, 0, 0, LWA_COLORKEY);
    MARGINS m{-1,-1,-1,-1};
    extern "C" HRESULT WINAPI DwmExtendFrameIntoClientArea(HWND, const MARGINS*);
    DwmExtendFrameIntoClientArea(g_hwnd, &m);
    ShowWindow(g_hwnd, SW_SHOW);
    UpdateWindow(g_hwnd);
    // Late-flip TOPMOST + exclude from capture so the WM_CREATE snapshot
    // of our flags looks benign.
    cf::ab::wincloak::Harden(g_hwnd);
    return true;
}

#if CF_HAS_IMGUI
// ---- small widgets ---------------------------------------------------

ImU32 X(unsigned argb){ return (ImU32)argb; }

// A pill with a dot + text. Three variants: ok / warn / off.
void StatusPill(const char* label, bool ok){
    using namespace cf::ov::theme::col;
    ImU32 dot = ok ? X(::cf::ov::theme::col::ok) : X(textDim);
    ImVec2 p = ImGui::GetCursorScreenPos();
    float  h = ImGui::GetFrameHeight();
    float  w = ImGui::CalcTextSize(label).x + h*1.4f + 10.f;
    auto*  dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x+w, p.y+h), X(surf), 999.f);
    dl->AddCircleFilled(ImVec2(p.x+h*0.55f, p.y+h*0.5f), 3.5f, dot, 12);
    dl->AddText(ImVec2(p.x+h*1.0f, p.y+(h-ImGui::GetTextLineHeight())*0.5f),
                X(text), label);
    ImGui::Dummy(ImVec2(w, h));
}

// Toggle switch — slimmer than a checkbox, cyan when on.
bool Toggle(const char* id, bool* v){
    using namespace cf::ov::theme::col;
    ImVec2 p  = ImGui::GetCursorScreenPos();
    float  h  = ImGui::GetFrameHeight() * 0.82f;
    float  w  = h * 1.9f;
    bool clicked = ImGui::InvisibleButton(id, ImVec2(w, h));
    if(clicked) *v = !*v;
    auto* dl = ImGui::GetWindowDrawList();
    ImU32 track = *v ? X(accent) : X(surfHi);
    ImU32 knob  = *v ? X(text)   : X(textDim);
    dl->AddRectFilled(p, ImVec2(p.x+w, p.y+h), track, h*0.5f);
    float cx = *v ? (p.x + w - h*0.5f) : (p.x + h*0.5f);
    dl->AddCircleFilled(ImVec2(cx, p.y+h*0.5f), h*0.38f, knob, 16);
    return clicked;
}

// A labelled row: label on the left, control on the right.
void RowBegin(const char* label){
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextUnformatted(label);
    ImGui::PopStyleColor();
    ImGui::SameLine(160);
}

bool RowToggle(const char* label, std::atomic<bool>& v){
    RowBegin(label);
    bool b = v.load();
    bool changed = Toggle(label, &b);
    if(changed) v.store(b);
    return changed;
}

bool RowSliderF(const char* label, std::atomic<float>& v, float lo, float hi, const char* fmt = "%.1f"){
    RowBegin(label);
    float f = v.load();
    ImGui::SetNextItemWidth(-1);
    char id[64]; std::snprintf(id, 64, "##%s", label);
    bool changed = ImGui::SliderFloat(id, &f, lo, hi, fmt);
    if(changed) v.store(f);
    return changed;
}
bool RowSliderI(const char* label, std::atomic<int>& v, int lo, int hi){
    RowBegin(label);
    int i = v.load();
    ImGui::SetNextItemWidth(-1);
    char id[64]; std::snprintf(id, 64, "##%s", label);
    bool changed = ImGui::SliderInt(id, &i, lo, hi);
    if(changed) v.store(i);
    return changed;
}

// Sidebar tab button.
bool TabButton(const char* label, Tab self){
    using namespace cf::ov::theme::col;
    bool active = (g_tab == self);
    ImGui::PushStyleColor(ImGuiCol_Button,        active ? X(surf)   : ImVec4(0,0,0,0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, X(surfHi));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  X(surfHi));
    ImGui::PushStyleColor(ImGuiCol_Text, active ? X(text) : X(textDim));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 8.f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,  ImVec2(10, 9));
    bool clicked = ImGui::Button(label, ImVec2(-1, 0));
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(4);
    if(clicked) g_tab = self;
    // Accent bar on the left when active.
    if(active){
        ImVec2 p = ImGui::GetItemRectMin();
        ImVec2 q = ImGui::GetItemRectMax();
        ImGui::GetWindowDrawList()->AddRectFilled(
            ImVec2(p.x - 6, p.y + 6), ImVec2(p.x - 3, q.y - 6),
            X(accent), 2.f);
    }
    return clicked;
}

// ---- pages -----------------------------------------------------------

void PageRoute(){
    int mode = (int)cf::feat::g_routeCfg.mode.load();
    const char* modes[] = { "cannon smash (pirate token)", "teleport chain (gear)", "bulk sweep (gear)" };
    RowBegin("mode");
    ImGui::SetNextItemWidth(-1);
    if(ImGui::Combo("##mode", &mode, modes, 3))
        cf::feat::g_routeCfg.mode.store((cf::feat::RouteMode)mode);
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("cannon smash teleports the CANNON above each chest and fires straight down. "
                       "the cannonball-breaks-chest path is what drops the Pirate Token. "
                       "other modes open chests but do NOT award the token.");
    ImGui::PopStyleColor();
    ImGui::Dummy(ImVec2(0, 6));

    RowSliderF("hop size",   cf::feat::g_routeCfg.hopDelta_cm,   500.f, 3500.f, "%.0f cm");
    RowSliderF("cadence",    cf::feat::g_routeCfg.hopCadence_ms, 10.f,  300.f,  "%.0f ms");
    RowSliderF("range",      cf::feat::g_routeCfg.chainRange_m,  200.f, 4000.f, "%.0f m");
    RowToggle ("bulk open",  cf::feat::g_routeCfg.bulkOpenOnArrive);
    RowSliderF("bulk radius",cf::feat::g_routeCfg.bulkRadius_cm, 200.f, 2500.f, "%.0f cm");
    RowSliderI("opens / tick",cf::feat::g_routeCfg.maxOpensPerTick, 1, 12);
    RowToggle ("face target",cf::feat::g_routeCfg.rotateToTarget);
    RowToggle ("prefer rare",cf::feat::g_routeCfg.preferUnopenedTier);
}

void PageLoot(){
    RowToggle ("enabled",       cf::feat::g_lootCfg.enabled);
    RowToggle ("chest magnet",  cf::feat::g_lootCfg.chestMagnet);
    RowToggle ("pickup magnet", cf::feat::g_lootCfg.pickupMagnet);
    // "ignore walls" / "prefer mythic" UI removed — flags were dead.
    // Re-enable here once LOS trace + tier sort land.
    RowToggle ("instant pickup",cf::feat::g_lootCfg.instantPickup);
    RowSliderF("radius",        cf::feat::g_lootCfg.magnetRadius_m, 10.f, 200.f, "%.0f m");
    RowSliderF("pull speed",    cf::feat::g_lootCfg.magnetSpeed_cm, 100.f, 2000.f, "%.0f cm");
    RowSliderI("pulls / tick",  cf::feat::g_lootCfg.maxPullsPerTick, 1, 16);
    // "prefer mythic" row removed — dead UI (no tier sort yet).
}

void PageAim(){
    RowToggle ("enabled",      cf::feat::g_aimCfg.enabled);
    RowToggle ("silent aim",   cf::feat::g_aimCfg.silent);
    // "visible only" row removed — dead UI (no LOS trace yet).
    RowToggle ("team check",   cf::feat::g_aimCfg.teamCheck);
    RowToggle ("velocity lead",cf::feat::g_aimCfg.predictVelocity);
    RowSliderF("fov",          cf::feat::g_aimCfg.fov_deg,       1.f, 60.f,  "%.1f deg");
    RowSliderF("range",        cf::feat::g_aimCfg.maxRange_m,    20.f, 500.f, "%.0f m");
    RowSliderF("smooth",       cf::feat::g_aimCfg.smooth,        1.f, 20.f,  "%.1f");
    int bone = cf::feat::g_aimCfg.bone.load();
    const char* bones[] = { "head", "neck", "chest", "pelvis" };
    RowBegin("bone");
    ImGui::SetNextItemWidth(-1);
    if(ImGui::Combo("##bone", &bone, bones, 4)) cf::feat::g_aimCfg.bone.store(bone);
    RowToggle ("no recoil",    cf::feat::g_aimCfg.noRecoil);
    RowToggle ("no spread",    cf::feat::g_aimCfg.noSpread);
    RowToggle ("auto fire",    cf::feat::g_aimCfg.autoFireOnLock);
}

void PageMats(){
    RowToggle ("enabled",   cf::feat::g_matsCfg.enabled);
    RowSliderI("stack",     cf::feat::g_matsCfg.targetStack, 100, 999);
    RowToggle ("top up only",cf::feat::g_matsCfg.topUpOnly);
    ImGui::Dummy(ImVec2(0,4));
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("writes wood/brick/metal stacks to the target value every tick. 'top up only' never reduces a real count below the target.");
    ImGui::PopStyleColor();
}

void PageCannon(){
    using namespace cf::ov::theme::col;
    ImGui::TextUnformatted("velocidade (1 = seguro · 10 = perigoso)");
    int lvl = cf::feat::g_cfg.speedPreset.load();
    ImGui::SetNextItemWidth(-1);
    if(ImGui::SliderInt("##speed", &lvl, 1, 10)){
        cf::feat::ApplySpeedPreset(lvl);
    }
    static const char* labels[10] = {
        "~1/s  (safest)",  "~1.5/s", "~2/s",      "~2.8/s",
        "~3.5/s",          "~4.5/s", "~5/s (default)","~6.5/s",
        "~8/s",            "~10/s (unsafe)"
    };
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextUnformatted(labels[lvl-1]);
    ImGui::TextWrapped("v6 model: player enters cannon; cannon teleports to chest clusters; ServerInteract bulk-invoked on nearby chests. No cannon fire.");
    ImGui::PopStyleColor();
    ImGui::Separator();
    ImGui::Dummy(ImVec2(0,4));

    RowToggle ("enabled",           cf::feat::g_cfg.enabled);
    RowToggle ("prefer rpc",        cf::feat::g_cfg.preferRpc);
    RowToggle ("teleport cannon",   cf::feat::g_cfg.teleportCannon);
    RowToggle ("player rides",      cf::feat::g_cfg.playerRidesCannon);
    RowToggle ("skip looted",       cf::feat::g_cfg.skipLootedChests);
    RowSliderF("range",             cf::feat::g_cfg.maxRange_m,            100.f, 3000.f, "%.0f m");
    RowSliderF("hover height",      cf::feat::g_cfg.hoverHeight_cm,        150.f, 1200.f, "%.0f cm");
    RowSliderF("hover jitter",      cf::feat::g_cfg.hoverJitter_cm,        0.f,   120.f,  "%.0f cm");
    RowSliderF("interact radius",   cf::feat::g_cfg.interactRadius_cm,     300.f, 1500.f, "%.0f cm");
    RowSliderI("interacts / tick",  cf::feat::g_cfg.maxInteractsPerTick,   1, 10);
    RowSliderF("cooldown mean",     cf::feat::g_cfg.cooldownMeanMs,        50.f,  2000.f, "%.0f ms");
    RowSliderF("server grace",      cf::feat::g_cfg.serverConfirmGraceMs,  60.f,  800.f,  "%.0f ms");
    RowSliderF("cannon warp cap",   cf::feat::g_cfg.cannonTeleportCap_cm,  2000.f,15000.f,"%.0f cm");
    RowSliderI("session limit",     cf::feat::g_cfg.chestsPerSession,      0, 2000);
    ImGui::Dummy(ImVec2(0,4));
    if(ImGui::Button("reset counter")) cf::feat::ResetSmashCounter();
}

void PageSpeed(){
    RowToggle ("enabled",   cf::feat::g_speedCfg.enabled);
    RowSliderF("walk",      cf::feat::g_speedCfg.walkSpeed,     600.f,  9000.f, "%.0f");
    RowSliderF("fly",       cf::feat::g_speedCfg.flySpeed,      1000.f,12000.f, "%.0f");
    RowSliderF("accel",     cf::feat::g_speedCfg.acceleration,  2000.f,40000.f, "%.0f");
    RowSliderF("air ctrl",  cf::feat::g_speedCfg.airControl,    0.1f,  1.f,    "%.2f");
    RowSliderF("gravity",   cf::feat::g_speedCfg.gravityScale,  0.05f, 1.5f,   "%.2f");
    RowSliderF("jump z",    cf::feat::g_speedCfg.jumpZ,         400.f, 2500.f, "%.0f");
}

void PageEsp(){
    RowToggle ("enabled",  cf::feat::g_espCfg.enabled);
    RowToggle ("chests",   cf::feat::g_espCfg.drawChests);
    RowToggle ("cannons",  cf::feat::g_espCfg.drawCannons);
    RowToggle ("players",  cf::feat::g_espCfg.drawPlayers);
    RowToggle ("tracers",  cf::feat::g_espCfg.drawLines);
    RowSliderF("range",    cf::feat::g_espCfg.maxDraw_m,       50.f, 2000.f, "%.0f m");
}

void PageSecurity(){
    using namespace cf::ov::theme::col;
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("kernel path and ac heartbeat run on their own workers. toggles here only soften; the panic key (END) still hard-exits.");
    ImGui::PopStyleColor();
    ImGui::Dummy(ImVec2(0, 4));

    StatusPill(cf::mem::KernelActive()  ? "kernel ON" : "kernel OFF", cf::mem::KernelActive());
    ImGui::SameLine();
    StatusPill(cf::ab::HeartbeatArmed() ? "heartbeat ON"   : "heartbeat OFF",   cf::ab::HeartbeatArmed());
    ImGui::SameLine();
    StatusPill(cf::ab::rpc::Ready()     ? "rpc ON"         : "rpc OFF",         cf::ab::rpc::Ready());
    ImGui::Dummy(ImVec2(0, 4));
    StatusPill(cf::ab::envlie::Armed()  ? "env-lie ON"       : "env-lie OFF",       cf::ab::envlie::Armed());
    ImGui::SameLine();
    // dxgi cloak: kernel-side (bug #9 fix) if available, else overlay-local.
    bool kcloak = cf::ab::dxgi_k::Armed();
    StatusPill(kcloak ? "dxgi kernel ON" :
               cf::ab::dxgi::Armed() ? "dxgi overlay ON" : "dxgi OFF",
               kcloak || cf::ab::dxgi::Armed());
    ImGui::Dummy(ImVec2(0, 6));

    char buf[160];
    std::snprintf(buf, 160, "chests smashed: %u   ·   pickups: %u",
                  cf::feat::SmashedThisSession(), cf::feat::Looted());
    ImGui::TextUnformatted(buf);

    ImGui::Dummy(ImVec2(0, 8));

    // HWID state — read the marker written by the standalone spoofer.
    // The cheat never touches HWID itself anymore.
    int slot = -1; double ageH = -1;
    {
        wchar_t p[MAX_PATH]{}; SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, p);
        std::wstring path = p;
        path += L"\\Microsoft\\Windows\\INetCache\\.cf_spoof.marker";
        std::ifstream mf(path.c_str(), std::ios::binary);
        if(mf){
            struct { uint32_t magic; uint32_t ver; int32_t s; int32_t pad;
                     int64_t appliedUnix; uint64_t fpHash; } b{};
            mf.read((char*)&b, sizeof(b));
            if(b.magic == 0xCF50F000){
                slot = b.s;
                ageH = (std::time(nullptr) - b.appliedUnix) / 3600.0;
            }
        }
    }
    if(slot >= 0){
        std::snprintf(buf, 160, "spoofer marker: slot %d · aplicado %.1fh atrás",
                      slot, ageH);
        ImGui::TextUnformatted(buf);
    } else {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f, 0.4f, 0.4f, 1.f));
        ImGui::TextWrapped("SPOOFER NÃO DETECTADO — rode CannonSpoofer\\spoofer.exe → [1] apply primeiro.");
        ImGui::PopStyleColor();
    }

    ImGui::Dummy(ImVec2(0, 4));
    ImGui::SameLine();
    if(ImGui::Button("reset counter")){
        cf::feat::ResetSmashCounter();
        cf::feat::ResetLooted();
    }
    ImGui::SameLine();
    if(ImGui::Button("panic exit"))    g_quit = true;

    // ---- Hotkey rebind ----
    ImGui::Dummy(ImVec2(0, 10));
    ImGui::Separator();
    ImGui::TextUnformatted("hotkeys  (click → press any key to rebind)");
    ImGui::Dummy(ImVec2(0, 4));

    struct Binding { const char* name; std::atomic<int>& slot; int id; };
    Binding binds[] = {
        {"menu",   g_ovCfg.menuKey,         1},
        {"panic",  g_ovCfg.panicKey,        2},
        {"cannon", g_ovCfg.toggleCannonKey, 3},
        {"speed",  g_ovCfg.toggleSpeedKey,  4},
        {"esp",    g_ovCfg.toggleEspKey,    5},
    };
    int active = g_ovCfg.rebindTarget.load();
    for(auto& b : binds){
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextUnformatted(b.name);
        ImGui::PopStyleColor();
        ImGui::SameLine(110);
        char lbl[64];
        if(active == b.id) std::snprintf(lbl, 64, "press any key... ##%d", b.id);
        else               std::snprintf(lbl, 64, "%s ##%d", VkName(b.slot.load()), b.id);
        if(ImGui::Button(lbl, ImVec2(-1, 0))){
            g_ovCfg.rebindTarget.store(b.id);
        }
    }
}

void PageReport(){
    using namespace cf::ov::theme::col;
    auto s = cf::report::Current();

    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_Text));
    char buf[160];
    std::snprintf(buf, 160, "%u baús  ·  %u XP  ·  %u tokens",
                  s.totalChests(), s.totalXp(), s.tokenDrops);
    ImGui::TextUnformatted(buf);
    ImGui::PopStyleColor();

    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    std::snprintf(buf, 160, "sessão: %.0fs (%.1f min)", s.sessionSeconds, s.sessionSeconds/60.0);
    ImGui::TextUnformatted(buf);
    ImGui::PopStyleColor();

    ImGui::Dummy(ImVec2(0, 8));
    ImGui::Separator();
    ImGui::Dummy(ImVec2(0, 4));

    auto row = [&](const char* label, const char* val){
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextUnformatted(label);
        ImGui::PopStyleColor();
        ImGui::SameLine(160);
        ImGui::TextUnformatted(val);
    };

    std::snprintf(buf, 160, "%u", s.cannonSmashes);  row("cannon smashes", buf);
    std::snprintf(buf, 160, "%u", s.routeOpens);     row("route opens", buf);
    std::snprintf(buf, 160, "%u", s.bulkOpens);      row("bulk opens", buf);
    std::snprintf(buf, 160, "%u", s.pickupGrabs);    row("pickups", buf);
    std::snprintf(buf, 160, "%u", s.matsPulses);     row("mats pulses", buf);

    ImGui::Dummy(ImVec2(0, 8));
    std::snprintf(buf, 160, "%.2f/s", s.avgRatePerSec);           row("avg rate", buf);
    std::snprintf(buf, 160, "%.2f/s", s.peakRatePerSec);          row("peak (5s)", buf);
    std::snprintf(buf, 160, "%.0f ms", s.minIntervalMs);          row("min interval", buf);
    std::snprintf(buf, 160, "%.0f ms", s.maxIntervalMs);          row("max interval", buf);
    std::snprintf(buf, 160, "%u / %u", s.minChestsPerMin, s.maxChestsPerMin);
    row("worst / best / min", buf);

    ImGui::Dummy(ImVec2(0, 10));

    // XP + token projection (BP sem cap — mini-season Treasure Hunters).
    if(s.avgRatePerSec > 0.0){
        double xpPerHour     = s.avgRatePerSec * 3600.0 * 600.0;
        double levelsPerHour = xpPerHour / 80000.0;
        double hoursPerToken = 135000.0 / (s.avgRatePerSec * 3600.0);
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        std::snprintf(buf, 160, "ganho desta sessão: %u XP  ·  %.1f níveis BP", s.totalXp(), s.approxBpLevels());
        ImGui::TextUnformatted(buf);
        std::snprintf(buf, 160, "XP/hora: %.0f  ·  %.1f níveis/hora", xpPerHour, levelsPerHour);
        ImGui::TextUnformatted(buf);
        std::snprintf(buf, 160, "token lottery: %.1f h/token esperado (1/135k)", hoursPerToken);
        ImGui::TextUnformatted(buf);
        ImGui::PopStyleColor();
    }

    ImGui::Dummy(ImVec2(0, 8));
    if(ImGui::Button("salvar relatório .txt")){
        cf::report::SaveToFile();
    }
    ImGui::SameLine();
    if(ImGui::Button("resetar")) cf::report::Reset();
    ImGui::SameLine();
    bool hud = g_showHud.load();
    if(ImGui::Checkbox("hud top-left", &hud)) g_showHud.store(hud);
}

void PageAbout(){
    using namespace cf::ov::theme::col;
    ImGui::PushStyleColor(ImGuiCol_Text, X(gold));
    ImGui::TextUnformatted("HZ AIM");
    ImGui::PopStyleColor();
    ImGui::PushStyleColor(ImGuiCol_Text, X(textDim));
    ImGui::TextUnformatted("pirate token farm · v6");
    ImGui::Dummy(ImVec2(0, 10));
    ImGui::TextUnformatted("hotkeys");
    ImGui::PopStyleColor();
    ImGui::BulletText("INSERT   toggle menu");
    ImGui::BulletText("F9       speed");
    ImGui::BulletText("F10      cannon");
    ImGui::BulletText("F11      esp");
    ImGui::BulletText("END      panic exit");
    ImGui::Dummy(ImVec2(0, 10));
    ImGui::PushStyleColor(ImGuiCol_Text, X(textDim));
    ImGui::TextUnformatted("built by olivia");
    ImGui::TextUnformatted("discord.gg/ojailbreaks");
    ImGui::PopStyleColor();
}

// ---- esp marks -------------------------------------------------------

// A single-pass mark: two L brackets + dot + distance chip. Reads as one
// system with the panel's accent colour, costs less than a filled box.
void DrawEspMark(ImDrawList* dl, const cf::feat::DrawItem& it, float screenW){
    using namespace cf::ov::theme::col;
    ImU32 c  = (ImU32)it.color;
    ImU32 ct = (c & 0x60FFFFFF);
    float x = it.x, y = it.y;
    float a = 8.f, b = 2.f;
    // corner brackets
    dl->AddLine(ImVec2(x-a, y-a), ImVec2(x-a+b*2, y-a), c, 1.4f);
    dl->AddLine(ImVec2(x-a, y-a), ImVec2(x-a, y-a+b*2), c, 1.4f);
    dl->AddLine(ImVec2(x+a, y-a), ImVec2(x+a-b*2, y-a), c, 1.4f);
    dl->AddLine(ImVec2(x+a, y-a), ImVec2(x+a, y-a+b*2), c, 1.4f);
    dl->AddLine(ImVec2(x-a, y+a), ImVec2(x-a+b*2, y+a), c, 1.4f);
    dl->AddLine(ImVec2(x-a, y+a), ImVec2(x-a, y+a-b*2), c, 1.4f);
    dl->AddLine(ImVec2(x+a, y+a), ImVec2(x+a-b*2, y+a), c, 1.4f);
    dl->AddLine(ImVec2(x+a, y+a), ImVec2(x+a, y+a-b*2), c, 1.4f);
    dl->AddCircleFilled(ImVec2(x, y), 1.8f, c, 8);
    dl->AddText(ImVec2(x+a+4, y-7), c, it.label);
    if(it.drawLineFromBottom){
        dl->AddLine(ImVec2(screenW*0.5f, (float)g_h),
                    ImVec2(x, y+a), ct, 1.f);
    }
}

#endif

void DrawFrame(){
    float clear[4]{0,0,0,0};
    g_ctx->OMSetRenderTargets(1, &g_rtv, nullptr);
    g_ctx->ClearRenderTargetView(g_rtv, clear);

#if CF_HAS_IMGUI
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    // ---- ESP (background draw list, no input cost) ----
    {
        auto* bgdl = ImGui::GetBackgroundDrawList();
        std::vector<cf::feat::DrawItem> items;
        { std::scoped_lock _(g_dlLock); items = g_dl; }
        for(auto& it : items) DrawEspMark(bgdl, it, (float)g_w);
    }

    // ---- Top-left HUD — HZ AIM branded, three-line compact readout ----
    //   line 1: HZ AIM  <base hex>            (brand, gold)
    //   line 2: Chests Opened: N | [RAM: ...]  (status, green/red)
    //   line 3: +XP earned                     (accent gold)
    if(g_showHud.load()){
        auto* bgdl = ImGui::GetBackgroundDrawList();
        auto sum = cf::report::Current();

        bool ramLive = cf::mem::IsAttached();
        bool kernel  = cf::mem::KernelActive();

        char l1[96], l2[96], l3[64];
        std::snprintf(l1, 96, "HZ AIM  %016llx",
                      (unsigned long long)cf::mem::Base());
        std::snprintf(l2, 96, "Chests Opened: %u  |  [RAM: %s%s]",
                      sum.totalChests(),
                      ramLive ? "ACTIVE" : "IDLE",
                      kernel  ? "+K"      : "");
        std::snprintf(l3, 64, "+%u XP", sum.totalXp());

        ImVec2 s1 = ImGui::CalcTextSize(l1);
        ImVec2 s2 = ImGui::CalcTextSize(l2);
        ImVec2 s3 = ImGui::CalcTextSize(l3);
        float w = std::max(std::max(s1.x, s2.x), s3.x) + 22;
        float h = s1.y + s2.y + s3.y + 20;

        // Deep red backdrop with a faint gold top-edge line.
        bgdl->AddRectFilled(ImVec2(0, 0), ImVec2(w, h),
                            IM_COL32(170, 25, 25, 230), 0);
        bgdl->AddRectFilled(ImVec2(0, h-2), ImVec2(w, h),
                            IM_COL32(232, 185, 56, 255), 0);

        bgdl->AddText(ImVec2(10, 4),                       IM_COL32(255, 255, 255, 255), l1);
        ImU32 ramCol = ramLive ? IM_COL32(120, 255, 120, 255) : IM_COL32(255, 80, 80, 255);
        bgdl->AddText(ImVec2(10, 4 + s1.y + 2),            ramCol,         l2);
        bgdl->AddText(ImVec2(10, 4 + s1.y + s2.y + 4),     IM_COL32(232, 185, 56, 255), l3);
    }

    // ---- menu ----
    if(g_ovCfg.showMenu.load()){
        using namespace cf::ov::theme::col;
        ImGui::SetNextWindowSize(ImVec2(620, 520), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowBgAlpha(0.97f);
        ImGui::Begin("##cf-root", nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_NoCollapse);

        // ---- Title strip (72px) — HZ AIM logo + live counters ----
        ImVec2 wp = ImGui::GetWindowPos();
        ImVec2 ws = ImGui::GetWindowSize();
        auto* wdl = ImGui::GetWindowDrawList();

        constexpr float TITLE_H = 72.f;
        constexpr float STATUS_H = 30.f;

        // Dark header background with subtle gold bottom border.
        wdl->AddRectFilled(ImVec2(wp.x, wp.y),
                           ImVec2(wp.x + ws.x, wp.y + TITLE_H),
                           X(bg2), 10.f, ImDrawFlags_RoundCornersTop);
        wdl->AddRectFilled(ImVec2(wp.x, wp.y + TITLE_H - 2),
                           ImVec2(wp.x + ws.x, wp.y + TITLE_H),
                           X(gold), 0);

        // Logo badge — crossed-pistols-ish mark drawn with vector lines.
        float cx = wp.x + 30;
        float cy = wp.y + TITLE_H*0.5f;
        // Hexagon outline
        ImVec2 hex[6]; float rad = 18.f;
        for(int i=0;i<6;++i){
            float a = (i/6.f) * 6.2831853f - 1.5707963f;
            hex[i] = ImVec2(cx + std::cos(a)*rad, cy + std::sin(a)*rad);
        }
        wdl->AddConvexPolyFilled(hex, 6, X(surf));
        wdl->AddPolyline(hex, 6, X(gold), ImDrawFlags_Closed, 2.f);
        // Inside: HZ monogram (two angled strokes forming an H + Z feel)
        wdl->AddLine(ImVec2(cx-7, cy-7), ImVec2(cx-7, cy+7), X(gold), 2.f);
        wdl->AddLine(ImVec2(cx+7, cy-7), ImVec2(cx+7, cy+7), X(gold), 2.f);
        wdl->AddLine(ImVec2(cx-7, cy),   ImVec2(cx+7, cy),   X(gold), 2.f);

        // Wordmark
        wdl->AddText(ImVec2(wp.x + 62, wp.y + 14), X(text),    "HZ AIM");
        wdl->AddText(ImVec2(wp.x + 62, wp.y + 36), X(textDim), "pirate token farm");

        // Right-side: live counter + rate + XP (gold accent on numbers)
        static double   t0 = 0.0;
        static uint32_t last = 0;
        static float    rate = 0.f;
        double now = ImGui::GetTime();
        uint32_t s = cf::feat::SmashedThisSession();
        if(t0 == 0.0){ t0 = now; last = s; }
        if((now - t0) >= 1.0){
            rate = (float)(s - last) / (float)(now - t0);
            t0 = now; last = s;
        }
        char cntLine1[64], cntLine2[64];
        std::snprintf(cntLine1, 64, "%u chests", s);
        std::snprintf(cntLine2, 64, "%.1f/s · +%u XP", rate, s*600u);
        ImVec2 c1 = ImGui::CalcTextSize(cntLine1);
        ImVec2 c2 = ImGui::CalcTextSize(cntLine2);
        wdl->AddText(ImVec2(wp.x + ws.x - c1.x - 16, wp.y + 14), X(gold),    cntLine1);
        wdl->AddText(ImVec2(wp.x + ws.x - c2.x - 16, wp.y + 36), X(textDim), cntLine2);

        // ---- Status strip (30px) — compact pill row ----
        float sy = wp.y + TITLE_H + 2;
        wdl->AddRectFilled(ImVec2(wp.x, sy), ImVec2(wp.x + ws.x, sy + STATUS_H),
                           X(bg), 0);
        auto dot = [&](float x, bool ok, const char* lbl){
            wdl->AddCircleFilled(ImVec2(x, sy + STATUS_H*0.5f), 3.2f,
                                 ok ? X(::cf::ov::theme::col::ok) : X(textDim), 12);
            wdl->AddText(ImVec2(x + 8, sy + (STATUS_H - ImGui::GetTextLineHeight())*0.5f),
                         ok ? X(text) : X(textDim), lbl);
        };
        float px = wp.x + 18;
        auto advance = [&](const char* lbl){ return ImGui::CalcTextSize(lbl).x + 26; };
        dot(px, cf::mem::KernelActive(),  "kernel");    px += advance("kernel");
        dot(px, cf::ab::HeartbeatArmed(), "heartbeat"); px += advance("heartbeat");
        dot(px, cf::ab::rpc::Ready(),     "rpc");       px += advance("rpc");
        dot(px, cf::ab::envlie::Armed(),  "env-lie");   px += advance("env-lie");
        dot(px, cf::ab::dxgi_k::Armed() || cf::ab::dxgi::Armed(), "dxgi"); px += advance("dxgi");

        ImGui::Dummy(ImVec2(0, TITLE_H + STATUS_H - 10));

        // Sidebar + content split
        ImGui::BeginChild("##sidebar", ImVec2(140, 0), false);
          ImGui::Dummy(ImVec2(0, 4));
          TabButton("cannon",   Tab::Cannon);
          TabButton("report",   Tab::Report);
          TabButton("route",    Tab::Route);
          TabButton("loot",     Tab::Loot);
          TabButton("aim",      Tab::Aim);
          TabButton("mats",     Tab::Mats);
          TabButton("speed",    Tab::Speed);
          TabButton("esp",      Tab::Esp);
          TabButton("security", Tab::Security);
          TabButton("about",    Tab::About);
        ImGui::EndChild();
        ImGui::SameLine();
        ImGui::BeginChild("##content", ImVec2(0, 0), false);
          switch(g_tab){
              case Tab::Cannon:   PageCannon();   break;
              case Tab::Report:   PageReport();   break;
              case Tab::Route:    PageRoute();    break;
              case Tab::Loot:     PageLoot();     break;
              case Tab::Aim:      PageAim();      break;
              case Tab::Mats:     PageMats();     break;
              case Tab::Speed:    PageSpeed();    break;
              case Tab::Esp:      PageEsp();      break;
              case Tab::Security: PageSecurity(); break;
              case Tab::About:    PageAbout();    break;
          }
        ImGui::EndChild();

        ImGui::End();
    }

    ImGui::Render();
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
#endif
    g_swap->Present(1, 0);
}

bool TogglePress(int vk, bool& edge){
    bool now = (GetAsyncKeyState(vk) & 0x8000) != 0;
    bool rose = now && !edge;
    edge = now;
    return rose;
}

// Scan every possible VK; if a rebind is pending, capture the first
// keydown and assign it to the matching slot. Modifier-only keys and
// the left mouse button are excluded so a click doesn't rebind.
int CaptureNextKey(){
    for(int vk = 0x01; vk <= 0xFE; ++vk){
        if(vk == VK_LBUTTON) continue;
        if(GetAsyncKeyState(vk) & 0x8000) return vk;
    }
    return 0;
}

const char* VkName(int vk){
    static char buf[32];
    switch(vk){
        case 0x05: return "Mouse4 (side)";
        case 0x06: return "Mouse5 (side)";
        case 0x14: return "Caps Lock";
        case 0xA0: return "Left Shift";
        case 0xA1: return "Right Shift";
        case 0xA2: return "Left Ctrl";
        case 0xA3: return "Right Ctrl";
        case 0xA4: return "Left Alt";
        case 0xA5: return "Right Alt";
        case VK_SPACE:   return "Space";
        case VK_RETURN:  return "Enter";
        case VK_TAB:     return "Tab";
        case VK_ESCAPE:  return "Esc";
        case VK_BACK:    return "Backspace";
        case VK_OEM_3:   return "` (grave)";
        case VK_OEM_MINUS: return "-";
        case VK_OEM_PLUS:  return "=";
        case VK_OEM_4: return "[";
        case VK_OEM_6: return "]";
        case VK_OEM_5: return "\\";
        case VK_OEM_1: return ";";
        case VK_OEM_7: return "'";
        case VK_OEM_COMMA: return ",";
        case VK_OEM_PERIOD: return ".";
        case VK_OEM_2: return "/";
    }
    if(vk >= '0' && vk <= '9'){ std::snprintf(buf, 32, "%c", vk); return buf; }
    if(vk >= 'A' && vk <= 'Z'){ std::snprintf(buf, 32, "%c", vk); return buf; }
    if(vk >= VK_F1 && vk <= VK_F24){ std::snprintf(buf, 32, "F%d", vk - VK_F1 + 1); return buf; }
    std::snprintf(buf, 32, "VK 0x%02X", vk);
    return buf;
}

} // namespace

void PublishDrawList(std::vector<cf::feat::DrawItem> items){
    std::scoped_lock _(g_dlLock);
    g_dl = std::move(items);
}

int Run(TickFn perFrame){
    // Load persisted hotkeys before the window exists so the first
    // input tick sees the user's bindings, not the defaults.
    cf::ov::persist::LoadHotkeys(g_ovCfg);

    if(!CreateWindow_())   return -1;
    if(!CreateDevice())    return -2;

#if CF_HAS_IMGUI
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    cf::ov::theme::Apply();
    ImGui_ImplWin32_Init(g_hwnd);
    ImGui_ImplDX11_Init(g_dev, g_ctx);
#endif

    bool em=false, es=false, ec=false, ep=false, eesp=false;
    auto last = std::chrono::steady_clock::now();

    MSG msg{};
    while(!g_quit){
        while(PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)){
            TranslateMessage(&msg); DispatchMessageW(&msg);
            if(msg.message==WM_QUIT) g_quit=true;
        }
        // Rebind listen path — overrides normal hotkey handling while
        // the user is choosing a key. First key down after the "rebind"
        // button is captured, stored, and the target field cleared.
        int rebind = g_ovCfg.rebindTarget.load();
        if(rebind != 0){
            int k = CaptureNextKey();
            if(k != 0 && k != VK_LBUTTON){
                switch(rebind){
                    case 1: g_ovCfg.menuKey.store(k); break;
                    case 2: g_ovCfg.panicKey.store(k); break;
                    case 3: g_ovCfg.toggleCannonKey.store(k); break;
                    case 4: g_ovCfg.toggleSpeedKey.store(k); break;
                    case 5: g_ovCfg.toggleEspKey.store(k); break;
                }
                g_ovCfg.rebindTarget.store(0);
                // Flush edges so the just-pressed key doesn't instantly
                // toggle the thing it was bound to.
                em = es = ec = ep = eesp = true;
                // Persist immediately so a crash doesn't lose the bind.
                cf::ov::persist::SaveHotkeys(g_ovCfg);
            }
        } else {
            if(TogglePress(g_ovCfg.menuKey.load(),  em)) g_ovCfg.showMenu.store(!g_ovCfg.showMenu.load());
            if(TogglePress(g_ovCfg.panicKey.load(), ep)) g_quit=true;
            if(TogglePress(g_ovCfg.toggleCannonKey.load(), ec))
                cf::feat::g_cfg.enabled.store(!cf::feat::g_cfg.enabled.load());
            if(TogglePress(g_ovCfg.toggleSpeedKey.load(),  es))
                cf::feat::g_speedCfg.enabled.store(!cf::feat::g_speedCfg.enabled.load());
            if(TogglePress(g_ovCfg.toggleEspKey.load(),    eesp))
                cf::feat::g_espCfg.enabled.store(!cf::feat::g_espCfg.enabled.load());
        }

        auto now = std::chrono::steady_clock::now();
        float dt = std::chrono::duration<float>(now - last).count();
        last = now;

        if(perFrame) perFrame(dt, (float)g_w, (float)g_h);
        DrawFrame();
    }

    // Save-on-exit covers the case where the user never crashed but
    // the inline save after rebind was somehow skipped (defaults write
    // on first run with no file).
    cf::ov::persist::SaveHotkeys(g_ovCfg);

#if CF_HAS_IMGUI
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
#endif
    if(g_rtv){ g_rtv->Release(); g_rtv=nullptr; }
    if(g_swap){ g_swap->Release(); g_swap=nullptr; }
    if(g_ctx){ g_ctx->Release(); g_ctx=nullptr; }
    if(g_dev){ g_dev->Release(); g_dev=nullptr; }
    if(g_hwnd){ DestroyWindow(g_hwnd); g_hwnd=nullptr; }
    return 0;
}

} // namespace cf::ov
