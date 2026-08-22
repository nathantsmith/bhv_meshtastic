#include "configuration.h"

#if !MESHTASTIC_EXCLUDE_HEALTH_TELEMETRY && __has_include(<MAX30105.h>)

#include "../mesh/generated/meshtastic/telemetry.pb.h"
#include "MAX30102Sensor.h"
#include "PpgSignalQuality.h"
#include "TelemetrySensor.h"
#include "concurrency/LockGuard.h"
#include <math.h>
#include <spo2_algorithm.h>
#include <string.h>

MAX30102Sensor::MAX30102Sensor() : TelemetrySensor(meshtastic_TelemetrySensorType_MAX30102, "MAX3010x") {}

void MAX30102Sensor::cacheRawWaveform(const uint32_t *ir, const uint32_t *red, uint16_t count)
{
    if (!ir || !red || count == 0) {
        return;
    }

    if (count > MAX30102_BUFFER_LEN) {
        count = MAX30102_BUFFER_LEN;
    }

    concurrency::LockGuard g(&waveformLock);
    memcpy(lastIrWaveform, ir, count * sizeof(uint32_t));
    memcpy(lastRedWaveform, red, count * sizeof(uint32_t));
    lastWaveformCount = count;
}

void MAX30102Sensor::clearRawWaveformCache()
{
    concurrency::LockGuard g(&waveformLock);
    memset(lastIrWaveform, 0, sizeof(lastIrWaveform));
    memset(lastRedWaveform, 0, sizeof(lastRedWaveform));
    lastWaveformCount = 0;
}

bool MAX30102Sensor::getRawWaveformSnapshot(uint32_t *irOut, uint32_t *redOut, uint16_t capacity, uint16_t *countOut)
{
    if (!irOut || !redOut || !countOut || capacity == 0) {
        return false;
    }

    concurrency::LockGuard g(&waveformLock);
    if (lastWaveformCount == 0) {
        return false;
    }

    uint16_t copyCount = capacity < lastWaveformCount ? capacity : lastWaveformCount;
    memcpy(irOut, lastIrWaveform, copyCount * sizeof(uint32_t));
    memcpy(redOut, lastRedWaveform, copyCount * sizeof(uint32_t));
    *countOut = copyCount;
    return true;
}

bool MAX30102Sensor::getDieTemperatureC(float *outC)
{
    if (!outC) {
        return false;
    }

    concurrency::LockGuard g(&metricsLock);
    if (!cachedHasDieTempC) {
        return false;
    }
    *outC = cachedDieTempC;
    return true;
}

void MAX30102Sensor::clearCachedMetrics()
{
    concurrency::LockGuard g(&metricsLock);
    hasEvaluatedWindow = false;
    cachedHasHeartRate = false;
    cachedHeartRate = 0;
    cachedHasSpO2 = false;
    cachedSpO2 = 0;
    cachedHasDieTempC = false;
    cachedDieTempC = 0.0f;
    cachedFingerPresent = false;
    latchedHasHeartRate = false;
    latchedHeartRate = 0;
    latchedHasSpO2 = false;
    latchedSpO2 = 0;
    latchedHasDieTempC = false;
    latchedDieTempC = 0.0f;
    lastStableHeartMs = 0;
    lastStableSpO2Ms = 0;
    lastStableTempMs = 0;
    hasHeartEma = false;
    heartEma = 0.0f;
    hasHeartOutputEma = false;
    heartOutputEma = 0.0f;
}

void MAX30102Sensor::resetSlidingState()
{
    memset(slidingIrWindow, 0, sizeof(slidingIrWindow));
    memset(slidingRedWindow, 0, sizeof(slidingRedWindow));
    slidingWriteIndex = 0;
    slidingSampleCount = 0;
    slidingNewSamplesSinceEval = 0;
    lastEvalMs = 0;
    max30100RawSampleCount = 0;
    activeEpochStartMeanIr = 0;
    epochAnchorCount = 0;
    noFingerEvalStreak = 0;
    alcOverflowEvents = 0;
}

void MAX30102Sensor::resetStabilityState()
{
    memset(hrStabilityWindow, 0, sizeof(hrStabilityWindow));
    memset(spo2StabilityWindow, 0, sizeof(spo2StabilityWindow));
    hrStabilityCount = 0;
    hrStabilityIndex = 0;
    spo2StabilityCount = 0;
    spo2StabilityIndex = 0;
}

void MAX30102Sensor::appendSlidingSample(uint32_t ir, uint32_t red)
{
    slidingIrWindow[slidingWriteIndex] = ir;
    slidingRedWindow[slidingWriteIndex] = red;
    slidingWriteIndex = (slidingWriteIndex + 1) % MAX30102_BUFFER_LEN;
    if (slidingSampleCount < MAX30102_BUFFER_LEN) {
        slidingSampleCount++;
    }
    slidingNewSamplesSinceEval++;
}

void MAX30102Sensor::copySlidingWindow(uint32_t *irOut, uint32_t *redOut, uint16_t count)
{
    if (!irOut || !redOut || count == 0 || count > MAX30102_BUFFER_LEN || slidingSampleCount < count) {
        return;
    }

    const uint16_t startIndex = (slidingWriteIndex + MAX30102_BUFFER_LEN - count) % MAX30102_BUFFER_LEN;
    for (uint16_t i = 0; i < count; ++i) {
        const uint16_t srcIndex = (startIndex + i) % MAX30102_BUFFER_LEN;
        irOut[i] = slidingIrWindow[srcIndex];
        redOut[i] = slidingRedWindow[srcIndex];
    }
}

bool MAX30102Sensor::configureMAX30102Profile(bool presenceMode)
{
    if (chipType != PulseOxChipType::MAX30102) {
        return false;
    }

    const byte powerLevel = presenceMode ? MAX30102_LED_POWER_PRESENCE : max30102ActiveLedPower;
    const byte sampleAverage = presenceMode ? 1 : 4; // Presence mode needs immediate samples after wake
    const byte leds = 2;                              // Red + IR slots (presence mode disables RED amplitude)
    const byte sampleRate = presenceMode ? 100 : 100;
    const int pulseWidth = presenceMode ? 118 : 411;
    const int adcRange = presenceMode ? 2048 : 4096;

    max30102.setup(powerLevel, sampleAverage, leds, sampleRate, pulseWidth, adcRange);
    if (presenceMode) {
        // Presence polling runs IR-only to minimize visible brightness and power.
        max30102.setPulseAmplitudeRed(0x00);
        max30102.setPulseAmplitudeIR(powerLevel);
    } else {
        max30102.setPulseAmplitudeRed(powerLevel);
        max30102.setPulseAmplitudeIR(powerLevel);
    }
    max30102.setPulseAmplitudeProximity(powerLevel);
    max30102.enableDIETEMPRDY();
    max30102.clearFIFO();
    LOG_INFO("MAX30102 profile: %s (LED=0x%02X, rate=%uHz)", presenceMode ? "presence" : "active", powerLevel, sampleRate);

    max30102PresenceMode = presenceMode;
    poorSignalEvalStreak = 0;
    stableSignalEvalStreak = 0;

    resetSlidingState();
    resetStabilityState();
    clearRawWaveformCache();
    clearCachedMetrics();
    return true;
}

void MAX30102Sensor::setMAX30102LedPower(uint8_t level)
{
    if (level > MAX30102_LED_POWER_ACTIVE_MAX) {
        level = MAX30102_LED_POWER_ACTIVE_MAX;
    }

    if (level == max30102ActiveLedPower) {
        return;
    }

    max30102ActiveLedPower = level;
    LOG_INFO("MAX30102 active LED power set to 0x%02X", level);
    if (chipType != PulseOxChipType::MAX30102 || !sensorActive || max30102PresenceMode) {
        return;
    }

    max30102.setPulseAmplitudeRed(level);
    max30102.setPulseAmplitudeIR(level);
    max30102.setPulseAmplitudeProximity(level);
}

void MAX30102Sensor::maybeAutoAdjustMAX30102LedPower(bool fingerPresent, bool stableHeart, bool hrValueValid)
{
    // Runtime LED auto-adjust is intentionally disabled. Active mode stays fixed at
    // MAX30102_LED_POWER_ACTIVE_DEFAULT unless changed explicitly via setMAX30102LedPower().
    (void)fingerPresent;
    (void)stableHeart;
    (void)hrValueValid;
    poorSignalEvalStreak = 0;
    stableSignalEvalStreak = 0;
    lastLedPowerAdjustMs = 0;
}

