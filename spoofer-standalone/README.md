# CannonSpoofer — standalone HWID spoofer

Decoupled from the cheat on purpose. Runs on its own; if EAC ever
flags the cheat binary, the spoofer survives — fresh account on the
already-spoofed HWID keeps working.

## Single binary: `spoofer.exe`

Double-click → interactive menu:

```
  ============================================
    CannonSpoofer — standalone HWID spoofer
  ============================================

  spoof ACTIVE — slot 7, 3.2h ago

    [1] apply     - pick next slot + spoof
    [2] restore   - put real HWID back
    [3] burn      - mark current slot bad (took a ban)
    [4] status    - detailed ledger view
    [5] exit

  choice:
```

For scripts, same binary accepts a subcommand:

```
spoofer.exe apply
spoofer.exe restore
spoofer.exe burn
spoofer.exe status
```

## Flow for a fresh account

```
1. Double-click CannonSpoofer   (as admin) → pick [1] apply
2. REBOOT
3. install Fortnite  (if not already)      → Epic sees spoofed HWID
4. create a NEW Epic account               → bound to spoofed HWID
5. (optional) launch the cheat             → marker check confirms
6. play / farm
```

## Flow after a ban

```
1. CannonSpoofer  → [3] burn       (marks current slot unusable)
2. CannonSpoofer  → [1] apply      (picks next clean slot)
3. REBOOT
4. new Epic account
```

## Flow to go back to clean state

```
1. CannonSpoofer  → [2] restore
2. REBOOT
```

## Marker file

Path: `%LOCALAPPDATA%\Microsoft\Windows\INetCache\.cf_spoof.marker`
(24 bytes: magic + version + slot + unix ts + fp hash)

The CannonFortnite cheat reads this at startup. If missing, it warns
`"spoofer not detected — run spoof-apply first"` and refuses to apply
any HWID fallback — it is NOT the spoofer's job anymore.

## Build

```
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

Output: `build/bin/Release/spoofer.exe`

Driver (separate tree, same spec as before):

```
msbuild driver/spoof.vcxproj /p:Configuration=Release /p:Platform=x64
sc create ShellHWAgent type= kernel start= demand binPath= path\to\spoof.sys
sc start  ShellHWAgent
```

Built by Olivia — https://discord.gg/ojailbreaks
