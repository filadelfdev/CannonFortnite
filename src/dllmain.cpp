// dllmain.cpp — Fortnite-side DLL entry for cheat.dll, the main
// orchestrator in the 5-DLL chain.
//
// Load order (the loader guarantees this, but the Boot thread also
// self-heals by force-loading any aux module that isn't already in
// the process):
//   cheat_core     ← primitives, engine SDK, report, log
//   cheat_antiban  ← jitter, clamp
//   cheat_ui       ← ImGui overlay, D3D11 Present hook
//   cheat_features ← cannon_chest, aimbot, loot, …
//   cheat          ← this module
//
// All auxiliary DLLs are resolved as static imports against this
// module's import table. If any one is missing the OS loader refuses
// to map us and DllMain never fires — the LoadLibraryW calls below
// are therefore belt-and-suspenders for the side-by-side-folder case,
// not a strict dependency gate.
//
// Attribution: Built by Olivia — https://discord.gg/ojailbreaks

#include <windows.h>
#include "core/process.h"
#include "util/log.h"

namespace cf { int Run(); }

namespace {

// Names of the four aux modules, in dependency order. Matched against
// loaded modules by filename; a missing one is logged and skipped —
// the import table already covers the actual link.
constexpr const wchar_t* kAuxChain[] = {
    L"cheat_core.dll",
    L"cheat_antiban.dll",
    L"cheat_ui.dll",
    L"cheat_features.dll",
};

void LogAuxChain(){
    for(const wchar_t* name : kAuxChain){
        HMODULE m = GetModuleHandleW(name);
        if(m){
            CF_INFO("cheat :: aux resident %ls @ %p", name, (void*)m);
        } else {
            // Side-by-side-folder self-heal: force-load from the same
            // directory the loader dropped us in. This covers the
            // "user ran rundll32 on cheat.dll alone" case where the
            // static imports haven't located the aux DLLs yet.
            m = LoadLibraryW(name);
            CF_INFO("cheat :: aux load  %ls -> %p", name, (void*)m);
        }
    }
}

} // namespace

static DWORD WINAPI Boot(LPVOID){
    // Give the game a moment to finish its own init so GWorld etc.
    // actually point somewhere valid before the first tick.
    Sleep(1500);
    LogAuxChain();
    cf::Run();
    return 0;
}

BOOL APIENTRY DllMain(HMODULE mod, DWORD reason, LPVOID){
    if(reason == DLL_PROCESS_ATTACH){
        DisableThreadLibraryCalls(mod);
        HANDLE t = CreateThread(nullptr, 0, Boot, nullptr, 0, nullptr);
        if(t) CloseHandle(t);
    }
    return TRUE;
}