bool MAX30102Sensor::updateLowPowerPresenceCache()
{
    const uint16_t recentCount = slidingSampleCount < 12 ? slidingSampleCount : 12;
    uint32_t meanIr = 0;
    uint32_t meanRed = 0;
    uint32_t maxIr = 0;
    uint32_t maxRed = 0;
    bool dcPresent = false;
    bool peakPresent = false;
    bool rawPresent = false;
    bool present = false;
    if (recentCount >= MAX3010X_PRESENCE_MIN_SAMPLES) {
        uint64_t sumIr = 0;
        uint64_t sumRed = 0;
        const uint16_t startIndex = (uint16_t)((slidingWriteIndex + MAX30102_BUFFER_LEN - recentCount) % MAX30102_BUFFER_LEN);
        for (uint16_t i = 0; i < recentCount; ++i) {
            const uint16_t idx = (uint16_t)((startIndex + i) % MAX30102_BUFFER_LEN);
            const uint32_t ir = slidingIrWindow[idx];
            const uint32_t red = slidingRedWindow[idx];
            sumIr += ir;
            sumRed += red;
            if (ir > maxIr) {
                maxIr = ir;
            }
            if (red > maxRed) {
                maxRed = red;
            }
        }
        meanIr = (uint32_t)(sumIr / recentCount);
        meanRed = (uint32_t)(sumRed / recentCount);
        // Presence polling uses IR-only LED power, so detection is based on IR levels only.
        dcPresent = (meanIr >= MAX3010X_PRESENCE_IR_DC_MIN_IR_ONLY);
        peakPresent = (maxIr >= MAX3010X_PRESENCE_IR_PEAK_MIN_IR_ONLY);
        rawPresent = dcPresent && peakPresent;
    }

    if (rawPresent) {
        if (max30102PresenceConsecutiveDetections < MAX3010X_PRESENCE_CONSECUTIVE_REQUIRED) {
            max30102PresenceConsecutiveDetections++;
        }
    } else {
        max30102PresenceConsecutiveDetections = 0;
    }
    present = rawPresent && (max30102PresenceConsecutiveDetections >= MAX3010X_PRESENCE_CONSECUTIVE_REQUIRED);

    const uint32_t nowMs = millis();
    // Only log once the detection block above actually ran, otherwise every line reports hard-coded zeros.
    // The scan cycle is WAKE_WINDOW (700 ms) + SCAN_INTERVAL (1000 ms) = 1700 ms while the rate limit is
    // 1000 ms, and sleep() zeroes the timestamp - so exactly one tick per wake was admitted and it was
    // always the first, taken right after resetSlidingState() when slidingSampleCount == 1. That is below
    // MAX3010X_PRESENCE_MIN_SAMPLES, so meanIr/maxIr were never computed. Every presence line ever logged
    // by this firmware (158/158 across a 5-minute capture) read "mean_ir=0 max_ir=0 n=1", which means the
    // presence thresholds have never been observable, let alone validated, from these logs.
    if ((recentCount >= MAX3010X_PRESENCE_MIN_SAMPLES) &&
        ((max30102PresenceStatsLastLogMs == 0) ||
         ((uint32_t)(nowMs - max30102PresenceStatsLastLogMs) >= MAX30102_PRESENCE_STATS_LOG_INTERVAL_MS))) {
        max30102PresenceStatsLastLogMs = nowMs;
        LOG_INFO("MAX30102 presence stats(low): present=%d raw=%d mean_ir=%u mean_red=%u max_ir=%u max_red=%u dc=%d peak=%d n=%u c=%u",
                 present, rawPresent, meanIr, meanRed, maxIr, maxRed, dcPresent, peakPresent, recentCount,
                 max30102PresenceConsecutiveDetections);
    }

    if (present != max30102PresenceState) {
        LOG_INFO("MAX30102 presence=%d (mean_ir=%u mean_red=%u max_ir=%u max_red=%u count=%u)", present, meanIr, meanRed, maxIr,
                 maxRed, recentCount);
    }
    max30102PresenceState = present;

    concurrency::LockGuard g(&metricsLock);
    hasEvaluatedWindow = true;
    cachedFingerPresent = present;
    cachedHasHeartRate = false;
    cachedHeartRate = 0;
    cachedHasSpO2 = false;
    cachedSpO2 = 0;
    cachedHasDieTempC = false;
    cachedDieTempC = 0.0f;
    latchedHasHeartRate = false;
    latchedHeartRate = 0;
    latchedHasSpO2 = false;
    latchedSpO2 = 0;
    latchedHasDieTempC = false;
    latchedDieTempC = 0.0f;
    hasHeartEma = false;
    heartEma = 0.0f;
    hasHeartOutputEma = false;
    heartOutputEma = 0.0f;
    return present;
}

bool MAX30102Sensor::readRegister(TwoWire *bus, uint8_t address, uint8_t reg, uint8_t *value)
{
    bus->beginTransmission(address);
    bus->write(reg);
    if (bus->endTransmission(false) != 0) {
        return false;
    }
    if (bus->requestFrom((int)address, 1) != 1) {
        return false;
    }
    *value = bus->read();
    return true;
}

bool MAX30102Sensor::writeRegister(TwoWire *bus, uint8_t address, uint8_t reg, uint8_t value)
{
    bus->beginTransmission(address);
    bus->write(reg);
    bus->write(value);
    return bus->endTransmission() == 0;
}

bool MAX30102Sensor::burstRead(TwoWire *bus, uint8_t address, uint8_t reg, uint8_t *buf, uint8_t len)
{
    bus->beginTransmission(address);
    bus->write(reg);
    if (bus->endTransmission(false) != 0) {
        return false;
    }
    if (bus->requestFrom((int)address, (int)len) != len) {
        return false;
    }
    for (uint8_t i = 0; i < len; i++) {
        buf[i] = bus->read();
    }
    return true;
}

bool MAX30102Sensor::readPartId(TwoWire *bus, uint8_t address, uint8_t *partId)
{
    return readRegister(bus, address, MAX30100_REG_PART_ID, partId);
}

bool MAX30102Sensor::initMAX30100(TwoWire *bus, uint8_t address)
{
    // Reset first to guarantee known state.
    if (!writeRegister(bus, address, MAX30100_REG_MODE_CONFIG, MAX30100_MODE_RESET)) {
        return false;
    }
    delay(20);

    // Set SPO2 mode (Red + IR) and keep effective algorithm input at 25Hz (via decimation below).
    // The Maxim SPO2/HR routine used here is tuned for 25Hz input windows.
    if (!writeRegister(bus, address, MAX30100_REG_MODE_CONFIG, MAX30100_MODE_HR_SPO2)) {
        return false;
    }
    if (!writeRegister(bus, address, MAX30100_REG_SPO2_CONFIG,
                       MAX30100_SPO2_HI_RES_EN | MAX30100_SPO2_SR_50HZ | MAX30100_SPO2_PW_1600US_16BIT)) {
        return false;
    }

    // Increased LED drive to improve PPG amplitude for MAX30100 modules.
    if (!writeRegister(bus, address, MAX30100_REG_LED_CONFIG, MAX30100_LED_IR_40MA_RED_40MA)) {
        return false;
    }

    // Clear FIFO pointers/counters.
    if (!writeRegister(bus, address, MAX30100_REG_FIFO_WRITE_POINTER, 0x00)) {
        return false;
    }
    if (!writeRegister(bus, address, MAX30100_REG_FIFO_OVERFLOW_COUNTER, 0x00)) {
        return false;
    }
    if (!writeRegister(bus, address, MAX30100_REG_FIFO_READ_POINTER, 0x00)) {
        return false;
    }

    // Clear any stale interrupt status.
    uint8_t ignored;
    readRegister(bus, address, MAX30100_REG_INTERRUPT_STATUS, &ignored);
    return true;
}

bool MAX30102Sensor::readMAX30100Temperature(TwoWire *bus, uint8_t address, float *tempC)
{
    uint8_t modeReg = 0;
    if (!readRegister(bus, address, MAX30100_REG_MODE_CONFIG, &modeReg)) {
        return false;
    }

    if (!writeRegister(bus, address, MAX30100_REG_MODE_CONFIG, modeReg | MAX30100_MODE_TEMP_EN)) {
        return false;
    }

    uint8_t statusReg = 0;
    uint32_t deadline = millis() + 250;
    while (millis() < deadline) {
        if (!readRegister(bus, address, MAX30100_REG_INTERRUPT_STATUS, &statusReg)) {
            return false;
        }
        if (statusReg & MAX30100_INT_TEMP_RDY) {
            break;
        }
        delay(5);
    }

    if (!(statusReg & MAX30100_INT_TEMP_RDY)) {
        return false;
    }

    uint8_t integerPart = 0;
    uint8_t fractionPart = 0;
    if (!readRegister(bus, address, MAX30100_REG_TEMP_INTEGER, &integerPart) ||
        !readRegister(bus, address, MAX30100_REG_TEMP_FRACTION, &fractionPart)) {
        return false;
    }

    *tempC = (int8_t)integerPart + ((fractionPart & 0x0F) * 0.0625f);
    return true;
}

void MAX30102Sensor::discardSampleWindow()
{
    // Deliberately narrower than resetSlidingState(): the accumulated PPG is unusable, but the active
    // epoch's DC anchor and no-finger streak describe the CONTACT, which has not changed. Clearing those
    // too would restart the epoch on every transient and churn the session.
    memset(slidingIrWindow, 0, sizeof(slidingIrWindow));
    memset(slidingRedWindow, 0, sizeof(slidingRedWindow));
    slidingWriteIndex = 0;
    slidingSampleCount = 0;
    slidingNewSamplesSinceEval = 0;
}

