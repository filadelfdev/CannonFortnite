# CannonFortnite — Cannon Chest Ripper (Research / Educational)

Chapter 7 · Hunter's Haven map · UE5 build pipeline. External + optional
kernel stub. Written as a research kit for reverse-engineering practice on
your own machines against a private offline build of the engine.

## Scope

- **External memory loop** against an Unreal Engine 5 process image.
- **Cannon → chest** routing: enumerates `BuildingContainer`, `AthenaSupplyDrop`,
  `AthenaLootChest` actors in the UWorld, finds the nearest `Pirate_Cannon`
  pawn/vehicle, computes a ballistic arc, punches the driver seat forward
  and primes fire at the right tick.
- **Max-speed traversal**: writes `CharacterMovement->MaxWalkSpeed`,
  `MaxFlySpeed`, `MaxAcceleration`, `AirControl`, `GravityScale` with
  snapback tick protection.
- **ESP overlay**: DirectX 11 shared surface via a transparent layered
  window, no DWM hook. ImGui menu, hotkey rebind.
- **Antiban string/behavior layer**: no module name on disk, PEB walks
  by hash, no `OpenProcess` on the live process (handle duplication
  through `NtQuerySystemInformation`), velocity clamping so written
  speed stays inside Riot/EAC heuristic bounds per tick.
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
