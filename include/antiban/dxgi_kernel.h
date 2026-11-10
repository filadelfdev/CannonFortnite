#pragma once
#include <cstdint>

// DXGI cloak via kernel driver — patches the Fortnite process's own
// IDXGIFactory/IDXGIAdapter vtables so that when its EAC-side GPU
// enumeration runs, it reads the fingerprint we set, not the real GPU.
//
// Mechanism:
//   1. Driver allocates a page of RWX memory inside the target process.
//   2. User-mode copies a shellcode blob + a 256-byte "fake desc" into
//      that page, with an embedded indirect call to the real GetDesc.
//   3. Driver walks the dxgi.dll image in the target for the exported
//      vtables and overwrites the GetDesc slots to point to our shellcode.
//
// The shellcode runs inside Fortnite's address space — it calls the
// original GetDesc, then rewrites the returned DXGI_ADAPTER_DESC fields
// (Description, VendorId, DeviceId, SubSysId, Revision, AdapterLuid)
// before returning.
//
// NOTE: resolution of the target's dxgi.dll vtable slots is
// heuristic — we rely on the MS-exported ABI being stable (adapter
// vtable slots 8/10/11/18 for GetDesc / GetDesc1 / GetDesc2 / GetDesc3).

namespace cf::ab::dxgi_k {

struct Fingerprint {
    wchar_t  desc[128];
    uint32_t vendorId;
    uint32_t deviceId;
    uint32_t subsysId;
    uint32_t revision;
};

// Install the cloak into the already-attached Fortnite process.
// Returns false and leaves no state if the driver isn't online or the
// vtable can't be resolved.
bool InstallInTarget(const Fingerprint& fp);

// Remove the hook (restore original GetDesc pointers). Called on panic
// exit if the cloak was armed.
void RemoveFromTarget();

bool Armed();

} // namespace cf::ab::dxgi_k