uint16_t MAX30102Sensor::ingestMAX30102Fifo(TwoWire *bus, uint8_t address)
{
    if (!bus || address == 0) {
        return 0;
    }

    // Sample-stream integrity, checked BEFORE draining.
    //
    // Everything downstream assumes samples are uniformly spaced at the effective 25 Hz: the vendor kernel
    // hard-codes that rate, and the autocorrelation converts lag to BPM with it. If the part dropped
    // samples, the time axis is wrong and every rate derived from the window is wrong with it - silently,
    // and in a way that looks like a plausible heart rate rather than an error.
    //
    // OVF_COUNTER must be read first because the part clears it as soon as a complete sample is popped.
    // It also disambiguates the pointer aliasing: a completely full FIFO has wr == rd, identical to empty,
    // so `available` alone cannot tell "nothing new" from "you lost everything".
    //
    // Scoped to ACTIVE mode on purpose. Presence scanning runs at 100 sps with no averaging, giving only
    // ~320 ms of FIFO, and it re-clears the buffer every wake anyway - so overflow there is expected,
    // harmless, and invalidating on it would break presence detection entirely.
    //
    // MEASURED, AND THE REASON THIS ONLY OBSERVES: on real hardware OVF_COUNTER is non-zero on virtually
    // every service call - 4.8 events/s against a 5/s service cadence, 5.3 lost samples per event, and
    // 25.5 lost samples/s against a 25 Hz production rate. Discarding the accumulated window on that flag
    // (which is what the design review recommended, unqualified) cleared the ring buffer every 200 ms so
    // it never reached the 100 samples an evaluation needs: measurement stopped completely, hasWindow
    // stayed 0, and the badge produced nothing at all.
    //
    // So the flag is real and continuous, not occasional. Until we understand WHY - whether the part is
    // producing faster than the configured 25 Hz, whether the drain loop's
    // available = (wr - rd) & 0x1F aliases a full FIFO to empty and stalls, or whether the counter simply
    // is not cleared the way the datasheet describes - it cannot be used as an invalidation trigger.
    // Counting it is genuinely new information; acting on it destroys the feature.
    if (!max30102PresenceMode) {
        // NOTE: register 0x05 is NOT read as an overflow counter here, and an earlier revision was wrong
        // to do so. Instrumented on hardware over 465 service calls, its value equalled the available
        // sample count (wr - rd) on 98.5% of reads, while FIFO occupancy never exceeded 6 of 32 - a fill
        // level at which overflow is impossible. Whatever this module returns there, it is not lost
        // samples, and treating it as such produced a "continuous overflow" alarm that was pure artefact.
        // The drain keeps up comfortably: ~5-6 samples per 200 ms service call against 32 of capacity.
        uint8_t intStatus = 0;
        if (readRegister(bus, address, MAX3010X_REG_INT_STATUS_1, &intStatus) && (intStatus & MAX3010X_INT_ALC_OVF)) {
            // Ambient-light cancellation saturated: this window's PPG reflects the ALC railing rather than
            // tissue. Reading the register clears the latched flag.
            alcOverflowEvents++;
        }

#ifdef BHV_PPG_DIAG
        // Log the raw FIFO pointers alongside the overflow counter. The open question is WHY
        // OVF_COUNTER is non-zero on essentially every service call while heart rate stays stable:
        // is the part outrunning the drain, or does available=(wr-rd)&0x1F alias a full FIFO to
        // empty and stall? Those look identical from the counter alone but differ completely here.
        {
            uint8_t wp = 0, rp = 0, ov2 = 0;
            readRegister(bus, address, MAX3010X_REG_FIFO_WRITE_POINTER, &wp);
            readRegister(bus, address, MAX3010X_REG_FIFO_READ_POINTER, &rp);
            readRegister(bus, address, MAX3010X_REG_OVF_COUNTER, &ov2);
            LOG_INFO("FIFODIAG wr=%u rd=%u avail=%u reg05=%u slid=%u", wp, rp,
                     (unsigned)((wp - rp) & MAX3010X_FIFO_POINTER_MASK), ov2, slidingSampleCount);
        }
#endif

        const uint32_t nowMs = millis();
        if (alcOverflowEvents && ((lastIntegrityLogMs == 0) || ((uint32_t)(nowMs - lastIntegrityLogMs) >= 5000))) {
            lastIntegrityLogMs = nowMs;
            LOG_WARN("MAX30102 integrity: alc_ovf_events=%u", alcOverflowEvents);
        }
    }

    uint16_t ingested = 0;

    // Drain in bounded passes so we do not monopolize the scheduler.
    for (uint8_t pass = 0; pass < 4; ++pass) {
        uint8_t writePointer = 0;
        uint8_t readPointer = 0;
        if (!readRegister(bus, address, MAX3010X_REG_FIFO_WRITE_POINTER, &writePointer) ||
            !readRegister(bus, address, MAX3010X_REG_FIFO_READ_POINTER, &readPointer)) {
            return ingested;
        }

        uint8_t available = (uint8_t)((writePointer - readPointer) & MAX3010X_FIFO_POINTER_MASK);
        if (available == 0) {
            break;
        }

        while (available > 0) {
            uint8_t samplesThisBurst = available;
            if (samplesThisBurst > MAX30102_MAX_SAMPLES_PER_BURST) {
                samplesThisBurst = MAX30102_MAX_SAMPLES_PER_BURST;
            }

            uint8_t raw[MAX30102_MAX_SAMPLES_PER_BURST * 6];
            const uint8_t bytesToRead = (uint8_t)(samplesThisBurst * 6);
            if (!burstRead(bus, address, MAX3010X_REG_FIFO_DATA, raw, bytesToRead)) {
                return ingested;
            }

            for (uint8_t i = 0; i < samplesThisBurst; ++i) {
                const uint8_t base = (uint8_t)(i * 6);
                uint32_t red = ((uint32_t)raw[base] << 16) | ((uint32_t)raw[base + 1] << 8) | raw[base + 2];
                uint32_t ir = ((uint32_t)raw[base + 3] << 16) | ((uint32_t)raw[base + 4] << 8) | raw[base + 5];
                red &= 0x3FFFF;
                ir &= 0x3FFFF;
                appendSlidingSample(ir, red);
                ingested++;
            }

            available = (uint8_t)(available - samplesThisBurst);
        }
    }

    return ingested;
}

uint16_t MAX30102Sensor::ingestMAX30100Fifo(TwoWire *bus, uint8_t address)
{
    uint16_t ingested = 0;

    // Read in short bursts to avoid starving the scheduler.
    for (uint8_t burst = 0; burst < 8; ++burst) {
        uint8_t wr = 0;
        uint8_t rd = 0;
        if (!readRegister(bus, address, MAX30100_REG_FIFO_WRITE_POINTER, &wr) ||
            !readRegister(bus, address, MAX30100_REG_FIFO_READ_POINTER, &rd)) {
            return ingested;
        }

        uint8_t available = (uint8_t)((wr - rd) & 0x0F);
        if (available == 0) {
            break;
        }

        uint8_t raw[4 * 16];
        if (!burstRead(bus, address, MAX30100_REG_FIFO_DATA, raw, (uint8_t)(available * 4))) {
            return ingested;
        }

        for (uint8_t i = 0; i < available; i++) {
            uint16_t ir = (uint16_t)((raw[i * 4] << 8) | raw[i * 4 + 1]);
            uint16_t red = (uint16_t)((raw[i * 4 + 2] << 8) | raw[i * 4 + 3]);

            if ((max30100RawSampleCount % MAX30100_RAW_TO_ALGO_DECIMATION) == 0) {
                appendSlidingSample(ir, red);
                ingested++;
            }
            max30100RawSampleCount++;
        }
    }

    return ingested;
}

void MAX30102Sensor::pushStabilitySample(uint32_t sample, uint32_t *window, uint8_t *count, uint8_t *index)
{
    if (!window || !count || !index) {
        return;
    }
    window[*index] = sample;
    *index = (uint8_t)((*index + 1) % STABILITY_WINDOW_SIZE);
    if (*count < STABILITY_WINDOW_SIZE) {
        (*count)++;
    }
}

bool MAX30102Sensor::isStablePercent(const uint32_t *window, uint8_t count, uint32_t maxSpreadPercent) const
{
    if (!window || count < STABILITY_MIN_COUNT) {
        return false;
    }

    uint32_t minValue = window[0];
    uint32_t maxValue = window[0];
    for (uint8_t i = 1; i < count; ++i) {
        if (window[i] < minValue) {
            minValue = window[i];
        }
        if (window[i] > maxValue) {
            maxValue = window[i];
        }
    }

    if (minValue == 0) {
        return false;
    }

    return ((uint64_t)maxValue * 100ULL) <= ((uint64_t)minValue * (100ULL + maxSpreadPercent));
}

bool MAX30102Sensor::isStableSpread(const uint32_t *window, uint8_t count, uint32_t maxSpreadAbsolute) const
{
    if (!window || count < STABILITY_MIN_COUNT) {
        return false;
    }

    uint32_t minValue = window[0];
    uint32_t maxValue = window[0];
    for (uint8_t i = 1; i < count; ++i) {
        if (window[i] < minValue) {
            minValue = window[i];
        }
        if (window[i] > maxValue) {
            maxValue = window[i];
        }
    }

    return (maxValue - minValue) <= maxSpreadAbsolute;
}

uint32_t MAX30102Sensor::medianOfWindow(const uint32_t *window, uint8_t count) const
{
    if (!window || count == 0) {
        return 0;
    }

    if (count > STABILITY_WINDOW_SIZE) {
        count = STABILITY_WINDOW_SIZE;
    }

    uint32_t sorted[STABILITY_WINDOW_SIZE];
    for (uint8_t i = 0; i < count; ++i) {
        sorted[i] = window[i];
    }

    // Small fixed-size insertion sort.
    for (uint8_t i = 1; i < count; ++i) {
        uint32_t key = sorted[i];
        int8_t j = (int8_t)i - 1;
        while (j >= 0 && sorted[(uint8_t)j] > key) {
            sorted[(uint8_t)(j + 1)] = sorted[(uint8_t)j];
            --j;
        }
        sorted[(uint8_t)(j + 1)] = key;
    }

    if ((count & 1U) != 0U) {
        return sorted[count / 2];
    }

    uint8_t hi = count / 2;
    uint8_t lo = (uint8_t)(hi - 1);
    return (uint32_t)(((uint64_t)sorted[lo] + (uint64_t)sorted[hi]) / 2ULL);
}

