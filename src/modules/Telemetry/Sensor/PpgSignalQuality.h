#pragma once

/**
 * Hardware-free PPG signal-quality primitives.
 *
 * Deliberately depends on nothing but the C++ standard library: no Arduino, no Wire, no MAX30105, no
 * Meshtastic globals. That is the whole point of the file. The decision logic that determines whether the
 * badge shows a number is the part most worth testing and, until this split, the part that could not be
 * tested at all - MAX30102Sensor.cpp compiles to an empty translation unit in the only host environment
 * because the SparkFun library is an Arduino-targets-only dependency.
 *
 * Keeping these as free functions over plain arrays means the native test build can exercise the exact
 * code the firmware runs, rather than a reimplementation that can drift from it.
 */

#include <math.h>
#include <stddef.h>
#include <stdint.h>

namespace ppg
{

/** Result of a periodicity search over a PPG window. */
struct Periodicity {
    bool found = false;
    float bestRho = -2.0f; ///< Pearson autocorrelation at the selected lag.
    uint16_t bestLag = 0;  ///< Lag in samples; 0 when nothing qualified.
};

/**
 * Best Pearson autocorrelation at an interior local maximum, preferring the fundamental period.
 *
 * @param ir            sample window (IR channel)
 * @param count         number of samples
 * @param minLag,maxLag inclusive lag search bounds, in samples
 * @param minOverlap    minimum overlapping samples required to score a lag
 * @param harmonicTol   peaks within this of the best are treated as ties, so the shortest period wins
 *
 * Pearson - centring and normalising each shifted segment on its OWN mean - is used rather than a biased
 * autocorrelation normalised by total window energy, because the latter is sensitive to baseline drift and
 * to window length, which makes any threshold chosen for it untransferable between windows.
 *
 * Only INTERIOR local maxima qualify. Monotonic decay is what drift and 1/f noise produce and it has no
 * peak; a real pulse train has one at the beat interval.
 *
 * The fundamental preference matters: a clean pulse train correlates almost as well at twice the beat
 * interval as at the beat interval itself, so taking the global maximum outright reports half the true
 * rate whenever the 2x peak edges ahead. Scanning ascending and taking the first peak statistically
 * indistinguishable from the best selects the shortest qualifying period.
 */
inline Periodicity bestPeriodicity(const uint32_t *ir, uint16_t count, uint16_t minLag, uint16_t maxLag,
                                   uint16_t minOverlap, float harmonicTol)
{
    Periodicity out;
    if (ir == nullptr || count == 0 || minLag < 1 || maxLag < minLag) {
        return out;
    }

    // Score lags [minLag-1, maxLag+1]; the +-1 neighbours are needed to test for an interior maximum.
    const uint16_t lo = (uint16_t)(minLag - 1);
    const uint16_t hi = (uint16_t)(maxLag + 1);
    const size_t span = (size_t)(hi - lo + 1);

    // Bounded on the stack: callers use physiological lag ranges, a few tens of entries.
    constexpr size_t kMaxSpan = 128;
    if (span > kMaxSpan) {
        return out;
    }
    float rho[kMaxSpan];
    for (size_t i = 0; i < span; ++i) {
        rho[i] = -2.0f;
    }

    for (uint16_t lag = lo; lag <= hi; ++lag) {
        if (lag >= count) {
            break;
        }
        const uint16_t m = (uint16_t)(count - lag);
        if (m < minOverlap) {
            continue;
        }

        double sa = 0.0, sb = 0.0;
        for (uint16_t i = 0; i < m; ++i) {
            sa += (double)ir[i];
            sb += (double)ir[i + lag];
        }
        const double ma = sa / (double)m;
        const double mb = sb / (double)m;

        double num = 0.0, da = 0.0, db = 0.0;
        for (uint16_t i = 0; i < m; ++i) {
            const double a = (double)ir[i] - ma;
            const double b = (double)ir[i + lag] - mb;
            num += a * b;
            da += a * a;
            db += b * b;
        }
        if (da > 0.0 && db > 0.0) {
            rho[lag - lo] = (float)(num / sqrt(da * db));
        }
    }

    auto isInteriorMax = [&](uint16_t lag) {
        const size_t k = (size_t)(lag - lo);
        if (k == 0 || k + 1 >= span) {
            return false;
        }
        if (rho[k] <= -2.0f || rho[k - 1] <= -2.0f || rho[k + 1] <= -2.0f) {
            return false;
        }
        return rho[k] >= rho[k - 1] && rho[k] >= rho[k + 1];
    };

    float best = -2.0f;
    for (uint16_t lag = minLag; lag <= maxLag; ++lag) {
        if (isInteriorMax(lag) && rho[lag - lo] > best) {
            best = rho[lag - lo];
        }
    }
    if (best <= -2.0f) {
        return out;
    }

    for (uint16_t lag = minLag; lag <= maxLag; ++lag) {
        if (isInteriorMax(lag) && rho[lag - lo] >= (best - harmonicTol)) {
            out.found = true;
            out.bestRho = rho[lag - lo];
            out.bestLag = lag;
            return out;
        }
    }
    return out;
}

/**
 * Red-channel integrity test for SpO2.
 *
 * The vendor R->SpO2 table is non-monotonic: it peaks at 100 across a wide plateau and returns 95-97 as the
 * ratio approaches zero. A RED channel that has failed therefore produces a REASSURING 96-97% rather than
 * an obviously broken one - degrading the channel makes the displayed number look better. So SpO2 requires
 * red to carry its own pulsatile component, and the ratio to be clear of the region where the table folds.
 *
 * All integer arithmetic; no division, so no rounding surprises.
 */
inline bool redChannelUsable(uint32_t acIr, uint32_t meanIr, uint32_t acRed, uint32_t meanRed,
                             uint32_t redPiMinPermyriad, uint32_t minRatioPercent)
{
    if (meanRed == 0 || meanIr == 0 || acIr == 0) {
        return false;
    }
    const bool perfusionOk = ((uint64_t)acRed * 10000ULL) >= ((uint64_t)meanRed * redPiMinPermyriad);
    const bool ratioOk =
        ((uint64_t)acRed * (uint64_t)meanIr * 100ULL) >= ((uint64_t)acIr * (uint64_t)meanRed * minRatioPercent);
    return perfusionOk && ratioOk;
}

/** Heart rate implied by an autocorrelation lag, given the effective sample rate. 0 if undefined. */
inline uint32_t bpmFromLag(uint16_t lag, uint32_t sampleRateHz)
{
    if (lag == 0) {
        return 0;
    }
    return (uint32_t)((60UL * sampleRateHz) / lag);
}

/** True when two rate estimates agree within tolerancePercent of the reference. */
inline bool ratesAgree(uint32_t candidateBpm, uint32_t referenceBpm, uint32_t tolerancePercent)
{
    if (candidateBpm == 0 || referenceBpm == 0) {
        return false;
    }
    const uint32_t diff = candidateBpm > referenceBpm ? candidateBpm - referenceBpm : referenceBpm - candidateBpm;
    return ((uint64_t)diff * 100ULL) <= ((uint64_t)referenceBpm * tolerancePercent);
}

} // namespace ppg
