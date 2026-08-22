#include "TestUtil.h"
#include "led/LocalLedCommandParser.h"
#include "led/LocalLedConfig.h"
#include "modules/LocalLedCommandModule.h"
#include <stdlib.h>
#include <string.h>
#include <unity.h>

class TestableLocalLedCommandModule : public LocalLedCommandModule
{
  public:
    using LocalLedCommandModule::handleReceived;
};

static LocalLedConfigStore *previousStore = nullptr;
static LocalLedConfigStore *testStore = nullptr;

static LocalLedCommandContext makeContext(bool hasChannel = true, uint8_t channel = 3)
{
    return LocalLedCommandContext{hasChannel, channel, false, 0, channel};
}

static meshtastic_MeshPacket makeTextPacket(const char *text, uint8_t channel = 0)
{
    meshtastic_MeshPacket packet = {};
    packet.which_payload_variant = meshtastic_MeshPacket_decoded_tag;
    packet.decoded.portnum = meshtastic_PortNum_TEXT_MESSAGE_APP;
    packet.channel = channel;
    packet.id = 42;
    packet.decoded.payload.size = strlen(text);
    memcpy(packet.decoded.payload.bytes, text, packet.decoded.payload.size);
    return packet;
}

void setUp(void)
{
    previousStore = localLedConfigStore;
    testStore = new LocalLedConfigStore();
    localLedConfigStore = testStore;
}

void tearDown(void)
{
    delete testStore;
    testStore = nullptr;
    localLedConfigStore = previousStore;
}

static void test_parser_ignores_non_command_text()
{
    CustomLedConfig config = {};
    LocalLedConfigStore::applyDefaults(&config);
    LocalLedCommandResult result = {};

    TEST_ASSERT_FALSE(handleLocalLedCommand(config, makeContext(), "hello world", &result));
}

static void test_set_default_led_and_get_default()
{
    CustomLedConfig config = {};
    LocalLedConfigStore::applyDefaults(&config);
    LocalLedCommandResult result = {};

    TEST_ASSERT_TRUE(handleLocalLedCommand(config, makeContext(), "#! set default led red blue", &result));
    TEST_ASSERT_TRUE(result.handled);
    TEST_ASSERT_TRUE(result.persist);
    TEST_ASSERT_EQUAL_STRING("OK default color color1=#FF0000 color2=#0000FF", result.response);

    memset(&result, 0, sizeof(result));
    TEST_ASSERT_TRUE(handleLocalLedCommand(config, makeContext(), "#! get default", &result));
    TEST_ASSERT_EQUAL_STRING("default color color1=#FF0000 color2=#0000FF idle_bpm=80 idle_delay=0 notify_pulses=3 send_pulses=1",
                             result.response);
}

static void test_command_prefix_aliases()
{
    CustomLedConfig config = {};
    LocalLedConfigStore::applyDefaults(&config);
    LocalLedCommandResult result = {};
    // U+1FAC0 ANATOMICAL HEART, spelled out so the source stays ASCII. Must be a single literal:
    // string-literal concatenation only works between literals, so `prefix " ..."` with prefix a
    // variable is a syntax error rather than a concatenation.
    TEST_ASSERT_TRUE(
        handleLocalLedCommand(config, makeContext(), "\xF0\x9F\xAB\x80" " set default led green", &result));
    TEST_ASSERT_TRUE(result.persist);
    TEST_ASSERT_EQUAL_STRING("OK default color color1=#00FF00 color2=#00FF00", result.response);

    memset(&result, 0, sizeof(result));
    TEST_ASSERT_TRUE(handleLocalLedCommand(config, makeContext(), "<3 get default led", &result));
    TEST_ASSERT_EQUAL_STRING("default color color1=#00FF00 color2=#00FF00", result.response);
}

static void test_node_scope_remains_compatible_alias()
{
    CustomLedConfig config = {};
    LocalLedConfigStore::applyDefaults(&config);
    LocalLedCommandResult result = {};

    TEST_ASSERT_TRUE(handleLocalLedCommand(config, makeContext(), "#! set node color cyan", &result));
    TEST_ASSERT_EQUAL_STRING("OK default color color1=#00FFFF color2=#00FFFF", result.response);
}

static void test_set_channel_led_with_resolved_channel_and_clear()
{
    CustomLedConfig config = {};
    LocalLedConfigStore::applyDefaults(&config);
    LocalLedCommandResult result = {};

    TEST_ASSERT_TRUE(handleLocalLedCommand(config, makeContext(true, 5), "#! set ch led amber", &result));
    TEST_ASSERT_EQUAL_STRING("OK ch=5 color color1=#FFBF00 color2=#FFBF00", result.response);
    TEST_ASSERT_TRUE(config.channels[5].configured);

    memset(&result, 0, sizeof(result));
    TEST_ASSERT_TRUE(handleLocalLedCommand(config, makeContext(true, 5), "#! get ch", &result));
    TEST_ASSERT_EQUAL_STRING(
        "ch=5 color color1=#FFBF00 color2=#FFBF00 configured=true notify_pulses=3 notify_override=false send_pulses=1 "
        "send_override=false",
        result.response);

    memset(&result, 0, sizeof(result));
    TEST_ASSERT_TRUE(handleLocalLedCommand(config, makeContext(true, 5), "#! clear ch led", &result));
    TEST_ASSERT_EQUAL_STRING("OK ch=5 color cleared", result.response);
    TEST_ASSERT_FALSE(config.channels[5].configured);
}

