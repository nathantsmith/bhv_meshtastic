#include "HeartbeatPixelThread.h"

#ifdef HAS_HEARTBEAT_NEOPIXELS

#include "configuration.h"
#include "concurrency/LockGuard.h"

#if !MESHTASTIC_EXCLUDE_HEALTH_TELEMETRY
#include "modules/Telemetry/HealthTelemetry.h"
#endif

#include <Arduino.h>
#include <math.h>

HeartbeatPixelThread *heartbeatPixelThread = nullptr;

// Pattern arrays are stored directly in the current physical LED order:
// new D1..D14 = old D4, D3, D2, D1, D12, D13, D14, D7, D6, D8, D11, D5, D9, D10.
const HeartbeatPixelThread::LedPulseConfig HeartbeatPixelThread::startupConfig[HeartbeatPixelThread::kLedCount] = {
    {0.185f, 0.220f}, {0.130f, 0.220f}, {0.075f, 0.220f}, {0.020f, 0.220f}, {0.640f, 0.220f}, {0.695f, 0.250f},
    {0.750f, 0.220f}, {0.350f, 0.220f}, {0.3225f, 0.220f}, {0.295f, 0.220f}, {0.420f, 0.220f}, {0.240f, 0.220f},
    {0.475f, 0.220f}, {0.530f, 0.220f},
};

const HeartbeatPixelThread::LedPulseConfig HeartbeatPixelThread::originalStartupConfig[HeartbeatPixelThread::kLedCount] = {
    {0.0972f, 0.50f}, {0.0873f, 0.50f}, {0.2016f, 0.50f}, {0.3237f, 0.50f}, {0.2241f, 0.50f}, {0.3065f, 0.50f},
    {0.3417f, 0.50f}, {0.2739f, 0.50f}, {0.2154f, 0.50f}, {0.1781f, 0.50f}, {0.0967f, 0.50f}, {0.0760f, 0.50f},
    {0.1110f, 0.50f}, {0.1004f, 0.50f},
};

const HeartbeatPixelThread::LedPulseConfig HeartbeatPixelThread::heartbeatConfig[HeartbeatPixelThread::kLedCount] = {
    {0.2585f, 0.3041f}, {0.0985f, 0.5741f}, {0.6712f, 0.7581f}, {0.5600f, 0.7200f}, {0.5723f, 0.2860f},
    {0.6145f, 0.3256f}, {0.6566f, 0.3256f}, {0.7100f, 0.3600f}, {0.6162f, 0.4135f}, {0.5223f, 0.4670f},
    {0.6712f, 0.7581f}, {0.4508f, 0.4550f}, {0.0985f, 0.5741f}, {0.2589f, 0.3016f},
};

// Per-LED brightness windows are stored in the current physical LED order.
// They were remapped from the original layout so the same physical LEDs keep the same visual trim.
const float HeartbeatPixelThread::kPixelMinBrightness[HeartbeatPixelThread::kLedCount] = {
    0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
};

const float HeartbeatPixelThread::kPixelMaxBrightness[HeartbeatPixelThread::kLedCount] = {
    0.8f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0.8f, 0.8f, 0.8f, 1.0f, 0.8f, 0.8f, 1.0f,
};

const float HeartbeatPixelThread::kPixelBrightnessRange[HeartbeatPixelThread::kLedCount] = {
    0.8f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0.8f, 0.8f, 0.8f, 1.0f, 0.8f, 0.8f, 1.0f,
};

const bool HeartbeatPixelThread::kPixelUsesLed1Color[HeartbeatPixelThread::kLedCount] = {
    true, true, true, true, false, false, false, true, true, true, false, true, false, false,
};

// Physical LED sequence for each notification color lane.
// Lane 1: D4, D3, D2, D1, D12, D9, D8.
const uint8_t HeartbeatPixelThread::kLed1NotificationSequence[HeartbeatPixelThread::kLed1SequenceLength] = {
    3, 2, 1, 0, 11, 8, 7,
};

// Lane 2: D10, D13, D14, D11, D5, D6, D7.
const uint8_t HeartbeatPixelThread::kLed2NotificationSequence[HeartbeatPixelThread::kLed2SequenceLength] = {
    9, 12, 13, 10, 4, 5, 6,
};

HeartbeatPixelThread::HeartbeatPixelThread()
    : concurrency::OSThread("HeartbeatPixels", kAnimationIntervalMs)
{
    heartbeatPixelThread = this;
    notifyDeepSleepObserver.observe(&notifyDeepSleep);
}

