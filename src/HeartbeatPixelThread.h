#pragma once

#ifdef HAS_HEARTBEAT_NEOPIXELS

#include "Observer.h"
#include "led/LocalLedConfig.h"
#include "concurrency/OSThread.h"
#include "concurrency/Lock.h"
#include "sleep.h"
#include <esp32-hal-rmt.h>

#ifndef HEARTBEAT_NEOPIXEL_COUNT_PER_STRIP
#define HEARTBEAT_NEOPIXEL_COUNT_PER_STRIP 7
#endif

#ifndef HEARTBEAT_NEOPIXEL_TYPE
#define HEARTBEAT_NEOPIXEL_TYPE (NEO_GRB + NEO_KHZ800)
#endif

class HeartbeatPixelThread : private concurrency::OSThread
{
  public:
    HeartbeatPixelThread();
    bool enqueueChannelNotification(uint8_t channel);
    bool enqueueDirectMessageNotification();
    bool enqueueDirectMessageNotification(uint32_t nodeNum);
    bool enqueueChannelSendNotification(uint8_t channel);
    bool enqueueDirectMessageSendNotification(uint32_t nodeNum);
    bool enqueueCommandStatusPattern(bool accepted);
    bool enqueueGiftPattern(uint8_t patternType, uint32_t color1, uint32_t color2);

  protected:
    int32_t runOnce() override;
    bool shouldRun(unsigned long time) override;
    long tillRun(unsigned long time) override;

  private:
    struct LedPulseConfig {
        float startTime;
        float pulseWidth;
    };

    struct RgbColor {
        uint8_t red;
        uint8_t green;
        uint8_t blue;
    };

    struct PendingNotification {
        bool used = false;
        uint8_t channel = 0;
        RgbColor led1Color = {};
        RgbColor led2Color = {};
        uint8_t pulseCount = kLocalLedDefaultNotificationPulses;
        double eligibleProgress = 0.0;
        bool led1Loaded = false;
        bool led2Loaded = false;
    };

    struct LedNotificationWindow {
        bool active = false;
        double startProgress = 0.0;
        double endProgress = 0.0;
    };

    struct NotificationLane {
        bool active = false;
        uint8_t channel = 0;
        RgbColor color = {};
        double laneEndProgress = 0.0;
        LedNotificationWindow ledWindows[HEARTBEAT_NEOPIXEL_COUNT_PER_STRIP * 2] = {};
    };

    struct NotificationSequenceTiming {
        double startOffset = 0.0;
        double endOffset = 0.0;
        double ledStartOffsets[HEARTBEAT_NEOPIXEL_COUNT_PER_STRIP * 2] = {};
    };

    struct PatternEvent {
        bool active = false;
        const LedPulseConfig *config = nullptr;
        RgbColor color = {};
        RgbColor color2 = {};
        uint8_t patternType = LED_PATTERN_SOLID;
        uint32_t startMs = 0;
        uint32_t durationMs = 0;
    };

    static constexpr uint8_t kCountPerStrip = HEARTBEAT_NEOPIXEL_COUNT_PER_STRIP;
    static constexpr uint8_t kLedCount = kCountPerStrip * 2;
    static constexpr uint32_t kAnimationIntervalMs = 25;
    static constexpr float kOutputScale = 0.35f;
    static constexpr uint16_t kStartupBpm = 80;
    static constexpr uint32_t kStartupDurationMs = 1100;
    static constexpr uint8_t kNotificationQueueSize = 8;
    static constexpr uint8_t kPatternQueueSize = 4;
    static constexpr uint32_t kGiftDurationMs = 3000;
    static constexpr uint8_t kLed1SequenceLength = 7;
    static constexpr uint8_t kLed2SequenceLength = 7;
    static constexpr uint8_t kBytesPerLed = 3;
    static constexpr uint8_t kRmtItemsPerByte = 8;
    static constexpr uint8_t kRmtItemsPerLed = kBytesPerLed * kRmtItemsPerByte;
    static constexpr double kInactiveSequenceOffset = -1.0;
    static const float kPixelMinBrightness[kLedCount];
    static const float kPixelMaxBrightness[kLedCount];
    static const float kPixelBrightnessRange[kLedCount];
    static const bool kPixelUsesLed1Color[kLedCount];
    static const uint8_t kLed1NotificationSequence[kLed1SequenceLength];
    static const uint8_t kLed2NotificationSequence[kLed2SequenceLength];

