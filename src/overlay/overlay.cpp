#include "overlay/overlay.h"
#include "features/cannon_chest.h"
#include "features/speed.h"
#include "util/log.h"

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <mutex>
#include <vector>
#include <chrono>

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
    wc.lpszClassName = L"svcSurface";
    RegisterClassExW(&wc);

    RECT r{0,0,(LONG)GetSystemMetrics(SM_CXSCREEN),(LONG)GetSystemMetrics(SM_CYSCREEN)};
    g_w = r.right; g_h = r.bottom;

    g_hwnd = CreateWindowExW(
        WS_EX_LAYERED|WS_EX_TRANSPARENT|WS_EX_TOPMOST|WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE,
        wc.lpszClassName, L"", WS_POPUP,
        r.left, r.top, g_w, g_h,
        nullptr, nullptr, wc.hInstance, nullptr);

    SetLayeredWindowAttributes(g_hwnd, 0, 0, LWA_COLORKEY);
    MARGINS m{-1,-1,-1,-1};
    extern "C" HRESULT WINAPI DwmExtendFrameIntoClientArea(HWND, const MARGINS*);
    DwmExtendFrameIntoClientArea(g_hwnd, &m);
    ShowWindow(g_hwnd, SW_SHOW);
    UpdateWindow(g_hwnd);
    return true;
}

