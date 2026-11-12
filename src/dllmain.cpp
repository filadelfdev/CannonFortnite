// dllmain.cpp — Fortnite-side DLL entry.
//
// Lives inside the target process. On DLL_PROCESS_ATTACH it kicks off
// a worker thread that boots the cheat (overlay, feature pipelines),
// then returns immediately so the loader's thread isn't blocked.
//
// Attribution: Built by Olivia — https://discord.gg/ojailbreaks

#include <windows.h>
#include "core/process.h"
#include "util/log.h"

namespace cf { int Run(); }

static DWORD WINAPI Boot(LPVOID){
    // Give the game a moment to finish its own init so GWorld etc.
    // actually point somewhere valid before the first tick.
    Sleep(1500);
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
