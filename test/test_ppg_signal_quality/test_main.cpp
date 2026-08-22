// Arduino.h must come first: it declares setup()/loop() with C linkage, which is what the portduino
// core's main() links against. Without it these compile as mangled C++ symbols and the test binary
// fails to link with "undefined reference to `setup'".
#include <Arduino.h>

#include "TestUtil.h"
#include "modules/Telemetry/Sensor/PpgSignalQuality.h"
#include <math.h>
#include <unity.h>

/**
 * Regression tests for the PPG signal-quality gates.
 *
 * These exist because the badge used to display fabricated heart rates and SpO2 values derived from pure
 * sensor noise, and because a completely dead RED channel used to report a reassuring 96-97%. Both were
 * found by replaying the production decision path offline; these tests pin the fixes so they cannot
 * silently regress.
 *
 * The constants mirror MAX30102Sensor.h. They are duplicated deliberately: if someone changes a threshold
 * in the sensor, these tests still assert the behaviour the threshold was chosen to produce, and the
 * duplication is what makes the change visible.
 */

static constexpr uint16_t kBufLen = 100;
static constexpr uint16_t kMinLag = 8;  // ~187 bpm at 25 Hz
static constexpr uint16_t kMaxLag = 41; // ~37 bpm at 25 Hz
static constexpr uint16_t kMinOverlap = 25;
static constexpr float kHarmonicTol = 0.05f;
static constexpr float kHrMinRho = 0.40f;
static constexpr float kSpo2MinRho = 0.50f;

void setUp(void) {}
void tearDown(void) {}

/** Deterministic PRNG so failures reproduce exactly; std::rand is not portable across libcs. */
static uint32_t rngState = 1u;
static void rngSeed(uint32_t s)
{
    rngState = s ? s : 1u;
}
static float rngGauss()
{
    // Box-Muller on a xorshift32 stream.
    auto next = []() {
        rngState ^= rngState << 13;
        rngState ^= rngState >> 17;
        rngState ^= rngState << 5;
        return rngState;
    };
    const float u1 = ((next() >> 8) + 1.0f) / 16777217.0f;
    const float u2 = ((next() >> 8) + 1.0f) / 16777217.0f;
    return sqrtf(-2.0f * logf(u1)) * cosf(6.2831853f * u2);
}

/** Synthetic PPG: systolic peak plus a dicrotic wave, matching the shape the vendor kernel expects. */
static void makePpg(uint32_t *out, float bpm, float perfusionPercent, uint32_t dc, float noiseSigma)
{
    const float f = bpm / 60.0f;
    for (uint16_t i = 0; i < kBufLen; ++i) {
        const float t = (float)i / 25.0f;
        float beat = sinf(6.2831853f * f * t) + 0.35f * sinf(12.566371f * f * t + 0.9f);
        beat /= 1.32f; // normalise to roughly +-1
        const float v = (float)dc * (1.0f + perfusionPercent / 100.0f * beat) + rngGauss() * noiseSigma;
        out[i] = (uint32_t)(v < 0.0f ? 0.0f : v);
    }
}

static void makeNoise(uint32_t *out, uint32_t dc, float sigma)
{
    for (uint16_t i = 0; i < kBufLen; ++i) {
        const float v = (float)dc + rngGauss() * sigma;
        out[i] = (uint32_t)(v < 0.0f ? 0.0f : v);
    }
}

static ppg::Periodicity score(const uint32_t *w)
{
    return ppg::bestPeriodicity(w, kBufLen, kMinLag, kMaxLag, kMinOverlap, kHarmonicTol);
}

// --- the headline regression: noise must not look like a pulse -------------------------------------

static void test_pure_noise_is_rejected_at_every_amplitude()
{
    // The fabrication bug was amplitude-independent: an absolute AC threshold is cleared by any
    // sufficiently noisy flat signal, so this sweeps the full range that used to produce readings.
    const float sigmas[] = {20.0f, 30.0f, 50.0f, 80.0f, 120.0f, 200.0f, 400.0f};
    uint32_t w[kBufLen];

    for (float sigma : sigmas) {
        int hrPasses = 0, spo2Passes = 0;
        const int trials = 200;
        for (int t = 0; t < trials; ++t) {
            rngSeed((uint32_t)(t * 2654435761u) ^ (uint32_t)sigma);
            makeNoise(w, 90000, sigma);
            const ppg::Periodicity p = score(w);
            if (p.found && p.bestRho >= kHrMinRho) {
                hrPasses++;
            }
            if (p.found && p.bestRho >= kSpo2MinRho) {
                spo2Passes++;
            }
        }
        // Measured false-accept rates are ~0.7% (HR) and <0.1% (SpO2); allow generous headroom so the
        // test is not flaky, while still failing loudly if the gate is removed (which gives ~100%).
        TEST_ASSERT_LESS_THAN_INT(trials / 10, hrPasses);
        TEST_ASSERT_LESS_THAN_INT(trials / 20, spo2Passes);
    }
}

// --- and real signal must still get through ---------------------------------------------------------

