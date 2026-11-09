#pragma once
#include <cstdint>

namespace cf::ab {

// EAC/BE periodically ship an integrity packet: a hash of each loaded
// module's .text + a snapshot of the thread list + a dump of the handle
// table. The hash is computed from a small enumerator the AC module
// invokes on its own timer. Three defences live here:
//
//   1. PinModuleHash: freeze the AC module's view of our own .text by
//      installing a page-guard-based read handler that returns the
//      pristine bytes the AC measured on load, even after we jmp-patch
//      hot paths. Target the AC module (easyanticheat_x64 / beclient_x64)
//      and leave the host (fortnite) alone.
//   2. MaskThreadTrace: before each AC heartbeat, flip the Teb's thread
//      locale and clear the ThreadImpersonationToken; AC uses both as
//      stable identifiers across heartbeats.
//   3. DecoyHandleTable: duplicate three random handles with recognisable
//      types (Event, Mutex, File) so the handle snapshot carries the
//      same count+shape as a clean session.
//
// Call Install() once after Attach(). The heartbeat worker runs on its
// own low-priority thread; it must be hidden via HideSelfThreads().

void InstallHeartbeat();
void StopHeartbeat();

// Returns true when the AC module is confirmed loaded and the pin is
// armed. Used by the overlay to display a green state dot.
bool HeartbeatArmed();

} // namespace cf::ab
