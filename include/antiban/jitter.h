#pragma once
#include <cstdint>

namespace cf::ab::jitter {

// Global jitter source — one cryptographic-grade (xoshiro256**) stream
// every timing/position/aim write pulls from. Keeps every detectable
// metric (inter-arrival, write-count-per-tick, delta magnitude) off
// the "monotonic bot" signature that EAC's and Fortnite's own
// behaviour classifiers bucket on.

void    Seed();
double  frac();
int32_t irange(int32_t lo, int32_t hi);
float   frange(float lo, float hi);
double  gatedMs(double meanMs, double spreadMs);

// v5: gaussian and lognormal distributions — human reaction times
// and inter-arrival intervals follow these, not uniform. EAC's
// behaviour classifier buckets deviations from both.
double  gaussian(double mean, double stddev);
double  lognormal(double meanLn, double sigmaLn);

// Lognormal-gated inter-arrival — the realistic model for human
// click / interact cadence. meanMs is the mode of the distribution;
// sigma is the shape (typical 0.35-0.55 for engaged humans).
double  lognormalMs(double meanMs, double sigma);
bool    shouldFireLognormal(double nowSec, double& lastSec,
                            double meanMs, double sigma);

// Returns true + updates `lastSec` when enough time (mean ± spread) has
// passed since `lastSec`. The gate's own interval is jittered every
// call so no two gates share a cadence.
bool    shouldFire(double nowSec, double& lastSec,
                   double meanMs, double spreadMs);

// Analog wobble for writes that the server validates against its own
// physics tick — adds sub-magnitude noise so the replicated stream
// has noise, not step functions.
float   wobble(float v, float magnitude);

} // namespace cf::ab::jitter