uint32_t MAX30102Sensor::trimmedMeanOfWindow(const uint32_t *window, uint8_t count) const
{
    if (!window || count == 0) {
        return 0;
    }

    if (count > STABILITY_WINDOW_SIZE) {
        count = STABILITY_WINDOW_SIZE;
    }

    uint32_t sorted[STABILITY_WINDOW_SIZE];
    for (uint8_t i = 0; i < count; ++i) {
        sorted[i] = window[i];
    }

    for (uint8_t i = 1; i < count; ++i) {
        uint32_t key = sorted[i];
        int8_t j = (int8_t)i - 1;
        while (j >= 0 && sorted[(uint8_t)j] > key) {
            sorted[(uint8_t)(j + 1)] = sorted[(uint8_t)j];
            --j;
        }
        sorted[(uint8_t)(j + 1)] = key;
    }

    uint8_t start = 0;
    uint8_t end = count;
    // For a full stability window, drop one low and one high outlier.
    if (count >= 5) {
        start = 1;
        end = (uint8_t)(count - 1);
    }

    uint64_t sum = 0;
    uint8_t n = 0;
    for (uint8_t i = start; i < end; ++i) {
        sum += sorted[i];
        ++n;
    }
    if (n == 0) {
        return sorted[count / 2];
    }

    return (uint32_t)((sum + (n / 2)) / n);
}

bool MAX30102Sensor::computePeriodicity(const uint32_t *ir, uint16_t count, float *bestRhoOut, uint16_t *bestLagOut) const
{
    // Implementation lives in PpgSignalQuality.h so the native test build can exercise the exact code the
    // firmware runs; this file cannot be compiled on the host because the SparkFun library is an
    // Arduino-targets-only dependency.
    const ppg::Periodicity r =
        ppg::bestPeriodicity(ir, count, AUTOCORR_MIN_LAG, AUTOCORR_MAX_LAG, AUTOCORR_MIN_OVERLAP, AUTOCORR_HARMONIC_TOLERANCE);
    if (bestRhoOut) {
        *bestRhoOut = r.bestRho;
    }
    if (bestLagOut) {
        *bestLagOut = r.bestLag;
    }
    return r.found;
}

bool MAX30102Sensor::detectFingerPresence(const uint32_t *ir, const uint32_t *red, uint16_t count) const
{
    if (!ir || !red || count == 0) {
        return false;
    }

    uint32_t minIr = ir[0];
    uint32_t maxIr = ir[0];
    uint32_t minRed = red[0];
    uint32_t maxRed = red[0];
    uint64_t sumIr = 0;
    uint64_t sumRed = 0;

    for (uint16_t i = 0; i < count; ++i) {
        uint32_t irValue = ir[i];
        uint32_t redValue = red[i];
        if (irValue < minIr) {
            minIr = irValue;
        }
        if (irValue > maxIr) {
            maxIr = irValue;
        }
        if (redValue < minRed) {
            minRed = redValue;
        }
        if (redValue > maxRed) {
            maxRed = redValue;
        }
        sumIr += irValue;
        sumRed += redValue;
    }

    uint32_t meanIr = (uint32_t)(sumIr / count);
    uint32_t meanRed = (uint32_t)(sumRed / count);
    uint32_t spanIr = maxIr - minIr;
    uint32_t spanRed = maxRed - minRed;

    const bool dcLevelOk = (meanIr >= MAX3010X_FINGER_IR_DC_MIN) && (meanRed >= MAX3010X_FINGER_RED_DC_MIN);
    const bool acLevelOk = (spanIr >= MAX3010X_FINGER_IR_AC_MIN) && (spanRed >= MAX3010X_FINGER_RED_AC_MIN);
    const bool pulsatilityOk =
        (meanIr > 0 && ((uint64_t)spanIr * 1000ULL) >= ((uint64_t)meanIr * MAX3010X_FINGER_PULSATILITY_PERMILLE)) ||
        (meanRed > 0 && ((uint64_t)spanRed * 1000ULL) >= ((uint64_t)meanRed * MAX3010X_FINGER_PULSATILITY_PERMILLE));

    // Presence detection answers "is something on the sensor?", NOT "is this a pulse?".
    //
    // An earlier revision required pulsatility here (dcLevelOk && acLevelOk && pulsatilityOk) to stop
    // noise being accepted as a finger. Measured on hardware, that rejected a real finger: a wearer at
    // 193278 IR DC with 801 counts of AC sits at 0.414% pulsatility against the 0.4% threshold, and the
    // red channel at ~0.28% is below it outright. Windows dipped under the bar intermittently, each dip
    // incremented the no-finger streak, and the session was killed and restarted every ~6 s - seven times
    // in a 45 s capture, with a heart rate never once reaching the display. Strictly worse than the bug
    // it was meant to fix.
    //
    // The noise-rejection job belongs to the periodicity gate instead, which does it far better: measured
    // best-autocorrelation is 0.97-0.99 on a real finger versus 0.227 on pure noise, against thresholds of
    // 0.40 (HR) and 0.50 (SpO2). Noise now passes presence detection and is then refused downstream, which
    // is the correct division of responsibility - a permissive "something is here" followed by a strict
    // "and it repeats like a heartbeat".
    //
    // Pulsatility is therefore back to being one of two ways to satisfy the AC requirement, as originally
    // written.
    //
    // acLevelOk is an absolute span in ADC counts (80 IR / 40 RED). Sensor noise alone clears it easily:
    // over a 100-sample window, Gaussian noise of sigma s has a peak-to-peak span of roughly 5s, so
    // s >= 30 LSB already exceeds 80 counts regardless of whether anything is touching the sensor. With
    // the old `acLevelOk || pulsatilityOk`, that absolute test short-circuited the ratio test entirely,
    // so a flat DC level plus noise - no cardiac component whatsoever - was accepted as a finger and fed
    // to the kernel, which duly produced heart rates and SpO2 values. Replaying the shipping decision
    // path on pure noise reproduced this: at sigma 80 on a 90000 DC, 100% of windows passed finger
    // detection and roughly half of all evaluations put a fabricated number on the OLED.
    //
    // MAX3010X_FINGER_PULSATILITY_PERMILLE (0.4%) was already defined and already correct; it was simply
    // bypassed. Requiring it narrows the fabrication band substantially - it removes the whole low-noise
    // region, where apparent perfusion is 0.17-0.22% - but it does NOT close it completely, because at
    // large noise amplitudes the apparent perfusion index also rises above 0.4%. Rejecting aperiodic
    // noise outright needs the periodicity gate, which is a separate change; this is the cheap half that
    // uses a constant the code already has.
    return dcLevelOk && (acLevelOk || pulsatilityOk);
}

