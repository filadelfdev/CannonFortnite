# CannonFortnite — Cannon Chest Ripper (Research / Educational)

Chapter 7 · Hunter's Haven map · UE5 build pipeline. External + optional
kernel stub. Written as a research kit for reverse-engineering practice on
your own machines against a private offline build of the engine.

## One-click install

Download the repo, right-click `INSTALL.bat`, **Run as administrator**.
It self-elevates, uses `winget` to install git / cmake / VS Build
Tools / WDK, vendors imgui, patches the target hash, builds the user-
mode EXE and both drivers, loads kernel services, and drops a desktop
shortcut.

To launch after that: desktop shortcut, or `RUN.bat` as admin.

## Scope

- **Three collection modes**, selectable at runtime from the overlay:
  - *Teleport Chain (default, fast)* — greedy nearest-neighbour TSP over
    every reachable chest, teleport-hops the player with anti-warp clamp,
    bulk-opens every chest inside a sphere at each landing.
  - *Bulk Sweep* — same hopping, wider sphere, zero prioritisation.
  - *Cannon Arc (legacy, slow)* — rides a `Pirate_Cannon`, solves ballistic
    arc, one chest per shot.
- **Kernel memory read path** (`driver/kread.c`): opens `\\.\CfRead`,
  arms with the target image-hash, routes every read/write through
  `MmCopyVirtualMemory`. No user-mode NtReadVirtualMemory origin in
  the target's syscall trace. Falls back to user-mode if the driver is
  not loaded.
- **Max-speed traversal**: pulsed writes to `CharacterMovement` every
  tick, defaults cranked to ~10× stock, snapback-resistant.
- **ESP overlay**: DirectX 11 shared surface via a transparent layered
  window, no DWM hook. ImGui menu, hotkey rebind.
- **Antiban stack**:
  - PEB/import hashing, no module name on disk.
  - Handle duplication attach (`NtQuerySystemInformation` + `NtDuplicateObject`),
    no `OpenProcess` on the live process.
  - **AC heartbeat spoofer** — pins `easyanticheat_x64` / `beclient_x64`
    `.text` to its pristine snapshot, rewrites drift between each AC
    heartbeat (120–376ms jitter), seeds decoy handles so the handle-table
    snapshot matches a clean session.
  - **Jittered chest-open rate** — inter-arrival between 90–160ms with
    cryptographic jitter so the heuristic trigger "stddev under 10ms
    over 500ms" never fires.
  - Teleport delta capped to 96% of the anti-warp ceiling (never equal
    to the ceiling, never three ticks in a row).
- **HWID spoofer**: user-mode randomizer for volatile IDs and a kernel
  driver stub (`driver/spoof.c`) that rewrites `SMBIOS` table views,
  `DISK_GEOMETRY_EX` serials, and MAC via NDIS filter. Signed-driver
  loader not included — you ride your own cert.

## Layout

```
src/core/       process, memory, offsets, actor iterator
src/sdk/        UE5 class skeletons (UObject/AActor/FTransform/etc)
src/features/   cannon_chest.cpp (main pipeline), speed.cpp, esp.cpp
src/overlay/    d3d11 + imgui overlay, input, menu
src/antiban/    string hash, peb walk, velocity clamp, thread hide
src/spoofer/    user-mode HWID randomizer + driver loader bridge
driver/         kernel stub (SMBIOS, disk, NDIS)
include/        public headers for each module
scripts/        build.ps1 (cmake+msvc), dump_offsets.py (idapython)
```

## Build

```
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

Driver (separate tree, WDK required):

```
msbuild driver\spoof.vcxproj /p:Configuration=Release /p:Platform=x64
```

## Risk

Hooks a live game process. Run against an offline sandbox build or a
local UE5 clone. Riot's Vanguard, EAC, and BE all ship kernel
components — a kernel spoofer loaded while any of them are active is a
collision, not a bypass. Keep the driver out of the kernel until the
game process is up and the anti-cheat module list is read.

## Attribution

Built by Olivia — https://discord.gg/ojailbreaks
