#include "core/process.h"

#include <windows.h>
#include <psapi.h>
#include <cstring>
#include <cstdio>

#pragma comment(lib, "psapi")

namespace cf::proc {

namespace {

// Parse a single IDA-style AOB token: "48" → {0x48, false} ; "??" → {0, true}.
bool ParseByte(const char* s, uint8_t& out, bool& wild){
    if(s[0] == '?' && (s[1] == '?' || s[1] == ' ' || s[1] == 0)){
        out = 0; wild = true; return true;
    }
    auto hexDigit = [](char c)->int{
        if(c>='0'&&c<='9') return c-'0';
        if(c>='a'&&c<='f') return c-'a'+10;
        if(c>='A'&&c<='F') return c-'A'+10;
        return -1;
    };
    int hi = hexDigit(s[0]);
    int lo = hexDigit(s[1]);
    if(hi<0 || lo<0) return false;
    out = (uint8_t)((hi<<4) | lo);
    wild = false;
    return true;
}

struct Needle {
    std::vector<uint8_t> bytes;
    std::vector<bool>    wild;
};

bool CompileNeedle(const char* aob, Needle& out){
    out.bytes.clear(); out.wild.clear();
    while(*aob){
        while(*aob == ' ') ++aob;
        if(!*aob) break;
        uint8_t b; bool w;
        if(!ParseByte(aob, b, w)) return false;
        out.bytes.push_back(b);
        out.wild.push_back(w);
        aob += 2;
    }
    return !out.bytes.empty();
}

uintptr_t FindIn(uintptr_t base, size_t size, const Needle& n){
    if(size < n.bytes.size()) return 0;
    const uint8_t* mem = reinterpret_cast<const uint8_t*>(base);
    size_t len = n.bytes.size();
    for(size_t i = 0; i <= size - len; ++i){
        size_t j = 0;
        for(; j < len; ++j){
            if(!n.wild[j] && mem[i+j] != n.bytes[j]) break;
        }
        if(j == len) return base + i;
    }
    return 0;
}

} // namespace

uintptr_t Base(){
    return reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
}

uintptr_t ModuleBase(const wchar_t* dllName){
    return reinterpret_cast<uintptr_t>(GetModuleHandleW(dllName));
}

size_t ModuleSize(const wchar_t* dllName){
    HMODULE m = GetModuleHandleW(dllName);
    if(!m) return 0;
    MODULEINFO mi{};
    if(!GetModuleInformation(GetCurrentProcess(), m, &mi, sizeof(mi))) return 0;
    return mi.SizeOfImage;
}

bool Read(uintptr_t addr, void* out, size_t n){
    std::memcpy(out, reinterpret_cast<const void*>(addr), n);
    return true;
}
bool Write(uintptr_t addr, const void* in, size_t n){
    std::memcpy(reinterpret_cast<void*>(addr), in, n);
    return true;
}

uintptr_t Pattern(const char* aob){
    return PatternIn(nullptr, aob);
}

uintptr_t PatternIn(const wchar_t* dllName, const char* aob){
    HMODULE m = dllName ? GetModuleHandleW(dllName) : GetModuleHandleW(nullptr);
    if(!m) return 0;
    MODULEINFO mi{};
    if(!GetModuleInformation(GetCurrentProcess(), m, &mi, sizeof(mi))) return 0;

    Needle n{};
    if(!CompileNeedle(aob, n)) return 0;
    return FindIn(reinterpret_cast<uintptr_t>(m), mi.SizeOfImage, n);
}

uintptr_t RipRel(uintptr_t instr, int opcodeLen, int instrLen){
    if(!instr) return 0;
    int32_t disp = *reinterpret_cast<int32_t*>(instr + opcodeLen);
    return instr + instrLen + disp;
}

} // namespace cf::proc