bool MAX30102Sensor::evaluateSlidingWindow(TwoWire *bus, uint8_t address)
{
    if (slidingSampleCount < MAX30102_BUFFER_LEN) {
        return false;
    }

    uint32_t irWindow[MAX30102_BUFFER_LEN];
    uint32_t redWindow[MAX30102_BUFFER_LEN];
    copySlidingWindow(irWindow, redWindow, MAX30102_BUFFER_LEN);

    uint64_t sumIr = 0;
    uint64_t sumRed = 0;
    uint32_t maxIr = 0;
    uint32_t maxRed = 0;
    uint32_t minIr = 0xFFFFFFFFu;
    uint32_t minRed = 0xFFFFFFFFu;
    for (uint16_t i = 0; i < MAX30102_BUFFER_LEN; ++i) {
        const uint32_t ir = irWindow[i];
        const uint32_t red = redWindow[i];
        sumIr += ir;
        sumRed += red;
        if (ir > maxIr) {
            maxIr = ir;
        }
        if (red > maxRed) {
            maxRed = red;
        }
        if (ir < minIr) {
            minIr = ir;
        }
        if (red < minRed) {
            minRed = red;
        }
    }
    const uint32_t meanIr = (uint32_t)(sumIr / MAX30102_BUFFER_LEN);
    const uint32_t meanRed = (uint32_t)(sumRed / MAX30102_BUFFER_LEN);
    lastEvalMeanIr = meanIr;
    lastEvalMeanRed = meanRed;

    const bool fingerPresent = detectFingerPresence(irWindow, redWindow, MAX30102_BUFFER_LEN);
#ifdef BHV_PPG_DIAG
    // Unconditional: the normal "SpO2 input" line is downstream of the finger gate, so with nobody
    // present the LED-drive sweep would log nothing at all.
    //
    // Also reports the window standard deviation in milli-counts. With no finger there is no cardiac
    // component, so that IS the noise floor - which makes the datasheet noise budget (shot + quantisation)
    // directly falsifiable rather than merely plausible, and lets the sqrt(I) shot-noise scaling be
    // checked across the LED drive sweep.
    {
        double sIr = 0.0, sRed = 0.0;
        for (uint16_t i = 0; i < MAX30102_BUFFER_LEN; ++i) {
            const double di = (double)irWindow[i] - (double)meanIr;
            const double dr = (double)redWindow[i] - (double)meanRed;
            sIr += di * di;
            sRed += dr * dr;
        }
        const uint32_t sdIrmc = (uint32_t)(sqrt(sIr / MAX30102_BUFFER_LEN) * 1000.0);
        const uint32_t sdRedmc = (uint32_t)(sqrt(sRed / MAX30102_BUFFER_LEN) * 1000.0);
        LOG_INFO("DCDIAG drive=0x%02X mean_ir=%u mean_red=%u max_ir=%u sd_ir_mc=%u sd_red_mc=%u finger=%d",
                 max30102ActiveLedPower, meanIr, meanRed, maxIr, sdIrmc, sdRedmc, fingerPresent ? 1 : 0);
    }
#endif

    // Anchor this epoch's reference DC on the first evaluation that actually sees a finger, then judge
    // "the finger has left" relative to that anchor rather than against a fixed count. Optical coupling
    // varies by more than an order of magnitude across wearers, so an absolute bar silently excludes
    // whole populations (see MAX3010X_POWERDOWN_DC_FRACTION_PERCENT).
    if (fingerPresent && epochAnchorCount < MAX3010X_EPOCH_ANCHOR_SAMPLES) {
        epochAnchorSamples[epochAnchorCount++] = meanIr;
        // Re-derive from the median of what we have so far, so the anchor self-corrects as the contact
        // settles instead of being fixed by an initial hard press.
        activeEpochStartMeanIr = medianOfWindow(epochAnchorSamples, epochAnchorCount);
        LOG_INFO("MAX30102 active epoch anchor: n=%u mean_ir=%u anchor=%u (collapse below %u)", epochAnchorCount, meanIr,
                 activeEpochStartMeanIr,
                 (uint32_t)((uint64_t)activeEpochStartMeanIr * MAX3010X_POWERDOWN_DC_FRACTION_PERCENT / 100ULL));
    }
    const bool signalCollapsed =
        (activeEpochStartMeanIr != 0) &&
        (meanIr < (uint32_t)((uint64_t)activeEpochStartMeanIr * MAX3010X_POWERDOWN_DC_FRACTION_PERCENT / 100ULL));

    // A single bad window must not end a session; require a streak before giving up.
    const bool measurementViable = fingerPresent && !signalCollapsed;
    if (measurementViable) {
        noFingerEvalStreak = 0;
    } else if (noFingerEvalStreak < MAX3010X_NO_FINGER_EVAL_STREAK_FOR_SLEEP) {
        noFingerEvalStreak++;
    }
    const bool giveUpOnEpoch = !measurementViable && (noFingerEvalStreak >= MAX3010X_NO_FINGER_EVAL_STREAK_FOR_SLEEP);
    const uint32_t nowMs = millis();
    if ((max30102PresenceStatsLastLogMs == 0) ||
        ((uint32_t)(nowMs - max30102PresenceStatsLastLogMs) >= MAX30102_PRESENCE_STATS_LOG_INTERVAL_MS)) {
        max30102PresenceStatsLastLogMs = nowMs;
        LOG_INFO("MAX30102 presence stats(active): present=%d mean_ir=%u mean_red=%u max_ir=%u max_red=%u n=%u", fingerPresent, meanIr,
                 meanRed, maxIr, maxRed, MAX30102_BUFFER_LEN);
    }
    if (!measurementViable) {
        resetStabilityState();
        clearRawWaveformCache();

        {
            concurrency::LockGuard g(&metricsLock);
            hasEvaluatedWindow = true;
            cachedFingerPresent = false;
            cachedHasHeartRate = false;
            cachedHeartRate = 0;
            cachedHasSpO2 = false;
            cachedSpO2 = 0;
            cachedHasDieTempC = false;
            cachedDieTempC = 0.0f;
            latchedHasHeartRate = false;
            latchedHeartRate = 0;
            latchedHasSpO2 = false;
            latchedSpO2 = 0;
            latchedHasDieTempC = false;
            latchedDieTempC = 0.0f;
            lastStableHeartMs = 0;
            lastStableSpO2Ms = 0;
            lastStableTempMs = 0;
            hasHeartEma = false;
            heartEma = 0.0f;
            hasHeartOutputEma = false;
            heartOutputEma = 0.0f;
        }
        // metricsLock is released above on purpose: sleep() performs I2C traffic and must not run while
        // the metrics mutex is held.
        if (giveUpOnEpoch && !keepAwake && chipType == PulseOxChipType::MAX30102 && max30102PresenceTriggeredActive) {
            max30102PresenceTriggeredActive = false;
            activeEpochStartMeanIr = 0;
            epochAnchorCount = 0;
            noFingerEvalStreak = 0;
            LOG_INFO("MAX30102 no finger in active eval (mean_ir=%u collapsed=%d), sleeping sensor (next scan in %ums)",
                     meanIr, signalCollapsed, MAX30102_PRESENCE_SCAN_INTERVAL_MS);
            sleep();
            max30102PresenceScanWakeStartedMs = 0;
            max30102PresenceScanNextWakeMs = millis() + MAX30102_PRESENCE_SCAN_INTERVAL_MS;
        }
        return true;
    }

    cacheRawWaveform(irWindow, redWindow, MAX30102_BUFFER_LEN);

    // Log input levels to algorithm so -999 / poor SpO2 can be correlated with weak signal
    LOG_INFO("SpO2 input: mean_ir=%u mean_red=%u max_ir=%u max_red=%u (n=%u)", meanIr, meanRed, maxIr, maxRed,
             (unsigned)MAX30102_BUFFER_LEN);

    int32_t spo2 = 0;
    int8_t spo2_valid = 0;
    int32_t selectedHeartRate = 0;
    int8_t selectedHeartRateValid = 0;
    maxim_heart_rate_and_oxygen_saturation(irWindow, MAX30102_BUFFER_LEN, redWindow, &spo2, &spo2_valid, &selectedHeartRate,
                                           &selectedHeartRateValid);
    // Periodicity. A heartbeat's distinguishing property is that it repeats, not that it is large; no
    // amplitude threshold separates a pulse from noise (see HR_MIN_AUTOCORR).
    float bestRho = -2.0f;
    uint16_t bestLag = 0;
    const bool periodic = computePeriodicity(irWindow, MAX30102_BUFFER_LEN, &bestRho, &bestLag);
    const bool hrPeriodicOk = periodic && (bestRho >= HR_MIN_AUTOCORR);
    const bool spo2PeriodicOk = periodic && (bestRho >= SPO2_MIN_AUTOCORR);
    // At 25 Hz effective sample rate, BPM = 60 * 25 / lag.
    const uint32_t autocorrBpm = ppg::bpmFromLag(bestLag, MAX3010X_EFFECTIVE_SAMPLE_RATE_HZ);

    bool hrValueValid = ((selectedHeartRateValid != 0) && (selectedHeartRate >= (int32_t)HEART_RATE_MIN_VALID) &&
                         (selectedHeartRate <= (int32_t)HEART_RATE_MAX_VALID) && hrPeriodicOk);

    // For SpO2, also require the periodicity lag to agree with the rate the kernel reported. Two
    // independent estimators landing on the same interval is much stronger evidence than either alone,
    // and it is cheap.
    const bool lagAgreesWithHr =
        (selectedHeartRate > 0) &&
        ppg::ratesAgree(autocorrBpm, (uint32_t)selectedHeartRate, SPO2_LAG_HR_TOLERANCE_PERCENT);
    // Red-channel integrity. A failed RED channel drives the ratio toward zero, which the vendor table
    // maps to a reassuring 96-97% (see SPO2_RED_PI_MIN_PERMYRIAD). Require that red actually carries a
    // pulsatile component, and that the ratio is above the region where the table folds back.
    const uint32_t acIr = (maxIr > minIr) ? (maxIr - minIr) : 0;
    const uint32_t acRed = (maxRed > minRed) ? (maxRed - minRed) : 0;
    const bool redPerfusionOk =
        (meanRed > 0) && (((uint64_t)acRed * 10000ULL) >= ((uint64_t)meanRed * SPO2_RED_PI_MIN_PERMYRIAD));
    const bool spo2RatioOk = (acIr > 0) && (meanRed > 0) && (((uint64_t)acRed * (uint64_t)meanIr * 100ULL) >=
                                                             ((uint64_t)acIr * (uint64_t)meanRed * SPO2_MIN_R_PERCENT));
    const bool redChannelOk =
        ppg::redChannelUsable(acIr, meanIr, acRed, meanRed, SPO2_RED_PI_MIN_PERMYRIAD, SPO2_MIN_R_PERCENT);

    bool spo2ValueValid = (spo2_valid != 0) && (spo2 != SPO2_INVALID_SENTINEL) &&
                          (spo2 >= (int32_t)SPO2_MIN_VALID) && (spo2 <= (int32_t)SPO2_MAX_VALID) && redChannelOk &&
                          spo2PeriodicOk && lagAgreesWithHr;

    if (hrValueValid) {
        pushStabilitySample((uint32_t)selectedHeartRate, hrStabilityWindow, &hrStabilityCount, &hrStabilityIndex);
    } else {
        hrStabilityCount = 0;
        hrStabilityIndex = 0;
    }

    if (spo2ValueValid) {
        pushStabilitySample((uint32_t)spo2, spo2StabilityWindow, &spo2StabilityCount, &spo2StabilityIndex);
    } else {
        spo2StabilityCount = 0;
        spo2StabilityIndex = 0;
    }

    bool stableHeart = hrValueValid && isStablePercent(hrStabilityWindow, hrStabilityCount, HR_STABILITY_PERCENT);
    bool stableSpO2 = spo2ValueValid && isStableSpread(spo2StabilityWindow, spo2StabilityCount, SPO2_STABILITY_SPREAD);
    uint32_t filteredHeart = stableHeart ? trimmedMeanOfWindow(hrStabilityWindow, hrStabilityCount) : 0;
    uint32_t filteredSpO2 = stableSpO2 ? medianOfWindow(spo2StabilityWindow, spo2StabilityCount) : 0;
    maybeAutoAdjustMAX30102LedPower(fingerPresent, stableHeart, hrValueValid);

    float tempC = 0.0f;
    bool tempValid = false;
    if (stableHeart) {
        // Read temperature when HR is stable (no longer require stable SpO2 so temp can show sooner)
        if (chipType == PulseOxChipType::MAX30102) {
            tempC = max30102.readTemperature();
            tempValid = !isnan(tempC) && (tempC > -40.0f) && (tempC < 120.0f);
        } else if (chipType == PulseOxChipType::MAX30100) {
            tempValid = readMAX30100Temperature(bus, address, &tempC);
        }
    }

    const uint32_t nowMsEval = millis();
    bool outputHasHeart = false;
    uint32_t outputHeart = 0;
    bool outputHasSpO2 = false;
    uint32_t outputSpO2 = 0;
    bool outputHasTemp = false;
    float outputTempC = 0.0f;
    bool usedHeartHold = false;
    uint32_t smoothedHeart = filteredHeart;
    concurrency::LockGuard g(&metricsLock);
    hasEvaluatedWindow = true;
    // Report the finger state we actually determined. This used to be `!belowPowerdownMeanThresholds`,
    // a second and far stricter definition of "present" that contradicted detectFingerPresence() above
    // and caused the downshift gate to sleep the sensor mid-measurement for most wearers.
    cachedFingerPresent = measurementViable;

    if (stableHeart) {
        if (!hasHeartEma) {
            heartEma = (float)filteredHeart;
            hasHeartEma = true;
        } else {
            heartEma = (1.0f - HEART_EMA_ALPHA) * heartEma + HEART_EMA_ALPHA * (float)filteredHeart;
        }
        smoothedHeart = (uint32_t)(heartEma + 0.5f);
        if (smoothedHeart < HEART_RATE_MIN_VALID) {
            smoothedHeart = HEART_RATE_MIN_VALID;
        } else if (smoothedHeart > HEART_RATE_MAX_VALID) {
            smoothedHeart = HEART_RATE_MAX_VALID;
        }
        if (!hasHeartOutputEma) {
            heartOutputEma = (float)smoothedHeart;
            hasHeartOutputEma = true;
        } else {
            heartOutputEma = (1.0f - HEART_OUTPUT_EMA_ALPHA) * heartOutputEma + HEART_OUTPUT_EMA_ALPHA * (float)smoothedHeart;
        }
        smoothedHeart = (uint32_t)(heartOutputEma + 0.5f);
        if (smoothedHeart < HEART_RATE_MIN_VALID) {
            smoothedHeart = HEART_RATE_MIN_VALID;
        } else if (smoothedHeart > HEART_RATE_MAX_VALID) {
            smoothedHeart = HEART_RATE_MAX_VALID;
        }
        latchedHasHeartRate = true;
        latchedHeartRate = smoothedHeart;
        lastStableHeartMs = nowMsEval;
    }
    if (stableSpO2) {
        latchedHasSpO2 = true;
        latchedSpO2 = filteredSpO2;
        lastStableSpO2Ms = nowMsEval;
    }
    // A valid-but-UNSTABLE SpO2 deliberately does NOT refresh the cache or its timestamp.
    //
    // This branch used to latch the raw value and stamp lastStableSpO2Ms, which made spo2HoldValid true
    // continuously and so bypassed the stability gate entirely: "stable" then chose only between the
    // median and the raw value, never between showing a number and showing none. Observed on real
    // hardware, a finger sliding off the sensor produced "SpO2 98%, stable" while the heart-rate
    // estimator behind it was swinging between 33 and 214 bpm.
    //
    // The hold exists to bridge brief instability in an otherwise good measurement. It is not a licence
    // to keep displaying a number once the evidence for it has gone, so only a stable, quality-gated
    // result may create or refresh the cache.
    if (tempValid && stableHeart) {
        latchedHasDieTempC = true;
        latchedDieTempC = tempC;
        lastStableTempMs = nowMsEval;
    }

    const bool heartHoldValid = latchedHasHeartRate && lastStableHeartMs != 0 &&
                                (uint32_t)(nowMsEval - lastStableHeartMs) <= STABLE_VALUE_HOLD_MS;
    const bool spo2HoldValid =
        latchedHasSpO2 && lastStableSpO2Ms != 0 && (uint32_t)(nowMsEval - lastStableSpO2Ms) <= STABLE_VALUE_HOLD_MS;
    const bool tempHoldValid =
        latchedHasDieTempC && lastStableTempMs != 0 && (uint32_t)(nowMsEval - lastStableTempMs) <= STABLE_VALUE_HOLD_MS;

    outputHasHeart = stableHeart || heartHoldValid;
    outputHeart = stableHeart ? smoothedHeart : (heartHoldValid ? latchedHeartRate : 0);
    usedHeartHold = !stableHeart && heartHoldValid;

    outputHasSpO2 = stableSpO2 || spo2HoldValid;
    outputSpO2 = stableSpO2 ? filteredSpO2 : (spo2HoldValid ? latchedSpO2 : 0);

    outputHasTemp = (tempValid && stableHeart) || tempHoldValid;
    outputTempC = (tempValid && stableHeart) ? tempC : (tempHoldValid ? latchedDieTempC : 0.0f);

    cachedHasHeartRate = outputHasHeart;
    cachedHeartRate = outputHeart;
    cachedHasSpO2 = outputHasSpO2;
    cachedSpO2 = outputSpO2;
    cachedHasDieTempC = outputHasTemp;
    cachedDieTempC = outputTempC;

    const bool usedSpO2Hold = !stableSpO2 && spo2HoldValid;
    LOG_INFO("HR eval: hr=%d valid=%d stable=%d hr_window_count=%u step=%u hr_out=%u hold=%d rho=%.2f lag=%u ac_bpm=%u",
             selectedHeartRate, hrValueValid, stableHeart, hrStabilityCount, MAX3010X_SLIDING_STEP, outputHeart,
             usedHeartHold, (double)bestRho, bestLag, autocorrBpm);
    LOG_INFO("SpO2 eval: spo2=%d valid=%d stable=%d spo2_window_count=%u spo2_out=%u hold=%d red_pi_ok=%d ratio_ok=%d "
             "ac_ir=%u ac_red=%u",
             (int)spo2, spo2ValueValid ? 1 : 0, stableSpO2 ? 1 : 0, spo2StabilityCount, outputSpO2, usedSpO2Hold ? 1 : 0,
             redPerfusionOk ? 1 : 0, spo2RatioOk ? 1 : 0, acIr, acRed);
    return true;
}

