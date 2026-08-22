#include "configuration.h"

#if !MESHTASTIC_EXCLUDE_HEALTH_TELEMETRY && __has_include(<MAX30105.h>)

#include "../mesh/generated/meshtastic/telemetry.pb.h"
#include "TelemetrySensor.h"
#include "concurrency/Lock.h"
#include <MAX30105.h>

#define MAX30102_BUFFER_LEN 100

class MAX30102Sensor : public TelemetrySensor
{
  private:
    enum class PulseOxChipType { UNKNOWN, MAX30100, MAX30102 };

    MAX30105 max30102 = MAX30105();
    uint32_t _speed = 200000UL;
    PulseOxChipType chipType = PulseOxChipType::UNKNOWN;

    static constexpr uint8_t MAX30100_PART_ID = 0x11;
    static constexpr uint8_t MAX30102_PART_ID = 0x15;

    // MAX30100 registers
    static constexpr uint8_t MAX30100_REG_INTERRUPT_STATUS = 0x00;
    static constexpr uint8_t MAX30100_REG_FIFO_WRITE_POINTER = 0x02;
    static constexpr uint8_t MAX30100_REG_FIFO_OVERFLOW_COUNTER = 0x03;
    static constexpr uint8_t MAX30100_REG_FIFO_READ_POINTER = 0x04;
    static constexpr uint8_t MAX30100_REG_FIFO_DATA = 0x05;
    static constexpr uint8_t MAX30100_REG_MODE_CONFIG = 0x06;
    static constexpr uint8_t MAX30100_REG_SPO2_CONFIG = 0x07;
    static constexpr uint8_t MAX30100_REG_LED_CONFIG = 0x09;
    static constexpr uint8_t MAX30100_REG_TEMP_INTEGER = 0x16;
    static constexpr uint8_t MAX30100_REG_TEMP_FRACTION = 0x17;
    static constexpr uint8_t MAX30100_REG_PART_ID = 0xFF;
    static constexpr uint8_t MAX3010X_REG_INT_STATUS_1 = 0x00;
    /** Ambient-light-cancellation overflow: the ALC has saturated, so this window's PPG is unusable. */
    static constexpr uint8_t MAX3010X_INT_ALC_OVF = 0x20;
    /** Counts samples LOST to a full FIFO. Cleared when a complete sample is popped, so read it FIRST. */
    static constexpr uint8_t MAX3010X_REG_OVF_COUNTER = 0x05;
    static constexpr uint8_t MAX3010X_REG_FIFO_WRITE_POINTER = 0x04;
    static constexpr uint8_t MAX3010X_REG_FIFO_READ_POINTER = 0x06;
    static constexpr uint8_t MAX3010X_REG_FIFO_DATA = 0x07;
    static constexpr uint8_t MAX3010X_FIFO_POINTER_MASK = 0x1F;
    static constexpr uint8_t MAX30102_MAX_SAMPLES_PER_BURST = 5; // 5 samples * 6 bytes = 30 bytes per I2C burst