int32_t HeartbeatPixelThread::runOnce()
{
    if (!initialized) {
        initializeHardware();
        if (!initialized) {
            return kAnimationIntervalMs;
        }
        startupStartMs = millis();
    }

    const uint32_t nowMs = millis();
    if (nextFrameMs == 0) {
        nextFrameMs = nowMs;
    }

#ifdef BHV_LED_CROSSTALK_TEST
    // DIAGNOSTIC BUILD ONLY - not compiled unless -D BHV_LED_CROSSTALK_TEST is set.
    //
    // Alternates the whole LED rail via ENABLE_VLED so the optical leak from the 14 WS2812Bs into the
    // MAX30102 photodiode can be measured by differencing, instead of argued about. With no finger on
    // the sensor the presence-mode log already reports mean_ir/mean_red every scan, so bucketing those
    // by rail state gives leak_ir and leak_red directly. No finger, and no user, required.
    {
        static uint32_t lastToggleMs = 0;
        static bool railOn = true;
        if (lastToggleMs == 0) {
            lastToggleMs = nowMs;
        }
        if ((uint32_t)(nowMs - lastToggleMs) >= BHV_LED_CROSSTALK_PERIOD_MS) {
            lastToggleMs = nowMs;
            railOn = !railOn;
            powerStrips(railOn);
            LOG_INFO("LEDRAIL %s", railOn ? "ON" : "OFF");
        }
        if (!railOn) {
            // Rail is down; skip rendering so nothing is clocked into an unpowered chain.
            return kAnimationIntervalMs;
        }
    }
#endif

    if (runningStartup) {
        renderStartupFrame(nowMs);
        if ((nowMs - startupStartMs) >= kStartupDurationMs) {
            runningStartup = false;
            idleOff();
        }
    } else {
        LocalLedEffectiveConfig effective = {
            0x0000FF, 0xFF0000, 80, 0, kLocalLedDefaultNotificationPulses, kLocalLedDefaultSendPulses, false, 0};
        if (localLedConfigStore) {
            const CustomLedConfig cfg = localLedConfigStore->getConfig();
            effective.led1_color = cfg.node_led1_color;
            effective.led2_color = cfg.node_led2_color;
            effective.idle_bpm = cfg.idle_bpm;
            effective.idle_delay_ms = cfg.idle_delay_ms;
            effective.notification_pulses = cfg.notification_pulses;
            effective.send_pulses = cfg.send_pulses;
        }
        bool heartRateActive = false;
#if !MESHTASTIC_EXCLUDE_HEALTH_TELEMETRY
        heartRateActive = healthTelemetryModule && healthTelemetryModule->isHeartRateActive();
#endif
        if (renderPatternFrame(nowMs)) {
            // One-shot status patterns deliberately override the live heartbeat for a short, unambiguous response.
        } else if (heartRateActive) {
            syncBpm(nowMs);
            renderHeartbeatFrame(nowMs, effective, true);
        } else if (!config.device.led_heartbeat_disabled) {
            currentBpm = effective.idle_bpm;
            renderHeartbeatFrame(nowMs, effective, true);
        } else if (notificationStateNeedsRender()) {
            currentBpm = effective.idle_bpm;
            heartbeatOffsetMs = 0.0f;
            renderHeartbeatFrame(nowMs, effective, false);
        } else {
            currentBpm = effective.idle_bpm;
            heartbeatOffsetMs = 0.0f;
            hasNotificationProgress = false;
            idleOff();
        }
    }

    uint32_t targetNextFrameMs = nextFrameMs + kAnimationIntervalMs;
    if ((int32_t)(nowMs - targetNextFrameMs) >= 0) {
        targetNextFrameMs = nowMs + kAnimationIntervalMs;
    }
    nextFrameMs = targetNextFrameMs;

    return RUN_SAME;
}

bool HeartbeatPixelThread::shouldRun(unsigned long time)
{
    if (!enabled) {
        return false;
    }
    if (nextFrameMs == 0) {
        return true;
    }
    return (int32_t)(time - nextFrameMs) >= 0;
}

long HeartbeatPixelThread::tillRun(unsigned long time)
{
    if (!enabled) {
        return __LONG_MAX__;
    }
    if (nextFrameMs == 0) {
        return 0;
    }
    return (long)((int32_t)(nextFrameMs - time));
}

