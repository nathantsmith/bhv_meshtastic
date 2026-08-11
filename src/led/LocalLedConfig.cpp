#include "LocalLedConfig.h"

#include "Channels.h"
#include "FSCommon.h"
#include "RTC.h"
#include "SPILock.h"
#include "concurrency/OSThread.h"
#include "concurrency/LockGuard.h"
#include "led/LocalLedCommandParser.h"
#include "mesh/MeshService.h"
#include "mesh/NodeDB.h"
#include "mesh/Router.h"
#include "power/PowerHAL.h"
#ifdef HAS_HEARTBEAT_NEOPIXELS
#include "HeartbeatPixelThread.h"
#endif

#include <string.h>

namespace
{
static constexpr const char *kFileName = "/prefs/custom_led.bin";
static constexpr uint32_t kMagic = 0x434C4544;
static constexpr uint16_t kVersion = 9;
static constexpr size_t kLegacySerializedSize = 94;
static constexpr size_t kV5SerializedSize = 103;
static constexpr size_t kV6SerializedSize = 113;
static constexpr size_t kV7SerializedSize = 253;
static constexpr size_t kV8SerializedSize = 273;
static constexpr size_t kSerializedSize = 274;
static constexpr uint32_t kLocalReplyDelayMs = 0;
static constexpr uint32_t kLocalReplyTimestampOffsetSecs = 1;
static constexpr uint32_t kLocalLedCommandBotNode = 0x4C454421; // !4C454421, "LED!"

class LocalLedReplyDispatcher : private concurrency::OSThread
{
  public:
    LocalLedReplyDispatcher() : concurrency::OSThread("LocalLedReply", UINT32_MAX) { enabled = false; }

    bool enqueue(meshtastic_MeshPacket *packet)
    {
        if (!packet) {
            return false;
        }

        concurrency::LockGuard guard(&lock);
        uint8_t slot = kQueueSize;
        for (uint8_t i = 0; i < kQueueSize; ++i) {
            if (!pending[i].packet) {
                slot = i;
                break;
            }
        }
        if (slot >= kQueueSize) {
            return false;
        }

        pending[slot].packet = packet;
        pending[slot].due_ms = millis() + kLocalReplyDelayMs;
        enabled = true;
        setIntervalFromNow(getNextDelayLocked(millis()));
        return true;
    }

  protected:
    int32_t runOnce() override
    {
        meshtastic_MeshPacket *ready[kQueueSize] = {};
        uint8_t readyCount = 0;
        uint32_t now = millis();
        uint32_t nextDelay = UINT32_MAX;

        {
            concurrency::LockGuard guard(&lock);
            for (uint8_t i = 0; i < kQueueSize; ++i) {
                if (!pending[i].packet) {
                    continue;
                }
                if ((int32_t)(pending[i].due_ms - now) <= 0) {
                    ready[readyCount++] = pending[i].packet;
                    pending[i].packet = nullptr;
                    pending[i].due_ms = 0;
                    continue;
                }

                uint32_t delay = pending[i].due_ms - now;
                if (delay < nextDelay) {
                    nextDelay = delay;
                }
            }

            if (nextDelay == UINT32_MAX) {
                enabled = false;
            }
        }

        for (uint8_t i = 0; i < readyCount; ++i) {
            if (service) {
                service->sendToPhone(ready[i]);
            } else {
                packetPool.release(ready[i]);
            }
        }

        if (nextDelay == UINT32_MAX) {
            return disable();
        }
        return (int32_t)nextDelay;
    }

  private:
    static constexpr uint8_t kQueueSize = 4;

    struct PendingReply {
        meshtastic_MeshPacket *packet = nullptr;
        uint32_t due_ms = 0;
    };

    uint32_t getNextDelayLocked(uint32_t now) const
    {
        uint32_t nextDelay = UINT32_MAX;
        for (uint8_t i = 0; i < kQueueSize; ++i) {
            if (!pending[i].packet) {
                continue;
            }
            uint32_t delay = ((int32_t)(pending[i].due_ms - now) <= 0) ? 0 : (pending[i].due_ms - now);
            if (delay < nextDelay) {
                nextDelay = delay;
            }
        }
        return nextDelay == UINT32_MAX ? UINT32_MAX : nextDelay;
    }

