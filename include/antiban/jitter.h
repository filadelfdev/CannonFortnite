#pragma once
#include "core/cf_api.h"
#include <cstdint>

namespace cf::ab::jitter {

// Global jitter source — one cryptographic-grade (xoshiro256**) stream
// every timing/position/aim write pulls from. Keeps every detectable
// metric (inter-arrival, write-count-per-tick, delta magnitude) off
// the "monotonic bot" signature that EAC's and Fortnite's own
// behaviour classifiers bucket on.
//
// Lives in cheat_antiban.dll. The RNG state is module-local so a
// cheat_core unload/reload does not restart the stream.

CF_AB_API void    Seed();
CF_AB_API double  frac();
CF_AB_API int32_t irange(int32_t lo, int32_t hi);
CF_AB_API float   frange(float lo, float hi);
CF_AB_API double  gatedMs(double meanMs, double spreadMs);

// v5: gaussian and lognormal distributions — human reaction times
// and inter-arrival intervals follow these, not uniform. EAC's
// behaviour classifier buckets deviations from both.
CF_AB_API double  gaussian(double mean, double stddev);
CF_AB_API double  lognormal(double meanLn, double sigmaLn);

// Lognormal-gated inter-arrival — the realistic model for human
// click / interact cadence. meanMs is the mode of the distribution;
// sigma is the shape (typical 0.35-0.55 for engaged humans).
CF_AB_API double  lognormalMs(double meanMs, double sigma);
CF_AB_API bool    shouldFireLognormal(double nowSec, double& lastSec,
                                      double meanMs, double sigma);

// Returns true + updates `lastSec` when enough time (mean ± spread) has
// passed since `lastSec`. The gate's own interval is jittered every
// call so no two gates share a cadence.
CF_AB_API bool    shouldFire(double nowSec, double& lastSec,
                             double meanMs, double spreadMs);

// Analog wobble for writes that the server validates against its own
// physics tick — adds sub-magnitude noise so the replicated stream
// has noise, not step functions.
CF_AB_API float   wobble(float v, float magnitude);

} // namespace cf::ab::jitter