    rmt_obj_t *rmtTx = nullptr;
    rmt_data_t rmtFrame[kLedCount * kRmtItemsPerLed] = {};
    mutable concurrency::Lock notificationLock;
    PendingNotification notificationQueue[kNotificationQueueSize];
    PatternEvent patternQueue[kPatternQueueSize];
    uint8_t notificationQueueHead = 0;
    uint8_t notificationQueueCount = 0;
    uint8_t patternQueueHead = 0;
    uint8_t patternQueueCount = 0;
    PatternEvent activePattern;
    NotificationLane led1NotificationLane;
    NotificationLane led2NotificationLane;
    NotificationSequenceTiming led1SequenceTiming;
    NotificationSequenceTiming led2SequenceTiming;

    bool initialized = false;
    bool runningStartup = true;
    bool stripsAreDark = true;
    bool stripsPowered = false;
    bool hasNotificationProgress = false;
    uint32_t startupStartMs = 0;
    uint32_t nextFrameMs = 0;
    double heartbeatOffsetMs = 0.0;
    double lastNotificationPhase = 0.0;
    double notificationProgress = 0.0;
    uint16_t currentBpm = 80;

    CallbackObserver<HeartbeatPixelThread, void *> notifyDeepSleepObserver =
        CallbackObserver<HeartbeatPixelThread, void *>(this, &HeartbeatPixelThread::handleDeepSleep);

    void initializeHardware();
    void initializeNotificationSequenceTimings();
    void renderStartupFrame(uint32_t nowMs);
    bool renderPatternFrame(uint32_t nowMs);
    void renderHeartbeatFrame(uint32_t nowMs, const LocalLedEffectiveConfig &effective, bool baseHeartbeatEnabled);
    void idleOff();
    void applyFrame(double cycleTimeMs, double activeWindowMs, double currentTimeMs, const LedPulseConfig *config,
                    const LocalLedEffectiveConfig &effective, bool baseHeartbeatEnabled = true);
    void updateNotificationSequences(double cycleTimeMs, double activeWindowMs, double currentTimeMs, bool allowNewNotifications);
    void processNotificationLane(NotificationLane &lane, const NotificationSequenceTiming &timing, bool useLed1Color,
                                 double previousProgress, double currentProgress, double activeWindowScale);
    bool loadNotificationLane(NotificationLane &lane, const NotificationSequenceTiming &timing, bool useLed1Color,
                              double laneStartProgress, double activeWindowScale);
    bool notificationChannelIsPendingLocked(uint8_t channel) const;
    bool notificationStateNeedsRender() const;
    bool enqueueNotification(const LocalLedEffectiveConfig &effective, uint8_t pulseCount);
    bool enqueuePattern(const LedPulseConfig *config, uint32_t color, uint32_t durationMs, uint8_t patternType = LED_PATTERN_SOLID,
                        uint32_t color2 = 0);
    PendingNotification *notificationQueueFrontLocked();
    void popNotificationQueueLocked();
    PatternEvent *patternQueueFrontLocked();
    void popPatternQueueLocked();
    void clearNotificationState();
    static bool crossedProgressPhase(double previousProgress, double currentProgress, double phase);
    static bool notificationAppliesToPixel(const NotificationLane &lane, uint8_t ledIndex, double currentProgress);
    static double sequenceOffsetToProgressOffset(const NotificationSequenceTiming &timing, double sequenceOffset,
                                                 double activeWindowScale);
    static NotificationSequenceTiming calculateNotificationSequenceTiming(const uint8_t *sequence, uint8_t sequenceLength);
    float calculateBrightness(double cycleTimeMs, double currentTimeMs, double startTimeMs, double pulseWidthMs) const;
    void syncBpm(uint32_t nowMs);
    void setPixel(uint8_t index, const RgbColor &color, float brightness);
    void encodePixel(uint8_t index, uint8_t red, uint8_t green, uint8_t blue);
    static void encodeByteToRmt(uint8_t value, rmt_data_t *dest);
    static RgbColor colorFromHex(uint32_t color);
    static RgbColor hsvToRgb(float hue, float saturation, float value);
    void applyPatternPixel(uint8_t patternType, uint8_t ledIndex, const RgbColor &baseColor, const RgbColor &altColor,
                           uint32_t nowMs, uint32_t patternStartMs, float envelopeBrightness, RgbColor *colorOut,
                           float *brightnessOut) const;
    void showStrips();
    void clearStrips();
    void powerStrips(bool on);
    int handleDeepSleep(void *unused);

    static const LedPulseConfig startupConfig[kLedCount];
    static const LedPulseConfig originalStartupConfig[kLedCount];
    static const LedPulseConfig heartbeatConfig[kLedCount];
};

extern HeartbeatPixelThread *heartbeatPixelThread;

#endif
