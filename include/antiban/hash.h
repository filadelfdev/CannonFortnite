#pragma once
#include <cstdint>
#include <cctype>
#include <cwctype>

namespace cf::ab {

constexpr uint64_t fnv1a(const char* s){
    uint64_t h = 0xcbf29ce484222325ULL;
    for(; *s; ++s){ h ^= (uint8_t)*s; h *= 0x100000001b3ULL; }
    return h;
}
constexpr uint64_t fnv1a_lower(const char* s){
    uint64_t h = 0xcbf29ce484222325ULL;
    for(; *s; ++s){
        char c = (*s>='A'&&*s<='Z')? char(*s+32) : *s;
        h ^= (uint8_t)c; h *= 0x100000001b3ULL;
    }
    return h;
}
inline uint64_t fnv1a_w(const wchar_t* s){
    uint64_t h = 0xcbf29ce484222325ULL;
    for(; *s; ++s){
        wchar_t c = std::towlower(*s);
        h ^= (uint8_t)(c & 0xFF);       h *= 0x100000001b3ULL;
        h ^= (uint8_t)((c >> 8) & 0xFF);h *= 0x100000001b3ULL;
    }
    return h;
}

// Compile-time string obfuscator — the module ships with xor'd strings so a
// strings -a sweep never returns anything human. Call .c() for the plaintext,
// which decrypts on stack, never into .rdata.
template<size_t N>
struct XStr {
    char data[N];
    char key;
    constexpr XStr(const char (&s)[N], char k=0x5A) : data{}, key(k) {
        for(size_t i=0;i<N;++i) data[i] = s[i]^k;
    }
    auto c() const {
        struct Live { char buf[N]; const char* p() const { return buf; } };
        Live live{};
        for(size_t i=0;i<N;++i) live.buf[i] = data[i]^key;
        return live;
    }
};
#define X(s) ([]{ constexpr cf::ab::XStr x(s); return x; }())

} // namespace cf::ab