void HeartbeatPixelThread::initializeHardware()
{
    powerStrips(true);
    const rmt_reserve_memsize_t rmtMemOptions[] = {RMT_MEM_256, RMT_MEM_192, RMT_MEM_128, RMT_MEM_64};
    for (const rmt_reserve_memsize_t memSize : rmtMemOptions) {
        rmtTx = rmtInit(HEARTBEAT_NEOPIXEL_LEFT_PIN, RMT_TX_MODE, memSize);
        if (rmtTx) {
            break;
        }
    }
    if (!rmtTx) {
        LOG_ERROR("HeartbeatPixels failed to initialize RMT on GPIO %d", HEARTBEAT_NEOPIXEL_LEFT_PIN);
        powerStrips(false);
        return;
    }
    rmtSetTick(rmtTx, 100.0f);
    initializeNotificationSequenceTimings();
    clearStrips();
    initialized = true;
}

void HeartbeatPixelThread::initializeNotificationSequenceTimings()
{
    led1SequenceTiming = calculateNotificationSequenceTiming(kLed1NotificationSequence, kLed1SequenceLength);
    led2SequenceTiming = calculateNotificationSequenceTiming(kLed2NotificationSequence, kLed2SequenceLength);
}

void HeartbeatPixelThread::renderStartupFrame(uint32_t nowMs)
{
    const double cycleTimeMs = (double)kStartupDurationMs;
    const double elapsedMs = (double)(nowMs - startupStartMs);
    LocalLedEffectiveConfig startupEffective = {
        0x0000FF, 0xFF0000, kStartupBpm, 0, kLocalLedDefaultNotificationPulses, kLocalLedDefaultSendPulses, true, 0};
    hasNotificationProgress = false;
    applyFrame(cycleTimeMs, cycleTimeMs, elapsedMs, startupConfig, startupEffective);
}

bool HeartbeatPixelThread::renderPatternFrame(uint32_t nowMs)
{
    PatternEvent pattern = {};
    {
        concurrency::LockGuard guard(&notificationLock);
        if (!activePattern.active) {
            PatternEvent *queued = patternQueueFrontLocked();
            if (!queued) {
                return false;
            }
            activePattern = *queued;
            activePattern.startMs = nowMs;
            popPatternQueueLocked();
        }

        if ((nowMs - activePattern.startMs) >= activePattern.durationMs) {
            activePattern = PatternEvent{};
            return false;
        }
        pattern = activePattern;
    }

    const double elapsedMs = (double)(nowMs - pattern.startMs);
    const double durationMs = (double)pattern.durationMs;
    for (uint8_t i = 0; i < kLedCount; ++i) {
        const float brightness =
            calculateBrightness(durationMs, elapsedMs, pattern.config[i].startTime * durationMs, pattern.config[i].pulseWidth * durationMs);
        setPixel(i, pattern.color, brightness);
    }
    showStrips();
    return true;
}

void HeartbeatPixelThread::renderHeartbeatFrame(uint32_t nowMs, const LocalLedEffectiveConfig &effective, bool baseHeartbeatEnabled)
{
    const double activeWindowMs = 60000.0 / (double)currentBpm;
    const double cycleTimeMs = activeWindowMs + (double)effective.idle_delay_ms;
    const double currentTimeMs = (double)nowMs + heartbeatOffsetMs;
    updateNotificationSequences(cycleTimeMs, activeWindowMs, currentTimeMs, true);
    applyFrame(cycleTimeMs, activeWindowMs, currentTimeMs, heartbeatConfig, effective, baseHeartbeatEnabled);
}

void HeartbeatPixelThread::idleOff()
{
    if (!stripsAreDark) {
        clearStrips();
    }
    powerStrips(false);
}

void HeartbeatPixelThread::applyFrame(double cycleTimeMs, double activeWindowMs, double currentTimeMs, const LedPulseConfig *config,
                                      const LocalLedEffectiveConfig &effective, bool baseHeartbeatEnabled)
{
    const RgbColor led1Color = colorFromHex(effective.led1_color);
    const RgbColor led2Color = colorFromHex(effective.led2_color);
    NotificationLane led1LaneSnapshot = {};
    NotificationLane led2LaneSnapshot = {};
    double progressSnapshot = 0.0;
    {
        concurrency::LockGuard guard(&notificationLock);
        led1LaneSnapshot = led1NotificationLane;
        led2LaneSnapshot = led2NotificationLane;
        progressSnapshot = notificationProgress;
    }

    for (uint8_t i = 0; i < kLedCount; ++i) {
        const float brightness =
            calculateBrightness(cycleTimeMs, currentTimeMs, config[i].startTime * activeWindowMs, config[i].pulseWidth * activeWindowMs);
        const bool pulseActive = brightness > 0.0f;
        const bool usesLed1Color = kPixelUsesLed1Color[i];
        const RgbColor &baseColor = usesLed1Color ? led1Color : led2Color;
        const bool useNotificationColor =
            pulseActive &&
            (usesLed1Color ? notificationAppliesToPixel(led1LaneSnapshot, i, progressSnapshot)
                           : notificationAppliesToPixel(led2LaneSnapshot, i, progressSnapshot));
        const RgbColor &color =
            useNotificationColor ? (usesLed1Color ? led1LaneSnapshot.color : led2LaneSnapshot.color) : baseColor;
        setPixel(i, color, (baseHeartbeatEnabled || useNotificationColor) ? brightness : 0.0f);
    }
    showStrips();
}

