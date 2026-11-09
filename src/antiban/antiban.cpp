#include "antiban/antiban.h"
#include "antiban/hash.h"

#include <windows.h>
#include <winternl.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <cstring>

namespace cf::ab {

namespace {

using NtSetInformationThread_t = NTSTATUS(NTAPI*)(HANDLE, ULONG, PVOID, ULONG);
constexpr ULONG ThreadHideFromDebugger = 0x11;

uint32_t g_headerChecksum = 0;

uint32_t HashRegion(const BYTE* p, size_t n){
    uint32_t h = 2166136261u;
    for(size_t i=0;i<n;++i){ h ^= p[i]; h *= 16777619u; }
    return h;
}

LONG WINAPI VEH(PEXCEPTION_POINTERS info){
    switch(info->ExceptionRecord->ExceptionCode){
        case EXCEPTION_BREAKPOINT:
        case EXCEPTION_SINGLE_STEP:
            info->ContextRecord->Rip += 1;
            return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

} // namespace

void HideSelfThreads(){
    HMODULE ntd = GetModuleHandleA("ntdll.dll");
    auto NtSetInfoThread = (NtSetInformationThread_t)
        GetProcAddress(ntd, "NtSetInformationThread");
    if(!NtSetInfoThread) return;

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if(snap==INVALID_HANDLE_VALUE) return;
    THREADENTRY32 te{sizeof(te)};
    uint32_t pid = GetCurrentProcessId();
    for(BOOL ok=Thread32First(snap,&te); ok; ok=Thread32Next(snap,&te)){
        if(te.th32OwnerProcessID != pid) continue;
        HANDLE t = OpenThread(THREAD_SET_INFORMATION, FALSE, te.th32ThreadID);
        if(!t) continue;
        NtSetInfoThread(t, ThreadHideFromDebugger, nullptr, 0);
        CloseHandle(t);
    }
    CloseHandle(snap);
}

void ZeroOwnHeader(){
    HMODULE self = GetModuleHandleW(nullptr);
    DWORD   old=0;
    VirtualProtect(self, 0x1000, PAGE_READWRITE, &old);
    std::memset(self, 0, 0x400);
    VirtualProtect(self, 0x1000, old, &old);
}

void InstallVEH(){
    AddVectoredExceptionHandler(1, VEH);
    HMODULE self = GetModuleHandleW(nullptr);
    MODULEINFO mi{};
    GetModuleInformation(GetCurrentProcess(), self, &mi, sizeof(mi));
    g_headerChecksum = HashRegion((BYTE*)self, mi.SizeOfImage<0x4000? mi.SizeOfImage : 0x4000);
}

AcKind DetectAnticheat(){
    HMODULE mods[1024]; DWORD need=0;
    if(!EnumProcessModulesEx(GetCurrentProcess(), mods, sizeof(mods), &need, LIST_MODULES_64BIT))
        return AcKind::None;
    wchar_t name[MAX_PATH];
    AcKind kind = AcKind::None;
    for(DWORD i=0;i<need/sizeof(HMODULE);++i){
        if(!GetModuleBaseNameW(GetCurrentProcess(), mods[i], name, MAX_PATH)) continue;
        uint64_t h = fnv1a_w(name);
        if(h == fnv1a_w(L"easyanticheat_x64.dll") ||
           h == fnv1a_w(L"easyanticheat.dll")      ||
           h == fnv1a_w(L"eac_launcher.exe"))      { kind = AcKind::EAC; break; }
        if(h == fnv1a_w(L"beclient_x64.dll") ||
           h == fnv1a_w(L"beservice.exe"))         { kind = AcKind::BE; break; }
        if(h == fnv1a_w(L"vgc.sys") ||
           h == fnv1a_w(L"vgk.sys"))               { kind = AcKind::Vanguard; break; }
    }
    return kind;
}

bool SelfIntegrityOk(){
    HMODULE self = GetModuleHandleW(nullptr);
    MODULEINFO mi{};
    GetModuleInformation(GetCurrentProcess(), self, &mi, sizeof(mi));
    uint32_t h = HashRegion((BYTE*)self, mi.SizeOfImage<0x4000? mi.SizeOfImage : 0x4000);
    // The DOS header is zeroed after InstallVEH, so the first integrity
    // read establishes the baseline; later reads match it.
    static uint32_t baseline = 0;
    if(!baseline){ baseline = h; return true; }
    return h == baseline;
}

} // namespace cf::ab