static void test_real_ppg_passes_both_gates_across_rates_and_perfusion()
{
    uint32_t w[kBufLen];
    const float perfusions[] = {2.26f, 1.0f, 0.5f, 0.3f}; // 2.26% is the badge's own measured IR value
    for (float pi : perfusions) {
        for (int bpm = 45; bpm <= 105; bpm += 5) {
            rngSeed((uint32_t)(bpm * 7919));
            makePpg(w, (float)bpm, pi, 178000, 80.0f); // 178000 = measured badge DC
            const ppg::Periodicity p = score(w);
            TEST_ASSERT_TRUE_MESSAGE(p.found, "no periodicity found on clean PPG");
            TEST_ASSERT_TRUE_MESSAGE(p.bestRho >= kSpo2MinRho, "clean PPG failed the SpO2 periodicity gate");
        }
    }
}

/** The bug this caught: autocorrelation locking to 2x the beat interval and reporting half the rate. */
static void test_lag_reports_the_fundamental_not_a_subharmonic()
{
    uint32_t w[kBufLen];
    for (int bpm = 45; bpm <= 105; bpm += 5) {
        rngSeed((uint32_t)(bpm * 104729));
        makePpg(w, (float)bpm, 1.0f, 178000, 20.0f);
        const ppg::Periodicity p = score(w);
        TEST_ASSERT_TRUE(p.found);
        const uint32_t est = ppg::bpmFromLag(p.bestLag, 25);
        // 85 and 100 bpm used to come back at half rate (~42 and ~50).
        TEST_ASSERT_TRUE_MESSAGE(ppg::ratesAgree(est, (uint32_t)bpm, 15), "autocorrelation lag is a subharmonic");
    }
}

static void test_flat_signal_has_no_periodicity()
{
    uint32_t w[kBufLen];
    for (uint16_t i = 0; i < kBufLen; ++i) {
        w[i] = 90000;
    }
    TEST_ASSERT_FALSE(score(w).found);
}

// --- red channel integrity --------------------------------------------------------------------------

static void test_dead_red_channel_is_refused()
{
    // A dead RED channel drives the ratio to ~0, which the vendor table maps to a reassuring 96-97%.
    // acRed of 0 must be refused no matter how healthy IR looks.
    TEST_ASSERT_FALSE(ppg::redChannelUsable(4000, 178000, 0, 163000, 20, 25));
    // 3% of a normal red AC: still comfortably rejected.
    TEST_ASSERT_FALSE(ppg::redChannelUsable(4000, 178000, 45, 163000, 20, 25));
}

static void test_healthy_red_channel_is_accepted()
{
    // The badge's own measured values: PI_ir 2.26%, PI_red 0.91%, R ~0.41.
    const uint32_t meanIr = 177858, meanRed = 162998;
    const uint32_t acIr = (uint32_t)(meanIr * 0.0226f);
    const uint32_t acRed = (uint32_t)(meanRed * 0.0091f);
    TEST_ASSERT_TRUE(ppg::redChannelUsable(acIr, meanIr, acRed, meanRed, 20, 25));
}

static void test_red_channel_guards_against_divide_by_zero()
{
    TEST_ASSERT_FALSE(ppg::redChannelUsable(0, 178000, 1000, 163000, 20, 25));
    TEST_ASSERT_FALSE(ppg::redChannelUsable(4000, 0, 1000, 163000, 20, 25));
    TEST_ASSERT_FALSE(ppg::redChannelUsable(4000, 178000, 1000, 0, 20, 25));
}

// --- small helpers ----------------------------------------------------------------------------------

static void test_bpm_from_lag()
{
    TEST_ASSERT_EQUAL_UINT32(60, ppg::bpmFromLag(25, 25)); // 1500/25
    TEST_ASSERT_EQUAL_UINT32(0, ppg::bpmFromLag(0, 25));
}

static void test_rates_agree()
{
    TEST_ASSERT_TRUE(ppg::ratesAgree(60, 60, 25));
    TEST_ASSERT_TRUE(ppg::ratesAgree(70, 60, 25));  // 16.7% off
    TEST_ASSERT_FALSE(ppg::ratesAgree(42, 85, 25)); // the subharmonic case
    TEST_ASSERT_FALSE(ppg::ratesAgree(0, 60, 25));
}

void setup()
{
    initializeTestEnvironment();
    UNITY_BEGIN();
    RUN_TEST(test_pure_noise_is_rejected_at_every_amplitude);
    RUN_TEST(test_real_ppg_passes_both_gates_across_rates_and_perfusion);
    RUN_TEST(test_lag_reports_the_fundamental_not_a_subharmonic);
    RUN_TEST(test_flat_signal_has_no_periodicity);
    RUN_TEST(test_dead_red_channel_is_refused);
    RUN_TEST(test_healthy_red_channel_is_accepted);
    RUN_TEST(test_red_channel_guards_against_divide_by_zero);
    RUN_TEST(test_bpm_from_lag);
    RUN_TEST(test_rates_agree);
    UNITY_END();
}

void loop() {}
