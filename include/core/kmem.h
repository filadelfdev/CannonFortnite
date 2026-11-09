#pragma once
#include <cstdint>
#include <cstddef>

// Kernel read/write path. Replaces the user-mode NtReadVirtualMemory
// call with an IOCTL into driver/kread.c, which uses MmCopyVirtualMemory
// from a worker SystemThread. Every user-mode VM syscall has a thread-
// origin fingerprint EAC profiles — kernel path strips all of it.

namespace cf::kmem {

bool Open();                  // open \\.\CfRead; false means user-mode fallback stays
void Close();
bool Online();                // driver live + armed

bool Read (uintptr_t addr, void* out, size_t n);
bool Write(uintptr_t addr, const void* in,  size_t n);

// Batched read — one syscall per batch. The hot actor loop lives on this.
struct Req { uintptr_t src; void* dst; size_t size; };
bool Batch(const Req* r, size_t count);

// Arm the driver with the target's EPROCESS, resolved by image-hash
// server-side. The driver walks PsActiveProcessHead so no user-mode
// process enum is needed.
bool Arm(uint64_t imageHashFnv1a);

} // namespace cf::kmem
