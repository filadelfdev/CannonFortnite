#pragma once

namespace cf::ab::dxgi {

// EAC enumerates DXGI adapters and hashes vendor/device/subsys/revision
// + the LUID + AdapterLuid into the HWID bundle. The HWID rotator can
// not touch these from the registry — they come live from the driver.
//
// This hook replaces the returned DXGI_ADAPTER_DESC{1,2,3} with values
// drawn from the active HWID slot's spoofer fingerprint, so every
// session shows a different GPU identity.

void Install();
void Remove();
bool Armed();

// Called by the rotator when a new slot is applied; feeds this layer
// the GPU identity to report. Deterministic per slot.
void SetReportedAdapter(const char* vendor, const char* device,
                        unsigned vendorId, unsigned deviceId,
                        unsigned subsysId, unsigned revision);

} // namespace cf::ab::dxgi
