#pragma once
#include "spoofer/spoofer.h"
#include <string>
#include <vector>

namespace cf::spoof::rot {

// HWID rotation ledger — persists N random fingerprints in an
// AES-GCM encrypted blob under %LOCALAPPDATA%\Microsoft\Windows\INetCache.
// On session start the loader picks the oldest unused slot, marks it
// "in-use", applies it user-mode + driver. On ban the slot gets
// burned and never selected again.

struct Slot {
    uint32_t    index{};
    Fingerprint fp{};
    uint64_t    lastUsedUnix{};
    bool        burned{};
};

bool LoadLedger(std::vector<Slot>& out);
bool SaveLedger(const std::vector<Slot>& ledger);
void Seed    (std::vector<Slot>& ledger, int count = 32);
bool PickNext(std::vector<Slot>& ledger, Fingerprint& out);
bool BurnCurrent(std::vector<Slot>& ledger);
int  RemainingClean(const std::vector<Slot>& ledger);

// Session-level accessors for the overlay HUD + "burn" button. These
// mirror the ledger the main loop loaded at startup; writes here mark
// the current slot and persist on next PickNext/BurnCurrent call.
int  CurrentSlotIndex();        // -1 if no slot active
int  TotalSlots();
int  CleanRemaining();
void MarkCurrentBurnedPersist();

} // namespace cf::spoof::rot
