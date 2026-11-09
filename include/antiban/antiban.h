#pragma once
#include <cstdint>

namespace cf::ab {

// Hide our own thread list from the target's enumeration — Fortnite's
// own integrity sweep walks GetThreadInformation + a NtQueryInformationThread
// trace. We flip ThreadHideFromDebugger on every worker we spawn.
void HideSelfThreads();

// Null out the module's PE header from our own memory so a toolhelp scan
// against our image doesn't find a recognisable DOS stub.
void ZeroOwnHeader();

// Install a vectored exception handler that eats single-step/breakpoint
// exceptions so a timing side-channel from the integrity module reads a
// clean process.
void InstallVEH();

// Detect if EAC / BE / Vanguard is loaded; if any is, hold all memory
// writes until a human says "proceed" via the overlay. Checked on attach.
enum class AcKind { None, EAC, BE, Vanguard, Other };
AcKind DetectAnticheat();

// One-shot module integrity ping — makes sure our own code hasn't been
// replaced from under us by another injector. Returns false on tamper.
bool SelfIntegrityOk();

} // namespace cf::ab
