#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace cf::mem {

// Attach via NtQuerySystemInformation + NtDuplicateObject — we never call
// OpenProcess on the live target. Detection surface for that call is the
// fattest in the EAC/BE heuristic set, so it stays dead here.
bool  Attach(const wchar_t* imageHash);   // hash, not name
void  Detach();

uintptr_t Base();          // main module base
uintptr_t ModuleBase(uint64_t nameHash);   // by image-name FNV1a
bool      IsAttached();
uint32_t  Pid();

bool Read (uintptr_t addr, void* out, size_t n);
bool Write(uintptr_t addr, const void* in,  size_t n);

template<typename T> T R(uintptr_t a){ T v{}; Read(a,&v,sizeof(T)); return v; }
template<typename T> bool W(uintptr_t a, const T& v){ return Write(a,&v,sizeof(T)); }

// Scatter read: coalesces adjacent regions so a hot actor loop makes
// a handful of syscalls per tick instead of one per field.
struct ScatterRange { uintptr_t src; void* dst; size_t size; };
bool Scatter(ScatterRange* r, size_t count);

// Pattern scan over a loaded module (image copy, not the live proc).
uintptr_t Pattern(uintptr_t modBase, size_t modSize,
                  const char* pattern, const char* mask);

} // namespace cf::mem