    // MAX30100 bit fields
    static constexpr uint8_t MAX30100_MODE_HR_SPO2 = 0x03;
    static constexpr uint8_t MAX30100_MODE_RESET = 0x40;
    static constexpr uint8_t MAX30100_MODE_SHUTDOWN = 0x80;
    static constexpr uint8_t MAX30100_MODE_TEMP_EN = 0x08;
    static constexpr uint8_t MAX30100_SPO2_HI_RES_EN = 0x40;
    static constexpr uint8_t MAX30100_SPO2_SR_50HZ = 0x00;
    static constexpr uint8_t MAX30100_SPO2_SR_100HZ = 0x04;
    static constexpr uint8_t MAX30100_SPO2_PW_1600US_16BIT = 0x03;
    // 4-bit per channel current code (0x0..0xF). 0xC is a stronger drive than default, without maxing output.
    // High nibble = IR current, low nibble = RED current.
    static constexpr uint8_t MAX30100_LED_IR_40MA_RED_40MA = 0xCC;
    static constexpr uint8_t MAX30100_INT_TEMP_RDY = 0x20;
    static constexpr uint8_t MAX30100_RAW_TO_ALGO_DECIMATION = 2; // 50Hz sensor output -> 25Hz algorithm input
    static constexpr uint16_t MAX3010X_SLIDING_STEP = 10;
    static constexpr uint8_t STABILITY_WINDOW_SIZE = 5;
    static constexpr uint8_t STABILITY_MIN_COUNT = 2;
    static constexpr uint32_t HR_STABILITY_PERCENT = 20;
    static constexpr uint32_t SPO2_STABILITY_SPREAD = 2;
    static constexpr uint32_t MAX3010X_FINGER_IR_DC_MIN = 2000;
    static constexpr uint32_t MAX3010X_FINGER_RED_DC_MIN = 1000;
    static constexpr uint32_t MAX3010X_FINGER_IR_AC_MIN = 80;
    static constexpr uint32_t MAX3010X_FINGER_RED_AC_MIN = 40;
    static constexpr uint32_t MAX3010X_PRESENCE_IR_DC_MIN = 80;   // Low-power presence mode threshold
    static constexpr uint32_t MAX3010X_PRESENCE_RED_DC_MIN = 40;  // Low-power presence mode threshold
    static constexpr uint32_t MAX3010X_PRESENCE_IR_PEAK_MIN = 140;  // Instantaneous peak threshold in low-power mode
    static constexpr uint32_t MAX3010X_PRESENCE_RED_PEAK_MIN = 70;  // Instantaneous peak threshold in low-power mode
    static constexpr uint32_t MAX3010X_PRESENCE_IR_DC_MIN_IR_ONLY = 3200;   // IR-only presence threshold (LED=0x02)
    static constexpr uint32_t MAX3010X_PRESENCE_IR_PEAK_MIN_IR_ONLY = 4500; // IR-only peak threshold (LED=0x02)
    static constexpr uint8_t MAX3010X_PRESENCE_MIN_SAMPLES = 6;
    static constexpr uint8_t MAX3010X_PRESENCE_CONSECUTIVE_REQUIRED = 2;
    /**
     * Signal-collapse detection for the active measurement epoch.
     *
     * These used to be absolute DC thresholds (red < 60000 && ir < 70000). That was ~30x stricter than
     * detectFingerPresence()'s own 2000/1000 gate, so any wearer whose optical coupling landed between
     * those two bars was declared "finger present", measured, and then force-slept before a reading could
     * stabilise. Measured dead band: IR DC 4000..70000 counts -> heart rate never displayed at all.
     *
     * Coupling varies enormously between people, so the replacement is RELATIVE: the epoch records its
     * own starting DC and treats a large fractional drop as the finger leaving. That adapts to whoever is
     * actually wearing the badge instead of to whoever it was tuned on.
     */
    static constexpr uint32_t MAX3010X_POWERDOWN_DC_FRACTION_PERCENT = 40;
    /** Require consecutive bad evaluations before sleeping, so one noisy window cannot abort a session. */
    static constexpr uint8_t MAX3010X_NO_FINGER_EVAL_STREAK_FOR_SLEEP = 2;
    static constexpr uint32_t MAX3010X_FINGER_PULSATILITY_PERMILLE = 4; // 0.4%
    static constexpr uint32_t HEART_RATE_MIN_VALID = 35;
    static constexpr uint32_t HEART_RATE_MAX_VALID = 220;
    static constexpr uint32_t SPO2_MIN_VALID = 70;
    static constexpr uint32_t SPO2_MAX_VALID = 100;
    /** Algorithm sentinel for "invalid" SpO2 (e.g. Maxim returns -999); never treat as valid. */
    static constexpr int32_t SPO2_INVALID_SENTINEL = -999;
    /**
     * Red-channel integrity gate for SpO2.
     *
     * The vendor R->SpO2 table is non-monotonic: it peaks at 100 over a wide plateau and returns 95-97 as
     * the ratio approaches zero. A RED channel that has failed - dead emitter, poor red coupling, or a DC
     * pedestal carrying no pulsatile component - drives the ratio toward zero and therefore produces a
     * REASSURING 96-97%, flagged stable, indistinguishable from a healthy reading. Degrading the red
     * channel makes the displayed number look better, not worse.
     *
     * Nothing downstream can recover from that, so SpO2 is refused unless the red channel demonstrably
     * carries a pulsatile signal of its own, and unless the ratio is above the region where the table
     * has folded back. Both thresholds were chosen against real badge captures: together they accept
     * ~98% of genuine on-finger windows while rejecting a red channel degraded to a few percent of normal.
     * These gate SpO2 only - heart rate comes from the IR channel and is deliberately not held hostage
     * to red-channel health.
     */
    /*
     * LIVENESS check, not a quality gate. Chosen from measured data, not intuition.
     *
     * Across six hardware conditions (~300 evaluations) the red perfusion index turned out to be
     * INVERSELY correlated with signal quality on this badge: good contact measured 0.207-0.208%, while
     * firm pressure gave 0.453% and a barely-resting finger 1.683% - because poor contact produces large
     * aperiodic excursions that dwarf a real pulse. A perfusion FLOOR therefore preferentially accepts the
     * conditions least worth trusting, which is why the old 0.20% value both bisected the healthy
     * distribution (median 0.207%) and failed to reject anything useful.
     *
     * Signal quality is enforced by periodicity, rate agreement and stability instead; those reject firm
     * and light contact at every threshold tested. This constant now only answers "is the red channel
     * alive at all", where a collapsed channel measures ~0.006%.
     *
     * Threshold sweep, pooled good contact (normal + cold, n=132) versus bad (firm + light):
     *     0.20% -> 40% of good accepted, 5% of bad
     *     0.12% -> 69%                 , 5%
     *     0.10% -> 72%                 , 5%     <- benefit saturates here
     *     0.05% -> 73%                 , 5%     (no further gain)
     * 0.10% is the knee: it recovers nearly all the available good-contact signal, still clears a dead
     * channel by ~17x, and goes no lower than the evidence supports. Lowest good window observed: 0.085%.
     */
    static constexpr uint32_t SPO2_RED_PI_MIN_PERMYRIAD = 10; // red perfusion index >= 0.10%
    static constexpr uint32_t SPO2_MIN_R_PERCENT = 25;        // ratio-of-ratios >= 0.25

