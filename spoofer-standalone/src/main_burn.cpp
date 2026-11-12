// spoof-burn.exe — marks the currently active slot as burned so
// PickNext never selects it again. Call this immediately after a ban
// on the current fingerprint.

#include "rotator.h"
#include "marker.h"
#include "util.h"

#include <cstdio>
#include <vector>
#include <windows.h>

int main(){
    SetConsoleTitleW(L"CannonSpoofer :: burn");
    cf::spoofer::util::Info("== burn current slot ==");

    cf::spoofer::marker::Blob m{};
    if(!cf::spoofer::marker::Read(m)){
        cf::spoofer::util::Err("no marker — no active slot to burn. Run spoof-apply first.");
        printf("\npress any key to exit..."); getchar();
        return 1;
    }

    std::vector<cf::spoof::rot::Slot> ledger;
    if(!cf::spoof::rot::LoadLedger(ledger)){
        cf::spoofer::util::Err("ledger missing/corrupt");
        printf("\npress any key to exit..."); getchar();
        return 2;
    }

    if(m.slot < 0 || m.slot >= (int)ledger.size()){
        cf::spoofer::util::Err("marker slot %d out of range (ledger size %zu)",
                               m.slot, ledger.size());
        printf("\npress any key to exit..."); getchar();
        return 3;
    }

    ledger[m.slot].burned = true;
    cf::spoof::rot::SaveLedger(ledger);
    cf::spoofer::marker::Delete();

    cf::spoofer::util::Info("slot %d marked BURNED. clean remaining: %d",
                            m.slot,
                            cf::spoof::rot::RemainingClean(ledger));
    cf::spoofer::util::Info("run spoof-apply for a fresh slot + reboot.");

    printf("\npress any key to exit..."); getchar();
    return 0;
}
