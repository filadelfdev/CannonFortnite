#include "antiban/env_lie.h"
#include "antiban/hash.h"
#include "util/log.h"

#include <windows.h>
#include <winternl.h>
#include <cstring>
#include <atomic>

namespace cf::ab::envlie {

namespace {

std::atomic<bool> g_armed{false};

constexpr ULONG SystemCodeIntegrityInformation       = 0x67;
constexpr ULONG SystemModuleInformation              = 0x0B;
constexpr ULONG SystemBootEnvironmentInformation     = 0x5A;

struct SYSTEM_CODEINTEGRITY_INFORMATION {
    ULONG  Length;
    ULONG  CodeIntegrityOptions;
};
constexpr ULONG CODEINTEGRITY_OPTION_TESTSIGN             = 0x02;
constexpr ULONG CODEINTEGRITY_OPTION_HVCI_KMCI_ENABLED    = 0x400;

using NtQuerySystemInformation_t = NTSTATUS(NTAPI*)(ULONG, PVOID, ULONG, PULONG);
using NtEnumerateKey_t           = NTSTATUS(NTAPI*)(HANDLE, ULONG, ULONG, PVOID, ULONG, PULONG);
using NtEnumerateValueKey_t      = NTSTATUS(NTAPI*)(HANDLE, ULONG, ULONG, PVOID, ULONG, PULONG);

NtQuerySystemInformation_t g_realNtQsi = nullptr;
NtEnumerateKey_t           g_realNtEk  = nullptr;
NtEnumerateValueKey_t      g_realNtEvk = nullptr;

// Hashes of the service / driver names we need to hide.
const uint64_t kHiddenHashes[] = {
    cf::ab::fnv1a_lower("CfRead"),
    cf::ab::fnv1a_lower("CfSpoof"),
    cf::ab::fnv1a_lower("cfread.sys"),
    cf::ab::fnv1a_lower("cfspoof.sys"),
};

bool MatchHidden(const wchar_t* name, size_t len){
    char buf[260];
    size_t n = len < 259 ? len : 259;
    for(size_t i=0;i<n;++i) buf[i] = (char)(name[i] < 128 ? name[i] : '?');
    buf[n] = 0;
    uint64_t h = cf::ab::fnv1a_lower(buf);
    for(auto hh : kHiddenHashes) if(h == hh) return true;
    return false;
}

NTSTATUS NTAPI HookedNtQsi(ULONG infoClass, PVOID buf, ULONG len, PULONG ret){
    NTSTATUS s = g_realNtQsi(infoClass, buf, len, ret);
    if(!NT_SUCCESS(s) || !buf) return s;

    if(infoClass == SystemCodeIntegrityInformation && len >= sizeof(SYSTEM_CODEINTEGRITY_INFORMATION)){
        auto* ci = (SYSTEM_CODEINTEGRITY_INFORMATION*)buf;
        // Pretend testsigning is OFF and HVCI is ON (the "secure" reading).
        ci->CodeIntegrityOptions &= ~CODEINTEGRITY_OPTION_TESTSIGN;
        ci->CodeIntegrityOptions |=  CODEINTEGRITY_OPTION_HVCI_KMCI_ENABLED;
        return s;
    }

    if(infoClass == SystemModuleInformation && *ret > 8){
        // RTL_PROCESS_MODULES layout.
        struct ModuleEntry {
            HANDLE Section; PVOID MappedBase; PVOID ImageBase;
            ULONG  ImageSize; ULONG Flags;
            USHORT LoadOrderIndex; USHORT InitOrderIndex;
            USHORT LoadCount; USHORT OffsetToFileName;
            CHAR   FullPathName[256];
        };
        struct Modules { ULONG Count; ModuleEntry Entries[1]; };
        auto* m = (Modules*)buf;
        for(ULONG i = 0; i < m->Count; ){
            auto& e = m->Entries[i];
            const char* name = e.FullPathName + e.OffsetToFileName;
            uint64_t h = cf::ab::fnv1a_lower(name);
            bool hide = false;
            for(auto hh : kHiddenHashes) if(h == hh){ hide = true; break; }
            if(hide){
                // Shift the tail down by one entry.
                memmove(&m->Entries[i], &m->Entries[i+1],
                        sizeof(ModuleEntry) * (m->Count - i - 1));
                m->Count--;
            } else {
                ++i;
            }
        }
        return s;
    }

    return s;
}

NTSTATUS NTAPI HookedNtEk(HANDLE k, ULONG idx, ULONG cls, PVOID buf, ULONG len, PULONG ret){
    NTSTATUS s = g_realNtEk(k, idx, cls, buf, len, ret);
    if(!NT_SUCCESS(s) || !buf) return s;

    // KEY_BASIC_INFORMATION layout for cls == 0.
    struct KeyBasic { LARGE_INTEGER lwt; ULONG titleIdx; ULONG nameLen; WCHAR name[1]; };
    if(cls == 0){
        auto* kb = (KeyBasic*)buf;
        if(MatchHidden(kb->name, kb->nameLen/2)){
            // Pretend this index doesn't exist.
            return STATUS_NO_MORE_ENTRIES;
        }
    }
    return s;
}

NTSTATUS NTAPI HookedNtEvk(HANDLE k, ULONG idx, ULONG cls, PVOID buf, ULONG len, PULONG ret){
    NTSTATUS s = g_realNtEvk(k, idx, cls, buf, len, ret);
    if(!NT_SUCCESS(s) || !buf) return s;

    struct ValueBasic { ULONG titleIdx; ULONG type; ULONG nameLen; WCHAR name[1]; };
    if(cls == 0){
        auto* vb = (ValueBasic*)buf;
        if(MatchHidden(vb->name, vb->nameLen/2)){
            return STATUS_NO_MORE_ENTRIES;
        }
    }
    return s;
}

// 14-byte absolute jmp trampoline installer. Returns the tramp that
// lets the hook call the original.
template<typename T>
T InstallJmp(void* target, void* hook){
    DWORD old=0; VirtualProtect(target, 16, PAGE_EXECUTE_READWRITE, &old);
    // alloc near trampoline
    auto tramp = (BYTE*)VirtualAlloc(nullptr, 64,
                                     MEM_COMMIT|MEM_RESERVE,
                                     PAGE_EXECUTE_READWRITE);
    memcpy(tramp, target, 16);
    tramp[16]=0xFF; tramp[17]=0x25;
    *(uint32_t*)(tramp+18)=0;
    *(uintptr_t*)(tramp+22)=(uintptr_t)target+16;

    BYTE patch[14] = { 0xFF, 0x25, 0, 0, 0, 0 };
    *(uintptr_t*)(patch+6)=(uintptr_t)hook;
    memcpy(target, patch, 14);
    VirtualProtect(target, 16, old, &old);
    return (T)tramp;
}

} // namespace

void InstallAll(){
    if(g_armed.exchange(true)) return;
    HMODULE ntd = GetModuleHandleA("ntdll.dll");

    auto nQsi = GetProcAddress(ntd, "NtQuerySystemInformation");
    auto nEk  = GetProcAddress(ntd, "NtEnumerateKey");
    auto nEvk = GetProcAddress(ntd, "NtEnumerateValueKey");

    if(nQsi) g_realNtQsi = InstallJmp<NtQuerySystemInformation_t>(nQsi, (void*)HookedNtQsi);
    if(nEk)  g_realNtEk  = InstallJmp<NtEnumerateKey_t          >(nEk,  (void*)HookedNtEk);
    if(nEvk) g_realNtEvk = InstallJmp<NtEnumerateValueKey_t     >(nEvk, (void*)HookedNtEvk);

    CF_INFO("env_lie: NtQsi=%p NtEk=%p NtEvk=%p", nQsi, nEk, nEvk);
}

void RemoveAll(){ /* process-death only; no restore needed */ }

bool Armed(){ return g_armed.load(); }

} // namespace cf::ab::envlie