    /**
     * Periodicity gate.
     *
     * Amplitude tests cannot separate a pulse from noise: a flat DC level plus enough noise clears any
     * absolute AC threshold, and at large noise amplitudes it clears the perfusion-ratio threshold too.
     * A heartbeat's distinguishing property is not that it is large, it is that it REPEATS. Replaying the
     * production path on pure noise produced kernel-valid heart rates on 100% of windows and put a number
     * on the screen for roughly half of all evaluations; no amplitude threshold closes that.
     *
     * So each window is scored by its best Pearson autocorrelation at an interior local maximum, searched
     * over lags corresponding to physiological rates. At 25 Hz, BPM = 1500/lag, so lag 8..41 spans about
     * 187 down to 37 bpm. Pearson (each shifted segment separately centred and normalised) is used rather
     * than a biased autocorrelation normalised by total window energy, because the latter is sensitive to
     * baseline drift and to window length, which makes its threshold untransferable between windows.
     *
     * Requiring an INTERIOR local maximum matters: monotonically decaying correlation is what drift and
     * 1/f noise produce, and it has no peak. A real pulse train has one at the beat interval.
     *
     * SpO2 is held to a stricter score than heart rate, and additionally to agreement between the lag and
     * the reported rate, because the SpO2 path has proven markedly more fragile on real hardware.
     * Cost is roughly 19k flops per evaluation at a 2 Hz cadence - well under 0.1% CPU on an ESP32-S3.
     */
    /*
     * Set from measured data: 371 evaluations over 8 hardware conditions, classified by ground truth
     * (steady contact = good; firm pressure / barely-resting / offset = bad).
     *
     * HR has no rate-agreement requirement - deliberately, so heart rate stays available when SpO2 is
     * withheld - which means this constant IS the whole HR quality gate. At the original 0.40 it admitted
     * 27% of known-bad windows, matching an observed 16% of light-touch evaluations displaying a heart
     * rate. Raising it trades a little availability for a lot of that:
     *
     *     rho   GOOD eligible   BAD eligible   separation
     *     0.40       93%            27%          65pp
     *     0.55       89%            21%          68pp
     *     0.65       87%            15%          71pp   <- marginal knee on THIS dataset
     *     0.70       82%            13%          69pp   (turns unfavourable)
     *
     * FINAL VALUE 0.45, arrived at by getting it wrong first.
     *
     * 0.55 was chosen from those pooled windows, then measured on hardware and found too tight. The
     * pooling was the error: it treats 193 windows drawn from three sessions as independent samples, when
     * the dominant source of variance is BETWEEN sessions. Measured medians for nominally identical
     * "steady contact" on the same finger, over consecutive captures:
     *
     *     session 1  rho 0.92    HR shown 100%
     *     session 2  rho 0.77    HR shown  61%
     *     session 3  rho 0.55    HR shown  25%   <- at a 0.55 gate
     *
     * Contact quality drifts with fatigue and position, so good contact spans roughly rho 0.55-0.92. A
     * gate at 0.55 sits on the bottom edge of that range and collapses availability on a merely-average
     * session - the same defect as the original 0.20% perfusion floor, and as the 70000-count DC bar
     * before it: a threshold placed inside the distribution it is judging.
     *
     * 0.45 sits below the worst observed good-contact session (0.55) and well above the bad-contact
     * median (0.24). For heart rate specifically it is better to under-gate and let the stability window
     * reject the remainder than to go quiet on a wearer whose contact is simply not perfect. Revisit with
     * multi-subject data, and pool by SESSION rather than by window.
     *
     * SPO2_MIN_AUTOCORR stays at 0.50: SpO2 additionally requires the autocorrelation lag to agree with
     * the kernel's rate within SPO2_LAG_HR_TOLERANCE_PERCENT, and that check already does the work.
     * Measured, raising this to 0.65 moves bad-window admission only 6% -> 5% while costing good contact,
     * so the extra strictness buys essentially nothing.
     */
    static constexpr float HR_MIN_AUTOCORR = 0.45f;
    static constexpr float SPO2_MIN_AUTOCORR = 0.50f;
    static constexpr uint16_t AUTOCORR_MIN_LAG = 8;  // ~187 bpm
    static constexpr uint16_t AUTOCORR_MAX_LAG = 41; // ~37 bpm
    static constexpr uint16_t AUTOCORR_MIN_OVERLAP = 25;
    /** 100 sps with 4x FIFO averaging; also the rate the vendor kernel hard-codes. */
    static constexpr uint32_t MAX3010X_EFFECTIVE_SAMPLE_RATE_HZ = 25;
    /** Peaks within this of the best are treated as ties, so the shortest (fundamental) period wins. */
    static constexpr float AUTOCORR_HARMONIC_TOLERANCE = 0.05f;
    /** SpO2 additionally requires the autocorrelation lag to agree with the reported rate, +-25%. */
    static constexpr uint32_t SPO2_LAG_HR_TOLERANCE_PERCENT = 25;
    /** Best interior-local-max Pearson autocorrelation of the IR window; false if none exists. */
    bool computePeriodicity(const uint32_t *ir, uint16_t count, float *bestRhoOut, uint16_t *bestLagOut) const;
    static constexpr uint32_t MAX3010X_EVAL_MIN_INTERVAL_MS = 500; // Limit HR/SpO2 algorithm cadence to ~2Hz
    static constexpr uint32_t STABLE_VALUE_HOLD_MS = 5000; // Keep last stable value briefly during transient instability
    static constexpr float HEART_EMA_ALPHA = 0.35f;        // Faster convergence while retaining smoothing
    static constexpr float HEART_OUTPUT_EMA_ALPHA = 0.55f; // Faster displayed HR lock-in
    static constexpr uint8_t MAX30102_LED_POWER_PRESENCE = 0x02;
    /** Default active LED power; higher (e.g. 0x2F) improves SpO2 algorithm success vs 0x1F. */
    static constexpr uint8_t MAX30102_LED_POWER_ACTIVE_DEFAULT = 0x2F;
    static constexpr uint8_t MAX30102_LED_POWER_ACTIVE_MAX = 0x4F;
    static constexpr uint8_t MAX30102_LED_POWER_STEP = 0x06;
    static constexpr uint8_t MAX30102_POOR_SIGNAL_STREAK_FOR_BOOST = 4;
    static constexpr uint8_t MAX30102_STABLE_SIGNAL_STREAK_FOR_REDUCE = 10;
    static constexpr uint32_t MAX30102_LED_POWER_ADJUST_INTERVAL_MS = 3000;
    /**
     * Grace period after switching to the active profile, before the downshift gate may sleep the sensor.
     * MUST exceed the time to the first evaluation, or the gate is armed before any evaluation can occur
     * and the "grace period" grants none. That budget is:
     *   4000 ms to refill the 100-sample window at 25 Hz (configureMAX30102Profile clears it)
     * +  500 ms MAX3010X_EVAL_MIN_INTERVAL_MS
     * +  200 ms sensorServiceIntervalMs tick granularity
     * = 4700 ms floor.
     */
    static constexpr uint32_t MAX30102_PRESENCE_ACTIVE_HOLD_MS = 5000;
    static constexpr uint32_t MAX30102_PRESENCE_STATS_LOG_INTERVAL_MS = 1000;
    static constexpr uint32_t MAX30102_PRESENCE_SCAN_WAKE_WINDOW_MS = 700;
    static constexpr uint32_t MAX30102_PRESENCE_SCAN_INTERVAL_MS = 1000;