bool HeartbeatPixelThread::enqueueChannelNotification(uint8_t channel)
{
    if (!localLedConfigStore) {
        return false;
    }

    const LocalLedEffectiveConfig effective = localLedConfigStore->getEffectiveConfigForChannel(channel);
    return enqueueNotification(effective, effective.notification_pulses);
}

bool HeartbeatPixelThread::enqueueDirectMessageNotification()
{
    if (!localLedConfigStore) {
        return false;
    }

    const LocalLedEffectiveConfig effective = localLedConfigStore->getEffectiveConfigForDirectMessage();
    return enqueueNotification(effective, effective.notification_pulses);
}

bool HeartbeatPixelThread::enqueueDirectMessageNotification(uint32_t nodeNum)
{
    if (!localLedConfigStore) {
        return false;
    }

    const LocalLedEffectiveConfig effective = localLedConfigStore->getEffectiveConfigForDirectMessage(nodeNum);
    return enqueueNotification(effective, effective.notification_pulses);
}

bool HeartbeatPixelThread::enqueueChannelSendNotification(uint8_t channel)
{
    if (!localLedConfigStore) {
        return false;
    }

    const LocalLedEffectiveConfig effective = localLedConfigStore->getEffectiveConfigForChannel(channel);
    return enqueueNotification(effective, effective.send_pulses);
}

bool HeartbeatPixelThread::enqueueDirectMessageSendNotification(uint32_t nodeNum)
{
    if (!localLedConfigStore) {
        return false;
    }

    const LocalLedEffectiveConfig effective = localLedConfigStore->getEffectiveConfigForDirectMessage(nodeNum);
    return enqueueNotification(effective, effective.send_pulses);
}

bool HeartbeatPixelThread::enqueueCommandStatusPattern(bool accepted)
{
    return enqueuePattern(originalStartupConfig, accepted ? 0x00FF00 : 0xFF0000, (uint32_t)(60000.0f / kStartupBpm));
}

bool HeartbeatPixelThread::enqueueNotification(const LocalLedEffectiveConfig &effective, uint8_t pulseCount)
{
    if (!effective.configured) {
        return false;
    }

    concurrency::LockGuard guard(&notificationLock);
    if (notificationChannelIsPendingLocked(effective.channel_index) || notificationQueueCount >= kNotificationQueueSize) {
        return false;
    }

    const uint8_t insertIndex = (notificationQueueHead + notificationQueueCount) % kNotificationQueueSize;
    notificationQueue[insertIndex].used = true;
    notificationQueue[insertIndex].channel = effective.channel_index;
    notificationQueue[insertIndex].led1Color = colorFromHex(effective.led1_color);
    notificationQueue[insertIndex].led2Color = colorFromHex(effective.led2_color);
    notificationQueue[insertIndex].pulseCount = pulseCount > 0 ? pulseCount : kLocalLedDefaultSendPulses;
    notificationQueue[insertIndex].eligibleProgress = hasNotificationProgress ? floor(notificationProgress) + 1.0 : 0.0;
    notificationQueue[insertIndex].led1Loaded = false;
    notificationQueue[insertIndex].led2Loaded = false;
    notificationQueueCount++;
    return true;
}

bool HeartbeatPixelThread::enqueuePattern(const LedPulseConfig *config, uint32_t color, uint32_t durationMs)
{
    if (!config || durationMs == 0) {
        return false;
    }

    concurrency::LockGuard guard(&notificationLock);
    if (patternQueueCount >= kPatternQueueSize) {
        return false;
    }

    const uint8_t insertIndex = (patternQueueHead + patternQueueCount) % kPatternQueueSize;
    patternQueue[insertIndex].active = true;
    patternQueue[insertIndex].config = config;
    patternQueue[insertIndex].color = colorFromHex(color);
    patternQueue[insertIndex].durationMs = durationMs;
    patternQueue[insertIndex].startMs = 0;
    patternQueueCount++;
    return true;
}

