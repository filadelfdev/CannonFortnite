// spoof-apply.exe — picks next clean rotator slot, applies the whole
// fingerprint stack (user-mode registry + MAC + kernel driver SMBIOS/
// disk/MAC permanent), writes the marker file the cheat consumes.
//
// Reboot afterwards so the kernel-level spoofs effect on the next boot.

#include "fingerprint.h"
#include "rotator.h"
#include "marker.h"
#include "util.h"

#include <windows.h>
#include <vector>
#include <cstdio>

int main(){
    SetConsoleTitleW(L"CannonSpoofer :: apply");
    cf::spoofer::util::Info("== apply ==");

    std::vector<cf::spoof::rot::Slot> ledger;
    if(!cf::spoof::rot::LoadLedger(ledger)){
        cf::spoofer::util::Info("seeding new ledger (32 slots)");
        cf::spoof::rot::Seed(ledger, 32);
        cf::spoof::rot::SaveLedger(ledger);
    }

    cf::spoof::Fingerprint fp{};
    if(!cf::spoof::rot::PickNext(ledger, fp)){
        cf::spoofer::util::Err("no clean slot left — ledger exhausted");
        printf("\npress any key to exit..."); getchar();
        return 2;
    }

    // Stash real HWID before applying so spoof-restore has a target.
    if(!cf::spoof::StashReal()){
        cf::spoofer::util::Warn("couldn't stash real HWID — restore may be incomplete");
    }

    bool um = cf::spoof::Apply(fp);
    bool kr = cf::spoof::ApplyKernel(fp);

    cf::spoof::rot::SaveLedger(ledger);

    uint64_t fpHash = cf::spoofer::util::fnv1a(fp.machineGuid.c_str());
    cf::spoofer::marker::Write(cf::spoof::rot::CurrentSlotIndex(), fpHash);

    cf::spoofer::util::Info("slot %d applied (user-mode=%s, kernel=%s)",
                            cf::spoof::rot::CurrentSlotIndex(),
                            um ? "ok" : "FAIL",
                            kr ? "ok" : "skipped");
    cf::spoofer::util::Info("clean slots remaining: %d",
                            cf::spoof::rot::RemainingClean(ledger));
    cf::spoofer::util::Info("REBOOT NOW so SMBIOS/disk/MAC spoof persists.");

    printf("\npress any key to exit..."); getchar();
    return (um ? 0 : 1);
}
