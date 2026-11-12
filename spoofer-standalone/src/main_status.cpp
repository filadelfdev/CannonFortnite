// spoof-status.exe — reports whether a fingerprint is active, which
// slot, how old, and how many clean slots remain in the ledger.

#include "rotator.h"
#include "marker.h"
#include "util.h"

#include <cstdio>
#include <vector>
#include <windows.h>

int main(){
    SetConsoleTitleW(L"CannonSpoofer :: status");
    printf("\n== CannonSpoofer status ==\n\n");

    cf::spoofer::marker::Blob m{};
    bool hasMarker = cf::spoofer::marker::Read(m);
    double age = cf::spoofer::marker::AgeHours();

    if(!hasMarker){
        printf("  marker    : NOT PRESENT  (no active spoof)\n");
    } else {
        printf("  marker    : present\n");
        printf("  slot      : %d\n", m.slot);
        printf("  applied   : %.1f hours ago\n", age);
        printf("  fp hash   : 0x%016llx\n", (unsigned long long)m.fpHash);
    }

    std::vector<cf::spoof::rot::Slot> ledger;
    if(cf::spoof::rot::LoadLedger(ledger)){
        int clean = cf::spoof::rot::RemainingClean(ledger);
        int burned = (int)ledger.size() - clean;
        printf("\n  ledger    : %zu slots total\n", ledger.size());
        printf("  clean     : %d\n", clean);
        printf("  burned    : %d\n", burned);
    } else {
        printf("\n  ledger    : NOT INITIALISED  (run spoof-apply once)\n");
    }

    printf("\npress any key to exit..."); getchar();
    return 0;
}