void HeartbeatPixelThread::updateNotificationSequences(double cycleTimeMs, double activeWindowMs, double currentTimeMs,
                                                       bool allowNewNotifications)
{
    double wrappedTimeMs = fmod(currentTimeMs, cycleTimeMs);
    if (wrappedTimeMs < 0) {
        wrappedTimeMs += cycleTimeMs;
    }
    const double currentPhase = wrappedTimeMs / cycleTimeMs;
    if (!hasNotificationProgress) {
        hasNotificationProgress = true;
        lastNotificationPhase = currentPhase;
        notificationProgress = currentPhase;
        return;
    }

    double phaseDelta = currentPhase - lastNotificationPhase;
    if (phaseDelta < 0.0) {
        phaseDelta += 1.0;
    }
    const double previousProgress = notificationProgress;
    notificationProgress += phaseDelta;

    {
        concurrency::LockGuard guard(&notificationLock);
        if (allowNewNotifications) {
            const double activeWindowScale = activeWindowMs / cycleTimeMs;
            processNotificationLane(led1NotificationLane, led1SequenceTiming, true, previousProgress, notificationProgress,
                                    activeWindowScale);
            processNotificationLane(led2NotificationLane, led2SequenceTiming, false, previousProgress, notificationProgress,
                                    activeWindowScale);
        }
    }

    lastNotificationPhase = currentPhase;
}

void HeartbeatPixelThread::processNotificationLane(NotificationLane &lane, const NotificationSequenceTiming &timing, bool useLed1Color,
                                                   double previousProgress, double currentProgress, double activeWindowScale)
{
    if (lane.active && currentProgress >= lane.laneEndProgress) {
        lane = NotificationLane{};
    }

    const double startPhase = timing.startOffset * activeWindowScale;
    if (lane.active || !crossedProgressPhase(previousProgress, currentProgress, startPhase)) {
        return;
    }

    double startProgress = floor(currentProgress - startPhase) + startPhase;
    if (startProgress > currentProgress) {
        startProgress -= 1.0;
    }
    if (!lane.active) {
        loadNotificationLane(lane, timing, useLed1Color, startProgress, activeWindowScale);
    }
}

bool HeartbeatPixelThread::loadNotificationLane(NotificationLane &lane, const NotificationSequenceTiming &timing, bool useLed1Color,
                                                double laneStartProgress, double activeWindowScale)
{
    PendingNotification *pending = notificationQueueFrontLocked();
    if (!pending) {
        return false;
    }
    if (notificationProgress < pending->eligibleProgress) {
        return false;
    }
    if (!useLed1Color && !pending->led1Loaded) {
        return false;
    }

    lane.active = true;
    lane.channel = pending->channel;
    lane.color = useLed1Color ? pending->led1Color : pending->led2Color;
    lane.laneEndProgress =
        laneStartProgress + sequenceOffsetToProgressOffset(timing, timing.endOffset + pending->pulseCount, activeWindowScale);
    for (uint8_t i = 0; i < kLedCount; ++i) {
        lane.ledWindows[i] = LedNotificationWindow{};
        if (timing.ledStartOffsets[i] >= 0.0) {
            lane.ledWindows[i].active = true;
            lane.ledWindows[i].startProgress =
                laneStartProgress + sequenceOffsetToProgressOffset(timing, timing.ledStartOffsets[i], activeWindowScale);
            lane.ledWindows[i].endProgress =
                laneStartProgress +
                sequenceOffsetToProgressOffset(timing, timing.ledStartOffsets[i] + pending->pulseCount, activeWindowScale);
        }
    }
    if (useLed1Color) {
        pending->led1Loaded = true;
    } else {
        pending->led2Loaded = true;
    }

    if (pending->led1Loaded && pending->led2Loaded) {
        popNotificationQueueLocked();
    }
    return true;
}

bool HeartbeatPixelThread::notificationChannelIsPendingLocked(uint8_t channel) const
{
    if ((led1NotificationLane.active && led1NotificationLane.channel == channel) ||
        (led2NotificationLane.active && led2NotificationLane.channel == channel)) {
        return true;
    }

    for (uint8_t i = 0; i < notificationQueueCount; ++i) {
        const uint8_t index = (notificationQueueHead + i) % kNotificationQueueSize;
        if (notificationQueue[index].used && notificationQueue[index].channel == channel) {
            return true;
        }
    }
    return false;
}

