#pragma once
#include <cstdint>
#include <cstdio>
#include <cstdarg>
#include <cctype>
#include <cwctype>

namespace cf::spoofer::util {

// Minimal FNV1a — mirrors cheat side so marker hash matches.
constexpr uint64_t fnv1a(const char* s){
    uint64_t h = 0xcbf29ce484222325ULL;
    for(; *s; ++s){ h ^= (uint8_t)*s; h *= 0x100000001b3ULL; }
    return h;
}
inline uint64_t fnv1a(const char* s, size_t n){
    uint64_t h = 0xcbf29ce484222325ULL;
    for(size_t i=0;i<n;++i){ h ^= (uint8_t)s[i]; h *= 0x100000001b3ULL; }
    return h;
}

inline void Info(const char* fmt, ...){
    char buf[2048];
    va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    printf("[spoofer] %s\n", buf);
}
inline void Warn(const char* fmt, ...){
    char buf[2048];
    va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    fprintf(stderr, "[spoofer][warn] %s\n", buf);
}
inline void Err(const char* fmt, ...){
    char buf[2048];
    va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    fprintf(stderr, "[spoofer][err]  %s\n", buf);
}

} // namespace cf::spoofer::util