static void test_explicit_channel_and_fallback_get()
{
    CustomLedConfig config = {};
    LocalLedConfigStore::applyDefaults(&config);
    config.node_led1_color = 0x00AA00;
    config.node_led2_color = 0xAA00AA;
    LocalLedCommandResult result = {};

    TEST_ASSERT_TRUE(handleLocalLedCommand(config, makeContext(true, 1), "#! get ch 2", &result));
    TEST_ASSERT_EQUAL_STRING(
        "ch=2 color color1=#00AA00 color2=#AA00AA configured=false notify_pulses=3 notify_override=false send_pulses=1 "
        "send_override=false",
        result.response);
}

static void test_help_and_colors_output()
{
    CustomLedConfig config = {};
    LocalLedConfigStore::applyDefaults(&config);
    LocalLedCommandResult result = {};

    TEST_ASSERT_TRUE(handleLocalLedCommand(config, makeContext(), "#! help", &result));
    TEST_ASSERT_EQUAL_STRING(
        "help: #! get default|ch|dm|hr [field]; #! set default|ch|dm|hr <field> <value>; #! clear ch|dm <field>. Try: #! help "
        "dm, #! help colors",
        result.response);

    memset(&result, 0, sizeof(result));
    TEST_ASSERT_TRUE(handleLocalLedCommand(config, makeContext(), "#! help colors", &result));
    TEST_ASSERT_EQUAL_STRING(
        "colors: red orange yellow green blue indigo violet purple pink white warmwhite cyan magenta teal lime amber gold off or "
        "#RRGGBB",
        result.response);
}

static void test_hr_sensitivity_command_is_recognized()
{
    CustomLedConfig config = {};
    LocalLedConfigStore::applyDefaults(&config);
    LocalLedCommandResult result = {};

    TEST_ASSERT_TRUE(handleLocalLedCommand(config, makeContext(), "#! help hr", &result));
    TEST_ASSERT_EQUAL_STRING(
        "hr: get sensitivity; set sensitivity low|medium|high|default|<1-79>. Higher values increase MAX3010x LED drive",
        result.response);

    memset(&result, 0, sizeof(result));
    TEST_ASSERT_TRUE(handleLocalLedCommand(config, makeContext(), "#! get hr sensitivity", &result));
    TEST_ASSERT_EQUAL_STRING("ERR hr sensor unavailable", result.response);
}

static void test_validation_errors()
{
    CustomLedConfig config = {};
    LocalLedConfigStore::applyDefaults(&config);
    LocalLedCommandResult result = {};

    TEST_ASSERT_TRUE(handleLocalLedCommand(config, makeContext(false), "#! get ch", &result));
    TEST_ASSERT_EQUAL_STRING("ERR no active channel", result.response);

    memset(&result, 0, sizeof(result));
    TEST_ASSERT_TRUE(handleLocalLedCommand(config, makeContext(), "#! set ch led red blue green", &result));
    TEST_ASSERT_EQUAL_STRING("ERR too many colors", result.response);

    memset(&result, 0, sizeof(result));
    TEST_ASSERT_TRUE(handleLocalLedCommand(config, makeContext(), "#! set default led nope", &result));
    TEST_ASSERT_EQUAL_STRING("ERR invalid color", result.response);

    memset(&result, 0, sizeof(result));
    TEST_ASSERT_TRUE(handleLocalLedCommand(config, makeContext(), "#! set default idle_bpm 0", &result));
    TEST_ASSERT_EQUAL_STRING("ERR invalid idle_bpm", result.response);

    memset(&result, 0, sizeof(result));
    TEST_ASSERT_TRUE(handleLocalLedCommand(config, makeContext(), "#! set default idle_delay 700000", &result));
    TEST_ASSERT_EQUAL_STRING("ERR invalid idle_delay", result.response);
}

