// loader.exe — standalone launcher that drops the cheat DLL into the
// target process.
//
// Keep this simple: open target → write DLL path → CreateRemoteThread
// on LoadLibraryW. This is the baseline injection; any hardening of
// how the DLL ends up in the process happens here, not in the DLL.
//
// Attribution: Built by Olivia — https://discord.gg/ojailbreaks

#include <windows.h>
#include <tlhelp32.h>
#include <cstdio>
#include <cwchar>
#include <filesystem>
#include <string>

namespace {

DWORD FindTargetPid(const wchar_t* exeName){
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if(snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe{sizeof(pe)};
    DWORD pid = 0;
    for(BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)){
        if(_wcsicmp(pe.szExeFile, exeName) == 0){ pid = pe.th32ProcessID; break; }
    }
    CloseHandle(snap);
    return pid;
}

bool InjectLoadLibrary(DWORD pid, const std::wstring& dllPath){
    HANDLE proc = OpenProcess(PROCESS_CREATE_THREAD|PROCESS_VM_OPERATION|
                              PROCESS_VM_WRITE|PROCESS_VM_READ|
                              PROCESS_QUERY_INFORMATION,
                              FALSE, pid);
    if(!proc) return false;

    size_t bytes = (dllPath.size() + 1) * sizeof(wchar_t);
    void* remote = VirtualAllocEx(proc, nullptr, bytes,
                                  MEM_COMMIT|MEM_RESERVE, PAGE_READWRITE);
    if(!remote){ CloseHandle(proc); return false; }
    SIZE_T wrote = 0;
    if(!WriteProcessMemory(proc, remote, dllPath.c_str(), bytes, &wrote) ||
       wrote != bytes){
        VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
        CloseHandle(proc);
        return false;
    }

    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    LPTHREAD_START_ROUTINE fnLoadLib =
        (LPTHREAD_START_ROUTINE)GetProcAddress(k32, "LoadLibraryW");

    HANDLE th = CreateRemoteThread(proc, nullptr, 0, fnLoadLib, remote, 0, nullptr);
    bool ok = th != nullptr;
    if(th){
        WaitForSingleObject(th, 5000);
        CloseHandle(th);
    }
    VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
    CloseHandle(proc);
    return ok;
}

} // namespace

int wmain(int argc, wchar_t** argv){
    SetConsoleTitleW(L"CF loader");

    // DLL to inject.
    //   argv[1]  → use that path (useful for the auxiliary DLLs listed
    //              below once they exist)
    //   no arg   → inject `cheat.dll` from the loader's folder
    //
    // Auxiliary DLLs (built as separate targets but shipped next to
    // cheat.dll) are currently stubbed — `cheat.dll` is the main
    // module that does the real work. Listed here so the loader can
    // walk the folder and inject them in order when we split:
    //   cheat.dll            — main entry, boots overlay + features
    //   cheat_core.dll       — process memory, hook engine (stub)
    //   cheat_antiban.dll    — jitter, clamp, behaviour layer (stub)
    //   cheat_ui.dll         — overlay + ImGui + D3D11 hook (stub)
    //   cheat_features.dll   — cannon chest, aimbot, esp (stub)
    std::wstring dllPath;
    if(argc > 1){
        dllPath = argv[1];
    } else {
        wchar_t me[MAX_PATH];
        GetModuleFileNameW(nullptr, me, MAX_PATH);
        std::filesystem::path p = me;
        p.replace_filename(L"cheat.dll");
        dllPath = p.wstring();
    }

    if(!std::filesystem::exists(dllPath)){
        fwprintf(stderr, L"[loader] cheat.dll not found: %ls\n", dllPath.c_str());
        fwprintf(stderr, L"[loader]   make sure cheat.dll sits next to loader.exe\n");
        return 2;
    }

    wprintf(L"[loader] waiting for Fortnite...\n");
    DWORD pid = 0;
    for(int i = 0; i < 120 && !pid; ++i){
        pid = FindTargetPid(L"FortniteClient-Win64-Shipping.exe");
        if(!pid) Sleep(1000);
    }
    if(!pid){
        fwprintf(stderr, L"[loader] Fortnite process not found (2 min timeout).\n");
        return 3;
    }
    wprintf(L"[loader] Fortnite PID=%lu — injecting %ls\n", pid, dllPath.c_str());

    if(!InjectLoadLibrary(pid, dllPath)){
        fwprintf(stderr, L"[loader] injection failed (error %lu)\n", GetLastError());
        return 4;
    }
    wprintf(L"[loader] done. Overlay should appear in-game.\n");
    return 0;
}
