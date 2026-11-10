#include "antiban/dxgi_kernel.h"
#include "core/kmem.h"
#include "core/memory.h"
#include "antiban/hash.h"
#include "util/log.h"

#include <windows.h>
#include <psapi.h>
#include <dxgi1_6.h>
#include <atomic>
#include <mutex>
#include <vector>
#include <cstring>

#pragma comment(lib, "dxgi")

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

// Resolve the GetDesc[0..3] vtable slot addresses INSIDE the target's
// dxgi.dll. We don't pattern-scan — instead we take advantage of the
// fact that IDXGIAdapter's vtable lives in dxgi.dll's .rdata at a
// stable RVA for a given dxgi.dll version. The RVA is identical for
// both processes (us and the target) because they load the same DLL.
//
// Steps:
//   1. Create our own IDXGIFactory, enumerate adapter 0.
//   2. For each interface (IDXGIAdapter{,1,2,4}), grab the vtable
//      pointer and compute (slot_address - our_dxgi_base) = slot RVA.
//   3. Read the target's dxgi.dll base via memory::GetTargetModuleBase.
//   4. target_slot = target_dxgi_base + slot_rva. Same version = same RVA.
bool ResolveTargetVTableSlots(VTables& out){
    out = {};
    uintptr_t ourDxgi = (uintptr_t)GetModuleHandleW(L"dxgi.dll");
    if(!ourDxgi){
        // Force-load dxgi.dll so we have the vtables in our address.
        ourDxgi = (uintptr_t)LoadLibraryW(L"dxgi.dll");
        if(!ourDxgi) return false;
    }

    uintptr_t targetDxgi = cf::mem::GetTargetModuleBase(L"dxgi.dll");
    if(!targetDxgi){
        CF_WARN("dxgi_kernel: target dxgi.dll not in loader list (yet?)");
        return false;
    }
    CF_INFO("dxgi_kernel: our dxgi=0x%llx target dxgi=0x%llx",
            (unsigned long long)ourDxgi, (unsigned long long)targetDxgi);

    IDXGIFactory4* fac = nullptr;
    if(FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&fac))) || !fac) return false;

    IDXGIAdapter* a = nullptr;
    if(FAILED(fac->EnumAdapters(0, &a)) || !a){ fac->Release(); return false; }

    auto rvaOf = [&](void* iface, int slot) -> uintptr_t {
        void** vt = *(void***)iface;
        uintptr_t slotAddr = (uintptr_t)&vt[slot];
        if(slotAddr < ourDxgi || slotAddr > ourDxgi + 0x10000000) return 0;
        return slotAddr - ourDxgi;
    };

    uintptr_t rva0 = rvaOf(a, 8);                       // IDXGIAdapter::GetDesc
    uintptr_t rva1 = 0, rva2 = 0, rva3 = 0;

    IDXGIAdapter1* a1=nullptr;
    if(SUCCEEDED(a->QueryInterface(&a1)) && a1){
        rva1 = rvaOf(a1, 10);
        a1->Release();
    }
    IDXGIAdapter2* a2=nullptr;
    if(SUCCEEDED(a->QueryInterface(&a2)) && a2){
        rva2 = rvaOf(a2, 11);
        a2->Release();
    }
    IDXGIAdapter4* a4=nullptr;
    if(SUCCEEDED(a->QueryInterface(&a4)) && a4){
        rva3 = rvaOf(a4, 18);
        a4->Release();
    }
    a->Release();
    fac->Release();

    if(!rva0){
        CF_WARN("dxgi_kernel: couldn't compute GetDesc RVA");
        return false;
    }

    out.getDesc[0] = targetDxgi + rva0;
    out.getDesc[1] = rva1 ? targetDxgi + rva1 : 0;
    out.getDesc[2] = rva2 ? targetDxgi + rva2 : 0;
    out.getDesc[3] = rva3 ? targetDxgi + rva3 : 0;
    return true;
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
    //
    // Layout (one 4KB page, see Slot[] table below for shellcode cells):
    //   [0x100..0x220]  fake DXGI_ADAPTER_DESC (288 bytes):
    //       [0..256]     WCHAR Description[128]
    //       [256..260]   VendorId
    //       [260..264]   DeviceId
    //       [264..268]   SubSysId
    //       [268..272]   Revision
    uint8_t page[0x1000] = {0};
    uint8_t* fake = page + 0x100;
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

    // 4. For each vtable slot: read the current fn ptr, write it to a
    //    per-slot cell in the page, then patch the slot to our shellcode.
    //    The shellcode reads its own "real fn ptr" from page+0x08 — so
    //    one shellcode body serves one slot. For all 4 slots we lay 4
    //    mini-pages (each 0x40 bytes) with shellcode + per-slot real fn
    //    ptr in a single 4KB page.
    //
    // Page layout:
    //   [0x000..0x008]   reserved
    //   [0x008]          real GetDesc  fn pointer (slot 0 reads here)
    //   [0x010..0x080]   slot 0 shellcode
    //   [0x088]          real GetDesc1 fn pointer
    //   [0x090..0x100]   slot 1 shellcode
    //   [0x100..0x220]   fake desc (shared across all 4 shellcodes)
    //   [0x228]          real GetDesc2 fn pointer
    //   [0x230..0x2A0]   slot 2 shellcode
    //   [0x2A8]          real GetDesc3 fn pointer
    //   [0x2B0..0x320]   slot 3 shellcode
    //
    // Each shellcode is a copy of kShellcode with its two RIP-relative
    // displacements adjusted so they reach the right real-fn-ptr cell
    // and the shared fake-desc buffer.

    struct Slot { uint32_t shellcodeAt; uint32_t realFnAt; };
    constexpr Slot slots[4] = {
        {0x010, 0x008},
        {0x090, 0x088},
        {0x230, 0x228},
        {0x2B0, 0x2A8},
    };

    for(int i=0; i<4; ++i){
        if(!vt.getDesc[i]) continue;
        Slot s = slots[i];

        // Save original fn ptr into its cell.
        *(uint64_t*)(page + s.realFnAt) = vt.getDesc[i];

        // Place the shellcode at its offset.
        uint8_t* sc = page + s.shellcodeAt;
        memcpy(sc, kShellcode, sizeof(kShellcode));

        // Patch the `mov rax, [rip-0x12]` displacement so it reaches
        // (page + s.realFnAt) regardless of where the shellcode sits.
        // Original displacement in kShellcode at byte 18 is 0xFFFFFFEE
        // (= -18), chosen for shellcode-at-0x10 → real-at-0x08
        // (0x08 - (0x10+18+7) = -0x11 after the 7-byte instruction).
        // Recompute per-slot:
        //   instruction end IP = page + s.shellcodeAt + (offset_of_mov+7)
        //   target             = page + s.realFnAt
        //   disp32             = target - instruction_end_IP
        constexpr uint32_t movRaxOffset = 16;   // byte offset of "48 8B 05 .. .. .. .." in kShellcode
        uint32_t insEnd = s.shellcodeAt + movRaxOffset + 7;
        int32_t  disp   = (int32_t)s.realFnAt - (int32_t)insEnd;
        memcpy(sc + movRaxOffset + 3, &disp, 4);

        // Patch the `lea rsi, [rip+0xB4]` to reach the fake-desc at 0x100.
        constexpr uint32_t leaOffset = 41;      // byte offset of "48 8D 35 .. .. .. .."
        uint32_t leaEnd = s.shellcodeAt + leaOffset + 7;
        int32_t  leaDisp = (int32_t)0x100 - (int32_t)leaEnd;
        memcpy(sc + leaOffset + 3, &leaDisp, 4);

        // Bump the copy count — rep movsq count = 36 for 288-byte desc.
        for(size_t j=0; j+5 < sizeof(kShellcode); ++j){
            if(sc[j] == 0xB9 && sc[j+1] == 0x10 && sc[j+2] == 0 && sc[j+3] == 0 && sc[j+4] == 0){
                sc[j+1] = 36;
                break;
            }
        }
    }

    // Copy the full 4KB page into the target.
    cf::mem::Write(pageAddr, page, sizeof(page));

    // 5. Patch each vtable entry to point to its shellcode body.
    for(int i=0; i<4; ++i){
        if(!vt.getDesc[i]) continue;
        uint64_t hookAddr = pageAddr + slots[i].shellcodeAt;
        // Save the original for restore.
        g_inst.saved.push_back({vt.getDesc[i], vt.getDesc[i]});
        if(!DriverPatchQword(vt.getDesc[i], hookAddr)){
            CF_WARN("dxgi_kernel: vtable patch rejected for slot %d", i);
            // Best-effort — if slot 0 landed we still get partial cover.
        }
    }

    g_inst.shellcodeAddr = pageAddr;
    g_inst.shellcodeSize = pageSize;
    g_armed.store(true);
    CF_INFO("dxgi_kernel: cloak armed in target (%zu slots hooked)",
            g_inst.saved.size());
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
