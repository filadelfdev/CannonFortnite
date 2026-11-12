// spoofer.exe — single entry point for CannonSpoofer.
//
// Running with no args → interactive menu.
// Running with `apply|restore|burn|status` → direct action (scripts).

#include "fingerprint.h"
#include "rotator.h"
#include "marker.h"
#include "util.h"

#include <windows.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

int Apply(){
    std::vector<cf::spoof::rot::Slot> ledger;
    if(!cf::spoof::rot::LoadLedger(ledger)){
        cf::spoofer::util::Info("seeding new ledger (32 slots)");
        cf::spoof::rot::Seed(ledger, 32);
        if(!cf::spoof::rot::SaveLedger(ledger)){
            cf::spoofer::util::Err("could not save new ledger — check write perms");
            return 2;
        }
    }
    cf::spoof::Fingerprint fp{};
    if(!cf::spoof::rot::PickNext(ledger, fp)){
        cf::spoofer::util::Err("no clean slot left — ledger exhausted");
        return 2;
    }

    // IMPORTANT: stash the REAL HWID BEFORE any Apply writes happen,
    // and only if no marker already exists (i.e. the stash would overwrite
    // an already-spoofed state with the spoofed values → would lose the
    // true originals forever).
    if(!cf::spoofer::marker::Exists()){
        if(!cf::spoof::StashReal()){
            cf::spoofer::util::Err("couldn't stash real HWID — refusing to apply");
            return 3;
        }
    } else {
        cf::spoofer::util::Warn("marker already exists — skipping StashReal to protect original backup");
    }

    bool um = cf::spoof::Apply(fp);
    bool kr = cf::spoof::ApplyKernel(fp);
    if(!cf::spoof::rot::SaveLedger(ledger)){
        cf::spoofer::util::Warn("ledger save failed — slot progression not persisted");
    }

    // ONLY write the marker when at least the user-mode leg succeeded —
    // otherwise other tools (the cheat) will believe a spoof is live
    // when nothing changed on disk.
    if(um){
        uint64_t fpHash = cf::spoofer::util::fnv1a(fp.machineGuid.c_str());
        cf::spoofer::marker::Write(cf::spoof::rot::CurrentSlotIndex(), fpHash);
    } else {
        cf::spoofer::util::Err("user-mode Apply FAILED — marker NOT written");
    }

    cf::spoofer::util::Info("slot %d applied (user-mode=%s, kernel=%s)",
                            cf::spoof::rot::CurrentSlotIndex(),
                            um ? "ok" : "FAIL",
                            kr ? "ok" : "skipped");
    cf::spoofer::util::Info("clean slots remaining: %d",
                            cf::spoof::rot::RemainingClean(ledger));
    cf::spoofer::util::Info("REBOOT NOW so SMBIOS/disk/MAC spoof persists.");
    return um ? 0 : 1;
}

int Restore(){
    if(!cf::spoof::RestoreReal()){
        cf::spoofer::util::Err("restore failed — stash missing or corrupt");
        return 1;
    }
    cf::spoofer::marker::Delete();
    cf::spoofer::util::Info("real HWID restored (user-mode layer)");
    cf::spoofer::util::Info("REBOOT NOW so kernel-level values flip back.");
    return 0;
}

int Burn(){
    cf::spoofer::marker::Blob m{};
    if(!cf::spoofer::marker::Read(m)){
        cf::spoofer::util::Err("no marker — no active slot to burn. Run apply first.");
        return 1;
    }
    std::vector<cf::spoof::rot::Slot> ledger;
    if(!cf::spoof::rot::LoadLedger(ledger)){
        cf::spoofer::util::Err("ledger missing/corrupt");
        return 2;
    }
    if(m.slot < 0 || m.slot >= (int)ledger.size()){
        cf::spoofer::util::Err("marker slot %d out of range (ledger size %zu)",
                               m.slot, ledger.size());
        return 3;
    }
    ledger[m.slot].burned = true;
    if(!cf::spoof::rot::SaveLedger(ledger)){
        cf::spoofer::util::Warn("ledger save failed — burned state not persisted");
    }

    // Restore the real HWID first so burn+apply doesn't layer a second
    // spoof on top of the previous one (otherwise the StashReal inside
    // Apply refuses to run, and Restore later writes back burned values
    // thinking they are the originals).
    cf::spoof::RestoreReal();
    cf::spoofer::marker::Delete();

    cf::spoofer::util::Info("slot %d marked BURNED + real HWID restored. clean remaining: %d",
                            m.slot, cf::spoof::rot::RemainingClean(ledger));
    cf::spoofer::util::Info("REBOOT, then run `spoofer apply` for the next slot.");
    return 0;
}

int Status(){
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
        printf("\n  ledger    : NOT INITIALISED  (run apply once)\n");
    }
    return 0;
}

int Menu(){
    SetConsoleTitleW(L"CannonSpoofer");
    for(;;){
        system("cls");
        printf("\n");
        printf("  ============================================\n");
        printf("    CannonSpoofer — standalone HWID spoofer\n");
        printf("    built by olivia  --  discord.gg/ojailbreaks\n");
        printf("  ============================================\n\n");

        // Quick status header.
        cf::spoofer::marker::Blob m{};
        if(cf::spoofer::marker::Read(m)){
            printf("  spoof ACTIVE — slot %d, %.1fh ago\n\n",
                   m.slot, cf::spoofer::marker::AgeHours());
        } else {
            printf("  spoof NOT ACTIVE — HWID is your real one\n\n");
        }

        printf("    [1] apply     - pick next slot + spoof\n");
        printf("    [2] restore   - put real HWID back\n");
        printf("    [3] burn      - mark current slot bad (took a ban)\n");
        printf("    [4] status    - detailed ledger view\n");
        printf("    [5] exit\n\n");
        printf("  choice: ");

        char buf[8]{};
        if(!fgets(buf, sizeof(buf), stdin)) return 0;
        int rc = 0;
        switch(buf[0]){
            case '1': printf("\n"); rc = Apply();   break;
            case '2': printf("\n"); rc = Restore(); break;
            case '3': printf("\n"); rc = Burn();    break;
            case '4': printf("\n"); rc = Status();  break;
            case '5': case 'q': case 'Q': return 0;
            default: continue;
        }
        (void)rc;
        printf("\npress any key to continue..."); getchar();
    }
}

} // namespace

int main(int argc, char** argv){
    if(argc < 2) return Menu();

    std::string cmd = argv[1];
    if(cmd == "apply")   return Apply();
    if(cmd == "restore") return Restore();
    if(cmd == "burn")    return Burn();
    if(cmd == "status")  return Status();

    fprintf(stderr, "usage: %s [apply|restore|burn|status]\n", argv[0]);
    fprintf(stderr, "       (no arg → interactive menu)\n");
    return 1;
}