int32_t MAX30102Sensor::runOnce()
{
    LOG_INFO("Init sensor: %s", sensorName);
    if (!hasSensor()) {
        return DEFAULT_SENSOR_MINIMUM_WAIT_TIME_BETWEEN_READS;
    }

    TwoWire *bus = nodeTelemetrySensorsMap[sensorType].second;
    uint8_t address = nodeTelemetrySensorsMap[sensorType].first;
    if (!bus || address == 0) {
        status = false;
        return initI2CSensor();
    }

    uint8_t partId = 0;
    if (!readPartId(bus, address, &partId)) {
        LOG_ERROR("MAX3010x ID read failed");
        status = false;
        return initI2CSensor();
    }

    if (partId == MAX30102_PART_ID) {
        chipType = PulseOxChipType::MAX30102;
        sensorName = "MAX30102";
        if (max30102.begin(*bus, _speed, address) == true) {
            max30102ActiveLedPower = MAX30102_LED_POWER_ACTIVE_DEFAULT;
            max30102PresenceMode = false;
            max30102PresenceTriggeredActive = false;
            max30102PresenceActiveSinceMs = 0;
            max30102PresenceConsecutiveDetections = 0;
            poorSignalEvalStreak = 0;
            stableSignalEvalStreak = 0;
            lastLedPowerAdjustMs = 0;
            max30102PresenceScanWakeStartedMs = 0;
            max30102PresenceScanNextWakeMs = 0;

            byte brightness = max30102ActiveLedPower; // 0=Off to 255=50mA
            byte sampleAverage = 4; // 1, 2, 4, 8, 16, 32
            byte leds = 2;          // 1 = Red only, 2 = Red + IR
            byte sampleRate = 100;  // 50, 100, 200, 400, 800, 1000, 1600, 3200
            int pulseWidth = 411;   // 69, 118, 215, 411
            int adcRange = 4096;    // 2048, 4096, 8192, 16384

            max30102.enableDIETEMPRDY();
            max30102.setup(brightness, sampleAverage, leds, sampleRate, pulseWidth, adcRange);
            sensorActive = true;
            LOG_DEBUG("MAX30102 init succeeded");
            status = true;
        } else {
            LOG_ERROR("MAX30102 init failed");
            status = false;
            sensorActive = false;
        }
    } else if (partId == MAX30100_PART_ID) {
        chipType = PulseOxChipType::MAX30100;
        sensorName = "MAX30100";
        max30102PresenceMode = false;
        status = initMAX30100(bus, address);
        sensorActive = status;
        if (status) {
            LOG_DEBUG("MAX30100 init succeeded");
        } else {
            LOG_ERROR("MAX30100 init failed");
        }
    } else {
        chipType = PulseOxChipType::UNKNOWN;
        max30102PresenceMode = false;
        LOG_ERROR("Unsupported MAX3010x part ID 0x%02x", partId);
        status = false;
        sensorActive = false;
    }

    resetSlidingState();
    resetStabilityState();
    clearRawWaveformCache();
    clearCachedMetrics();
    max30102PresenceState = false;
    max30102PresenceTriggeredActive = false;
    max30102PresenceActiveSinceMs = 0;
    max30102PresenceStatsLastLogMs = 0;
    max30102PresenceConsecutiveDetections = 0;
    max30102PresenceScanWakeStartedMs = 0;
    max30102PresenceScanNextWakeMs = 0;

    if (status && !keepAwake) {
        sleep();
    }

    return initI2CSensor();
}

