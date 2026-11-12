#pragma once
#include <cstdint>
#include <cstddef>
#include <string>

// Process-space memory access. Internal model: the DLL runs INSIDE the
// Fortnite process, so every pointer we read is just a dereference.
// No syscalls, no handle duplication, no driver — the pointer IS the
// address.
//
// This header exists so feature code stays abstract: features call
// cf::proc::R<T>(addr) and never care which model (external/internal)
// is backing them. Swap implementations by swapping process.cpp.

namespace cf::proc {

// Base of the main module (FortniteClient-Win64-Shipping.exe).
uintptr_t Base();

// Base + size of a loaded module by name (e.g., L"dxgi.dll", L"d3d11.dll").
uintptr_t ModuleBase(const wchar_t* dllName);
size_t    ModuleSize(const wchar_t* dllName);

// Raw typed read/write — same pointer deref, no syscall.
template<typename T> inline T R(uintptr_t a){
    return *reinterpret_cast<T*>(a);
}
template<typename T> inline bool W(uintptr_t a, const T& v){
    *reinterpret_cast<T*>(a) = v;
    return true;
}

// Byte-level copy (for structs larger than what R<T> handles cleanly).
bool Read (uintptr_t addr, void* out, size_t n);
bool Write(uintptr_t addr, const void* in,  size_t n);

// Pattern scan the main module (or a named one). AOB string format:
//   "48 8B 05 ?? ?? ?? ?? 48 8B 88"
uintptr_t Pattern(const char* ida_aob);
uintptr_t PatternIn(const wchar_t* dllName, const char* ida_aob);

// Resolve a RIP-relative displacement at `instr + offset` (default 3
// for the common `MOV r64, [RIP+disp32]` prolog). Returns the target
// address or 0 if the window is out of range.
uintptr_t RipRel(uintptr_t instr, int opcodeLen = 3, int instrLen = 7);

} // namespace cf::proc