static void test_persistence_round_trip()
{
    CustomLedConfig config = {};
    LocalLedConfigStore::applyDefaults(&config);
    config.node_led1_color = 0x123456;
    config.node_led2_color = 0xABCDEF;
    config.idle_bpm = 42;
    config.idle_delay_ms = 2500;
    config.channels[4].led1_color = 0x001122;
    config.channels[4].led2_color = 0x334455;
    config.channels[4].configured = true;

    // Must be at least LocalLedConfig.cpp's kSerializedSize (273 at format v8); serializeConfig
    // refuses a smaller buffer. This was 128 - correct for the v7 format, silently stale after v8
    // grew the record, and invisible because this file did not compile.
    uint8_t buffer[320] = {};
    size_t used = 0;
    TEST_ASSERT_TRUE(LocalLedConfigStore::serializeConfig(config, buffer, sizeof(buffer), &used));

    CustomLedConfig decoded = {};
    TEST_ASSERT_TRUE(LocalLedConfigStore::deserializeConfig(buffer, used, &decoded));
    TEST_ASSERT_EQUAL_HEX32(0x123456, decoded.node_led1_color);
    TEST_ASSERT_EQUAL_HEX32(0xABCDEF, decoded.node_led2_color);
    TEST_ASSERT_EQUAL_UINT16(42, decoded.idle_bpm);
    TEST_ASSERT_EQUAL_UINT32(2500, decoded.idle_delay_ms);
    TEST_ASSERT_TRUE(decoded.channels[4].configured);
    TEST_ASSERT_EQUAL_HEX32(0x001122, decoded.channels[4].led1_color);
    TEST_ASSERT_EQUAL_HEX32(0x334455, decoded.channels[4].led2_color);
}

static void test_store_effective_config_falls_back_and_overrides()
{
    LocalLedCommandContext context = makeContext(true, 2);
    LocalLedCommandResult result = {};

    TEST_ASSERT_TRUE(testStore->handleCommand("#! set default led green purple", context, &result));
    TEST_ASSERT_TRUE(testStore->handleCommand("#! set ch 2 led cyan", context, &result));

    LocalLedEffectiveConfig channelTwo = testStore->getEffectiveConfigForChannel(2);
    TEST_ASSERT_TRUE(channelTwo.configured);
    TEST_ASSERT_EQUAL_HEX32(0x00FFFF, channelTwo.led1_color);
    TEST_ASSERT_EQUAL_HEX32(0x00FFFF, channelTwo.led2_color);

    LocalLedEffectiveConfig channelOne = testStore->getEffectiveConfigForChannel(1);
    TEST_ASSERT_FALSE(channelOne.configured);
    TEST_ASSERT_EQUAL_HEX32(0x00FF00, channelOne.led1_color);
    TEST_ASSERT_EQUAL_HEX32(0x8000FF, channelOne.led2_color);
}

static void test_local_phone_command_helper_consumes_command()
{
    meshtastic_MeshPacket packet = makeTextPacket("#! set default idle_bpm 24", 4);

    TEST_ASSERT_TRUE(handleLocalLedPhoneCommand(packet, nullptr));
    TEST_ASSERT_EQUAL_UINT8(4, testStore->getActiveChannel());
    TEST_ASSERT_EQUAL_UINT16(24, testStore->getConfig().idle_bpm);
}

static void test_over_air_command_is_consumed_but_does_not_mutate()
{
    TestableLocalLedCommandModule module;
    meshtastic_MeshPacket packet = makeTextPacket("#! set ch 6 led #FF0000 #00FFFF", 1);

    // An over-air command is still consumed, so it is not surfaced as chat, but it must
    // not mutate this node's LED config -- any node could otherwise repaint a stranger's
    // badge. Mutation requires the operator to opt in at build time. See the
    // local_client_origin gate in src/led/LocalLedConfig.cpp.
    TEST_ASSERT_EQUAL(ProcessMessage::STOP, module.handleReceived(packet));
#if defined(USERPREFS_BHV_ACCEPT_OVER_AIR_LED) && USERPREFS_BHV_ACCEPT_OVER_AIR_LED
    TEST_ASSERT_TRUE(testStore->getConfig().channels[6].configured);
    TEST_ASSERT_EQUAL_HEX32(0xFF0000, testStore->getConfig().channels[6].led1_color);
    TEST_ASSERT_EQUAL_HEX32(0x00FFFF, testStore->getConfig().channels[6].led2_color);
#else
    TEST_ASSERT_FALSE(testStore->getConfig().channels[6].configured);
#endif
}

void setup()
{
    initializeTestEnvironment();

    UNITY_BEGIN();
    RUN_TEST(test_parser_ignores_non_command_text);
    RUN_TEST(test_set_default_led_and_get_default);
    RUN_TEST(test_command_prefix_aliases);
    RUN_TEST(test_node_scope_remains_compatible_alias);
    RUN_TEST(test_set_channel_led_with_resolved_channel_and_clear);
    RUN_TEST(test_explicit_channel_and_fallback_get);
    RUN_TEST(test_help_and_colors_output);
    RUN_TEST(test_hr_sensitivity_command_is_recognized);
    RUN_TEST(test_validation_errors);
    RUN_TEST(test_persistence_round_trip);
    RUN_TEST(test_store_effective_config_falls_back_and_overrides);
    RUN_TEST(test_local_phone_command_helper_consumes_command);
    RUN_TEST(test_over_air_command_is_consumed_but_does_not_mutate);
    exit(UNITY_END());
}

void loop() {}