void MAX30102Sensor::setup() {}

bool MAX30102Sensor::canSleep()
{
    return true;
}

bool MAX30102Sensor::isActive()
{
    return sensorActive;
}

uint8_t MAX30102Sensor::getSensitivity() const
{
    return max30102ActiveLedPower;
}

uint8_t MAX30102Sensor::getMaxSensitivity() const
{
    return MAX30102_LED_POWER_ACTIVE_MAX;
}

void MAX30102Sensor::setSensitivity(uint8_t level)
{
    setMAX30102LedPower(level);
}

bool MAX30102Sensor::isHrEngaged() const
{
    if (chipType == PulseOxChipType::MAX30102) {
        return max30102PresenceTriggeredActive;
    }
    if (chipType == PulseOxChipType::MAX30100) {
        return sensorActive;
    }
    return false;
}

void MAX30102Sensor::sleep()
{
    if (!hasSensor() || !sensorActive) {
        return;
    }

    TwoWire *bus = nodeTelemetrySensorsMap[sensorType].second;
    uint8_t address = nodeTelemetrySensorsMap[sensorType].first;
    if (!bus || address == 0) {
        return;
    }

    if (chipType == PulseOxChipType::MAX30102) {
        max30102.shutDown();
        sensorActive = false;
        max30102PresenceMode = false;
        max30102PresenceState = false;
        max30102PresenceTriggeredActive = false;
        max30102PresenceActiveSinceMs = 0;
        max30102PresenceStatsLastLogMs = 0;
        max30102PresenceConsecutiveDetections = 0;
        max30102PresenceScanWakeStartedMs = 0;
        poorSignalEvalStreak = 0;
        stableSignalEvalStreak = 0;
    } else if (chipType == PulseOxChipType::MAX30100) {
        uint8_t modeReg = 0;
        if (readRegister(bus, address, MAX30100_REG_MODE_CONFIG, &modeReg) &&
            writeRegister(bus, address, MAX30100_REG_MODE_CONFIG, modeReg | MAX30100_MODE_SHUTDOWN)) {
            sensorActive = false;
        }
    }
}

void MAX30102Sensor::prepareDeepSleep()
{
    if (!hasSensor()) {
        return;
    }

    keepAwake = false;

    TwoWire *bus = nodeTelemetrySensorsMap[sensorType].second;
    uint8_t address = nodeTelemetrySensorsMap[sensorType].first;
    if (!bus || address == 0) {
        return;
    }

    if (chipType == PulseOxChipType::MAX30102) {
        max30102.setPulseAmplitudeRed(0x00);
        max30102.setPulseAmplitudeIR(0x00);
        max30102.setPulseAmplitudeGreen(0x00);
        max30102.setPulseAmplitudeProximity(0x00);
        max30102.clearFIFO();
        max30102.shutDown();
    } else if (chipType == PulseOxChipType::MAX30100) {
        writeRegister(bus, address, MAX30100_REG_LED_CONFIG, 0x00);
        uint8_t modeReg = 0;
        if (readRegister(bus, address, MAX30100_REG_MODE_CONFIG, &modeReg)) {
            writeRegister(bus, address, MAX30100_REG_MODE_CONFIG, modeReg | MAX30100_MODE_SHUTDOWN);
        } else {
            writeRegister(bus, address, MAX30100_REG_MODE_CONFIG, MAX30100_MODE_SHUTDOWN);
        }
    }

    sensorActive = false;
    max30102PresenceMode = false;
    max30102PresenceState = false;
    max30102PresenceTriggeredActive = false;
    max30102PresenceActiveSinceMs = 0;
    max30102PresenceStatsLastLogMs = 0;
    max30102PresenceConsecutiveDetections = 0;
    max30102PresenceScanWakeStartedMs = 0;
    max30102PresenceScanNextWakeMs = 0;
    poorSignalEvalStreak = 0;
    stableSignalEvalStreak = 0;
    resetSlidingState();
    resetStabilityState();
    clearRawWaveformCache();
    clearCachedMetrics();
    LOG_INFO("MAX3010x prepared for deep sleep");
}

uint32_t MAX30102Sensor::wakeUp()
{
    if (!hasSensor() || sensorActive) {
        return 0;
    }

    TwoWire *bus = nodeTelemetrySensorsMap[sensorType].second;
    uint8_t address = nodeTelemetrySensorsMap[sensorType].first;
    if (!bus || address == 0) {
        return 0;
    }

    if (chipType == PulseOxChipType::MAX30102) {
        max30102.wakeUp();
        delay(3);
        max30102.clearFIFO();
        sensorActive = true;
        max30102PresenceMode = false;
        max30102PresenceState = false;
        max30102PresenceTriggeredActive = false;
        max30102PresenceActiveSinceMs = 0;
        max30102PresenceStatsLastLogMs = 0;
        max30102PresenceConsecutiveDetections = 0;
        max30102PresenceScanWakeStartedMs = 0;
        max30102PresenceScanNextWakeMs = 0;
    } else if (chipType == PulseOxChipType::MAX30100) {
        uint8_t modeReg = 0;
        if (readRegister(bus, address, MAX30100_REG_MODE_CONFIG, &modeReg) &&
            writeRegister(bus, address, MAX30100_REG_MODE_CONFIG, modeReg & ~MAX30100_MODE_SHUTDOWN)) {
            writeRegister(bus, address, MAX30100_REG_FIFO_WRITE_POINTER, 0x00);
            writeRegister(bus, address, MAX30100_REG_FIFO_OVERFLOW_COUNTER, 0x00);
            writeRegister(bus, address, MAX30100_REG_FIFO_READ_POINTER, 0x00);
            sensorActive = true;
        }
    }

    return 0;
}

void MAX30102Sensor::setStayAwake(bool stayAwakeNow)
{
    keepAwake = stayAwakeNow;
    if (stayAwakeNow) {
        max30102PresenceConsecutiveDetections = 0;
        max30102PresenceScanWakeStartedMs = 0;
        max30102PresenceScanNextWakeMs = 0;
    }
}

