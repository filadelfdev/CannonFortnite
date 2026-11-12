#pragma once
#include <cstdint>

// Marker file the spoofer writes after a successful apply + the
// CannonFortnite cheat reads to confirm a clean session is armed.
//
// Path: %LOCALAPPDATA%\Microsoft\Windows\INetCache\.cf_spoof.marker
// Content: 24 bytes
//   uint32 magic   = 0xCF50F000
//   uint32 version = 1
//   int32  slot    = active rotator slot index
//   int64  applied = unix ts of last apply
//   uint64 fpHash  = fnv1a(machineGuid) — quick sanity check

namespace cf::spoofer::marker {

struct Blob {
    uint32_t magic;
    uint32_t version;
    int32_t  slot;
    int32_t  pad;
    int64_t  appliedUnix;
    uint64_t fpHash;
};

bool Write(int32_t slot, uint64_t fpHash);
bool Read (Blob& out);
bool Delete();
bool Exists();

// Hours since last apply — negative if no marker.
double AgeHours();

} // namespace cf::spoofer::marker