    concurrency::Lock waveformLock;
    concurrency::Lock metricsLock;
    uint32_t lastIrWaveform[MAX30102_BUFFER_LEN] = {0};
    uint32_t lastRedWaveform[MAX30102_BUFFER_LEN] = {0};
    uint16_t lastWaveformCount = 0;
    void cacheRawWaveform(const uint32_t *ir, const uint32_t *red, uint16_t count);
    void clearRawWaveformCache();

    uint32_t slidingIrWindow[MAX30102_BUFFER_LEN] = {0};
    uint32_t slidingRedWindow[MAX30102_BUFFER_LEN] = {0};
    uint16_t slidingWriteIndex = 0;
    uint16_t slidingSampleCount = 0;
    uint16_t slidingNewSamplesSinceEval = 0;
    uint32_t lastEvalMs = 0;
    uint32_t lastEvalMeanIr = 0;
    uint32_t lastEvalMeanRed = 0;
    uint32_t lastDownshiftGateLogMs = 0;
    /** Diagnostics: windows seen with ambient-light-cancellation overflow. */
    uint32_t alcOverflowEvents = 0;
    uint32_t lastIntegrityLogMs = 0;
    /**
     * Reference IR DC for the current active epoch; 0 = not yet established.
     *
     * Taken as the MEDIAN of the first MAX3010X_EPOCH_ANCHOR_SAMPLES finger-present evaluations rather
     * than the first one alone. Initial contact often contains a placement transient - a hard press that
     * then relaxes - and anchoring on that single high window would make the legitimate settled contact
     * that follows look like a 60% collapse and abort the session.
     */
    uint32_t activeEpochStartMeanIr = 0;
    static constexpr uint8_t MAX3010X_EPOCH_ANCHOR_SAMPLES = 3;
    uint32_t epochAnchorSamples[MAX3010X_EPOCH_ANCHOR_SAMPLES] = {0};
    uint8_t epochAnchorCount = 0;
    /** Consecutive evaluations with no finger / collapsed signal. Gates the downshift to presence scanning. */
    uint8_t noFingerEvalStreak = 0;
    uint8_t max30100RawSampleCount = 0;
    bool max30102PresenceMode = false;
    bool max30102PresenceState = false;
    bool max30102PresenceTriggeredActive = false;
    uint32_t max30102PresenceActiveSinceMs = 0;
    uint32_t max30102PresenceStatsLastLogMs = 0;
    uint8_t max30102PresenceConsecutiveDetections = 0;
    uint32_t max30102PresenceScanWakeStartedMs = 0;
    uint32_t max30102PresenceScanNextWakeMs = 0;
    uint8_t max30102ActiveLedPower = MAX30102_LED_POWER_ACTIVE_DEFAULT;
    uint8_t poorSignalEvalStreak = 0;
    uint8_t stableSignalEvalStreak = 0;
    uint32_t lastLedPowerAdjustMs = 0;

