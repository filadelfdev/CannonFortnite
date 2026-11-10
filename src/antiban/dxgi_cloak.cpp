#include "antiban/dxgi_cloak.h"
#include "util/log.h"

#include <windows.h>
#include <dxgi1_6.h>
#include <atomic>
#include <mutex>
#include <cstring>
#include <cwchar>

#pragma comment(lib, "dxgi")

namespace cf::ab::dxgi {

namespace {

struct Reported {
    wchar_t  desc[128] = L"NVIDIA GeForce RTX 3060";
    unsigned vendorId  = 0x10DE;
    unsigned deviceId  = 0x2504;
    unsigned subsysId  = 0x38461462;
    unsigned revision  = 0xA1;
    LUID     luid      {};
} g_rep;
std::mutex g_lock;
std::atomic<bool> g_armed{false};

// IDXGIAdapter::GetDesc vtable index is 8 (IUnknown=0,1,2 + IDXGIObject=3,4,5,6
// + IDXGIAdapter=7:EnumOutputs,8:GetDesc,...).
using GetDesc_t  = HRESULT(STDMETHODCALLTYPE*)(IDXGIAdapter*,  DXGI_ADAPTER_DESC*);
using GetDesc1_t = HRESULT(STDMETHODCALLTYPE*)(IDXGIAdapter1*, DXGI_ADAPTER_DESC1*);
using GetDesc2_t = HRESULT(STDMETHODCALLTYPE*)(IDXGIAdapter2*, DXGI_ADAPTER_DESC2*);
using GetDesc3_t = HRESULT(STDMETHODCALLTYPE*)(IDXGIAdapter4*, DXGI_ADAPTER_DESC3*);

GetDesc_t  g_realGetDesc  = nullptr;
GetDesc1_t g_realGetDesc1 = nullptr;
GetDesc2_t g_realGetDesc2 = nullptr;
GetDesc3_t g_realGetDesc3 = nullptr;

void Apply(DXGI_ADAPTER_DESC* d){
    std::scoped_lock _(g_lock);
    wcsncpy_s(d->Description, g_rep.desc, _TRUNCATE);
    d->VendorId  = g_rep.vendorId;
    d->DeviceId  = g_rep.deviceId;
    d->SubSysId  = g_rep.subsysId;
    d->Revision  = g_rep.revision;
    d->AdapterLuid = g_rep.luid;
}

HRESULT STDMETHODCALLTYPE HookedGetDesc(IDXGIAdapter* a, DXGI_ADAPTER_DESC* d){
    HRESULT hr = g_realGetDesc(a, d);
    if(SUCCEEDED(hr)) Apply(d);
    return hr;
}
HRESULT STDMETHODCALLTYPE HookedGetDesc1(IDXGIAdapter1* a, DXGI_ADAPTER_DESC1* d){
    HRESULT hr = g_realGetDesc1(a, d);
    if(SUCCEEDED(hr)) Apply((DXGI_ADAPTER_DESC*)d);
    return hr;
}
HRESULT STDMETHODCALLTYPE HookedGetDesc2(IDXGIAdapter2* a, DXGI_ADAPTER_DESC2* d){
    HRESULT hr = g_realGetDesc2(a, d);
    if(SUCCEEDED(hr)) Apply((DXGI_ADAPTER_DESC*)d);
    return hr;
}
HRESULT STDMETHODCALLTYPE HookedGetDesc3(IDXGIAdapter4* a, DXGI_ADAPTER_DESC3* d){
    HRESULT hr = g_realGetDesc3(a, d);
    if(SUCCEEDED(hr)) Apply((DXGI_ADAPTER_DESC*)d);
    return hr;
}

template<typename T>
void PatchVtable(void* iface, int slot, void* hook, T& stash){
    void** vt = *(void***)iface;
    DWORD old;
    VirtualProtect(&vt[slot], sizeof(void*), PAGE_READWRITE, &old);
    stash = (T)vt[slot];
    vt[slot] = hook;
    VirtualProtect(&vt[slot], sizeof(void*), old, &old);
}

} // namespace

void SetReportedAdapter(const char* vendor, const char* device,
                        unsigned vendorId, unsigned deviceId,
                        unsigned subsysId, unsigned revision){
    std::scoped_lock _(g_lock);
    (void)vendor;    // kept for API compatibility; desc field uses device
    swprintf_s(g_rep.desc, L"%hs", device);
    g_rep.vendorId = vendorId;
    g_rep.deviceId = deviceId;
    g_rep.subsysId = subsysId;
    g_rep.revision = revision;
    // Fresh LUID per session.
    LUID l{};
    AllocateLocallyUniqueId(&l);
    g_rep.luid = l;
}

void Install(){
    if(g_armed.exchange(true)) return;

    // IMPORTANT caveat: this patches the vtable of OUR process's adapter
    // objects. The game process (Fortnite) creates its own factory and
    // adapter COM objects in its own address space; those vtables are
    // untouched by this patch — the hook only affects DXGI queries made
    // from OUR overlay process (which the AC doesn't poll anyway).
    //
    // Full coverage requires one of:
    //   - IAT hook on CreateDXGIFactory* inside the target module
    //     (reached via the kernel driver's invoke path)
    //   - Kernel-side DXGKRNL hook on the per-adapter descriptor shuttle
    //
    // Both are heavier than the scope of v5. This stub exists so later
    // attack-surface audits don't assume the overlay's GPU identity
    // leaks — ours doesn't after this. The game's identity still does.
    IDXGIFactory4* fac = nullptr;
    if(FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&fac)))){
        g_armed.store(false);
        return;
    }

    IDXGIAdapter* a = nullptr;
    if(SUCCEEDED(fac->EnumAdapters(0, &a)) && a){
        PatchVtable(a, 8, (void*)HookedGetDesc, g_realGetDesc);

        IDXGIAdapter1* a1=nullptr;
        if(SUCCEEDED(a->QueryInterface(&a1)) && a1){
            PatchVtable(a1, 10, (void*)HookedGetDesc1, g_realGetDesc1);
            a1->Release();
        }
        IDXGIAdapter2* a2=nullptr;
        if(SUCCEEDED(a->QueryInterface(&a2)) && a2){
            PatchVtable(a2, 11, (void*)HookedGetDesc2, g_realGetDesc2);
            a2->Release();
        }
        IDXGIAdapter4* a4=nullptr;
        if(SUCCEEDED(a->QueryInterface(&a4)) && a4){
            PatchVtable(a4, 18, (void*)HookedGetDesc3, g_realGetDesc3);
            a4->Release();
        }
        a->Release();
    }
    fac->Release();
    CF_INFO("dxgi cloak armed (overlay-process vtable; game-side unhooked)");
}

void Remove(){ /* process-death only */ }
bool Armed(){ return g_armed.load(); }

} // namespace cf::ab::dxgi