    mutable concurrency::Lock lock;
    PendingReply pending[kQueueSize];
};

// Cheap HSV(hue, 1, 1)->RGB for building a legacy-compatible rainbow step list. Intentionally
// separate from HeartbeatPixelThread's own hsvToRgb: that one renders locally every 25ms and lives
// on a private class; this one runs a handful of times when a remote animation is scheduled.
uint32_t hueToHex(float hue)
{
    const float h6 = (hue - floorf(hue)) * 6.0f;
    const int sector = (int)h6;
    const float f = h6 - (float)sector;
    const uint8_t rise = (uint8_t)roundf(f * 255.0f);
    const uint8_t fall = (uint8_t)roundf((1.0f - f) * 255.0f);
    uint8_t r = 255, g = rise, b = 0;
    switch (sector) {
    case 1:
        r = fall; g = 255; b = 0;
        break;
    case 2:
        r = 0; g = 255; b = rise;
        break;
    case 3:
        r = 0; g = fall; b = 255;
        break;
    case 4:
        r = rise; g = 0; b = 255;
        break;
    case 5:
        r = 255; g = 0; b = fall;
        break;
    default:
        break;
    }
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

// Plays a visible effect on a DM peer using nothing but ordinary "#! set default color" commands,
// sent a few seconds apart, so it works even against a peer running unmodified upstream firmware
// that has no idea what a "gift" or LED pattern is. Necessarily leaves the peer's default color
// changed afterward (there is no over-the-air way to read back and later restore their prior
// color), so each sequence intentionally lands on the firmware's documented default blue/red.
class RemoteAnimatorThread : private concurrency::OSThread
{
  public:
    RemoteAnimatorThread() : concurrency::OSThread("RemoteAnimator", UINT32_MAX) { enabled = false; }

    bool startRainbow(uint32_t targetNode, uint8_t channelIndex)
    {
        if (enabled) {
            return false;
        }
        stepCount = 0;
        for (uint8_t i = 0; i < kRainbowSteps && stepCount < kMaxSteps - 1; ++i) {
            const float hue1 = (float)i / (float)kRainbowSteps;
            // Complementary (opposite side of the color wheel) so the badge's two LED halves read
            // as a distinct two-tone combo each step, instead of a single flat color.
            const uint32_t color1 = hueToHex(hue1);
            const uint32_t color2 = hueToHex(hue1 + 0.5f);
            snprintf(steps[stepCount], sizeof(steps[stepCount]), "#! set default color #%06X #%06X", (unsigned int)color1,
                     (unsigned int)color2);
            stepCount++;
        }
        appendLandingStep();
        return start(targetNode, channelIndex);
    }

    bool startBlink(uint32_t targetNode, uint8_t channelIndex, uint32_t color)
    {
        if (enabled) {
            return false;
        }
        stepCount = 0;
        for (uint8_t i = 0; i < kBlinkSteps && stepCount < kMaxSteps - 1; ++i) {
            const uint32_t stepColor = (i % 2 == 0) ? (color & 0xFFFFFF) : 0x000000;
            snprintf(steps[stepCount], sizeof(steps[stepCount]), "#! set default color #%06X", (unsigned int)stepColor);
            stepCount++;
        }
        appendLandingStep();
        return start(targetNode, channelIndex);
    }

  protected:
    int32_t runOnce() override
    {
        if (nextStep >= stepCount) {
            enabled = false;
            return disable();
        }
        sendStep(steps[nextStep]);
        nextStep++;
        if (nextStep >= stepCount) {
            enabled = false;
            return disable();
        }
        return (int32_t)kStepIntervalMs;
    }

  private:
    static constexpr uint8_t kRainbowSteps = 12;
    static constexpr uint8_t kBlinkSteps = 6;
    static constexpr uint8_t kMaxSteps = kRainbowSteps + 1;
    // Long enough that each color is clearly visible after the ~750ms green command-ack flash
    // that every "#! set default color" step triggers on the receiving badge.
    static constexpr uint32_t kStepIntervalMs = 10000;
    static constexpr uint32_t kStartDelayMs = 200;

    void appendLandingStep()
    {
        if (stepCount < kMaxSteps) {
            snprintf(steps[stepCount], sizeof(steps[stepCount]), "#! set default color #0000FF #FF0000");
            stepCount++;
        }
    }

    bool start(uint32_t targetNode, uint8_t channelIndex)
    {
        target = targetNode;
        channel = channelIndex;
        nextStep = 0;
        enabled = true;
        setIntervalFromNow(kStartDelayMs);
        return true;
    }

    void sendStep(const char *text)
    {
        if (!router) {
            return;
        }
        meshtastic_MeshPacket *p = router->allocForSending();
        if (!p) {
            return;
        }
        p->to = target;
        p->channel = channel;
        p->decoded.portnum = meshtastic_PortNum_TEXT_MESSAGE_APP;
        size_t length = strlen(text);
        if (length > sizeof(p->decoded.payload.bytes)) {
            length = sizeof(p->decoded.payload.bytes);
        }
        p->decoded.payload.size = length;
        memcpy(p->decoded.payload.bytes, text, length);
        if (service) {
            service->sendToMesh(p);
        } else {
            packetPool.release(p);
        }
    }

    uint32_t target = 0;
    uint8_t channel = 0;
    char steps[kMaxSteps][40] = {};
    uint8_t stepCount = 0;
    uint8_t nextStep = 0;
};

bool hasAnyChannelOverrides(const CustomLedConfig &config)
{
    for (size_t i = 0; i < 8; ++i) {
        if (config.channels[i].configured) {
            return true;
        }
    }
    if (config.direct_message.configured) {
        return true;
    }
    for (size_t i = 0; i < kLocalLedDirectMessageUserCapacity; ++i) {
        if (config.direct_message_nodes[i] != 0 && config.direct_message_users[i].configured) {
            return true;
        }
    }
    return false;
}

bool isLegacyWhiteDefaultConfig(const CustomLedConfig &config)
{
    return config.node_led1_color == 0xFFFFFF && config.node_led2_color == 0xFFFFFF && config.idle_bpm == 30 &&
           config.idle_delay_ms == 1000 && !hasAnyChannelOverrides(config);
}

bool isLegacyBlueRedDefaultConfig(const CustomLedConfig &config)
{
    return config.node_led1_color == 0x0000FF && config.node_led2_color == 0xFF0000 && config.idle_bpm == 30 &&
           config.idle_delay_ms == 1000 && !hasAnyChannelOverrides(config);
}

bool isCurrentBlueRedDefaultConfig(const CustomLedConfig &config)
{
    return config.node_led1_color == 0x0000FF && config.node_led2_color == 0xFF0000 && config.idle_bpm == 30 &&
           config.idle_delay_ms == 0 && !hasAnyChannelOverrides(config);
}

void writeUint16(uint8_t *buffer, size_t &offset, uint16_t value)
{
    buffer[offset++] = (uint8_t)(value & 0xFF);
    buffer[offset++] = (uint8_t)((value >> 8) & 0xFF);
}

void writeUint32(uint8_t *buffer, size_t &offset, uint32_t value)
{
    buffer[offset++] = (uint8_t)(value & 0xFF);
    buffer[offset++] = (uint8_t)((value >> 8) & 0xFF);
    buffer[offset++] = (uint8_t)((value >> 16) & 0xFF);
    buffer[offset++] = (uint8_t)((value >> 24) & 0xFF);
}

bool readUint16(const uint8_t *buffer, size_t length, size_t &offset, uint16_t *value)
{
    if (offset + 2 > length) {
        return false;
    }
    *value = (uint16_t)buffer[offset] | ((uint16_t)buffer[offset + 1] << 8);
    offset += 2;
    return true;
}

bool readUint32(const uint8_t *buffer, size_t length, size_t &offset, uint32_t *value)
{
    if (offset + 4 > length) {
        return false;
    }
    *value = (uint32_t)buffer[offset] | ((uint32_t)buffer[offset + 1] << 8) | ((uint32_t)buffer[offset + 2] << 16) |
             ((uint32_t)buffer[offset + 3] << 24);
    offset += 4;
    return true;
}
} // namespace

LocalLedConfigStore *localLedConfigStore = nullptr;
static LocalLedReplyDispatcher *localLedReplyDispatcher = nullptr;
static RemoteAnimatorThread *remoteAnimatorThread = nullptr;

LocalLedConfigStore::LocalLedConfigStore() : activeChannel(channels.getPrimaryIndex())
{
    applyDefaults(&config);
}

void LocalLedConfigStore::applyDefaults(CustomLedConfig *defaults)
{
    if (!defaults) {
        return;
    }

    defaults->node_led1_color = 0x0000FF;
    defaults->node_led2_color = 0xFF0000;
    defaults->idle_bpm = 80;
    defaults->idle_delay_ms = 0;
    defaults->notification_pulses = kLocalLedDefaultNotificationPulses;
    defaults->send_pulses = kLocalLedDefaultSendPulses;
    defaults->node_pattern = LED_PATTERN_SOLID;
    for (size_t i = 0; i < 8; ++i) {
        defaults->channels[i].led1_color = 0;
        defaults->channels[i].led2_color = 0;
        defaults->channels[i].notification_pulses = 0;
        defaults->channels[i].send_pulses = 0;
        defaults->channels[i].configured = false;
    }
    defaults->direct_message.led1_color = 0;
    defaults->direct_message.led2_color = 0;
    defaults->direct_message.notification_pulses = 0;
    defaults->direct_message.send_pulses = 0;
    defaults->direct_message.configured = false;
    for (size_t i = 0; i < kLocalLedDirectMessageUserCapacity; ++i) {
        defaults->direct_message_nodes[i] = 0;
        defaults->direct_message_users[i].led1_color = 0;
        defaults->direct_message_users[i].led2_color = 0;
        defaults->direct_message_users[i].notification_pulses = 0;
        defaults->direct_message_users[i].send_pulses = 0;
        defaults->direct_message_users[i].configured = false;
    }
}

bool LocalLedConfigStore::serializeConfig(const CustomLedConfig &source, uint8_t *buffer, size_t capacity, size_t *usedBytes)
{
    if (!buffer || capacity < kSerializedSize) {
        return false;
    }

    size_t offset = 0;
    writeUint32(buffer, offset, kMagic);
    writeUint16(buffer, offset, kVersion);
    writeUint16(buffer, offset, 8);
    writeUint32(buffer, offset, source.node_led1_color);
    writeUint32(buffer, offset, source.node_led2_color);
    writeUint16(buffer, offset, source.idle_bpm);
    writeUint32(buffer, offset, source.idle_delay_ms);
    buffer[offset++] = source.notification_pulses;
    buffer[offset++] = source.send_pulses;
    for (size_t i = 0; i < 8; ++i) {
        writeUint32(buffer, offset, source.channels[i].led1_color);
        writeUint32(buffer, offset, source.channels[i].led2_color);
        buffer[offset++] = source.channels[i].notification_pulses;
        buffer[offset++] = source.channels[i].send_pulses;
        buffer[offset++] = source.channels[i].configured ? 1 : 0;
    }
    writeUint32(buffer, offset, source.direct_message.led1_color);
    writeUint32(buffer, offset, source.direct_message.led2_color);
    buffer[offset++] = source.direct_message.notification_pulses;
    buffer[offset++] = source.direct_message.send_pulses;
    buffer[offset++] = source.direct_message.configured ? 1 : 0;
    for (size_t i = 0; i < kLocalLedDirectMessageUserCapacity; ++i) {
        writeUint32(buffer, offset, source.direct_message_nodes[i]);
        writeUint32(buffer, offset, source.direct_message_users[i].led1_color);
        writeUint32(buffer, offset, source.direct_message_users[i].led2_color);
        buffer[offset++] = source.direct_message_users[i].notification_pulses;
        buffer[offset++] = source.direct_message_users[i].send_pulses;
        buffer[offset++] = source.direct_message_users[i].configured ? 1 : 0;
    }
    buffer[offset++] = source.node_pattern;

    if (usedBytes) {
        *usedBytes = offset;
    }
    return true;
}

bool LocalLedConfigStore::deserializeConfig(const uint8_t *buffer, size_t length, CustomLedConfig *destination)
{
    if (!buffer || !destination || length < kLegacySerializedSize) {
        return false;
    }

    size_t offset = 0;
    uint32_t magic = 0;
    uint16_t version = 0;
    uint16_t channelCount = 0;
    if (!readUint32(buffer, length, offset, &magic) || !readUint16(buffer, length, offset, &version) ||
        !readUint16(buffer, length, offset, &channelCount)) {
        return false;
    }
    if (magic != kMagic || channelCount != 8 || (version < 1 || version > kVersion) ||
        (version >= 5 && length < kV5SerializedSize) || (version >= 6 && length < kV6SerializedSize) ||
        (version >= 7 && length < kV7SerializedSize) || (version >= 8 && length < kV8SerializedSize) ||
        (version >= 9 && length < kSerializedSize)) {
        return false;
    }

    CustomLedConfig decoded = {};
    applyDefaults(&decoded);
    if (!readUint32(buffer, length, offset, &decoded.node_led1_color) ||
        !readUint32(buffer, length, offset, &decoded.node_led2_color) || !readUint16(buffer, length, offset, &decoded.idle_bpm) ||
        !readUint32(buffer, length, offset, &decoded.idle_delay_ms)) {
        return false;
    }
    if (version >= 5) {
        if (offset >= length) {
            return false;
        }
        decoded.notification_pulses = buffer[offset++];
        if (version >= 8) {
            if (offset >= length) {
                return false;
            }
            decoded.send_pulses = buffer[offset++];
        }
    }
    for (size_t i = 0; i < 8; ++i) {
        if (!readUint32(buffer, length, offset, &decoded.channels[i].led1_color) ||
            !readUint32(buffer, length, offset, &decoded.channels[i].led2_color) || offset >= length) {
            return false;
        }
        if (version >= 5) {
            decoded.channels[i].notification_pulses = buffer[offset++];
            if (version >= 8) {
                if (offset >= length) {
                    return false;
                }
                decoded.channels[i].send_pulses = buffer[offset++];
            }
            if (offset >= length) {
                return false;
            }
        }
        decoded.channels[i].configured = buffer[offset++] != 0;
    }
    if (version >= 6) {
        if (!readUint32(buffer, length, offset, &decoded.direct_message.led1_color) ||
            !readUint32(buffer, length, offset, &decoded.direct_message.led2_color) || offset >= length) {
            return false;
        }
        decoded.direct_message.notification_pulses = buffer[offset++];
        if (version >= 8) {
            if (offset >= length) {
                return false;
            }
            decoded.direct_message.send_pulses = buffer[offset++];
        }
        if (offset >= length) {
            return false;
        }
        decoded.direct_message.configured = buffer[offset++] != 0;
    }
    if (version >= 7) {
        for (size_t i = 0; i < kLocalLedDirectMessageUserCapacity; ++i) {
            if (!readUint32(buffer, length, offset, &decoded.direct_message_nodes[i]) ||
                !readUint32(buffer, length, offset, &decoded.direct_message_users[i].led1_color) ||
                !readUint32(buffer, length, offset, &decoded.direct_message_users[i].led2_color) || offset >= length) {
                return false;
            }
            decoded.direct_message_users[i].notification_pulses = buffer[offset++];
            if (version >= 8) {
                if (offset >= length) {
                    return false;
                }
                decoded.direct_message_users[i].send_pulses = buffer[offset++];
            }
            if (offset >= length) {
                return false;
            }
            decoded.direct_message_users[i].configured = buffer[offset++] != 0;
            if (decoded.direct_message_nodes[i] == 0) {
                decoded.direct_message_users[i].configured = false;
                decoded.direct_message_users[i].notification_pulses = 0;
                decoded.direct_message_users[i].send_pulses = 0;
            }
        }
    }
    if (version >= 9) {
        if (offset >= length) {
            return false;
        }
        decoded.node_pattern = buffer[offset++];
    }

    if (decoded.idle_bpm < 1 || decoded.idle_bpm > 600 || decoded.idle_delay_ms > 600000 ||
        decoded.notification_pulses < 1 || decoded.notification_pulses > kLocalLedMaxNotificationPulses ||
        decoded.send_pulses < 1 || decoded.send_pulses > kLocalLedMaxNotificationPulses || decoded.node_pattern > kLedPatternMax) {
        return false;
    }
    for (size_t i = 0; i < 8; ++i) {
        if (decoded.channels[i].notification_pulses > kLocalLedMaxNotificationPulses ||
            decoded.channels[i].send_pulses > kLocalLedMaxNotificationPulses) {
            return false;
        }
    }
    if (decoded.direct_message.notification_pulses > kLocalLedMaxNotificationPulses ||
        decoded.direct_message.send_pulses > kLocalLedMaxNotificationPulses) {
        return false;
    }
    for (size_t i = 0; i < kLocalLedDirectMessageUserCapacity; ++i) {
        if (decoded.direct_message_users[i].notification_pulses > kLocalLedMaxNotificationPulses ||
            decoded.direct_message_users[i].send_pulses > kLocalLedMaxNotificationPulses) {
            return false;
        }
    }

    if (version == 1 && isLegacyWhiteDefaultConfig(decoded)) {
        decoded.node_led1_color = 0x0000FF;
        decoded.node_led2_color = 0xFF0000;
        decoded.idle_delay_ms = 0;
    }

    if ((version == 1 || version == 2) && isLegacyBlueRedDefaultConfig(decoded)) {
        decoded.idle_bpm = 80;
        decoded.idle_delay_ms = 0;
    }

    if (version == 3 && isCurrentBlueRedDefaultConfig(decoded)) {
        decoded.idle_delay_ms = 0;
        decoded.idle_bpm = 80;
    }

    *destination = decoded;
    return true;
}

bool LocalLedConfigStore::isSupportedChannel(uint8_t channel)
{
    return channel < 8;
}

void LocalLedConfigStore::load()
{
    CustomLedConfig loaded = {};
    applyDefaults(&loaded);

#ifdef FSCom
    concurrency::LockGuard fileGuard(spiLock);
    auto file = FSCom.open(kFileName, FILE_O_READ);
    if (file) {
        uint8_t buffer[kSerializedSize] = {};
        size_t bytesRead = file.read(buffer, sizeof(buffer));
        file.close();
        if (!deserializeConfig(buffer, bytesRead, &loaded)) {
            applyDefaults(&loaded);
        }
    }
#endif

    concurrency::LockGuard configGuard(&lock);
    config = loaded;
    activeChannel = channels.getPrimaryIndex();
}

bool LocalLedConfigStore::save()
{
    if (!powerHAL_isPowerLevelSafe()) {
        return false;
    }

    CustomLedConfig snapshot = {};
    {
        concurrency::LockGuard guard(&lock);
        snapshot = config;
    }

#ifdef FSCom
    uint8_t buffer[kSerializedSize] = {};
    size_t usedBytes = 0;
    if (!serializeConfig(snapshot, buffer, sizeof(buffer), &usedBytes)) {
        return false;
    }

    concurrency::LockGuard guard(spiLock);
    FSCom.mkdir("/prefs");
    if (FSCom.exists(kFileName)) {
        FSCom.remove(kFileName);
    }
    auto file = FSCom.open(kFileName, FILE_O_WRITE);
    if (!file) {
        return false;
    }
    size_t written = file.write(buffer, usedBytes);
    file.flush();
    file.close();
    return written == usedBytes;
#else
    return true;
#endif
}

bool LocalLedConfigStore::handleCommand(const char *text, const LocalLedCommandContext &context, LocalLedCommandResult *result)
{
    if (!result) {
        return false;
    }

    LocalLedCommandResult localResult = {};
    CustomLedConfig working = {};
    {
        concurrency::LockGuard guard(&lock);
        working = config;
    }

    if (!handleLocalLedCommand(working, context, text, &localResult)) {
        *result = localResult;
        return false;
    }

    // SECURITY: an over-the-air command must not silently mutate/persist this node's
    // LED configuration. Only locally-originated commands (from the node's own
    // connected client) may mutate, unless the operator explicitly opts in to
    // accepting over-air LED commands (e.g. for consensual light shows). This honors
    // local_client_origin, which was previously plumbed through the context but unused.
    if (localResult.persist && !context.local_client_origin) {
#if defined(USERPREFS_BHV_ACCEPT_OVER_AIR_LED) && USERPREFS_BHV_ACCEPT_OVER_AIR_LED
        // operator has opted in: allow the over-air mutation to apply
#else
        localResult.persist = false;      // ignore the mutation
        localResult.consume_packet = true; // still consume it so it is not shown as chat
#endif
    }

    {
        concurrency::LockGuard guard(&lock);
        if (context.has_resolved_incoming_channel && isSupportedChannel(context.resolved_incoming_channel)) {
            activeChannel = context.resolved_incoming_channel;
        }
        if (localResult.persist) {
            config = working;
        }
    }

    if (localResult.persist) {
        save();
    }

    // Resolved before the ack-flash decision below, since whether the animator actually accepted the
    // job (it refuses a second job while one is still in flight) can only be known here, not by the
    // transport-agnostic parser that produced the optimistic "OK" response text.
    if (localResult.trigger_remote_animation && remoteAnimatorThread) {
        const uint32_t animTarget = localResult.remote_animation_broadcast ? NODENUM_BROADCAST : localResult.remote_animation_target;
        bool started;
        if (localResult.remote_animation_kind == REMOTE_ANIM_BLINK) {
            started = remoteAnimatorThread->startBlink(animTarget, localResult.remote_animation_channel,
                                                        localResult.remote_animation_color);
        } else {
            started = remoteAnimatorThread->startRainbow(animTarget, localResult.remote_animation_channel);
        }
        if (!started) {
            snprintf(localResult.response, sizeof(localResult.response), "ERR animation already in progress");
        }
    }

#ifdef HAS_HEARTBEAT_NEOPIXELS
    if (heartbeatPixelThread) {
        if (localResult.trigger_gift) {
            heartbeatPixelThread->enqueueGiftPattern(localResult.gift_pattern, localResult.gift_color1, localResult.gift_color2);
        } else if (strncmp(localResult.response, "OK", 2) == 0) {
            heartbeatPixelThread->enqueueCommandStatusPattern(true);
        } else if (strncmp(localResult.response, "ERR", 3) == 0) {
            heartbeatPixelThread->enqueueCommandStatusPattern(false);
        }
    }
#endif

    *result = localResult;
    return localResult.handled;
}

void LocalLedConfigStore::setActiveChannel(uint8_t channel)
{
    if (!isSupportedChannel(channel)) {
        return;
    }

    concurrency::LockGuard guard(&lock);
    activeChannel = channel;
}

uint8_t LocalLedConfigStore::getActiveChannel() const
{
    concurrency::LockGuard guard(&lock);
    return activeChannel;
}

CustomLedConfig LocalLedConfigStore::getConfig() const
{
    concurrency::LockGuard guard(&lock);
    return config;
}

LocalLedEffectiveConfig LocalLedConfigStore::getEffectiveConfigForChannel(uint8_t channel) const
{
    CustomLedConfig snapshot = {};
    {
        concurrency::LockGuard guard(&lock);
        snapshot = config;
    }

    uint8_t resolvedChannel = isSupportedChannel(channel) ? channel : channels.getPrimaryIndex();
    LocalLedEffectiveConfig effective = {
        snapshot.node_led1_color,
        snapshot.node_led2_color,
        snapshot.idle_bpm,
        snapshot.idle_delay_ms,
        snapshot.notification_pulses,
        snapshot.send_pulses,
        false,
        resolvedChannel,
        LED_PATTERN_SOLID,
    };
    if (snapshot.channels[resolvedChannel].configured) {
        effective.led1_color = snapshot.channels[resolvedChannel].led1_color;
        effective.led2_color = snapshot.channels[resolvedChannel].led2_color;
        if (snapshot.channels[resolvedChannel].notification_pulses > 0) {
            effective.notification_pulses = snapshot.channels[resolvedChannel].notification_pulses;
        }
        if (snapshot.channels[resolvedChannel].send_pulses > 0) {
            effective.send_pulses = snapshot.channels[resolvedChannel].send_pulses;
        }
        effective.configured = true;
    }
    return effective;
}

LocalLedEffectiveConfig LocalLedConfigStore::getEffectiveConfigForDirectMessage() const
{
    CustomLedConfig snapshot = {};
    {
        concurrency::LockGuard guard(&lock);
        snapshot = config;
    }

    LocalLedEffectiveConfig effective = {
        snapshot.node_led1_color,
        snapshot.node_led2_color,
        snapshot.idle_bpm,
        snapshot.idle_delay_ms,
        snapshot.notification_pulses,
        snapshot.send_pulses,
        false,
        kLocalLedDirectMessageIndex,
        LED_PATTERN_SOLID,
    };
    if (snapshot.direct_message.configured) {
        effective.led1_color = snapshot.direct_message.led1_color;
        effective.led2_color = snapshot.direct_message.led2_color;
        if (snapshot.direct_message.notification_pulses > 0) {
            effective.notification_pulses = snapshot.direct_message.notification_pulses;
        }
        if (snapshot.direct_message.send_pulses > 0) {
            effective.send_pulses = snapshot.direct_message.send_pulses;
        }
        effective.configured = true;
    }
    return effective;
}

LocalLedEffectiveConfig LocalLedConfigStore::getEffectiveConfigForDirectMessage(uint32_t nodeNum) const
{
    CustomLedConfig snapshot = {};
    {
        concurrency::LockGuard guard(&lock);
        snapshot = config;
    }

    LocalLedEffectiveConfig effective = {
        snapshot.node_led1_color,
        snapshot.node_led2_color,
        snapshot.idle_bpm,
        snapshot.idle_delay_ms,
        snapshot.notification_pulses,
        snapshot.send_pulses,
        false,
        kLocalLedDirectMessageIndex,
        LED_PATTERN_SOLID,
    };
    const ChannelLedConfig *dmConfig = snapshot.direct_message.configured ? &snapshot.direct_message : nullptr;
    int8_t userSlot = -1;
    for (size_t i = 0; i < kLocalLedDirectMessageUserCapacity; ++i) {
        if (snapshot.direct_message_nodes[i] == nodeNum) {
            userSlot = (int8_t)i;
            if (snapshot.direct_message_users[i].configured) {
                dmConfig = &snapshot.direct_message_users[i];
            }
            break;
        }
    }
    if (dmConfig) {
        effective.led1_color = dmConfig->led1_color;
        effective.led2_color = dmConfig->led2_color;
        if (userSlot >= 0) {
            effective.channel_index = kLocalLedDirectMessageUserBaseIndex + (uint8_t)userSlot;
        }
        if (userSlot >= 0 && snapshot.direct_message_users[userSlot].notification_pulses > 0) {
            effective.notification_pulses = snapshot.direct_message_users[userSlot].notification_pulses;
        }
        if (userSlot >= 0 && snapshot.direct_message_users[userSlot].send_pulses > 0) {
            effective.send_pulses = snapshot.direct_message_users[userSlot].send_pulses;
        }
        if (dmConfig->notification_pulses > 0 && !(userSlot >= 0 && snapshot.direct_message_users[userSlot].notification_pulses > 0)) {
            effective.notification_pulses = dmConfig->notification_pulses;
        }
        if (dmConfig->send_pulses > 0 && !(userSlot >= 0 && snapshot.direct_message_users[userSlot].send_pulses > 0)) {
            effective.send_pulses = dmConfig->send_pulses;
        }
        effective.configured = true;
    }
    return effective;
}

LocalLedEffectiveConfig LocalLedConfigStore::getEffectiveConfigForActiveChannel() const
{
    return getEffectiveConfigForChannel(getActiveChannel());
}

meshtastic_MeshPacket *LocalLedConfigStore::createLocalReplyPacket(const char *text, uint8_t channel, uint32_t requestId,
                                                                   uint32_t from, uint32_t to) const
{
    if (!text) {
        return nullptr;
    }

    meshtastic_MeshPacket *packet = packetPool.allocZeroed();
    if (!packet) {
        return nullptr;
    }

    packet->which_payload_variant = meshtastic_MeshPacket_decoded_tag;
    packet->id = generatePacketId();
    packet->from = from;
    packet->to = to;
    packet->channel = isSupportedChannel(channel) ? channel : channels.getPrimaryIndex();
    packet->rx_time = getValidTime(RTCQualityFromNet) + kLocalReplyTimestampOffsetSecs;
    packet->decoded.portnum = meshtastic_PortNum_TEXT_MESSAGE_APP;
    packet->decoded.request_id = requestId;

    size_t length = strlen(text);
    if (length > sizeof(packet->decoded.payload.bytes)) {
        length = sizeof(packet->decoded.payload.bytes);
    }
    packet->decoded.payload.size = length;
    memcpy(packet->decoded.payload.bytes, text, length);
    return packet;
}

meshtastic_MeshPacket *LocalLedConfigStore::createLocalAckPacket(uint8_t channel, uint32_t requestId) const
{
    meshtastic_MeshPacket *packet = packetPool.allocZeroed();
    if (!packet) {
        return nullptr;
    }

    meshtastic_Routing routing = meshtastic_Routing_init_default;
    routing.error_reason = meshtastic_Routing_Error_NONE;
    routing.which_variant = meshtastic_Routing_error_reason_tag;

    packet->which_payload_variant = meshtastic_MeshPacket_decoded_tag;
    packet->id = generatePacketId();
    packet->from = nodeDB->getNodeNum();
    packet->to = nodeDB->getNodeNum();
    packet->channel = isSupportedChannel(channel) ? channel : channels.getPrimaryIndex();
    packet->rx_time = getValidTime(RTCQualityFromNet);
    packet->priority = meshtastic_MeshPacket_Priority_ACK;
    packet->decoded.portnum = meshtastic_PortNum_ROUTING_APP;
    packet->decoded.request_id = requestId;
    packet->decoded.payload.size =
        pb_encode_to_bytes(packet->decoded.payload.bytes, sizeof(packet->decoded.payload.bytes), &meshtastic_Routing_msg, &routing);
    return packet;
}

bool localLedResolveIncomingChannel(const meshtastic_MeshPacket &packet, uint8_t *channelOut)
{
    uint8_t channel = packet.channel ? packet.channel : channels.getPrimaryIndex();
    if (channel >= 8) {
        return false;
    }
    if (channelOut) {
        *channelOut = channel;
    }
    return true;
}

void setupLocalLedConfigStore()
{
    if (!localLedConfigStore) {
        localLedConfigStore = new LocalLedConfigStore();
    }
    if (!localLedReplyDispatcher) {
        localLedReplyDispatcher = new LocalLedReplyDispatcher();
    }
    if (!remoteAnimatorThread) {
        remoteAnimatorThread = new RemoteAnimatorThread();
    }
    localLedConfigStore->load();
}

bool enqueueLocalLedReplyPacket(meshtastic_MeshPacket *packet)
{
    return localLedReplyDispatcher && localLedReplyDispatcher->enqueue(packet);
}

bool handleLocalLedPhoneCommand(const meshtastic_MeshPacket &packet, meshtastic_MeshPacket **replyPacket)
{
    if (replyPacket) {
        *replyPacket = nullptr;
    }
    if (!localLedConfigStore || packet.which_payload_variant != meshtastic_MeshPacket_decoded_tag ||
        packet.decoded.portnum != meshtastic_PortNum_TEXT_MESSAGE_APP || packet.decoded.payload.size == 0) {
        return false;
    }

    char text[256] = {};
    size_t length = packet.decoded.payload.size;
    if (length > sizeof(text) - 1) {
        length = sizeof(text) - 1;
    }
    memcpy(text, packet.decoded.payload.bytes, length);

    uint8_t resolvedChannel = 0;
    LocalLedCommandContext context = {
        localLedResolveIncomingChannel(packet, &resolvedChannel),
        resolvedChannel,
        packet.to != 0 && !isBroadcast(packet.to),
        packet.to,
        true,
        packet.id,
        resolvedChannel,
    };
    LocalLedCommandResult result = {};
    if (!localLedConfigStore->handleCommand(text, context, &result)) {
        return false;
    }

    meshtastic_MeshPacket *ackPacket = localLedConfigStore->createLocalAckPacket(context.reply_channel, packet.id);
    if (ackPacket && service) {
        service->sendToPhone(ackPacket);
    } else if (ackPacket) {
        packetPool.release(ackPacket);
    }

    if (replyPacket && result.response[0] != '\0') {
        uint32_t localNode = nodeDB->getNodeNum();
        uint32_t replyFrom = kLocalLedCommandBotNode;
        uint32_t replyTo = NODENUM_BROADCAST;
        if (context.has_direct_message_peer && context.direct_message_peer != localNode) {
            replyFrom = context.direct_message_peer;
            replyTo = localNode;
        }
        *replyPacket = localLedConfigStore->createLocalReplyPacket(result.response, context.reply_channel, packet.id, replyFrom, replyTo);
    }

    return true;
}