    bool keepAwake = true;
    bool sensorActive = false;
    bool hasEvaluatedWindow = false;

    bool cachedHasHeartRate = false;
    uint32_t cachedHeartRate = 0;
    bool cachedHasSpO2 = false;
    uint32_t cachedSpO2 = 0;
    bool cachedHasDieTempC = false;
    float cachedDieTempC = 0.0f;
    bool cachedFingerPresent = false;
    bool latchedHasHeartRate = false;
    uint32_t latchedHeartRate = 0;
    bool latchedHasSpO2 = false;
    uint32_t latchedSpO2 = 0;
    bool latchedHasDieTempC = false;
    float latchedDieTempC = 0.0f;
    uint32_t lastStableHeartMs = 0;
    uint32_t lastStableSpO2Ms = 0;
    uint32_t lastStableTempMs = 0;
    bool hasHeartEma = false;
    float heartEma = 0.0f;
    bool hasHeartOutputEma = false;
    float heartOutputEma = 0.0f;

    uint32_t hrStabilityWindow[STABILITY_WINDOW_SIZE] = {0};
    uint8_t hrStabilityCount = 0;
    uint8_t hrStabilityIndex = 0;
    uint32_t spo2StabilityWindow[STABILITY_WINDOW_SIZE] = {0};
    uint8_t spo2StabilityCount = 0;
    uint8_t spo2StabilityIndex = 0;

