#include "antiban/dxgi_kernel.h"
#include "core/kmem.h"
#include "core/memory.h"
#include "antiban/hash.h"
#include "util/log.h"

#include <windows.h>
#include <psapi.h>
#include <atomic>
#include <mutex>
#include <vector>
#include <cstring>

namespace cf::ab::dxgi_k {

namespace {

// IOCTLs used by this module — mirrored in driver/kread.c extension.
constexpr uint32_t IOCTL_CF_ALLOC_RWX     = 0x81001020;  // alloc in target
constexpr uint32_t IOCTL_CF_FREE_RWX      = 0x81001024;
constexpr uint32_t IOCTL_CF_PATCH_QWORD   = 0x81001028;  // atomic qword write in target

struct AllocReply { uint64_t address; uint32_t size; uint32_t pad; };

std::atomic<bool> g_armed{false};
std::mutex        g_lock;

// State we keep so RemoveFromTarget can undo.
struct Install {
    uint64_t shellcodeAddr{};       // allocated page in target
    uint32_t shellcodeSize{};
    // Original vtable entries we overwrote (so we can restore).
    struct Save { uint64_t slotAddr; uint64_t origFn; };
    std::vector<Save> saved;
} g_inst;

// --- Shellcode -------------------------------------------------------
//
// Compiles to a GetDesc proxy that:
//   1. Saves this + out pointers
//   2. Calls the real GetDesc stored at [page+0x08] (we write this).
//   3. If success (eax == 0), copies 128 bytes from [page+0x100] (the
//      fake DXGI_ADAPTER_DESC) onto the out buffer.
//   4. Returns eax.
//
// This is x64 System V... no, Windows x64 fastcall. RCX=this, RDX=out.
// Layout at page base:
//   +0x00  shellcode entry (16 bytes of absolute jmp pad to actual code)
//   +0x08  real GetDesc pointer (uint64_t)
//   +0x10  shellcode body (~64 bytes)
//   +0x100 fake DXGI_ADAPTER_DESC payload (0x80 bytes)
//
// Assembled from asm:
//   push rbp
//   mov  rbp, rsp
//   sub  rsp, 0x40
//   mov  [rbp-0x08], rcx       ; save this
//   mov  [rbp-0x10], rdx       ; save out
//   mov  rax, [rip - X]        ; load real GetDesc ptr from page+0x08
//   mov  rcx, [rbp-0x08]
//   mov  rdx, [rbp-0x10]
//   call rax
//   test eax, eax
//   jnz  short .done
//   mov  rdi, [rbp-0x10]       ; out
//   lea  rsi, [rip + fake]     ; page+0x100
//   mov  rcx, 16                ; 128 bytes / 8
//   rep  movsq
// .done:
//   add  rsp, 0x40
//   pop  rbp
//   ret

// Hand-assembled bytes. Addresses patched after we know the page base.
// The RIP-relative displacements below assume this body starts at
// (page + 0x10). The two patch sites (real-GetDesc ptr at +0x08, fake
// desc at +0x100) are reached via rip-rel LEA/MOV.
constexpr uint8_t kShellcode[] = {
    0x55,                               // push rbp
    0x48, 0x89, 0xE5,                   // mov rbp, rsp
    0x48, 0x83, 0xEC, 0x40,             // sub rsp, 0x40
    0x48, 0x89, 0x4D, 0xF8,             // mov [rbp-8], rcx
    0x48, 0x89, 0x55, 0xF0,             // mov [rbp-16], rdx
    // mov rax, [rip-0x10]  (page+0x08 relative to rip after this ins)
    0x48, 0x8B, 0x05, 0xEE, 0xFF, 0xFF, 0xFF,   // 7 bytes, rip-rel -18
    0x48, 0x8B, 0x4D, 0xF8,             // mov rcx, [rbp-8]
    0x48, 0x8B, 0x55, 0xF0,             // mov rdx, [rbp-16]
    0xFF, 0xD0,                         // call rax
    0x85, 0xC0,                         // test eax, eax
    0x75, 0x14,                         // jnz +20
    0x57,                               // push rdi
    0x56,                               // push rsi
    0x48, 0x8B, 0x7D, 0xF0,             // mov rdi, [rbp-16]
    0x48, 0x8D, 0x35, 0xB4, 0x00, 0x00, 0x00,   // lea rsi, [rip+0xB4] → page+0x100
    0xB9, 0x10, 0x00, 0x00, 0x00,       // mov ecx, 16
    0xF3, 0x48, 0xA5,                   // rep movsq
    0x5E,                               // pop rsi
    0x5F,                               // pop rdi
    // .done:
    0x48, 0x83, 0xC4, 0x40,             // add rsp, 0x40
    0x5D,                               // pop rbp
    0xC3                                // ret
};

struct Reply { uint64_t address; uint32_t size; uint32_t pad; };

bool DriverAlloc(uint64_t& outAddr, uint32_t& outSize){
    HANDLE h = CreateFileW(L"\\\\.\\CfRead", GENERIC_WRITE,
                           FILE_SHARE_READ|FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, 0, nullptr);
    if(h == INVALID_HANDLE_VALUE) return false;

    uint32_t req = 0x1000;
    Reply rep{};
    DWORD ret = 0;
    bool ok = DeviceIoControl(h, IOCTL_CF_ALLOC_RWX,
                              &req, sizeof(req),
                              &rep, sizeof(rep), &ret, nullptr) != 0
              && ret == sizeof(rep);
    CloseHandle(h);
    if(!ok || rep.address == 0) return false;
    outAddr = rep.address;
    outSize = rep.size;
    return true;
}

bool DriverPatchQword(uint64_t addr, uint64_t value){
    struct { uint64_t addr; uint64_t value; } req{addr, value};
    HANDLE h = CreateFileW(L"\\\\.\\CfRead", GENERIC_WRITE,
                           FILE_SHARE_READ|FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, 0, nullptr);
    if(h == INVALID_HANDLE_VALUE) return false;
    DWORD ret = 0;
    bool ok = DeviceIoControl(h, IOCTL_CF_PATCH_QWORD,
                              &req, sizeof(req),
                              nullptr, 0, &ret, nullptr) != 0;
    CloseHandle(h);
    return ok;
}

// Resolve the Fortnite-side IDXGIAdapter::GetDesc vtable slot.
// Returns slot addresses for GetDesc / GetDesc1 / GetDesc2 / GetDesc3.
// Heuristic: find dxgi.dll in the target, scan the .rdata for a
// well-known vtable prologue, return the slot addresses to overwrite.
//
// Status on the current build: this is the hardest part of the
// install. The function below returns slot addresses by scanning for
// the IDXGIAdapter vtable which starts with QueryInterface/AddRef/Release
// pointers all into dxgi.dll's .text. We emit those addresses but the
// scan is build-sensitive — expect to retune on Fortnite patches.
struct VTables { uint64_t getDesc[4]; };

bool ResolveTargetVTableSlots(VTables& out){
    // Resolve dxgi.dll base in the target. Driver uses CF_READ to
    // walk ldr modules; simpler here: read PEB.Ldr.InLoadOrderModuleList.
    // Left as a TODO stub — needs the driver's ModuleEnum IOCTL that
    // kread.c doesn't yet expose. Returns false for now so the install
    // reports clean failure instead of writing into unmapped memory.
    (void)out;
    CF_WARN("dxgi_kernel: vtable resolver stubbed — needs driver ModuleEnum IOCTL");
    return false;
}

} // namespace

bool Armed(){ return g_armed.load(); }

bool InstallInTarget(const Fingerprint& fp){
    if(!cf::kmem::Online()){
        CF_WARN("dxgi_kernel: driver offline");
        return false;
    }
    std::scoped_lock _(g_lock);
    if(g_armed.load()) return true;

    // 1. Allocate a page in the target.
    uint64_t pageAddr = 0; uint32_t pageSize = 0;
    if(!DriverAlloc(pageAddr, pageSize)){
        CF_WARN("dxgi_kernel: alloc failed");
        return false;
    }
    CF_INFO("dxgi_kernel: page at target 0x%llx (%u bytes)",
            (unsigned long long)pageAddr, pageSize);

    // 2. Build the page contents.
    //   +0x00: shellcode (16 bytes padding + body)
    //   +0x08: real GetDesc fn pointer (filled per vtable slot below)
    //   +0x10: shellcode body
    //   +0x100: fake DXGI_ADAPTER_DESC (128 bytes)
    uint8_t page[0x1000] = {0};
    memcpy(page + 0x10, kShellcode, sizeof(kShellcode));

    // Fake DXGI_ADAPTER_DESC layout at +0x100 (128 bytes):
    //   [0x00..0x100] WCHAR Description[128]
    //   [0x100]        UINT VendorId
    //   [0x104]        UINT DeviceId
    //   [0x108]        UINT SubSysId
    //   [0x10C]        UINT Revision
    // But DXGI_ADAPTER_DESC is only 128 bytes total wrong — real layout:
    //   WCHAR Description[128]   (256 bytes, UTF-16)
    //   UINT VendorId
    //   UINT DeviceId
    //   UINT SubSysId
    //   UINT Revision
    //   SIZE_T DedicatedVideoMemory
    //   SIZE_T DedicatedSystemMemory
    //   SIZE_T SharedSystemMemory
    //   LUID AdapterLuid
    // Our shellcode overwrites only the first 128 bytes of the output.
    // That covers Description[64] + VendorId + DeviceId + SubSysId +
    // Revision + the first field of memory counts — enough for EAC's
    // identity hash.
    uint8_t* fake = page + 0x100;
    memcpy(fake, fp.desc, 128);                            // Description truncated
    *(uint32_t*)(fake + 0x100 - 128) = fp.vendorId;
    // Simpler: lay out matching real struct order up to 128 bytes.
    // DXGI_ADAPTER_DESC starts with Description[128]-wchar (256 bytes
    // total), so 128 bytes only covers Description[64] — vendorId lives
    // past the copy boundary. We expand the shellcode copy below to
    // 288 bytes to cover the full desc.

    // Rewrite shellcode `mov ecx, 16` → `mov ecx, 36` (288 / 8 = 36).
    // Instruction at offset 0x10 + 0x24 in kShellcode (the mov ecx,imm).
    // Patch the imm from 0x10 to 0x24.
    // Find and patch inline:
    for(size_t i=0; i+5 < sizeof(kShellcode); ++i){
        if(page[0x10 + i] == 0xB9 && page[0x10 + i + 1] == 0x10){
            page[0x10 + i + 1] = 36;   // copy 288 bytes
            break;
        }
    }

    // Lay the full 288-byte fake desc:
    //   [0..256]   WCHAR Description[128]
    //   [256..260] VendorId
    //   [260..264] DeviceId
    //   [264..268] SubSysId
    //   [268..272] Revision
    memset(fake, 0, 288);
    size_t dlen = 0;
    while(dlen < 127 && fp.desc[dlen]) ++dlen;
    memcpy(fake, fp.desc, (dlen + 1) * sizeof(wchar_t));
    *(uint32_t*)(fake + 256) = fp.vendorId;
    *(uint32_t*)(fake + 260) = fp.deviceId;
    *(uint32_t*)(fake + 264) = fp.subsysId;
    *(uint32_t*)(fake + 268) = fp.revision;

    // 3. Resolve target vtable slots.
    VTables vt{};
    if(!ResolveTargetVTableSlots(vt)){
        CF_WARN("dxgi_kernel: no vtable — cloak NOT installed");
        return false;
    }

    // 4. Write real-GetDesc pointer into page +0x08 (one per hook —
    //    for simplicity we only hook GetDesc[0]; GetDesc1/2/3 share
    //    the same page by giving each its own sub-page in a real build).
    // Patch the real fn pointer slot.
    *(uint64_t*)(page + 0x08) = vt.getDesc[0];

    // Copy the full 4KB page into the target.
    cf::mem::Write(pageAddr, page, sizeof(page));

    // 5. Patch vtable entry to point to our shellcode body (page+0x10).
    uint64_t hookAddr = pageAddr + 0x10;
    if(!DriverPatchQword(vt.getDesc[0], hookAddr)){
        CF_WARN("dxgi_kernel: vtable patch rejected");
        return false;
    }

    g_inst.shellcodeAddr = pageAddr;
    g_inst.shellcodeSize = pageSize;
    g_inst.saved.push_back({vt.getDesc[0], vt.getDesc[0]});    // real stays, we wrote hook

    g_armed.store(true);
    CF_INFO("dxgi_kernel: cloak armed in target");
    return true;
}

void RemoveFromTarget(){
    std::scoped_lock _(g_lock);
    if(!g_armed.load()) return;
    for(auto& s : g_inst.saved){
        DriverPatchQword(s.slotAddr, s.origFn);
    }
    // Driver-side free of the RWX page omitted — on process exit the
    // target reclaims it. Non-leaking across one session.
    g_inst = {};
    g_armed.store(false);
}

} // namespace cf::ab::dxgi_k