bool HeartbeatPixelThread::notificationStateNeedsRender() const
{
    concurrency::LockGuard guard(&notificationLock);
    return notificationQueueCount > 0 || led1NotificationLane.active || led2NotificationLane.active || activePattern.active ||
           patternQueueCount > 0;
}

HeartbeatPixelThread::PendingNotification *HeartbeatPixelThread::notificationQueueFrontLocked()
{
    if (notificationQueueCount == 0) {
        return nullptr;
    }
    PendingNotification &front = notificationQueue[notificationQueueHead];
    return front.used ? &front : nullptr;
}

void HeartbeatPixelThread::popNotificationQueueLocked()
{
    if (notificationQueueCount == 0) {
        return;
    }

    notificationQueue[notificationQueueHead] = PendingNotification{};
    notificationQueueHead = (notificationQueueHead + 1) % kNotificationQueueSize;
    notificationQueueCount--;
}

HeartbeatPixelThread::PatternEvent *HeartbeatPixelThread::patternQueueFrontLocked()
{
    if (patternQueueCount == 0) {
        return nullptr;
    }
    PatternEvent &front = patternQueue[patternQueueHead];
    return front.active ? &front : nullptr;
}

void HeartbeatPixelThread::popPatternQueueLocked()
{
    if (patternQueueCount == 0) {
        return;
    }

    patternQueue[patternQueueHead] = PatternEvent{};
    patternQueueHead = (patternQueueHead + 1) % kPatternQueueSize;
    patternQueueCount--;
}

void HeartbeatPixelThread::clearNotificationState()
{
    concurrency::LockGuard guard(&notificationLock);
    notificationQueueHead = 0;
    notificationQueueCount = 0;
    for (uint8_t i = 0; i < kNotificationQueueSize; ++i) {
        notificationQueue[i] = PendingNotification{};
    }
    patternQueueHead = 0;
    patternQueueCount = 0;
    for (uint8_t i = 0; i < kPatternQueueSize; ++i) {
        patternQueue[i] = PatternEvent{};
    }
    activePattern = PatternEvent{};
    led1NotificationLane = NotificationLane{};
    led2NotificationLane = NotificationLane{};
    hasNotificationProgress = false;
}

bool HeartbeatPixelThread::crossedProgressPhase(double previousProgress, double currentProgress, double phase)
{
    double triggerProgress = floor(previousProgress - phase) + phase;
    if (triggerProgress <= previousProgress) {
        triggerProgress += 1.0;
    }
    return triggerProgress <= currentProgress;
}

bool HeartbeatPixelThread::notificationAppliesToPixel(const NotificationLane &lane, uint8_t ledIndex, double currentProgress)
{
    if (!lane.active || ledIndex >= kLedCount || !lane.ledWindows[ledIndex].active) {
        return false;
    }

    const LedNotificationWindow &window = lane.ledWindows[ledIndex];
    return currentProgress >= window.startProgress && currentProgress < window.endProgress;
}

double HeartbeatPixelThread::sequenceOffsetToProgressOffset(const NotificationSequenceTiming &timing, double sequenceOffset,
                                                            double activeWindowScale)
{
    const double absoluteSequencePosition = timing.startOffset + sequenceOffset;
    const double cycleOffset = floor(absoluteSequencePosition);
    const double phaseOffset = absoluteSequencePosition - cycleOffset;
    return cycleOffset + ((phaseOffset - timing.startOffset) * activeWindowScale);
}

HeartbeatPixelThread::NotificationSequenceTiming
HeartbeatPixelThread::calculateNotificationSequenceTiming(const uint8_t *sequence, uint8_t sequenceLength)
{
    NotificationSequenceTiming timing = {};
    for (uint8_t i = 0; i < kLedCount; ++i) {
        timing.ledStartOffsets[i] = kInactiveSequenceOffset;
    }

    if (sequenceLength == 0) {
        return timing;
    }

    timing.startOffset = heartbeatConfig[sequence[0]].startTime;
    double ledStartOffset = timing.startOffset;
    timing.ledStartOffsets[sequence[0]] = 0.0;
    for (uint8_t i = 1; i < sequenceLength; ++i) {
        const uint8_t ledIndex = sequence[i];
        const double offset = heartbeatConfig[ledIndex].startTime;
        double nextStartOffset = floor(ledStartOffset - offset) + offset;
        while (nextStartOffset <= ledStartOffset) {
            nextStartOffset += 1.0;
        }
        ledStartOffset = nextStartOffset;
        timing.ledStartOffsets[ledIndex] = ledStartOffset - timing.startOffset;
    }

    timing.endOffset = timing.ledStartOffsets[sequence[sequenceLength - 1]];
    return timing;
}