    bool readRegister(TwoWire *bus, uint8_t address, uint8_t reg, uint8_t *value);
    bool writeRegister(TwoWire *bus, uint8_t address, uint8_t reg, uint8_t value);
    bool burstRead(TwoWire *bus, uint8_t address, uint8_t reg, uint8_t *buf, uint8_t len);
    bool initMAX30100(TwoWire *bus, uint8_t address);
    bool readMAX30100Temperature(TwoWire *bus, uint8_t address, float *tempC);
    bool readPartId(TwoWire *bus, uint8_t address, uint8_t *partId);
    uint16_t ingestMAX30102Fifo(TwoWire *bus, uint8_t address);
    uint16_t ingestMAX30100Fifo(TwoWire *bus, uint8_t address);
    void appendSlidingSample(uint32_t ir, uint32_t red);
    void copySlidingWindow(uint32_t *irOut, uint32_t *redOut, uint16_t count);
    void resetSlidingState();
    /** Discard the accumulated sample window WITHOUT disturbing the active epoch anchor or streak. */
    void discardSampleWindow();
    void resetStabilityState();
    bool detectFingerPresence(const uint32_t *ir, const uint32_t *red, uint16_t count) const;
    void pushStabilitySample(uint32_t sample, uint32_t *window, uint8_t *count, uint8_t *index);
    bool isStablePercent(const uint32_t *window, uint8_t count, uint32_t maxSpreadPercent) const;
    bool isStableSpread(const uint32_t *window, uint8_t count, uint32_t maxSpreadAbsolute) const;
    uint32_t medianOfWindow(const uint32_t *window, uint8_t count) const;
    uint32_t trimmedMeanOfWindow(const uint32_t *window, uint8_t count) const;
    bool configureMAX30102Profile(bool presenceMode);
    void setMAX30102LedPower(uint8_t level);
    void maybeAutoAdjustMAX30102LedPower(bool fingerPresent, bool stableHeart, bool hrValueValid);
    bool updateLowPowerPresenceCache();
    bool evaluateSlidingWindow(TwoWire *bus, uint8_t address);
    void clearCachedMetrics();

  protected:
    virtual void setup() override;

  public:
    MAX30102Sensor();
    virtual int32_t runOnce() override;
    virtual void sleep() override;
    virtual uint32_t wakeUp() override;
    virtual bool isActive() override;
    virtual bool canSleep() override;
    virtual bool getMetrics(meshtastic_Telemetry *measurement) override;
    void setStayAwake(bool stayAwake);
    void prepareDeepSleep();
    bool serviceSensor();
    bool getRawWaveformSnapshot(uint32_t *irOut, uint32_t *redOut, uint16_t capacity, uint16_t *countOut);
    /**
     * MAX3010x DIE temperature, for local diagnostic display and future R compensation only.
     *
     * Deliberately NOT routed through getMetrics(): meshtastic_HealthMetrics.temperature is documented as
     * "Body temperature in degrees Celsius" and leaves the badge over LoRa/MQTT, where third-party clients
     * render it as exactly that. A package temperature is not a body temperature. Any UI using this must
     * label it as die temperature.
     */
    bool getDieTemperatureC(float *outC);
    uint8_t getSensitivity() const;
    uint8_t getMaxSensitivity() const;
    void setSensitivity(uint8_t level);
    /** True when finger is detected and sensor is in active HR mode (for UI auto-navigate). */
    bool isHrEngaged() const;
};

#endif
