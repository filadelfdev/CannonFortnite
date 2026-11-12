// spoof-restore.exe — writes the real HWID back from the stash file,
// deletes the marker. Reboot to make kernel-level restore effective.

#include "fingerprint.h"
#include "marker.h"
#include "util.h"

#include <cstdio>
#include <windows.h>

int main(){
    SetConsoleTitleW(L"CannonSpoofer :: restore");
    cf::spoofer::util::Info("== restore ==");

    if(!cf::spoof::RestoreReal()){
        cf::spoofer::util::Err("restore failed — stash missing or corrupt");
        printf("\npress any key to exit..."); getchar();
        return 1;
    }
    cf::spoofer::marker::Delete();

    cf::spoofer::util::Info("real HWID restored (user-mode layer)");
    cf::spoofer::util::Info("REBOOT NOW so kernel-level values flip back.");

    printf("\npress any key to exit..."); getchar();
    return 0;
}