bool MAX30102Sensor::serviceSensor()
{
    if (!hasSensor() || !isInitialized() || !status || chipType == PulseOxChipType::UNKNOWN) {
        return false;
    }

    TwoWire *bus = nodeTelemetrySensorsMap[sensorType].second;
    uint8_t address = nodeTelemetrySensorsMap[sensorType].first;
    if (!bus || address == 0) {
        return false;
    }

    if (!keepAwake) {
        if (chipType == PulseOxChipType::MAX30102) {
            if (!max30102PresenceTriggeredActive) {
                const uint32_t nowMs = millis();
                if (!sensorActive) {
                    if (max30102PresenceScanNextWakeMs != 0 && nowMs < max30102PresenceScanNextWakeMs) {
                        return false;
                    }
                    wakeUp();
                    if (!sensorActive) {
                        return false;
                    }
                    configureMAX30102Profile(true);
                    max30102PresenceScanWakeStartedMs = nowMs;
                    max30102PresenceScanNextWakeMs = 0;
                    LOG_INFO("MAX30102 presence scan wake (LED=0x%02X)", MAX30102_LED_POWER_PRESENCE);
                } else if (!max30102PresenceMode) {
                    configureMAX30102Profile(true);
                    max30102PresenceScanWakeStartedMs = nowMs;
                }

                ingestMAX30102Fifo(bus, address);
                const bool present = updateLowPowerPresenceCache();
                if (!present) {
                    const uint32_t nowAfterScanMs = millis();
                    if (max30102PresenceScanWakeStartedMs != 0 &&
                        ((uint32_t)(nowAfterScanMs - max30102PresenceScanWakeStartedMs) >= MAX30102_PRESENCE_SCAN_WAKE_WINDOW_MS)) {
                        LOG_INFO("MAX30102 no presence, sleeping sensor (next scan in %ums)",
                                 MAX30102_PRESENCE_SCAN_INTERVAL_MS);
                        sleep();
                        max30102PresenceScanWakeStartedMs = 0;
                        max30102PresenceScanNextWakeMs = nowAfterScanMs + MAX30102_PRESENCE_SCAN_INTERVAL_MS;
                    }
                    return false;
                }

                max30102PresenceTriggeredActive = true;
                max30102PresenceActiveSinceMs = millis();
                max30102PresenceScanWakeStartedMs = 0;
                max30102PresenceScanNextWakeMs = 0;
                LOG_INFO("MAX30102 presence detected, switching to active profile (LED=0x%02X)", max30102ActiveLedPower);
                configureMAX30102Profile(false);
            }
        }

        if (chipType != PulseOxChipType::MAX30102) {
            if (sensorActive) {
                sleep();
            }
            return false;
        }
    } else {
        max30102PresenceState = false;
        max30102PresenceTriggeredActive = false;
        max30102PresenceScanWakeStartedMs = 0;
        max30102PresenceScanNextWakeMs = 0;
    }

    if (!sensorActive) {
        wakeUp();
    }

    if (!sensorActive) {
        return false;
    }

    if (chipType == PulseOxChipType::MAX30102 && max30102PresenceMode) {
        configureMAX30102Profile(false);
    }

#ifdef BHV_PPG_DIAG_ADCSWEEP
    // Sweep the ADC full-scale range at FIXED LED drive. If the measured noise floor stays constant in
    // COUNTS it is output-referred (digital/quantisation-like); if it stays constant in PICOAMPS it is
    // input-referred (analog front-end). That decides whether range and drive can be traded for SNR.
    {
        static const int kRanges[] = {2048, 4096, 8192, 16384};
        static uint8_t rIdx = 0;
        static uint32_t lastRangeMs = 0;
        const uint32_t nowR = millis();
        if (lastRangeMs == 0) { lastRangeMs = nowR; }
        if ((uint32_t)(nowR - lastRangeMs) >= BHV_PPG_DIAG_ADCSWEEP) {
            lastRangeMs = nowR;
            rIdx = (uint8_t)((rIdx + 1) % 4);
            // Register values are private to the vendor .cpp; SPO2_CONFIG[6:5] per the datasheet.
            static const uint8_t kRangeBits[] = {0x00, 0x20, 0x40, 0x60};
            max30102.setADCRange(kRangeBits[rIdx]);
            discardSampleWindow();
            LOG_INFO("ADCSWEEP range=%d nA", kRanges[rIdx]);
        }
    }
#endif

#ifdef BHV_PPG_DIAG_LEDSWEEP
    // With NO finger, mean_ir vs LED drive decomposes the optical pedestal: the intercept is ambient
    // plus dark current (independent of drive), the slope is light returning from the enclosure/air
    // without traversing tissue. Neither needs a person present.
    {
        static const uint8_t kSweep[] = {0x00, 0x08, 0x10, 0x18, 0x20, 0x2F, 0x40, 0x5F, 0x7F};
        static uint8_t sweepIdx = 0;
        static uint32_t lastSweepMs = 0;
        const uint32_t nowSweep = millis();
        if (lastSweepMs == 0) { lastSweepMs = nowSweep; }
        if ((uint32_t)(nowSweep - lastSweepMs) >= BHV_PPG_DIAG_LEDSWEEP) {
            lastSweepMs = nowSweep;
            sweepIdx = (uint8_t)((sweepIdx + 1) % (sizeof(kSweep) / sizeof(kSweep[0])));
            max30102ActiveLedPower = kSweep[sweepIdx];
            max30102.setPulseAmplitudeRed(max30102ActiveLedPower);
            max30102.setPulseAmplitudeIR(max30102ActiveLedPower);
            discardSampleWindow();
            LOG_INFO("LEDSWEEP drive=0x%02X (%u x 0.2mA = %u.%u mA)", max30102ActiveLedPower,
                     max30102ActiveLedPower, (max30102ActiveLedPower * 2) / 10, (max30102ActiveLedPower * 2) % 10);
        }
    }
#endif

    if (chipType == PulseOxChipType::MAX30102) {
        ingestMAX30102Fifo(bus, address);
    } else if (chipType == PulseOxChipType::MAX30100) {
        ingestMAX30100Fifo(bus, address);
    }

    bool evaluated = false;
    if (slidingSampleCount >= MAX30102_BUFFER_LEN && slidingNewSamplesSinceEval >= MAX3010X_SLIDING_STEP) {
        const uint32_t nowMs = millis();
        const bool intervalElapsed =
            (lastEvalMs == 0) || ((uint32_t)(nowMs - lastEvalMs) >= MAX3010X_EVAL_MIN_INTERVAL_MS);

        if (intervalElapsed) {
            if (evaluateSlidingWindow(bus, address)) {
                evaluated = true;
            }
            lastEvalMs = nowMs;

            // Keep true sliding data in FIFO/ring buffer, but collapse queued "step events" so we
            // run at the requested cadence and always evaluate the newest 100-sample window.
            slidingNewSamplesSinceEval = (uint16_t)(slidingNewSamplesSinceEval % MAX3010X_SLIDING_STEP);
        }
    }

    if (!keepAwake && chipType == PulseOxChipType::MAX30102 && max30102PresenceTriggeredActive) {
        bool fingerPresent = false;
        bool hasWindow = false;
        {
            concurrency::LockGuard g(&metricsLock);
            fingerPresent = cachedFingerPresent;
            hasWindow = hasEvaluatedWindow;
        }

        const uint32_t nowMs = millis();
        const bool holdElapsed = (uint32_t)(nowMs - max30102PresenceActiveSinceMs) >= MAX30102_PRESENCE_ACTIVE_HOLD_MS;
        const bool streakReached = noFingerEvalStreak >= MAX3010X_NO_FINGER_EVAL_STREAK_FOR_SLEEP;
        if ((lastDownshiftGateLogMs == 0) || ((uint32_t)(nowMs - lastDownshiftGateLogMs) >= 1000)) {
            lastDownshiftGateLogMs = nowMs;
            LOG_INFO("MAX30102 downshift gate: hold=%d hasWindow=%d finger=%d mean_ir=%u mean_red=%u anchor=%u streak=%u "
                     "active=%d mode=%s",
                     holdElapsed, hasWindow, fingerPresent, lastEvalMeanIr, lastEvalMeanRed, activeEpochStartMeanIr,
                     noFingerEvalStreak, max30102PresenceTriggeredActive, max30102PresenceMode ? "presence" : "active");
        }
        if (holdElapsed && hasWindow && !fingerPresent && streakReached) {
            max30102PresenceTriggeredActive = false;
            activeEpochStartMeanIr = 0;
            epochAnchorCount = 0;
            noFingerEvalStreak = 0;
            LOG_INFO("MAX30102 no finger, entering sleep presence-scan mode");
            sleep();
            max30102PresenceScanWakeStartedMs = 0;
            max30102PresenceScanNextWakeMs = millis() + MAX30102_PRESENCE_SCAN_INTERVAL_MS;
        }
    }

    return evaluated;
}

bool MAX30102Sensor::getMetrics(meshtastic_Telemetry *measurement)
{
    if (!measurement || !hasSensor() || !isInitialized() || !status) {
        return false;
    }

    bool localHasEvaluatedWindow = false;
    bool localHasHeartRate = false;
    bool localHasSpO2 = false;
    uint32_t localHeartRate = 0;
    uint32_t localSpO2 = 0;

    {
        concurrency::LockGuard g(&metricsLock);
        localHasEvaluatedWindow = hasEvaluatedWindow;
        localHasHeartRate = cachedHasHeartRate;
        localHeartRate = cachedHeartRate;
        localHasSpO2 = cachedHasSpO2;
        localSpO2 = cachedSpO2;
    }

    if (!localHasEvaluatedWindow) {
        return false;
    }

    // Nothing worth reporting is not the same as a reading of zero.
    //
    // hasEvaluatedWindow only means "a window has been processed at some point", so with honest gating
    // active - which withholds far more often than the original always-show behaviour - this returned
    // true with every field absent. Callers treat a true return as "there is a measurement", and the
    // result went out on the wire as `temperature=0.000000, heart_bpm=0, spO2=0`: a packet asserting a
    // heart rate of zero, observed in real capture. Report nothing rather than nothing-shaped-as-zero.
    if (!localHasHeartRate && !localHasSpO2) {
        return false;
    }

    measurement->which_variant = meshtastic_Telemetry_health_metrics_tag;
    measurement->variant.health_metrics = meshtastic_HealthMetrics_init_zero;

    measurement->variant.health_metrics.has_heart_bpm = localHasHeartRate;
    if (localHasHeartRate) {
        measurement->variant.health_metrics.heart_bpm = localHeartRate;
    }

    measurement->variant.health_metrics.has_spO2 = localHasSpO2;
    if (localHasSpO2) {
        measurement->variant.health_metrics.spO2 = localSpO2;
    }

    // The MAX3010x has only a DIE-temperature sensor, whose documented purpose is compensating the
    // temperature dependence of the SpO2 subsystem (the red LED's wavelength shifts with temperature).
    // meshtastic_HealthMetrics.temperature is documented as "Body temperature in degrees Celsius"
    // (protobufs/meshtastic/telemetry.proto), and it leaves this badge over LoRa and MQTT into
    // third-party clients that will render it as exactly that. A package temperature is not a body
    // temperature, so this sensor never populates the field; it belongs solely to the MLX90614 object
    // temperature. The die reading is retained internally for diagnostics and future R compensation.
    measurement->variant.health_metrics.has_temperature = false;

    return true;
}

#endif
