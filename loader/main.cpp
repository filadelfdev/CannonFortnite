// loader.exe — chain injector for the 5-DLL cheat.
//
// The cheat is split into five binaries that must land in the target
// process in dependency order:
//
//     cheat_core.dll       — process memory, engine SDK, log, report
//     cheat_antiban.dll    — jitter, clamp, behaviour layer
//     cheat_ui.dll         — ImGui overlay, D3D11 Present hook
//     cheat_features.dll   — cannon_chest, aimbot, loot, esp, …
//     cheat.dll            — main orchestrator (runs on DllMain)
//
// The loader opens Fortnite, allocates a wide string, and calls
// LoadLibraryW remotely for each DLL in order. Order matters: cheat.dll
// has static imports against every aux DLL, so those must already be
// resident when it loads or the import table fails to resolve.
//
// Attribution: Built by Olivia — https://discord.gg/ojailbreaks

#include <windows.h>
#include <tlhelp32.h>
#include <cstdio>
#include <cwchar>
#include <filesystem>
#include <string>
#include <array>

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

// One remote LoadLibraryW round-trip. Returns true on success. Shares
// nothing between calls, so a failure on one DLL doesn't poison the
// next one — the caller decides whether to continue.
bool InjectOne(HANDLE proc, const std::wstring& dllPath){
    size_t bytes = (dllPath.size() + 1) * sizeof(wchar_t);
    void* remote = VirtualAllocEx(proc, nullptr, bytes,
                                  MEM_COMMIT|MEM_RESERVE, PAGE_READWRITE);
    if(!remote) return false;

    SIZE_T wrote = 0;
    if(!WriteProcessMemory(proc, remote, dllPath.c_str(), bytes, &wrote) ||
       wrote != bytes){
        VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
        return false;
    }

    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    LPTHREAD_START_ROUTINE fnLoadLib =
        (LPTHREAD_START_ROUTINE)GetProcAddress(k32, "LoadLibraryW");

    HANDLE th = CreateRemoteThread(proc, nullptr, 0, fnLoadLib, remote, 0, nullptr);
    DWORD exitCode = 0;
    bool ok = false;
    if(th){
        WaitForSingleObject(th, 10000);
        GetExitCodeThread(th, &exitCode);
        CloseHandle(th);
        // Exit code is the HMODULE LoadLibraryW returned — truncated to
        // 32 bits on x64 but still non-zero when the load succeeded.
        ok = exitCode != 0;
    }
    VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
    return ok;
}

} // namespace

int wmain(int argc, wchar_t** /*argv*/){
    SetConsoleTitleW(L"CF loader");

    // Everything is expected to live in the same folder as loader.exe.
    wchar_t me[MAX_PATH];
    GetModuleFileNameW(nullptr, me, MAX_PATH);
    std::filesystem::path baseDir = std::filesystem::path(me).parent_path();

    // Dependency order — the aux DLLs first so cheat.dll's import
    // resolution lands on already-resident modules.
    static constexpr std::array<const wchar_t*, 5> kChain = {
        L"cheat_core.dll",
        L"cheat_antiban.dll",
        L"cheat_ui.dll",
        L"cheat_features.dll",
        L"cheat.dll",
    };

    // Preflight — every file must exist before we attach to Fortnite
    // so we don't inject a partial chain.
    for(const wchar_t* name : kChain){
        auto p = baseDir / name;
        if(!std::filesystem::exists(p)){
            fwprintf(stderr, L"[loader] missing: %ls\n", p.c_str());
            fwprintf(stderr, L"[loader]   all 5 DLLs must sit next to loader.exe\n");
            return 2;
        }
    }

    // Allow a single-arg override that skips the preflight — "this
    // file, nothing else" — same path argc > 1 used before the split.
    // Kept so the existing debugging workflow (iterate one DLL at a
    // time) still works.
    if(argc > 1){
        fwprintf(stderr, L"[loader] single-DLL mode is no longer supported;\n"
                         L"[loader]   drop a complete chain next to loader.exe\n");
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
    wprintf(L"[loader] Fortnite PID=%lu\n", pid);

    HANDLE proc = OpenProcess(PROCESS_CREATE_THREAD|PROCESS_VM_OPERATION|
                              PROCESS_VM_WRITE|PROCESS_VM_READ|
                              PROCESS_QUERY_INFORMATION,
                              FALSE, pid);
    if(!proc){
        fwprintf(stderr, L"[loader] OpenProcess failed (error %lu)\n", GetLastError());
        return 4;
    }

    // Inject in dependency order; abort the chain on the first failure
    // so we don't leave the target with a half-initialised cheat.
    for(const wchar_t* name : kChain){
        auto path = (baseDir / name).wstring();
        if(!InjectOne(proc, path)){
            fwprintf(stderr, L"[loader] injection failed on %ls (error %lu)\n",
                     name, GetLastError());
            CloseHandle(proc);
            return 5;
        }
        wprintf(L"[loader]   injected %ls\n", name);
    }

    CloseHandle(proc);
    wprintf(L"[loader] all 5 DLLs loaded. Overlay should appear in-game.\n");
    return 0;
}
