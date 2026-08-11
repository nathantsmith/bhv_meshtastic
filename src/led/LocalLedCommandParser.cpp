#include "led/LocalLedCommandParser.h"

#if !MESHTASTIC_EXCLUDE_HEALTH_TELEMETRY && !defined(ARCH_PORTDUINO)
#include "modules/Telemetry/HealthTelemetry.h"
#endif

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

namespace
{
struct NamedColor {
    const char *name;
    uint32_t value;
};

static const NamedColor kNamedColors[] = {
    {"red", 0xFF0000},       {"orange", 0xFF8000}, {"yellow", 0xFFFF00}, {"green", 0x00FF00},    {"blue", 0x0000FF},
    {"indigo", 0x4B0082},    {"violet", 0x8F00FF}, {"purple", 0x8000FF}, {"pink", 0xFF4080},     {"white", 0xFFFFFF},
    {"warmwhite", 0xFFF0D0}, {"cyan", 0x00FFFF},   {"magenta", 0xFF00FF}, {"teal", 0x008080},     {"lime", 0x80FF00},
    {"amber", 0xFFBF00},     {"gold", 0xFFD700},   {"off", 0x000000},
};

struct NamedPattern {
    const char *name;
    uint8_t value;
};

static const NamedPattern kNamedPatterns[] = {
    {"solid", LED_PATTERN_SOLID}, {"rainbow", LED_PATTERN_RAINBOW}, {"sparkle", LED_PATTERN_SPARKLE},
    {"strobe", LED_PATTERN_STROBE}, {"chase", LED_PATTERN_CHASE},
};

static const char *kHelpText =
    "help: #! get default|ch|dm|hr [field]; #! set default|ch|dm|hr <field> <value>; #! clear ch|dm <field>; #! gift "
    "<pattern> [c1] [c2]; #! animate rainbow|blink (in a DM). Try: #! help dm, #! help colors, #! help patterns, #! help gift, "
    "#! help animate";
static const char *kNodeHelpText =
    "default: get [color|idle_bpm|idle_delay|notify_pulses|send_pulses|pattern]; set color <c1> [c2]; set idle_bpm <1-600>; "
    "set idle_delay <0-600000>; set notify_pulses <1-20>; set send_pulses <1-20>; set pattern <solid|rainbow|sparkle|strobe|chase>";
static const char *kChannelHelpText =
    "ch: get [n] [color|notify_pulses|send_pulses]; set [n] color <c1> [c2]; set [n] notify_pulses|send_pulses "
    "<0-20>; clear [n] color|notify_pulses|send_pulses";
static const char *kDirectMessageHelpText =
    "dm: get [color|notify_pulses|send_pulses]; set color <c1> [c2]; set notify_pulses|send_pulses <0-20>; list dm; "
    "clear color|notify_pulses|send_pulses|all|<slot>";
static const char *kHeartRateHelpText =
    "hr: get sensitivity; set sensitivity low|medium|high|default|<1-79>. Higher values increase MAX3010x LED drive";
static const char *kGetHelpText =
    "get: #! get default [color|idle_bpm|idle_delay|notify_pulses|send_pulses|pattern]; #! get ch [n] "
    "[color|notify_pulses|send_pulses]; #! get dm [color|notify_pulses|send_pulses]; #! get hr sensitivity";
static const char *kSetHelpText =
    "set: default color|idle_bpm|idle_delay|notify_pulses|send_pulses|pattern; ch [n] color|notify_pulses|send_pulses; dm "
    "color|notify_pulses|send_pulses; hr sensitivity";
static const char *kClearHelpText =
    "clear: #! clear ch [n] color|notify_pulses|send_pulses; #! clear dm color|notify_pulses|send_pulses|all|<slot>";
static const char *kColorHelpText =
    "color: set default color <c1> [c2]; set ch [n] color <c1> [c2]; set dm color <c1> [c2]. led is accepted as an alias";
static const char *kNotifyPulsesHelpText =
    "notify_pulses: default <1-20>; ch [n] <0-20>; dm <0-20>. 0 uses default for ch/dm";
static const char *kSendPulsesHelpText =
    "send_pulses: default <1-20>; ch [n] <0-20>; dm <0-20>. 0 uses default for ch/dm";
static const char *kColorsText =
    "colors: red orange yellow green blue indigo violet purple pink white warmwhite cyan magenta teal lime amber gold off or "
    "#RRGGBB";
static const char *kPatternsText = "patterns: solid rainbow sparkle strobe chase";
static const char *kPatternHelpText = "pattern: #! set default pattern <solid|rainbow|sparkle|strobe|chase>; #! get default pattern";
static const char *kGiftHelpText =
    "gift: #! gift <solid|rainbow|sparkle|strobe|chase> [c1] [c2] - plays a one-time LED burst on the receiving badge only; "
    "does not change their saved settings. Colors default to the receiver's own colors if omitted";
static const char *kAnimateHelpText =
    "animate: #! animate rainbow | #! animate blink [color] - sent inside a direct message, targets that peer only. Add "
    "\"ch [n]\" to instead broadcast to a whole channel (n defaults to the current/resolved channel), affecting every "
    "listening badge - opt-in only, never implied. Works even on unmodified firmware via ordinary set-color commands. "
    "Permanently changes default color (ends on the standard blue/red default) - there is no way to restore prior colors";

size_t commandPrefixLength(const char *text)
{
    if (!text) {
        return 0;
    }
    if (text[0] == '#' && text[1] == '!') {
        return 2;
    }
    if (text[0] == '<' && text[1] == '3') {
        return 2;
    }
    static const char kAnatomicalHeartPrefix[] = "\xF0\x9F\xAB\x80";
    if (strncmp(text, kAnatomicalHeartPrefix, sizeof(kAnatomicalHeartPrefix) - 1) == 0) {
        return sizeof(kAnatomicalHeartPrefix) - 1;
    }
    return 0;
}

void setResponse(LocalLedCommandResult *result, bool persist, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    vsnprintf(result->response, sizeof(result->response), format, args);
    va_end(args);
    result->handled = true;
    result->consume_packet = true;
    result->persist = persist;
}

void setUnknown(LocalLedCommandResult *result)
{
    setResponse(result, false, "ERR unknown command; try #! help");
}

bool isHelpToken(const char *text)
{
    return text && (strcasecmp(text, "help") == 0 || strcmp(text, "?") == 0);
}

bool isColorField(const char *text)
{
    return text && (strcasecmp(text, "color") == 0 || strcasecmp(text, "led") == 0);
}

bool isDefaultScope(const char *text)
{
    return text && (strcasecmp(text, "default") == 0 || strcasecmp(text, "node") == 0);
}

bool isContextualScopedField(const char *text)
{
    return isColorField(text) || strcasecmp(text, "notify_pulses") == 0 || strcasecmp(text, "send_pulses") == 0;
}

void handleChannelGet(const CustomLedConfig &config, const LocalLedCommandContext &context, uint8_t argc, char *argv[],
                      LocalLedCommandResult *result);
void handleChannelSet(CustomLedConfig &config, const LocalLedCommandContext &context, uint8_t argc, char *argv[],
                      LocalLedCommandResult *result);
void handleChannelClear(CustomLedConfig &config, const LocalLedCommandContext &context, uint8_t argc, char *argv[],
                        LocalLedCommandResult *result);
void handleDirectMessageGet(const CustomLedConfig &config, const LocalLedCommandContext &context, uint8_t argc, char *argv[],
                            LocalLedCommandResult *result);
void handleDirectMessageSet(CustomLedConfig &config, const LocalLedCommandContext &context, uint8_t argc, char *argv[],
                            LocalLedCommandResult *result);
void handleDirectMessageClear(CustomLedConfig &config, const LocalLedCommandContext &context, uint8_t argc, char *argv[],
                              LocalLedCommandResult *result);

bool setHelpForTopic(const char *topic, LocalLedCommandResult *result)
{
    if (!topic || strcasecmp(topic, "all") == 0) {
        setResponse(result, false, "%s", kHelpText);
        return true;
    }
    if (isDefaultScope(topic)) {
        setResponse(result, false, "%s", kNodeHelpText);
        return true;
    }
    if (strcasecmp(topic, "ch") == 0 || strcasecmp(topic, "channel") == 0) {
        setResponse(result, false, "%s", kChannelHelpText);
        return true;
    }
    if (strcasecmp(topic, "dm") == 0 || strcasecmp(topic, "direct") == 0) {
        setResponse(result, false, "%s", kDirectMessageHelpText);
        return true;
    }
    if (strcasecmp(topic, "hr") == 0 || strcasecmp(topic, "heart") == 0 || strcasecmp(topic, "heartrate") == 0 ||
        strcasecmp(topic, "heart_rate") == 0) {
        setResponse(result, false, "%s", kHeartRateHelpText);
        return true;
    }
    if (strcasecmp(topic, "get") == 0) {
        setResponse(result, false, "%s", kGetHelpText);
        return true;
    }
    if (strcasecmp(topic, "set") == 0) {
        setResponse(result, false, "%s", kSetHelpText);
        return true;
    }
    if (strcasecmp(topic, "clear") == 0) {
        setResponse(result, false, "%s", kClearHelpText);
        return true;
    }
    if (strcasecmp(topic, "list") == 0) {
        setResponse(result, false, "list: #! list dm");
        return true;
    }
    if (strcasecmp(topic, "colors") == 0 || strcasecmp(topic, "color") == 0) {
        setResponse(result, false, "%s", kColorsText);
        return true;
    }
    if (strcasecmp(topic, "patterns") == 0 || strcasecmp(topic, "pattern") == 0) {
        setResponse(result, false, "%s", kPatternsText);
        return true;
    }
    if (strcasecmp(topic, "gift") == 0) {
        setResponse(result, false, "%s", kGiftHelpText);
        return true;
    }
    if (strcasecmp(topic, "animate") == 0) {
        setResponse(result, false, "%s", kAnimateHelpText);
        return true;
    }
    if (strcasecmp(topic, "color") == 0 || strcasecmp(topic, "led") == 0) {
        setResponse(result, false, "%s", kColorHelpText);
        return true;
    }
    if (strcasecmp(topic, "notify_pulses") == 0 || strcasecmp(topic, "pulses") == 0) {
        setResponse(result, false, "%s", kNotifyPulsesHelpText);
        return true;
    }
    if (strcasecmp(topic, "send_pulses") == 0) {
        setResponse(result, false, "%s", kSendPulsesHelpText);
        return true;
    }
    if (strcasecmp(topic, "sensitivity") == 0) {
        setResponse(result, false, "%s", kHeartRateHelpText);
        return true;
    }
    return false;
}

bool handleContextualScopedCommand(CustomLedConfig &config, const LocalLedCommandContext &context, const char *verb, uint8_t argc,
                                   char *argv[], LocalLedCommandResult *result)
{
    if (argc == 0 || !isContextualScopedField(argv[0])) {
        return false;
    }
    if (strcasecmp(verb, "get") != 0 && strcasecmp(verb, "set") != 0 && strcasecmp(verb, "clear") != 0) {
        return false;
    }

    if (context.has_direct_message_peer) {
        if (strcasecmp(verb, "get") == 0) {
            handleDirectMessageGet(config, context, argc, argv, result);
        } else if (strcasecmp(verb, "set") == 0) {
            handleDirectMessageSet(config, context, argc, argv, result);
        } else {
            handleDirectMessageClear(config, context, argc, argv, result);
        }
        return true;
    }

    if (strcasecmp(verb, "get") == 0) {
        handleChannelGet(config, context, argc, argv, result);
    } else if (strcasecmp(verb, "set") == 0) {
        handleChannelSet(config, context, argc, argv, result);
    } else {
        handleChannelClear(config, context, argc, argv, result);
    }
    return true;
}

void formatColor(char *buffer, size_t bufferSize, uint32_t color)
{
    snprintf(buffer, bufferSize, "#%06X", (unsigned int)(color & 0xFFFFFF));
}

bool parseUnsigned(const char *text, uint32_t *value)
{
    if (!text || !*text) {
        return false;
    }
    uint32_t parsed = 0;
    for (const char *cursor = text; *cursor; ++cursor) {
        if (!isdigit((unsigned char)*cursor)) {
            return false;
        }
        parsed = (parsed * 10U) + (uint32_t)(*cursor - '0');
    }
    *value = parsed;
    return true;
}

bool parseChannelToken(const char *text, uint8_t *channel)
{
    uint32_t parsed = 0;
    if (!parseUnsigned(text, &parsed)) {
        return false;
    }
    if (parsed > 7) {
        *channel = 0xFF;
        return true;
    }
    *channel = (uint8_t)parsed;
    return true;
}

bool parseHexColor(const char *text, uint32_t *color)
{
    if (!text || strlen(text) != 7 || text[0] != '#') {
        return false;
    }
    uint32_t value = 0;
    for (size_t i = 1; i < 7; ++i) {
        char c = text[i];
        uint32_t digit = 0;
        if (c >= '0' && c <= '9') {
            digit = (uint32_t)(c - '0');
        } else if (c >= 'A' && c <= 'F') {
            digit = (uint32_t)(c - 'A' + 10);
        } else if (c >= 'a' && c <= 'f') {
            digit = (uint32_t)(c - 'a' + 10);
        } else {
            return false;
        }
        value = (value << 4) | digit;
    }
    *color = value;
    return true;
}

bool parseNamedColor(const char *text, uint32_t *color)
{
    for (size_t i = 0; i < sizeof(kNamedColors) / sizeof(kNamedColors[0]); ++i) {
        if (strcasecmp(text, kNamedColors[i].name) == 0) {
            *color = kNamedColors[i].value;
            return true;
        }
    }
    return false;
}

bool parseColor(const char *text, uint32_t *color)
{
    return parseHexColor(text, color) || parseNamedColor(text, color);
}

bool parsePattern(const char *text, uint8_t *pattern)
{
    if (!text) {
        return false;
    }
    for (size_t i = 0; i < sizeof(kNamedPatterns) / sizeof(kNamedPatterns[0]); ++i) {
        if (strcasecmp(text, kNamedPatterns[i].name) == 0) {
            *pattern = kNamedPatterns[i].value;
            return true;
        }
    }
    return false;
}

const char *patternName(uint8_t pattern)
{
    for (size_t i = 0; i < sizeof(kNamedPatterns) / sizeof(kNamedPatterns[0]); ++i) {
        if (kNamedPatterns[i].value == pattern) {
            return kNamedPatterns[i].name;
        }
    }
    return "solid";
}

uint8_t tokenize(char *text, char *tokens[], uint8_t maxTokens)
{
    uint8_t count = 0;
    char *cursor = text;
    while (*cursor && count < maxTokens) {
        while (*cursor == ' ' || *cursor == '\t') {
            ++cursor;
        }
        if (!*cursor) {
            break;
        }
        tokens[count++] = cursor;
        while (*cursor && *cursor != ' ' && *cursor != '\t') {
            ++cursor;
        }
        if (!*cursor) {
            break;
        }
        *cursor++ = '\0';
    }
    return count;
}

bool resolveChannel(const LocalLedCommandContext &context, bool hasExplicitChannel, uint8_t explicitChannel, uint8_t *targetChannel,
                    LocalLedCommandResult *result)
{
    if (hasExplicitChannel) {
        if (explicitChannel > 7) {
            setResponse(result, false, "ERR invalid channel");
            return false;
        }
        *targetChannel = explicitChannel;
        return true;
    }
    if (!context.has_resolved_incoming_channel) {
        setResponse(result, false, "ERR no active channel");
        return false;
    }
    *targetChannel = context.resolved_incoming_channel;
    return true;
}

void assignColors(uint32_t *led1, uint32_t *led2, uint32_t first, bool hasSecond, uint32_t second)
{
    *led1 = first;
    *led2 = hasSecond ? second : first;
}

int8_t findDirectMessageUserSlot(const CustomLedConfig &config, uint32_t nodeNum)
{
    if (nodeNum == 0) {
        return -1;
    }
    for (uint8_t i = 0; i < kLocalLedDirectMessageUserCapacity; ++i) {
        if (config.direct_message_nodes[i] == nodeNum) {
            return (int8_t)i;
        }
    }
    return -1;
}

int8_t findEmptyDirectMessageUserSlot(const CustomLedConfig &config)
{
    for (uint8_t i = 0; i < kLocalLedDirectMessageUserCapacity; ++i) {
        if (config.direct_message_nodes[i] == 0) {
            return (int8_t)i;
        }
    }
    return -1;
}

void clearDirectMessageUserSlot(CustomLedConfig &config, uint8_t slot)
{
    if (slot >= kLocalLedDirectMessageUserCapacity) {
        return;
    }
    config.direct_message_nodes[slot] = 0;
    config.direct_message_users[slot].led1_color = 0;
    config.direct_message_users[slot].led2_color = 0;
    config.direct_message_users[slot].notification_pulses = 0;
    config.direct_message_users[slot].send_pulses = 0;
    config.direct_message_users[slot].configured = false;
}

void pruneDirectMessageUserSlot(CustomLedConfig &config, uint8_t slot)
{
    if (slot >= kLocalLedDirectMessageUserCapacity) {
        return;
    }
    if (!config.direct_message_users[slot].configured && config.direct_message_users[slot].notification_pulses == 0 &&
        config.direct_message_users[slot].send_pulses == 0) {
        clearDirectMessageUserSlot(config, slot);
    }
}

ChannelLedConfig *getDirectMessageCommandTarget(CustomLedConfig &config, const LocalLedCommandContext &context, bool allocate,
                                                LocalLedCommandResult *result, bool *isUser, uint8_t *slotOut)
{
    if (isUser) {
        *isUser = false;
    }
    if (slotOut) {
        *slotOut = 0;
    }
    if (!context.has_direct_message_peer || context.direct_message_peer == 0) {
        return &config.direct_message;
    }

    int8_t slot = findDirectMessageUserSlot(config, context.direct_message_peer);
    if (slot < 0 && allocate) {
        slot = findEmptyDirectMessageUserSlot(config);
        if (slot >= 0) {
            config.direct_message_nodes[slot] = context.direct_message_peer;
        }
    }
    if (slot < 0) {
        setResponse(result, false, allocate ? "ERR dm user table full" : "ERR dm user not configured");
        return nullptr;
    }
    if (isUser) {
        *isUser = true;
    }
    if (slotOut) {
        *slotOut = (uint8_t)slot;
    }
    return &config.direct_message_users[slot];
}

void handleNodeGet(const CustomLedConfig &config, uint8_t argc, char *argv[], LocalLedCommandResult *result)
{
    if (argc == 1 && isHelpToken(argv[0])) {
        setResponse(result, false, "%s", kGetHelpText);
        return;
    }

    char led1[8] = {};
    char led2[8] = {};
    formatColor(led1, sizeof(led1), config.node_led1_color);
    formatColor(led2, sizeof(led2), config.node_led2_color);

    if (argc == 0) {
        setResponse(result, false,
                    "default color color1=%s color2=%s idle_bpm=%u idle_delay=%lu notify_pulses=%u send_pulses=%u pattern=%s", led1,
                    led2, config.idle_bpm, (unsigned long)config.idle_delay_ms, config.notification_pulses, config.send_pulses,
                    patternName(config.node_pattern));
        return;
    }
    if (argc != 1) {
        setUnknown(result);
        return;
    }
    if (isColorField(argv[0])) {
        setResponse(result, false, "default color color1=%s color2=%s", led1, led2);
        return;
    }
    if (strcasecmp(argv[0], "pattern") == 0) {
        setResponse(result, false, "default pattern=%s", patternName(config.node_pattern));
        return;
    }
    if (strcasecmp(argv[0], "idle_bpm") == 0) {
        setResponse(result, false, "default idle_bpm=%u", config.idle_bpm);
        return;
    }
    if (strcasecmp(argv[0], "idle_delay") == 0) {
        setResponse(result, false, "default idle_delay=%lu", (unsigned long)config.idle_delay_ms);
        return;
    }
    if (strcasecmp(argv[0], "notify_pulses") == 0) {
        setResponse(result, false, "default notify_pulses=%u", config.notification_pulses);
        return;
    }
    if (strcasecmp(argv[0], "send_pulses") == 0) {
        setResponse(result, false, "default send_pulses=%u", config.send_pulses);
        return;
    }
    setUnknown(result);
}

void handleNodeSet(CustomLedConfig &config, uint8_t argc, char *argv[], LocalLedCommandResult *result)
{
    if (argc == 0) {
        setResponse(result, false, "%s", kNodeHelpText);
        return;
    }
    if (argc == 1 && isHelpToken(argv[0])) {
        setResponse(result, false, "%s", kSetHelpText);
        return;
    }

    if (isColorField(argv[0])) {
        if (argc == 2 && isHelpToken(argv[1])) {
            setResponse(result, false, "%s", kColorHelpText);
            return;
        }
        if (argc == 1) {
            setResponse(result, false, "ERR missing value");
            return;
        }
        if (argc > 3) {
            setResponse(result, false, "ERR too many colors");
            return;
        }

        uint32_t color1 = 0;
        uint32_t color2 = 0;
        if (!parseColor(argv[1], &color1) || (argc == 3 && !parseColor(argv[2], &color2))) {
            setResponse(result, false, "ERR invalid color");
            return;
        }
        assignColors(&config.node_led1_color, &config.node_led2_color, color1, argc == 3, color2);

        char led1[8] = {};
        char led2[8] = {};
        formatColor(led1, sizeof(led1), config.node_led1_color);
        formatColor(led2, sizeof(led2), config.node_led2_color);
        setResponse(result, true, "OK default color color1=%s color2=%s", led1, led2);
        return;
    }

    if (strcasecmp(argv[0], "pattern") == 0) {
        if (argc == 2 && isHelpToken(argv[1])) {
            setResponse(result, false, "%s", kPatternHelpText);
            return;
        }
        if (argc == 1) {
            setResponse(result, false, "ERR missing value");
            return;
        }
        uint8_t pattern = 0;
        if (argc != 2 || !parsePattern(argv[1], &pattern)) {
            setResponse(result, false, "ERR invalid pattern");
            return;
        }
        config.node_pattern = pattern;
        setResponse(result, true, "OK default pattern=%s", patternName(pattern));
        return;
    }

    if (strcasecmp(argv[0], "idle_bpm") == 0) {
        uint32_t value = 0;
        if (argc == 2 && isHelpToken(argv[1])) {
            setResponse(result, false, "idle_bpm: #! set default idle_bpm <1-600>");
            return;
        }
        if (argc == 1) {
            setResponse(result, false, "ERR missing value");
            return;
        }
        if (argc != 2 || !parseUnsigned(argv[1], &value) || value < 1 || value > 600) {
            setResponse(result, false, "ERR invalid idle_bpm");
            return;
        }
        config.idle_bpm = (uint16_t)value;
        setResponse(result, true, "OK default idle_bpm=%u", config.idle_bpm);
        return;
    }

    if (strcasecmp(argv[0], "idle_delay") == 0) {
        uint32_t value = 0;
        if (argc == 2 && isHelpToken(argv[1])) {
            setResponse(result, false, "idle_delay: #! set default idle_delay <0-600000>");
            return;
        }
        if (argc == 1) {
            setResponse(result, false, "ERR missing value");
            return;
        }
        if (argc != 2 || !parseUnsigned(argv[1], &value) || value > 600000) {
            setResponse(result, false, "ERR invalid idle_delay");
            return;
        }
        config.idle_delay_ms = value;
        setResponse(result, true, "OK default idle_delay=%lu", (unsigned long)config.idle_delay_ms);
        return;
    }

    if (strcasecmp(argv[0], "notify_pulses") == 0) {
        uint32_t value = 0;
        if (argc == 2 && isHelpToken(argv[1])) {
            setResponse(result, false, "%s", kNotifyPulsesHelpText);
            return;
        }
        if (argc == 1) {
            setResponse(result, false, "ERR missing value");
            return;
        }
        if (argc != 2 || !parseUnsigned(argv[1], &value) || value < 1 || value > kLocalLedMaxNotificationPulses) {
            setResponse(result, false, "ERR invalid notify_pulses");
            return;
        }
        config.notification_pulses = (uint8_t)value;
        setResponse(result, true, "OK default notify_pulses=%u", config.notification_pulses);
        return;
    }

    if (strcasecmp(argv[0], "send_pulses") == 0) {
        uint32_t value = 0;
        if (argc == 2 && isHelpToken(argv[1])) {
            setResponse(result, false, "%s", kSendPulsesHelpText);
            return;
        }
        if (argc == 1) {
            setResponse(result, false, "ERR missing value");
            return;
        }
        if (argc != 2 || !parseUnsigned(argv[1], &value) || value < 1 || value > kLocalLedMaxNotificationPulses) {
            setResponse(result, false, "ERR invalid send_pulses");
            return;
        }
        config.send_pulses = (uint8_t)value;
        setResponse(result, true, "OK default send_pulses=%u", config.send_pulses);
        return;
    }

    setUnknown(result);
}

void formatChannelResponse(const CustomLedConfig &config, uint8_t channel, LocalLedCommandResult *result)
{
    uint32_t led1 = config.node_led1_color;
    uint32_t led2 = config.node_led2_color;
    bool configured = config.channels[channel].configured;
    uint8_t notifyPulses = config.channels[channel].notification_pulses > 0 ? config.channels[channel].notification_pulses
                                                                            : config.notification_pulses;
    uint8_t sendPulses = config.channels[channel].send_pulses > 0 ? config.channels[channel].send_pulses : config.send_pulses;
    bool notifyOverride = config.channels[channel].notification_pulses > 0;
    bool sendOverride = config.channels[channel].send_pulses > 0;
    if (configured) {
        led1 = config.channels[channel].led1_color;
        led2 = config.channels[channel].led2_color;
    }

    char led1Text[8] = {};
    char led2Text[8] = {};
    formatColor(led1Text, sizeof(led1Text), led1);
    formatColor(led2Text, sizeof(led2Text), led2);
    setResponse(result, false,
                "ch=%u color color1=%s color2=%s configured=%s notify_pulses=%u notify_override=%s send_pulses=%u "
                "send_override=%s",
                channel, led1Text, led2Text, configured ? "true" : "false", notifyPulses, notifyOverride ? "true" : "false",
                sendPulses, sendOverride ? "true" : "false");
}

void formatDirectMessageResponse(const CustomLedConfig &config, const LocalLedCommandContext &context, LocalLedCommandResult *result)
{
    uint32_t led1 = config.node_led1_color;
    uint32_t led2 = config.node_led2_color;
    bool configured = config.direct_message.configured;
    bool notifyOverride = config.direct_message.notification_pulses > 0;
    bool userOverride = false;
    int8_t slot = -1;
    uint8_t notifyPulses = config.direct_message.notification_pulses > 0 ? config.direct_message.notification_pulses
                                                                         : config.notification_pulses;
    uint8_t sendPulses = config.direct_message.send_pulses > 0 ? config.direct_message.send_pulses : config.send_pulses;
    const ChannelLedConfig *dmConfig = config.direct_message.configured ? &config.direct_message : nullptr;
    if (context.has_direct_message_peer) {
        slot = findDirectMessageUserSlot(config, context.direct_message_peer);
        if (slot >= 0) {
            userOverride = config.direct_message_users[slot].configured || config.direct_message_users[slot].notification_pulses > 0 ||
                           config.direct_message_users[slot].send_pulses > 0;
            if (config.direct_message_users[slot].configured) {
                dmConfig = &config.direct_message_users[slot];
                configured = true;
            }
            if (config.direct_message_users[slot].notification_pulses > 0) {
                notifyPulses = config.direct_message_users[slot].notification_pulses;
                notifyOverride = true;
            }
            if (config.direct_message_users[slot].send_pulses > 0) {
                sendPulses = config.direct_message_users[slot].send_pulses;
            }
        }
    }
    if (dmConfig) {
        led1 = dmConfig->led1_color;
        led2 = dmConfig->led2_color;
    }

    char led1Text[8] = {};
    char led2Text[8] = {};
    formatColor(led1Text, sizeof(led1Text), led1);
    formatColor(led2Text, sizeof(led2Text), led2);
    if (context.has_direct_message_peer) {
        setResponse(result, false,
                    "dm user=!%08X slot=%d color1=%s color2=%s configured=%s pulses=%u send_pulses=%u user_override=%s",
                    (unsigned int)context.direct_message_peer, slot, led1Text, led2Text, configured ? "true" : "false", notifyPulses,
                    sendPulses, userOverride ? "true" : "false");
        return;
    }
    setResponse(result, false,
                "dm default color1=%s color2=%s configured=%s notify_pulses=%u notify_override=%s send_pulses=%u",
                led1Text, led2Text, configured ? "true" : "false", notifyPulses, notifyOverride ? "true" : "false", sendPulses);
}

void handleChannelGet(const CustomLedConfig &config, const LocalLedCommandContext &context, uint8_t argc, char *argv[],
                      LocalLedCommandResult *result)
{
    if (argc == 1 && isHelpToken(argv[0])) {
        setResponse(result, false, "%s", kGetHelpText);
        return;
    }

    uint8_t index = 0;
    bool hasExplicitChannel = false;
    uint8_t explicitChannel = 0;

    if (argc > 0 && parseChannelToken(argv[0], &explicitChannel)) {
        hasExplicitChannel = true;
        index = 1;
    }

    uint8_t targetChannel = 0;
    if (!resolveChannel(context, hasExplicitChannel, explicitChannel, &targetChannel, result)) {
        return;
    }

    if (argc == index) {
        formatChannelResponse(config, targetChannel, result);
        return;
    }
    if (argc == index + 1 && isHelpToken(argv[index])) {
        setResponse(result, false, "%s", kGetHelpText);
        return;
    }
    if (argc == index + 1 && isColorField(argv[index])) {
        formatChannelResponse(config, targetChannel, result);
        return;
    }
    if (argc == index + 1 && strcasecmp(argv[index], "notify_pulses") == 0) {
        uint8_t notifyPulses = config.channels[targetChannel].notification_pulses > 0 ? config.channels[targetChannel].notification_pulses
                                                                                      : config.notification_pulses;
        setResponse(result, false, "ch=%u notify_pulses=%u override=%s", targetChannel, notifyPulses,
                    config.channels[targetChannel].notification_pulses > 0 ? "true" : "false");
        return;
    }
    if (argc == index + 1 && strcasecmp(argv[index], "send_pulses") == 0) {
        uint8_t sendPulses = config.channels[targetChannel].send_pulses > 0 ? config.channels[targetChannel].send_pulses
                                                                            : config.send_pulses;
        setResponse(result, false, "ch=%u send_pulses=%u override=%s", targetChannel, sendPulses,
                    config.channels[targetChannel].send_pulses > 0 ? "true" : "false");
        return;
    }
    setUnknown(result);
}

void handleChannelSet(CustomLedConfig &config, const LocalLedCommandContext &context, uint8_t argc, char *argv[],
                      LocalLedCommandResult *result)
{
    if (argc == 1 && isHelpToken(argv[0])) {
        setResponse(result, false, "%s", kSetHelpText);
        return;
    }

    uint8_t index = 0;
    bool hasExplicitChannel = false;
    uint8_t explicitChannel = 0;

    if (argc > 0 && parseChannelToken(argv[0], &explicitChannel)) {
        hasExplicitChannel = true;
        index = 1;
    }

    uint8_t targetChannel = 0;
    if (!resolveChannel(context, hasExplicitChannel, explicitChannel, &targetChannel, result)) {
        return;
    }

    if (argc <= index) {
        setResponse(result, false, "%s", kChannelHelpText);
        return;
    }
    if (argc == index + 1 && isHelpToken(argv[index])) {
        setResponse(result, false, "%s", kSetHelpText);
        return;
    }
    if (strcasecmp(argv[index], "notify_pulses") == 0) {
        uint32_t value = 0;
        if (argc == index + 2 && isHelpToken(argv[index + 1])) {
            setResponse(result, false, "%s", kNotifyPulsesHelpText);
            return;
        }
        if (argc == index + 1) {
            setResponse(result, false, "ERR missing value");
            return;
        }
        if (argc != index + 2 || !parseUnsigned(argv[index + 1], &value) || value > kLocalLedMaxNotificationPulses) {
            setResponse(result, false, "ERR invalid notify_pulses");
            return;
        }
        config.channels[targetChannel].notification_pulses = (uint8_t)value;
        if (value == 0) {
            setResponse(result, true, "OK ch=%u notify_pulses default", targetChannel);
        } else {
            setResponse(result, true, "OK ch=%u notify_pulses=%u", targetChannel, config.channels[targetChannel].notification_pulses);
        }
        return;
    }
    if (strcasecmp(argv[index], "send_pulses") == 0) {
        uint32_t value = 0;
        if (argc == index + 2 && isHelpToken(argv[index + 1])) {
            setResponse(result, false, "%s", kSendPulsesHelpText);
            return;
        }
        if (argc == index + 1) {
            setResponse(result, false, "ERR missing value");
            return;
        }
        if (argc != index + 2 || !parseUnsigned(argv[index + 1], &value) || value > kLocalLedMaxNotificationPulses) {
            setResponse(result, false, "ERR invalid send_pulses");
            return;
        }
        config.channels[targetChannel].send_pulses = (uint8_t)value;
        if (value == 0) {
            setResponse(result, true, "OK ch=%u send_pulses default", targetChannel);
        } else {
            setResponse(result, true, "OK ch=%u send_pulses=%u", targetChannel, config.channels[targetChannel].send_pulses);
        }
        return;
    }
    if (!isColorField(argv[index])) {
        setUnknown(result);
        return;
    }
    if (argc == index + 2 && isHelpToken(argv[index + 1])) {
        setResponse(result, false, "%s", kColorHelpText);
        return;
    }
    if (argc == index + 1) {
        setResponse(result, false, "ERR missing value");
        return;
    }
    if (argc > index + 3) {
        setResponse(result, false, "ERR too many colors");
        return;
    }

    uint32_t color1 = 0;
    uint32_t color2 = 0;
    if (!parseColor(argv[index + 1], &color1) || (argc == index + 3 && !parseColor(argv[index + 2], &color2))) {
        setResponse(result, false, "ERR invalid color");
        return;
    }

    assignColors(&config.channels[targetChannel].led1_color, &config.channels[targetChannel].led2_color, color1,
                 argc == index + 3, color2);
    config.channels[targetChannel].configured = true;

    char led1[8] = {};
    char led2[8] = {};
    formatColor(led1, sizeof(led1), config.channels[targetChannel].led1_color);
    formatColor(led2, sizeof(led2), config.channels[targetChannel].led2_color);
    setResponse(result, true, "OK ch=%u color color1=%s color2=%s", targetChannel, led1, led2);
}

void handleChannelClear(CustomLedConfig &config, const LocalLedCommandContext &context, uint8_t argc, char *argv[],
                        LocalLedCommandResult *result)
{
    if (argc == 1 && isHelpToken(argv[0])) {
        setResponse(result, false, "%s", kClearHelpText);
        return;
    }

    uint8_t index = 0;
    bool hasExplicitChannel = false;
    uint8_t explicitChannel = 0;

    if (argc > 0 && parseChannelToken(argv[0], &explicitChannel)) {
        hasExplicitChannel = true;
        index = 1;
    }

    uint8_t targetChannel = 0;
    if (!resolveChannel(context, hasExplicitChannel, explicitChannel, &targetChannel, result)) {
        return;
    }

    if (argc != index + 1) {
        setResponse(result, false, "%s", kClearHelpText);
        return;
    }

    if (isHelpToken(argv[index])) {
        setResponse(result, false, "%s", kClearHelpText);
        return;
    }

    if (strcasecmp(argv[index], "notify_pulses") == 0) {
        config.channels[targetChannel].notification_pulses = 0;
        setResponse(result, true, "OK ch=%u notify_pulses default", targetChannel);
        return;
    }
    if (strcasecmp(argv[index], "send_pulses") == 0) {
        config.channels[targetChannel].send_pulses = 0;
        setResponse(result, true, "OK ch=%u send_pulses default", targetChannel);
        return;
    }
    if (!isColorField(argv[index])) {
        setUnknown(result);
        return;
    }

    config.channels[targetChannel].led1_color = 0;
    config.channels[targetChannel].led2_color = 0;
    config.channels[targetChannel].configured = false;
    setResponse(result, true, "OK ch=%u color cleared", targetChannel);
}

void handleDirectMessageGet(const CustomLedConfig &config, const LocalLedCommandContext &context, uint8_t argc, char *argv[],
                            LocalLedCommandResult *result)
{
    if (argc == 0 || (argc == 1 && isColorField(argv[0]))) {
        formatDirectMessageResponse(config, context, result);
        return;
    }
    if (argc == 1 && isHelpToken(argv[0])) {
        setResponse(result, false, "%s", kGetHelpText);
        return;
    }
    if (argc == 1 && strcasecmp(argv[0], "notify_pulses") == 0) {
        uint8_t notifyPulses =
            config.direct_message.notification_pulses > 0 ? config.direct_message.notification_pulses : config.notification_pulses;
        bool override = config.direct_message.notification_pulses > 0;
        int8_t slot = context.has_direct_message_peer ? findDirectMessageUserSlot(config, context.direct_message_peer) : -1;
        if (slot >= 0 && config.direct_message_users[slot].notification_pulses > 0) {
            notifyPulses = config.direct_message_users[slot].notification_pulses;
            override = true;
        }
        if (context.has_direct_message_peer) {
            setResponse(result, false, "dm user=!%08X slot=%d notify_pulses=%u override=%s",
                        (unsigned int)context.direct_message_peer, slot, notifyPulses, override ? "true" : "false");
            return;
        }
        setResponse(result, false, "dm notify_pulses=%u override=%s", notifyPulses, override ? "true" : "false");
        return;
    }
    if (argc == 1 && strcasecmp(argv[0], "send_pulses") == 0) {
        uint8_t sendPulses = config.direct_message.send_pulses > 0 ? config.direct_message.send_pulses : config.send_pulses;
        bool override = config.direct_message.send_pulses > 0;
        int8_t slot = context.has_direct_message_peer ? findDirectMessageUserSlot(config, context.direct_message_peer) : -1;
        if (slot >= 0 && config.direct_message_users[slot].send_pulses > 0) {
            sendPulses = config.direct_message_users[slot].send_pulses;
            override = true;
        }
        if (context.has_direct_message_peer) {
            setResponse(result, false, "dm user=!%08X slot=%d send_pulses=%u override=%s",
                        (unsigned int)context.direct_message_peer, slot, sendPulses, override ? "true" : "false");
            return;
        }
        setResponse(result, false, "dm send_pulses=%u override=%s", sendPulses, override ? "true" : "false");
        return;
    }
    setUnknown(result);
}

void handleDirectMessageSet(CustomLedConfig &config, const LocalLedCommandContext &context, uint8_t argc, char *argv[],
                            LocalLedCommandResult *result)
{
    if (argc == 0) {
        setResponse(result, false, "%s", kDirectMessageHelpText);
        return;
    }
    if (argc == 1 && isHelpToken(argv[0])) {
        setResponse(result, false, "%s", kSetHelpText);
        return;
    }
    if (strcasecmp(argv[0], "notify_pulses") == 0) {
        uint32_t value = 0;
        if (argc == 2 && isHelpToken(argv[1])) {
            setResponse(result, false, "%s", kNotifyPulsesHelpText);
            return;
        }
        if (argc == 1) {
            setResponse(result, false, "ERR missing value");
            return;
        }
        if (argc != 2 || !parseUnsigned(argv[1], &value) || value > kLocalLedMaxNotificationPulses) {
            setResponse(result, false, "ERR invalid notify_pulses");
            return;
        }
        bool isUser = false;
        uint8_t slot = 0;
        ChannelLedConfig *target = getDirectMessageCommandTarget(config, context, value > 0, result, &isUser, &slot);
        if (!target) {
            return;
        }
        target->notification_pulses = (uint8_t)value;
        if (value == 0) {
            if (isUser) {
                pruneDirectMessageUserSlot(config, slot);
                setResponse(result, true, "OK dm user=%u notify_pulses default", slot);
            } else {
                setResponse(result, true, "OK dm notify_pulses default");
            }
        } else {
            if (isUser) {
                setResponse(result, true, "OK dm user=%u notify_pulses=%u", slot, target->notification_pulses);
            } else {
                setResponse(result, true, "OK dm notify_pulses=%u", target->notification_pulses);
            }
        }
        return;
    }
    if (strcasecmp(argv[0], "send_pulses") == 0) {
        uint32_t value = 0;
        if (argc == 2 && isHelpToken(argv[1])) {
            setResponse(result, false, "%s", kSendPulsesHelpText);
            return;
        }
        if (argc == 1) {
            setResponse(result, false, "ERR missing value");
            return;
        }
        if (argc != 2 || !parseUnsigned(argv[1], &value) || value > kLocalLedMaxNotificationPulses) {
            setResponse(result, false, "ERR invalid send_pulses");
            return;
        }
        bool isUser = false;
        uint8_t slot = 0;
        ChannelLedConfig *target = getDirectMessageCommandTarget(config, context, value > 0, result, &isUser, &slot);
        if (!target) {
            return;
        }
        target->send_pulses = (uint8_t)value;
        if (value == 0) {
            if (isUser) {
                pruneDirectMessageUserSlot(config, slot);
                setResponse(result, true, "OK dm user=%u send_pulses default", slot);
            } else {
                setResponse(result, true, "OK dm send_pulses default");
            }
        } else {
            if (isUser) {
                setResponse(result, true, "OK dm user=%u send_pulses=%u", slot, target->send_pulses);
            } else {
                setResponse(result, true, "OK dm send_pulses=%u", target->send_pulses);
            }
        }
        return;
    }
    if (!isColorField(argv[0])) {
        setUnknown(result);
        return;
    }
    if (argc == 2 && isHelpToken(argv[1])) {
        setResponse(result, false, "%s", kColorHelpText);
        return;
    }
    if (argc == 1) {
        setResponse(result, false, "ERR missing value");
        return;
    }
    if (argc > 3) {
        setResponse(result, false, "ERR too many colors");
        return;
    }

    uint32_t color1 = 0;
    uint32_t color2 = 0;
    if (!parseColor(argv[1], &color1) || (argc == 3 && !parseColor(argv[2], &color2))) {
        setResponse(result, false, "ERR invalid color");
        return;
    }

    bool isUser = false;
    uint8_t slot = 0;
    ChannelLedConfig *target = getDirectMessageCommandTarget(config, context, true, result, &isUser, &slot);
    if (!target) {
        return;
    }
    assignColors(&target->led1_color, &target->led2_color, color1, argc == 3, color2);
    target->configured = true;

    char led1[8] = {};
    char led2[8] = {};
    formatColor(led1, sizeof(led1), target->led1_color);
    formatColor(led2, sizeof(led2), target->led2_color);
    if (isUser) {
        setResponse(result, true, "OK dm user=%u !%08X color1=%s color2=%s", slot, (unsigned int)context.direct_message_peer, led1,
                    led2);
    } else {
        setResponse(result, true, "OK dm color color1=%s color2=%s", led1, led2);
    }
}

void handleDirectMessageClear(CustomLedConfig &config, const LocalLedCommandContext &context, uint8_t argc, char *argv[],
                              LocalLedCommandResult *result)
{
    if (argc != 1 || isHelpToken(argv[0])) {
        setResponse(result, false, "%s", kClearHelpText);
        return;
    }
    if (strcasecmp(argv[0], "all") == 0) {
        for (uint8_t i = 0; i < kLocalLedDirectMessageUserCapacity; ++i) {
            clearDirectMessageUserSlot(config, i);
        }
        setResponse(result, true, "OK dm users cleared");
        return;
    }
    uint32_t slotValue = 0;
    if (parseUnsigned(argv[0], &slotValue)) {
        if (slotValue >= kLocalLedDirectMessageUserCapacity) {
            setResponse(result, false, "ERR invalid dm slot");
            return;
        }
        clearDirectMessageUserSlot(config, (uint8_t)slotValue);
        setResponse(result, true, "OK dm user=%u cleared", (unsigned int)slotValue);
        return;
    }
    if (strcasecmp(argv[0], "notify_pulses") == 0) {
        bool isUser = false;
        uint8_t slot = 0;
        ChannelLedConfig *target = getDirectMessageCommandTarget(config, context, false, result, &isUser, &slot);
        if (!target) {
            return;
        }
        target->notification_pulses = 0;
        if (isUser) {
            pruneDirectMessageUserSlot(config, slot);
            setResponse(result, true, "OK dm user=%u notify_pulses default", slot);
        } else {
            setResponse(result, true, "OK dm notify_pulses default");
        }
        return;
    }
    if (strcasecmp(argv[0], "send_pulses") == 0) {
        bool isUser = false;
        uint8_t slot = 0;
        ChannelLedConfig *target = getDirectMessageCommandTarget(config, context, false, result, &isUser, &slot);
        if (!target) {
            return;
        }
        target->send_pulses = 0;
        if (isUser) {
            pruneDirectMessageUserSlot(config, slot);
            setResponse(result, true, "OK dm user=%u send_pulses default", slot);
        } else {
            setResponse(result, true, "OK dm send_pulses default");
        }
        return;
    }
    if (!isColorField(argv[0])) {
        setUnknown(result);
        return;
    }

    bool isUser = false;
    uint8_t slot = 0;
    ChannelLedConfig *target = getDirectMessageCommandTarget(config, context, false, result, &isUser, &slot);
    if (!target) {
        return;
    }
    target->led1_color = 0;
    target->led2_color = 0;
    target->configured = false;
    if (isUser) {
        pruneDirectMessageUserSlot(config, slot);
        setResponse(result, true, "OK dm user=%u color cleared", slot);
    } else {
        setResponse(result, true, "OK dm color cleared");
    }
}

void handleDirectMessageList(const CustomLedConfig &config, LocalLedCommandResult *result)
{
    char response[sizeof(result->response)] = {};
    size_t offset = 0;
    offset += snprintf(response + offset, sizeof(response) - offset, "dm users:");
    bool any = false;
    for (uint8_t i = 0; i < kLocalLedDirectMessageUserCapacity && offset < sizeof(response); ++i) {
        if (config.direct_message_nodes[i] == 0) {
            continue;
        }
        any = true;
        offset += snprintf(response + offset, sizeof(response) - offset, " %u=!%08X", i,
                           (unsigned int)config.direct_message_nodes[i]);
    }
    if (!any) {
        snprintf(response, sizeof(response), "dm users: none");
    }
    setResponse(result, false, "%s", response);
}

bool parseHeartRateSensitivity(const char *text, uint8_t *level)
{
    if (!text || !level) {
        return false;
    }
    if (strcasecmp(text, "low") == 0) {
        *level = 0x1F;
        return true;
    }
    if (strcasecmp(text, "medium") == 0 || strcasecmp(text, "default") == 0) {
        *level = 0x2F;
        return true;
    }
    if (strcasecmp(text, "high") == 0) {
        *level = 0x4F;
        return true;
    }

    uint32_t value = 0;
    if (!parseUnsigned(text, &value) || value < 1 || value > 0x4F) {
        return false;
    }
    *level = (uint8_t)value;
    return true;
}

bool getHeartRateSensitivity(uint8_t *level, uint8_t *maxLevel)
{
#if !MESHTASTIC_EXCLUDE_HEALTH_TELEMETRY && !defined(ARCH_PORTDUINO)
    return healthTelemetryModule && healthTelemetryModule->getHeartRateSensitivity(level, maxLevel);
#else
    (void)level;
    (void)maxLevel;
    return false;
#endif
}

bool setHeartRateSensitivity(uint8_t level)
{
#if !MESHTASTIC_EXCLUDE_HEALTH_TELEMETRY && !defined(ARCH_PORTDUINO)
    return healthTelemetryModule && healthTelemetryModule->setHeartRateSensitivity(level);
#else
    (void)level;
    return false;
#endif
}

void handleHeartRateCommand(const char *verb, uint8_t argc, char *argv[], LocalLedCommandResult *result)
{
    if (argc == 0 || (argc == 1 && isHelpToken(argv[0]))) {
        setResponse(result, false, "%s", kHeartRateHelpText);
        return;
    }

    if (strcasecmp(argv[0], "sensitivity") != 0) {
        setUnknown(result);
        return;
    }

    if (strcasecmp(verb, "get") == 0) {
        if (argc != 1) {
            setUnknown(result);
            return;
        }
        uint8_t level = 0;
        uint8_t maxLevel = 0;
        if (!getHeartRateSensitivity(&level, &maxLevel)) {
            setResponse(result, false, "ERR hr sensor unavailable");
            return;
        }
        setResponse(result, false, "hr sensitivity=%u max=%u hex=0x%02X", level, maxLevel, level);
        return;
    }

    if (strcasecmp(verb, "set") == 0) {
        if (argc == 2 && isHelpToken(argv[1])) {
            setResponse(result, false, "%s", kHeartRateHelpText);
            return;
        }
        uint8_t level = 0;
        if (argc != 2 || !parseHeartRateSensitivity(argv[1], &level)) {
            setResponse(result, false, "ERR invalid sensitivity");
            return;
        }
        if (!setHeartRateSensitivity(level)) {
            setResponse(result, false, "ERR hr sensor unavailable");
            return;
        }
        setResponse(result, false, "OK hr sensitivity=%u hex=0x%02X", level, level);
        return;
    }

    setUnknown(result);
}
} // namespace

