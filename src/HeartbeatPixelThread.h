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
        uint32_t startMs = 0;
        uint32_t durationMs = 0;
    };

    static constexpr uint8_t kCountPerStrip = HEARTBEAT_NEOPIXEL_COUNT_PER_STRIP;
    static constexpr uint8_t kLedCount = kCountPerStrip * 2;
    static constexpr uint32_t kAnimationIntervalMs = 25;
    /**
     * Global LED brightness scale. This is SUPPLY-LIMITED, not an aesthetic choice - do not raise it.
     *
     * The +5VL rail comes from a TPS61040 (U1) running in DCM peak-current PFM off Vext through D15.
     * Its deliverable output current is only about 105 mA typical and roughly 57 mA on a low battery -
     * far below the ~840 mA a naive "14 x 60 mA" reading of the WS2812B datasheet suggests, because the
     * boost simply cannot source it.
     *
     * At 0.35 with the default single-colour-channel patterns the animation peaks near 55 mA, which
     * already leaves only a few percent of margin on a low battery. Raising this scale, or selecting a
     * white preset (all three channels lit), pushes demand past what the boost can supply; +5VL then
     * collapses on animation peaks, the chain browns out below the WS2812B's 3.5 V minimum, and the
     * symptom looks like a firmware bug because it tracks the animation rate.
     *
     * A per-frame current budget in applyFrame() would make bright presets safe by scaling them down
     * instead of browning out; until that exists, this constant is the only thing holding the line.
     */
    static constexpr float kOutputScale = 0.35f;

    /*
     * Supply-limited current budget for the whole LED frame.
     *
     * +5VL comes from a TPS61040 in DCM peak-current PFM. SPICE modelling of the extracted topology
     * (Vext-D15 -> L1 -> D16 -> C3, Q2 high-side to +5VL) puts the maximum sustainable load before the
     * rail falls below the WS2812B's 3.5 V minimum at:
     *
     *     VBAT 4.2  Ipk 550mA -> 203 mA        VBAT 3.7  Ipk 400mA -> 130 mA
     *     VBAT 3.4  Ipk 400mA -> 119 mA        VBAT 3.0  Ipk 250mA ->  67 mA   <- worst corner
     *
     * The shipped animation peaks near 55 mA, leaving only 1.21x headroom in that worst corner. The
     * user-selectable white preset draws ~137 mA and browns the rail out in every corner except a full
     * battery with a best-case part - simulated sag 1.06 V at nominal.
     *
     * So the frame current is capped rather than left to collapse the supply: a frame that would exceed
     * the budget is scaled down uniformly, which makes bright presets legal-but-dimmer instead of
     * rail-collapsing. 55 mA keeps ~20% margin below the 67 mA worst corner.
     */
    static constexpr uint16_t kFrameCurrentBudgetMilliAmps = 55;
    /** WS2812B: ~20 mA per colour channel at full PWM, plus ~1 mA quiescent per device. */
    static constexpr float kMilliAmpsPerChannelFull = 20.0f;
    static constexpr float kQuiescentMilliAmpsPerLed = 1.0f;
    static constexpr uint16_t kStartupBpm = 80;
    static constexpr uint32_t kStartupDurationMs = 1100;
    static constexpr uint8_t kNotificationQueueSize = 8;
    static constexpr uint8_t kPatternQueueSize = 4;
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
    bool enqueuePattern(const LedPulseConfig *config, uint32_t color, uint32_t durationMs);
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
    /** Running sum of PWM codes for the frame being built, used for the supply current budget. */
    uint32_t frameCodeSum = 0;
    /** Scale applied to the NEXT frame if the last one exceeded the budget; 1.0 = unrestricted. */
    float frameCurrentScale = 1.0f;
    void updateFrameCurrentScale();
    static void encodeByteToRmt(uint8_t value, rmt_data_t *dest);
    static RgbColor colorFromHex(uint32_t color);
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
