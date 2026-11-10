#include "antiban/jitter.h"
#include <windows.h>
#include <bcrypt.h>
#include <cmath>
#include <cstring>

#pragma comment(lib, "bcrypt")

namespace cf::ab::jitter {

namespace {

// xoshiro256** — small, fast, no low-bit bias. One stream per worker
// thread so the shared bench doesn't serialise on a mutex.
thread_local uint64_t s[4]{};
thread_local bool     seeded = false;

inline uint64_t rotl(uint64_t x, int k){ return (x<<k) | (x>>(64-k)); }

uint64_t next(){
    uint64_t result = rotl(s[1]*5, 7) * 9;
    uint64_t t      = s[1] << 17;
    s[2] ^= s[0]; s[3] ^= s[1]; s[1] ^= s[2]; s[0] ^= s[3];
    s[2] ^= t;    s[3]  = rotl(s[3], 45);
    return result;
}

void seedIfNeeded(){
    if(seeded) return;
    uint8_t buf[32];
    BCryptGenRandom(nullptr, buf, sizeof(buf), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    std::memcpy(s, buf, sizeof(s));
    if((s[0]|s[1]|s[2]|s[3]) == 0) s[0] = 0x9E3779B97F4A7C15ULL;
    seeded = true;
}

} // namespace

void Seed(){ seedIfNeeded(); }

double frac(){
    seedIfNeeded();
    return (next() >> 11) * (1.0 / 9007199254740992.0);
}

int32_t irange(int32_t lo, int32_t hi){
    if(hi <= lo) return lo;
    uint64_t span = (uint64_t)(hi - lo + 1);
    return lo + (int32_t)(next() % span);
}

float frange(float lo, float hi){
    return lo + (float)frac() * (hi - lo);
}

double gatedMs(double meanMs, double spreadMs){
    return meanMs + (frac()*2.0 - 1.0) * spreadMs;
}

bool shouldFire(double nowSec, double& lastSec, double meanMs, double spreadMs){
    double gate = gatedMs(meanMs, spreadMs) * 0.001;
    if((nowSec - lastSec) < gate) return false;
    lastSec = nowSec;
    return true;
}

float wobble(float v, float magnitude){
    return v + (float)(frac()*2.0 - 1.0) * magnitude;
}

double gaussian(double mean, double stddev){
    // Box-Muller.
    double u1 = frac(); if(u1 < 1e-9) u1 = 1e-9;
    double u2 = frac();
    double z  = std::sqrt(-2.0 * std::log(u1)) * std::cos(6.2831853071795864769 * u2);
    return mean + z * stddev;
}

double lognormal(double meanLn, double sigmaLn){
    return std::exp(gaussian(meanLn, sigmaLn));
}

double lognormalMs(double meanMs, double sigma){
    // Convert mode to meanLn so the output peaks at `meanMs`.
    double meanLn = std::log(meanMs) + sigma * sigma;
    return lognormal(meanLn, sigma);
}

bool shouldFireLognormal(double nowSec, double& lastSec,
                         double meanMs, double sigma){
    static thread_local double currentGate = 0.0;
    if(currentGate == 0.0) currentGate = lognormalMs(meanMs, sigma) * 0.001;
    if((nowSec - lastSec) < currentGate) return false;
    lastSec = nowSec;
    // Roll a fresh gate for the next interval.
    currentGate = lognormalMs(meanMs, sigma) * 0.001;
    return true;
}

} // namespace cf::ab::jitter