float HeartbeatPixelThread::calculateBrightness(double cycleTimeMs, double currentTimeMs, double startTimeMs, double pulseWidthMs) const
{
    const double wrappedTime = fmod(currentTimeMs, cycleTimeMs);
    const double endTimeMs = startTimeMs + pulseWidthMs;

    if ((endTimeMs < cycleTimeMs) && (wrappedTime >= startTimeMs) && (wrappedTime <= fmod(endTimeMs, cycleTimeMs))) {
        return (float)(0.5 - 0.5 * cos((wrappedTime - startTimeMs) / pulseWidthMs * 2.0 * PI));
    }
    if ((endTimeMs > cycleTimeMs) && ((wrappedTime >= startTimeMs) || (wrappedTime <= fmod(endTimeMs, cycleTimeMs)))) {
        if (wrappedTime >= startTimeMs) {
            return (float)(0.5 - 0.5 * cos((wrappedTime - startTimeMs) / pulseWidthMs * 2.0 * PI));
        }
        return (float)(0.5 - 0.5 * cos((wrappedTime + cycleTimeMs - startTimeMs) / pulseWidthMs * 2.0 * PI));
    }

    return 0.0f;
}

void HeartbeatPixelThread::syncBpm(uint32_t nowMs)
{
#if !MESHTASTIC_EXCLUDE_HEALTH_TELEMETRY
    uint8_t measuredBpm = 0;
    if (healthTelemetryModule && healthTelemetryModule->getCurrentHeartBpm(&measuredBpm) && measuredBpm >= 30 && measuredBpm <= 220 &&
        measuredBpm != currentBpm) {
        const double currentCycleMs = 60000.0 / (double)currentBpm;
        heartbeatOffsetMs = fmod((double)nowMs + heartbeatOffsetMs, currentCycleMs) * (double)currentBpm / (double)measuredBpm -
                            (double)nowMs;
        currentBpm = measuredBpm;
    }
#else
    (void)nowMs;
#endif
}

HeartbeatPixelThread::RgbColor HeartbeatPixelThread::colorFromHex(uint32_t color)
{
    return RgbColor{(uint8_t)((color >> 16) & 0xFF), (uint8_t)((color >> 8) & 0xFF), (uint8_t)(color & 0xFF)};
}

void HeartbeatPixelThread::setPixel(uint8_t index, const RgbColor &color, float brightness)
{
    const float scaledBrightness = kPixelMinBrightness[index] + (brightness * kPixelBrightnessRange[index]);
    // frameCurrentScale throttles the NEXT frame when the previous one exceeded what the boost can
    // supply. Applying it one frame late costs a single animation tick (kAnimationIntervalMs) and avoids
    // buffering the whole frame to measure it, which is not worth the RAM on this part.
    const float gain = kOutputScale * scaledBrightness * frameCurrentScale;
    const uint8_t red = (uint8_t)roundf((float)color.red * gain);
    const uint8_t green = (uint8_t)roundf((float)color.green * gain);
    const uint8_t blue = (uint8_t)roundf((float)color.blue * gain);
    encodePixel(index, red, green, blue);
}

void HeartbeatPixelThread::updateFrameCurrentScale()
{
    // Convert the frame's summed PWM codes into an estimated supply current, and derive the scale for the
    // next frame. See kFrameCurrentBudgetMilliAmps for the SPICE-derived limits this protects.
    const float mA = ((float)frameCodeSum / 255.0f) * kMilliAmpsPerChannelFull +
                     (float)kLedCount * kQuiescentMilliAmpsPerLed;
    if (mA > (float)kFrameCurrentBudgetMilliAmps) {
        const float headroom = (float)kFrameCurrentBudgetMilliAmps - (float)kLedCount * kQuiescentMilliAmpsPerLed;
        const float lit = mA - (float)kLedCount * kQuiescentMilliAmpsPerLed;
        float next = (headroom > 0.0f && lit > 0.0f) ? (headroom / lit) : 1.0f;
        // Ease toward the target rather than stepping, so a bright frame does not visibly flicker.
        frameCurrentScale = (frameCurrentScale * 0.5f) + (next * 0.5f);
        if (frameCurrentScale < 0.05f) {
            frameCurrentScale = 0.05f;
        }
    } else if (frameCurrentScale < 1.0f) {
        frameCurrentScale += 0.05f;
        if (frameCurrentScale > 1.0f) {
            frameCurrentScale = 1.0f;
        }
    }
    frameCodeSum = 0;
}

