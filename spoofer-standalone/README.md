# CannonSpoofer — standalone HWID spoofer

Decoupled from the cheat on purpose. Runs on its own; if EAC ever
flags the cheat binary, the spoofer survives — fresh account on the
already-spoofed HWID keeps working.

## Binaries

| EXE | Purpose |
|-----|---------|
| `spoof-apply.exe`   | Pick next clean rotator slot, apply user-mode + kernel fingerprint, write marker. **Reboot after.** |
| `spoof-restore.exe` | Write real HWID back from stash, delete marker. Reboot after. |
| `spoof-burn.exe`    | Mark the currently active slot as burned (call right after a ban on that fingerprint). |
| `spoof-status.exe`  | Show whether a marker is active, slot, age, clean/burned counts. |

## Flow for a fresh account

```
1. spoof-apply.exe   (as admin)        → fresh slot picked, applied
2. REBOOT
3. install Fortnite  (if not already)  → Epic sees spoofed HWID
4. create a NEW Epic account           → Epic binds account to spoofed HWID
5. (optional) launch the cheat         → marker check confirms spoof is live
6. play / farm
7. END key on the cheat (or just close it)
8. keep the HWID spoofed for the account's life
```

## Flow after a ban

```
1. spoof-burn.exe    (as admin)        → marks current slot as unusable
2. spoof-apply.exe                     → picks the next clean slot
3. REBOOT
4. create another NEW Epic account
5. resume
```

## Flow to go back to clean state (sell the PC / debug)

```
1. spoof-restore.exe (as admin)
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

Output: `build/bin/Release/spoof-{apply,restore,burn,status}.exe`

Driver (separate tree, same spec as before):

```
msbuild driver/spoof.vcxproj /p:Configuration=Release /p:Platform=x64
sc create ShellHWAgent type= kernel start= demand binPath= path\to\spoof.sys
sc start  ShellHWAgent
```

Built by Olivia — https://discord.gg/ojailbreaks