void DrawFrame(){
    float clear[4]{0,0,0,0};
    g_ctx->OMSetRenderTargets(1, &g_rtv, nullptr);
    g_ctx->ClearRenderTargetView(g_rtv, clear);

#if CF_HAS_IMGUI
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    // ESP pass — background draw list so nothing eats input.
    auto* dl = ImGui::GetBackgroundDrawList();
    std::vector<cf::feat::DrawItem> items;
    { std::scoped_lock _(g_dlLock); items = g_dl; }
    for(auto& it : items){
        dl->AddRect(ImVec2(it.x-10, it.y-14), ImVec2(it.x+10, it.y+14),
                    (ImU32)it.color, 2.f, 0, 1.4f);
        dl->AddText(ImVec2(it.x+12, it.y-7), (ImU32)it.color, it.label);
        if(it.drawLineFromBottom){
            dl->AddLine(ImVec2((float)g_w*0.5f, (float)g_h),
                        ImVec2(it.x, it.y+14),
                        ((ImU32)it.color & 0x80FFFFFF), 1.f);
        }
    }

    if(g_ovCfg.showMenu.load()){
        ImGui::SetNextWindowSize(ImVec2(360, 420), ImGuiCond_FirstUseEver);
        ImGui::Begin("CF :: cannon-chest ripper", nullptr, ImGuiWindowFlags_NoSavedSettings);

        if(ImGui::CollapsingHeader("cannon → chest", ImGuiTreeNodeFlags_DefaultOpen)){
            bool  e   = cf::feat::g_cfg.enabled.load();
            bool  tp  = cf::feat::g_cfg.teleportIntoCannon.load();
            bool  af  = cf::feat::g_cfg.autoFire.load();
            bool  skl = cf::feat::g_cfg.skipLootedChests.load();
            bool  oot = cf::feat::g_cfg.openOnTouch.load();
            float rng = cf::feat::g_cfg.maxRange_m.load();
            float ms  = cf::feat::g_cfg.cannonMuzzleSpeed.load();
            float cd  = cf::feat::g_cfg.cooldownSec.load();
            int   bpb = cf::feat::g_cfg.chestsPerBurst.load();
            if(ImGui::Checkbox("enabled",             &e))   cf::feat::g_cfg.enabled.store(e);
            if(ImGui::Checkbox("teleport into cannon",&tp))  cf::feat::g_cfg.teleportIntoCannon.store(tp);
            if(ImGui::Checkbox("auto-fire",           &af))  cf::feat::g_cfg.autoFire.store(af);
            if(ImGui::Checkbox("open on touch",       &oot)) cf::feat::g_cfg.openOnTouch.store(oot);
            if(ImGui::Checkbox("skip looted",         &skl)) cf::feat::g_cfg.skipLootedChests.store(skl);
            if(ImGui::SliderFloat("range (m)",        &rng, 100.f, 2500.f))  cf::feat::g_cfg.maxRange_m.store(rng);
            if(ImGui::SliderFloat("muzzle speed",     &ms,  1500.f, 9000.f)) cf::feat::g_cfg.cannonMuzzleSpeed.store(ms);
            if(ImGui::SliderFloat("cooldown (s)",     &cd,  0.1f, 3.f))      cf::feat::g_cfg.cooldownSec.store(cd);
            if(ImGui::SliderInt  ("chests per burst", &bpb, 1, 10))          cf::feat::g_cfg.chestsPerBurst.store(bpb);
        }

        if(ImGui::CollapsingHeader("speed")){
            bool  e   = cf::feat::g_speedCfg.enabled.load();
            float ws  = cf::feat::g_speedCfg.walkSpeed.load();
            float fs  = cf::feat::g_speedCfg.flySpeed.load();
            float ac  = cf::feat::g_speedCfg.acceleration.load();
            float air = cf::feat::g_speedCfg.airControl.load();
            float grv = cf::feat::g_speedCfg.gravityScale.load();
            float jmp = cf::feat::g_speedCfg.jumpZ.load();
            if(ImGui::Checkbox("speed enabled",  &e))   cf::feat::g_speedCfg.enabled.store(e);
            if(ImGui::SliderFloat("walk",        &ws,  600.f, 9000.f))  cf::feat::g_speedCfg.walkSpeed.store(ws);
            if(ImGui::SliderFloat("fly",         &fs,  1000.f,12000.f)) cf::feat::g_speedCfg.flySpeed.store(fs);
            if(ImGui::SliderFloat("accel",       &ac,  2000.f,40000.f)) cf::feat::g_speedCfg.acceleration.store(ac);
            if(ImGui::SliderFloat("air control", &air, 0.1f, 1.f))      cf::feat::g_speedCfg.airControl.store(air);
            if(ImGui::SliderFloat("gravity",     &grv, 0.05f,1.5f))     cf::feat::g_speedCfg.gravityScale.store(grv);
            if(ImGui::SliderFloat("jump z",      &jmp, 400.f, 2500.f))  cf::feat::g_speedCfg.jumpZ.store(jmp);
        }

        if(ImGui::CollapsingHeader("esp")){
            bool e   = cf::feat::g_espCfg.enabled.load();
            bool c   = cf::feat::g_espCfg.drawChests.load();
            bool ca  = cf::feat::g_espCfg.drawCannons.load();
            bool pl  = cf::feat::g_espCfg.drawPlayers.load();
            bool ln  = cf::feat::g_espCfg.drawLines.load();
            float r  = cf::feat::g_espCfg.maxDraw_m.load();
            if(ImGui::Checkbox("esp enabled",    &e))  cf::feat::g_espCfg.enabled.store(e);
            if(ImGui::Checkbox("chests",         &c))  cf::feat::g_espCfg.drawChests.store(c);
            if(ImGui::Checkbox("cannons",        &ca)) cf::feat::g_espCfg.drawCannons.store(ca);
            if(ImGui::Checkbox("players",        &pl)) cf::feat::g_espCfg.drawPlayers.store(pl);
            if(ImGui::Checkbox("tracers",        &ln)) cf::feat::g_espCfg.drawLines.store(ln);
            if(ImGui::SliderFloat("range (m)",   &r, 50.f, 2000.f)) cf::feat::g_espCfg.maxDraw_m.store(r);
        }

        ImGui::Separator();
        ImGui::TextUnformatted("INSERT menu  ·  F9 speed  ·  F10 cannon  ·  F11 esp  ·  END panic");
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

} // namespace

void PublishDrawList(std::vector<cf::feat::DrawItem> items){
    std::scoped_lock _(g_dlLock);
    g_dl = std::move(items);
}

int Run(TickFn perFrame){
    if(!CreateWindow_())   return -1;
    if(!CreateDevice())    return -2;

#if CF_HAS_IMGUI
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
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
        if(TogglePress(g_ovCfg.menuKey.load(),  em)) g_ovCfg.showMenu.store(!g_ovCfg.showMenu.load());
        if(TogglePress(g_ovCfg.panicKey.load(), ep)) g_quit=true;
        if(TogglePress(g_ovCfg.toggleCannonKey.load(), ec))
            cf::feat::g_cfg.enabled.store(!cf::feat::g_cfg.enabled.load());
        if(TogglePress(g_ovCfg.toggleSpeedKey.load(),  es))
            cf::feat::g_speedCfg.enabled.store(!cf::feat::g_speedCfg.enabled.load());
        if(TogglePress(g_ovCfg.toggleEspKey.load(),    eesp))
            cf::feat::g_espCfg.enabled.store(!cf::feat::g_espCfg.enabled.load());

        auto now = std::chrono::steady_clock::now();
        float dt = std::chrono::duration<float>(now - last).count();
        last = now;

        if(perFrame) perFrame(dt, (float)g_w, (float)g_h);
        DrawFrame();
    }

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