bool handleLocalLedCommand(CustomLedConfig &config, const LocalLedCommandContext &context, const char *text,
                           LocalLedCommandResult *result)
{
    if (!result) {
        return false;
    }

    memset(result, 0, sizeof(*result));
    const size_t prefixLength = commandPrefixLength(text);
    if (prefixLength == 0) {
        return false;
    }

    char buffer[256] = {};
    strncpy(buffer, text + prefixLength, sizeof(buffer) - 1);
    char *tokens[8] = {};
    const uint8_t tokenCount = tokenize(buffer, tokens, 8);
    if (tokenCount == 0) {
        setResponse(result, false, "%s", kHelpText);
        return true;
    }

    if (isHelpToken(tokens[0])) {
        if (tokenCount == 1) {
            setResponse(result, false, "%s", kHelpText);
        } else if (tokenCount != 2 || !setHelpForTopic(tokens[1], result)) {
            setUnknown(result);
        }
        return true;
    }

    if (strcasecmp(tokens[0], "gift") == 0) {
        if (tokenCount < 2 || isHelpToken(tokens[1])) {
            setResponse(result, false, "%s", kGiftHelpText);
            return true;
        }
        uint8_t pattern = 0;
        if (!parsePattern(tokens[1], &pattern)) {
            setResponse(result, false, "ERR invalid pattern");
            return true;
        }
        if (tokenCount > 4) {
            setResponse(result, false, "ERR too many colors");
            return true;
        }
        uint32_t color1 = config.node_led1_color;
        uint32_t color2 = config.node_led2_color;
        if (tokenCount >= 3) {
            if (!parseColor(tokens[2], &color1)) {
                setResponse(result, false, "ERR invalid color");
                return true;
            }
            color2 = color1;
        }
        if (tokenCount >= 4) {
            if (!parseColor(tokens[3], &color2)) {
                setResponse(result, false, "ERR invalid color");
                return true;
            }
        }
        result->handled = true;
        result->consume_packet = true;
        result->persist = false;
        result->trigger_gift = true;
        result->gift_pattern = pattern;
        result->gift_color1 = color1;
        result->gift_color2 = color2;
        snprintf(result->response, sizeof(result->response), "OK gift %s color1=#%06X color2=#%06X", patternName(pattern),
                 (unsigned int)(color1 & 0xFFFFFF), (unsigned int)(color2 & 0xFFFFFF));
        return true;
    }

    if (strcasecmp(tokens[0], "animate") == 0) {
        if (tokenCount < 2 || isHelpToken(tokens[1])) {
            setResponse(result, false, "%s", kAnimateHelpText);
            return true;
        }
        const bool isBlink = strcasecmp(tokens[1], "blink") == 0;
        if (!isBlink && strcasecmp(tokens[1], "rainbow") != 0) {
            setResponse(result, false, "ERR unknown animate pattern; try rainbow or blink");
            return true;
        }

        // Optional "ch [n]" opt-in switches the target from the current DM peer to a broadcast on
        // channel n (or the resolved/current channel if n is omitted). Never implied - a bare
        // "#! animate rainbow" always stays scoped to a single DM peer.
        uint8_t argIndex = 2;
        bool broadcast = false;
        bool hasExplicitChannel = false;
        uint8_t explicitChannel = 0;
        if (tokenCount > argIndex && strcasecmp(tokens[argIndex], "ch") == 0) {
            broadcast = true;
            argIndex++;
            if (tokenCount > argIndex && parseChannelToken(tokens[argIndex], &explicitChannel)) {
                hasExplicitChannel = true;
                argIndex++;
            }
        }

        uint32_t color = 0xFFFFFF;
        if (isBlink && tokenCount > argIndex) {
            if (!parseColor(tokens[argIndex], &color)) {
                setResponse(result, false, "ERR invalid color");
                return true;
            }
            argIndex++;
        }
        if (tokenCount > argIndex) {
            setResponse(result, false, isBlink ? "ERR too many colors" : "ERR too many arguments");
            return true;
        }

        uint32_t targetNode = 0;
        uint8_t targetChannel = 0;
        if (broadcast) {
            if (!resolveChannel(context, hasExplicitChannel, explicitChannel, &targetChannel, result)) {
                return true;
            }
        } else {
            if (!context.has_direct_message_peer) {
                setResponse(result, false, "ERR animate requires an active direct message; add \"ch [n]\" to broadcast instead");
                return true;
            }
            targetNode = context.direct_message_peer;
            targetChannel = context.reply_channel;
        }

        result->handled = true;
        result->consume_packet = true;
        result->persist = false;
        result->trigger_remote_animation = true;
        result->remote_animation_kind = isBlink ? REMOTE_ANIM_BLINK : REMOTE_ANIM_RAINBOW;
        result->remote_animation_target = targetNode;
        result->remote_animation_broadcast = broadcast;
        result->remote_animation_channel = targetChannel;
        result->remote_animation_color = color;
        snprintf(result->response, sizeof(result->response), "OK animate %s -> %s over ~%d min", tokens[1],
                 broadcast ? "broadcasting to channel" : "sending to peer", isBlink ? 1 : 2);
        return true;
    }

    if (tokenCount == 2 && isHelpToken(tokens[1])) {
        if (!setHelpForTopic(tokens[0], result)) {
            setUnknown(result);
        }
        return true;
    }

    if (tokenCount < 2) {
        if (!setHelpForTopic(tokens[0], result)) {
            setUnknown(result);
        }
        return true;
    }

    const char *verb = tokens[0];
    const char *scope = tokens[1];
    char **argv = tokenCount > 2 ? &tokens[2] : nullptr;
    uint8_t argc = tokenCount > 2 ? (uint8_t)(tokenCount - 2) : 0;

    if (isHelpToken(scope)) {
        if (!setHelpForTopic(verb, result)) {
            setUnknown(result);
        }
        return true;
    }

    if (isContextualScopedField(scope)) {
        char **contextArgv = &tokens[1];
        const uint8_t contextArgc = (uint8_t)(tokenCount - 1);
        if (handleContextualScopedCommand(config, context, verb, contextArgc, contextArgv, result)) {
            return true;
        }
    }

    if (isDefaultScope(scope)) {
        if (argc == 1 && isHelpToken(argv[0])) {
            if (!setHelpForTopic(verb, result)) {
                setResponse(result, false, "%s", kNodeHelpText);
            }
            return true;
        }
        if (strcasecmp(verb, "get") == 0) {
            handleNodeGet(config, argc, argv, result);
            return true;
        }
        if (strcasecmp(verb, "set") == 0) {
            handleNodeSet(config, argc, argv, result);
            return true;
        }
        setUnknown(result);
        return true;
    }

    if (strcasecmp(scope, "ch") == 0) {
        if (argc == 1 && isHelpToken(argv[0])) {
            if (!setHelpForTopic(verb, result)) {
                setResponse(result, false, "%s", kChannelHelpText);
            }
            return true;
        }
        if (strcasecmp(verb, "get") == 0) {
            handleChannelGet(config, context, argc, argv, result);
            return true;
        }
        if (strcasecmp(verb, "set") == 0) {
            handleChannelSet(config, context, argc, argv, result);
            return true;
        }
        if (strcasecmp(verb, "clear") == 0) {
            handleChannelClear(config, context, argc, argv, result);
            return true;
        }
        setUnknown(result);
        return true;
    }

    if (strcasecmp(scope, "dm") == 0 || strcasecmp(scope, "direct") == 0) {
        if (argc == 1 && isHelpToken(argv[0])) {
            if (!setHelpForTopic(verb, result)) {
                setResponse(result, false, "%s", kDirectMessageHelpText);
            }
            return true;
        }
        if (strcasecmp(verb, "get") == 0) {
            handleDirectMessageGet(config, context, argc, argv, result);
            return true;
        }
        if (strcasecmp(verb, "set") == 0) {
            handleDirectMessageSet(config, context, argc, argv, result);
            return true;
        }
        if (strcasecmp(verb, "clear") == 0) {
            handleDirectMessageClear(config, context, argc, argv, result);
            return true;
        }
        if (strcasecmp(verb, "list") == 0) {
            if (argc != 0) {
                setUnknown(result);
            } else {
                handleDirectMessageList(config, result);
            }
            return true;
        }
        setUnknown(result);
        return true;
    }

    if (strcasecmp(scope, "hr") == 0 || strcasecmp(scope, "heart") == 0 || strcasecmp(scope, "heartrate") == 0 ||
        strcasecmp(scope, "heart_rate") == 0) {
        handleHeartRateCommand(verb, argc, argv, result);
        return true;
    }

    setUnknown(result);
    return true;
}