void HeartbeatPixelThread::encodePixel(uint8_t index, uint8_t red, uint8_t green, uint8_t blue)
{
    if (index >= kLedCount) {
        return;
    }

    frameCodeSum += (uint32_t)red + (uint32_t)green + (uint32_t)blue;

    rmt_data_t *pixel = &rmtFrame[index * kRmtItemsPerLed];
    encodeByteToRmt(green, pixel + (0 * kRmtItemsPerByte));
    encodeByteToRmt(red, pixel + (1 * kRmtItemsPerByte));
    encodeByteToRmt(blue, pixel + (2 * kRmtItemsPerByte));
}

void HeartbeatPixelThread::encodeByteToRmt(uint8_t value, rmt_data_t *dest)
{
    for (uint8_t bit = 0; bit < kRmtItemsPerByte; ++bit) {
        const bool one = (value & (1U << (7 - bit))) != 0;
        dest[bit].level0 = 1;
        dest[bit].duration0 = one ? 8 : 4;
        dest[bit].level1 = 0;
        dest[bit].duration1 = one ? 4 : 8;
    }
}

void HeartbeatPixelThread::showStrips()
{
    updateFrameCurrentScale();
    powerStrips(true);
    if (!rmtTx || !rmtWriteBlocking(rmtTx, rmtFrame, kLedCount * kRmtItemsPerLed)) {
        LOG_WARN("HeartbeatPixels failed to write RMT frame");
        return;
    }
    stripsAreDark = false;
}

void HeartbeatPixelThread::clearStrips()
{
    for (uint8_t i = 0; i < kLedCount; ++i) {
        encodePixel(i, 0, 0, 0);
    }
    showStrips();
    stripsAreDark = true;
}

void HeartbeatPixelThread::powerStrips(bool on)
{
    if (stripsPowered == on) {
        return;
    }
#ifdef HEARTBEAT_NEOPIXEL_POWER_PIN
    pinMode(HEARTBEAT_NEOPIXEL_POWER_PIN, OUTPUT);
    if (!on) {
        delay(1); // Let a preceding all-off NeoPixel frame latch before removing strip power.
    }
    digitalWrite(HEARTBEAT_NEOPIXEL_POWER_PIN, on ? HIGH : LOW);
    if (on) {
        // Wait for +5VL to actually reach the WS2812B's 3.5 V minimum before clocking a frame at it.
        //
        // This used to be delay(1), which is not enough. The rail is a TPS61040 boost feeding C4 plus the
        // board-wide +5VL pour through Q2, and C4's value is not specified anywhere in the hardware design
        // (the schematic Value field is the literal library symbol name "C_Polarized" and the BOM matches),
        // while its CP_Elec_8x10.5 footprint spans 100-470 uF. Ramp to 3.5 V is roughly 7 ms at 220 uF and
        // ~11 ms at 470 uF; at 1 ms the rail is still around 0.6 V. Clocking WS2812B data into a chain
        // whose supply is below spec gives wrong colours or dropped frames, and because it tracks the
        // animation it reads as a firmware bug.
        //
        // 30 ms, not 12, and the difference came from simulating it rather than estimating.
        //
        // A SPICE model of the extracted topology - Vext-D15 -> L1 -> D16 -> C3, Q2 high-side into C4,
        // with the converter represented by its DCM peak-current envelope - gives time-to-3.5 V of:
        //
        //     C4=100uF   2.8 - 4.3 ms      C4=330uF    9.0 - 13.6 ms
        //     C4=220uF   6.0 - 9.2 ms      C4=470uF   12.8 - 19.3 ms
        //
        // across VBAT 4.2 down to 3.0. A 500-run Monte Carlo over the datasheet peak-current spread
        // (250-550 mA), L1 +-20%, C4 across its whole unspecified range and VBAT 3.0-4.2 puts
        // P(rail not ready at 12 ms) at 29.2%, at 25 ms at 0.2%, and at 30 ms at 0.0%.
        //
        // The dominant uncertainty is C4, whose value is not specified anywhere in the hardware design.
        // If it is ever pinned down this can be tightened; until then the delay has to cover the whole
        // footprint range. It runs only on power-state transitions, never at frame cadence.
        delay(30);
    }
#else
    (void)on;
#endif
    stripsPowered = on;
}

int HeartbeatPixelThread::handleDeepSleep(void *unused)
{
    (void)unused;
    clearNotificationState();
    if (initialized) {
        clearStrips();
    }
    powerStrips(false);
    if (rmtTx) {
        rmtDeinit(rmtTx);
        rmtTx = nullptr;
    }
    initialized = false;
    return 0;
}

#endif
