#include "core/memory.h"
#include "core/kmem.h"
#include "antiban/hash.h"

#include <windows.h>
#include <winternl.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <mutex>
#include <atomic>
#include <cstring>
#include <vector>

#pragma comment(lib, "ntdll")

namespace {

using NtQuerySystemInformation_t = NTSTATUS (NTAPI*)(ULONG, PVOID, ULONG, PULONG);
using NtDuplicateObject_t        = NTSTATUS (NTAPI*)(HANDLE, HANDLE, HANDLE, PHANDLE,
                                                     ACCESS_MASK, ULONG, ULONG);
using NtReadVirtualMemory_t      = NTSTATUS (NTAPI*)(HANDLE, PVOID, PVOID, SIZE_T, PSIZE_T);
using NtWriteVirtualMemory_t     = NTSTATUS (NTAPI*)(HANDLE, PVOID, PVOID, SIZE_T, PSIZE_T);
using NtQueryInformationProcess_t= NTSTATUS (NTAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);

struct Nt {
    NtQuerySystemInformation_t  QuerySystemInformation{};
    NtDuplicateObject_t         DuplicateObject{};
    NtReadVirtualMemory_t       ReadVirtualMemory{};
    NtWriteVirtualMemory_t      WriteVirtualMemory{};
    NtQueryInformationProcess_t QueryInformationProcess{};
} g_nt;

struct State {
    HANDLE   hProc{};
    uint32_t pid{};
    uintptr_t base{};
    std::atomic<bool> useKernel{false};
    std::mutex lock;
} g;

// Hash resolver — strings never land on disk.
template<typename T> T Resolve(HMODULE m, uint64_t hash){
    auto dos = (PIMAGE_DOS_HEADER)m;
    auto nt  = (PIMAGE_NT_HEADERS)((BYTE*)m + dos->e_lfanew);
    auto exp = (PIMAGE_EXPORT_DIRECTORY)((BYTE*)m +
               nt->OptionalHeader.DataDirectory[0].VirtualAddress);
    auto names= (DWORD*)((BYTE*)m + exp->AddressOfNames);
    auto ords = (WORD*) ((BYTE*)m + exp->AddressOfNameOrdinals);
    auto funcs= (DWORD*)((BYTE*)m + exp->AddressOfFunctions);
    for(DWORD i=0;i<exp->NumberOfNames;++i){
        auto name=(const char*)((BYTE*)m + names[i]);
        if(cf::ab::fnv1a(name)==hash) return (T)((BYTE*)m + funcs[ords[i]]);
    }
    return nullptr;
}

constexpr uint64_t H(const char* s){ return cf::ab::fnv1a(s); }

void LoadNt(){
    HMODULE ntd = GetModuleHandleA("ntdll.dll");
    g_nt.QuerySystemInformation  = Resolve<NtQuerySystemInformation_t >(ntd, H("NtQuerySystemInformation"));
    g_nt.DuplicateObject         = Resolve<NtDuplicateObject_t        >(ntd, H("NtDuplicateObject"));
    g_nt.ReadVirtualMemory       = Resolve<NtReadVirtualMemory_t      >(ntd, H("NtReadVirtualMemory"));
    g_nt.WriteVirtualMemory      = Resolve<NtWriteVirtualMemory_t     >(ntd, H("NtWriteVirtualMemory"));
    g_nt.QueryInformationProcess = Resolve<NtQueryInformationProcess_t>(ntd, H("NtQueryInformationProcess"));
}

// SystemHandleInformation = 0x10 / SystemExtendedHandleInformation = 0x40
constexpr ULONG SystemExtendedHandleInformation = 0x40;

struct SYSTEM_HANDLE_TABLE_ENTRY_INFO_EX {
    PVOID    Object;
    ULONG_PTR UniqueProcessId;
    ULONG_PTR HandleValue;
    ULONG    GrantedAccess;
    USHORT   CreatorBackTraceIndex;
    USHORT   ObjectTypeIndex;
    ULONG    HandleAttributes;
    ULONG    Reserved;
};
struct SYSTEM_HANDLE_INFORMATION_EX {
    ULONG_PTR NumberOfHandles;
    ULONG_PTR Reserved;
    SYSTEM_HANDLE_TABLE_ENTRY_INFO_EX Handles[1];
};

uint32_t FindPidByImageHash(uint64_t hash){
    // walk via Toolhelp — cheap, well-aged, and the PID we return isn't
    // the handle we use. The handle comes from the duplicate path below.
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if(snap==INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe{sizeof(pe)};
    uint32_t pid=0;
    for(BOOL ok=Process32FirstW(snap,&pe); ok; ok=Process32NextW(snap,&pe)){
        if(cf::ab::fnv1a_w(pe.szExeFile)==hash){ pid=pe.th32ProcessID; break; }
    }
    CloseHandle(snap);
    return pid;
}

HANDLE DuplicateFromAnyProcess(uint32_t targetPid){
    ULONG size = 1<<20;
    std::vector<BYTE> buf(size);
    NTSTATUS st=0;
    for(int tries=0; tries<8; ++tries){
        ULONG ret=0;
        st = g_nt.QuerySystemInformation(SystemExtendedHandleInformation,
                                          buf.data(), (ULONG)buf.size(), &ret);
        if(st==0) break;
        buf.resize(buf.size()*2);
    }
    if(st) return nullptr;

    auto info = (SYSTEM_HANDLE_INFORMATION_EX*)buf.data();
    HANDLE cur = GetCurrentProcess();
    for(ULONG_PTR i=0;i<info->NumberOfHandles;++i){
        auto& e = info->Handles[i];
        if((uint32_t)e.UniqueProcessId == GetCurrentProcessId()) continue;
        if(!(e.GrantedAccess & PROCESS_VM_READ))                 continue;
        HANDLE src = OpenProcess(PROCESS_DUP_HANDLE, FALSE, (DWORD)e.UniqueProcessId);
        if(!src) continue;
        HANDLE dup=nullptr;
        if(g_nt.DuplicateObject(src, (HANDLE)e.HandleValue, cur, &dup,
                                 PROCESS_VM_READ|PROCESS_VM_WRITE|PROCESS_QUERY_INFORMATION,
                                 0, 0)==0 && dup){
            // check the duped handle points at targetPid
            PROCESS_BASIC_INFORMATION pbi{};
            ULONG ret=0;
            if(g_nt.QueryInformationProcess(dup,0,&pbi,sizeof(pbi),&ret)==0 &&
               (uint32_t)(uintptr_t)pbi.UniqueProcessId == targetPid){
                CloseHandle(src);
                return dup;
            }
            CloseHandle(dup);
        }
        CloseHandle(src);
    }
    return nullptr;
}

uintptr_t FindMainModule(HANDLE h){
    HMODULE mods[512];
    DWORD need=0;
    if(!EnumProcessModulesEx(h, mods, sizeof(mods), &need, LIST_MODULES_64BIT)) return 0;
    return (uintptr_t)mods[0];
}

} // namespace

namespace cf::mem {

bool Attach(const wchar_t* imageHash){
    if(!g_nt.ReadVirtualMemory) LoadNt();
    uint64_t hash = std::wcstoull(imageHash, nullptr, 16);
    uint32_t pid  = FindPidByImageHash(hash);
    if(!pid) return false;
    HANDLE h = DuplicateFromAnyProcess(pid);
    if(!h){
        // fallback: direct open with fewer rights, catch the detection
        // noise in antiban instead of giving up.
        h = OpenProcess(PROCESS_VM_READ|PROCESS_VM_WRITE|PROCESS_QUERY_INFORMATION,
                        FALSE, pid);
        if(!h) return false;
    }
    std::scoped_lock _(g.lock);
    g.hProc=h; g.pid=pid; g.base=FindMainModule(h);
    return g.base!=0;
}

void Detach(){
    std::scoped_lock _(g.lock);
    if(g.hProc) CloseHandle(g.hProc);
    g = {};
}

uintptr_t Base(){ return g.base; }
uint32_t  Pid (){ return g.pid;  }
bool IsAttached(){ return g.hProc!=nullptr && g.base!=0; }

uintptr_t ModuleBase(uint64_t nameHash){
    HMODULE mods[1024]; DWORD need=0;
    if(!EnumProcessModulesEx(g.hProc, mods, sizeof(mods), &need, LIST_MODULES_64BIT)) return 0;
    wchar_t name[MAX_PATH];
    for(DWORD i=0;i<need/sizeof(HMODULE);++i){
        if(GetModuleBaseNameW(g.hProc, mods[i], name, MAX_PATH)){
            if(cf::ab::fnv1a_w(name)==nameHash) return (uintptr_t)mods[i];
        }
    }
    return 0;
}

bool Read(uintptr_t addr, void* out, size_t n){
    if(g.useKernel.load() && cf::kmem::Online())
        return cf::kmem::Read(addr, out, n);
    if(!g.hProc) return false;
    SIZE_T r=0;
    return g_nt.ReadVirtualMemory(g.hProc,(PVOID)addr,out,n,&r)==0 && r==n;
}

bool Write(uintptr_t addr, const void* in, size_t n){
    if(g.useKernel.load() && cf::kmem::Online())
        return cf::kmem::Write(addr, in, n);
    if(!g.hProc) return false;
    SIZE_T w=0;
    return g_nt.WriteVirtualMemory(g.hProc,(PVOID)addr,(PVOID)in,n,&w)==0 && w==n;
}

bool Scatter(ScatterRange* r, size_t count){
    if(g.useKernel.load() && cf::kmem::Online()){
        std::vector<cf::kmem::Req> reqs(count);
        for(size_t i=0;i<count;++i) reqs[i] = {r[i].src, r[i].dst, r[i].size};
        return cf::kmem::Batch(reqs.data(), reqs.size());
    }
    bool ok=true;
    for(size_t i=0;i<count;++i) ok &= Read(r[i].src, r[i].dst, r[i].size);
    return ok;
}

void UseKernel(bool on){ g.useKernel.store(on); }
bool KernelActive(){ return g.useKernel.load() && cf::kmem::Online(); }

uintptr_t Pattern(uintptr_t modBase, size_t modSize,
                  const char* pattern, const char* mask){
    size_t len = strlen(mask);
    if(len == 0 || modSize < len) return 0;

    // Chunked scan — Fortnite's main image is 100+ MB and a single
    // vector<BYTE>(modSize) would allocate that in one block. Scan
    // 2 MB at a time with a `len`-byte overlap so hits straddling a
    // chunk boundary aren't lost.
    constexpr size_t kChunk = 2 * 1024 * 1024;
    std::vector<BYTE> buf(kChunk + len);

    size_t pos = 0;
    size_t end = modSize - len;
    while(pos <= end){
        size_t take = std::min(kChunk + len, modSize - pos);
        if(!Read(modBase + pos, buf.data(), take)) return 0;
        size_t scan = take >= len ? take - len : 0;
        for(size_t i=0; i <= scan; ++i){
            size_t j = 0;
            for(; j < len; ++j){
                if(mask[j] == '?') continue;
                if(buf[i+j] != (BYTE)pattern[j]) break;
            }
            if(j == len) return modBase + pos + i;
        }
        if(take < kChunk + len) break;   // last chunk was short
        pos += kChunk;
    }
    return 0;
}

} // namespace cf::mem
