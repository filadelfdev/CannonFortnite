#pragma once
#include <string>
#include <vector>
#include <cstdint>

namespace cf::spoof {

// User-mode spoof — for volatile and registry-backed IDs the game
// reaches from user space: MachineGuid, HwProfileGuid, drive serial
// cached in WMI, MAC on the active NIC, Windows build fingerprint.
// Volatile means: reverted on reboot unless the driver loader keeps it
// pinned. Kernel-grade IDs (SMBIOS UUID, raw disk serial from ATA
// identify, MAC burned on NIC ROM) need driver/spoof.c loaded.

struct Fingerprint {
    std::string machineGuid;          // HKLM\SOFTWARE\Microsoft\Cryptography\MachineGuid
    std::string hwProfileGuid;        // HKLM\System\CurrentControlSet\Control\IDConfigDB\Hardware Profiles\0001
    std::string productId;            // HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion
    std::string installDate;
    std::string diskSerial;
    std::string volumeSerial;         // per volume, random per boot
    std::vector<std::string> macs;    // per adapter
    std::string smbiosUuid;           // read-only here; driver writes
};

Fingerprint Read();
bool        Randomize(Fingerprint& out);   // fills `out` with fresh values
bool        Apply(const Fingerprint& fp);   // writes what user mode can

// Bridge to the kernel driver: opens \\.\CfSpoof, sends the SMBIOS + disk
// payload. If the driver isn't loaded, returns false and leaves the
// user-mode spoof in place.
bool ApplyKernel(const Fingerprint& fp);

// Persist / restore the current real fingerprint to %TEMP% under a
// scrambled name so a panic restore returns the machine to clean state.
bool StashReal();
bool RestoreReal();

} // namespace cf::spoof
