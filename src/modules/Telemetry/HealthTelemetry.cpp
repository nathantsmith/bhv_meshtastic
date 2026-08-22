#include "configuration.h"

#if !MESHTASTIC_EXCLUDE_HEALTH_TELEMETRY && !defined(ARCH_PORTDUINO)

#include "../mesh/generated/meshtastic/telemetry.pb.h"
#include "Default.h"
#include "HealthTelemetry.h"
#include "MeshService.h"
#include "NodeDB.h"
#include "PowerFSM.h"
#include "RTC.h"
#include "Router.h"
#include "TransmitHistory.h"
#include "UnitConversions.h"
#include "main.h"
#include "power.h"
#include "sleep.h"
#include "target_specific.h"
#include <OLEDDisplay.h>
#include <OLEDDisplayUi.h>
#if HAS_SCREEN
#include "graphics/Screen.h"
#include "graphics/ScreenFonts.h"
#include "graphics/SharedUIDisplay.h"
#endif

// Sensors
#include "Sensor/MAX30102Sensor.h"
#include "Sensor/MLX90614Sensor.h"

MAX30102Sensor max30102Sensor;
MLX90614Sensor mlx90614Sensor;

HealthTelemetryModule *healthTelemetryModule = nullptr;

#define FAILED_STATE_SENSOR_READ_MULTIPLIER 10
#define DISPLAY_RECEIVEID_MEASUREMENTS_ON_SCREEN true

#if (HAS_SCREEN)
#include "graphics/ScreenFonts.h"
#endif
#include <Throttle.h>

static constexpr uint16_t TX_HISTORY_KEY_HEALTH_TELEMETRY = 0x8003;

static int16_t mapWaveSampleToY(uint32_t sample, uint32_t minValue, uint32_t maxValue, int16_t top, int16_t height)
{
    if (height <= 1 || maxValue <= minValue) {
        return top + (height / 2);
    }

    const uint32_t range = maxValue - minValue;
    const uint32_t clamped = sample < minValue ? minValue : (sample > maxValue ? maxValue : sample);
    const uint32_t scaled = (uint32_t)(((uint64_t)(clamped - minValue) * (uint32_t)(height - 1)) / range);
    return top + (height - 1) - (int16_t)scaled;
}

static void drawWaveformLane(OLEDDisplay *display, const uint32_t *samples, uint16_t count, int16_t x, int16_t y, int16_t width,
                             int16_t height, const char *label)
{
    if (!display || !samples || count == 0 || width < 4 || height < 4) {
        return;
    }

    display->drawRect(x, y, width, height);
    if (label && label[0]) {
        display->drawString(x + 2, y, label);
    }

    const int16_t innerX = x + 1;
    const int16_t innerY = y + 1;
    const int16_t innerWidth = width - 2;
    const int16_t innerHeight = height - 2;
    if (innerWidth < 2 || innerHeight < 2) {
        return;
    }

    uint32_t minValue = samples[0];
    uint32_t maxValue = samples[0];
    for (uint16_t i = 1; i < count; i++) {
        if (samples[i] < minValue) {
            minValue = samples[i];
        }
        if (samples[i] > maxValue) {
            maxValue = samples[i];
        }
    }

    if (maxValue <= minValue) {
        display->drawHorizontalLine(innerX, innerY + (innerHeight / 2), innerWidth);
        return;
    }

    int16_t prevX = innerX;
    int16_t prevY = mapWaveSampleToY(samples[0], minValue, maxValue, innerY, innerHeight);
    for (int16_t px = 1; px < innerWidth; px++) {
        uint16_t idx = (uint16_t)(((uint32_t)px * (uint32_t)(count - 1)) / (uint32_t)(innerWidth - 1));
        int16_t curX = innerX + px;
        int16_t curY = mapWaveSampleToY(samples[idx], minValue, maxValue, innerY, innerHeight);
        display->drawLine(prevX, prevY, curX, curY);
        prevX = curX;
        prevY = curY;
    }
}

int32_t HealthTelemetryModule::runOnce()
{
    if (sleepOnNextExecution == true) {
        sleepOnNextExecution = false;
        uint32_t nightyNightMs = Default::getConfiguredOrDefaultMs(moduleConfig.telemetry.health_update_interval,
                                                                   default_telemetry_broadcast_interval_secs);
        LOG_DEBUG("Sleep for %ims, then awake to send metrics again", nightyNightMs);
        doDeepSleep(nightyNightMs, true, false);
    }

    const bool measurementEnabled = moduleConfig.telemetry.health_measurement_enabled;
    const bool healthScreenEnabled = moduleConfig.telemetry.health_screen_enabled;
#if HAS_SCREEN
    const bool pollForScreen = healthScreenEnabled && screen && screen->isScreenOn();
#else
    const bool pollForScreen = healthScreenEnabled;
#endif
    const bool moduleEnabled = measurementEnabled || healthScreenEnabled;

    if (!moduleEnabled) {
        if (max30102Sensor.hasSensor()) {
            max30102Sensor.setStayAwake(false);
            max30102Sensor.sleep();
        }
        // If this module is not enabled and the user doesn't want the display screen, don't waste any OSThread time on it.
        return disable();
    }

    const uint32_t meshSendIntervalMs =
        Default::getConfiguredOrDefaultMsScaled(moduleConfig.telemetry.health_update_interval,
                                                default_telemetry_broadcast_interval_secs, numOnlineNodes);
    uint32_t result = min(sendToPhoneIntervalMs, meshSendIntervalMs);
    // Must match the condition guarding serviceSensor() below. Servicing the sensor at only
    // healthPollIntervalMs (1000 ms) starves the presence state machine: the scan wake window is
    // MAX30102_PRESENCE_SCAN_WAKE_WINDOW_MS (700 ms) and activation needs
    // MAX3010X_PRESENCE_CONSECUTIVE_REQUIRED (2) consecutive detections, so at one evaluation per second
    // exactly one lands inside each wake window, the consecutive counter reaches 1, and sleep() then
    // resets it to 0. Presence can never fire, no matter how good the signal is.
    //
    // Measured: a firm fingertip passed both the DC and peak gates on 19 of 19 presence evaluations and
    // still produced zero activations, because this branch had dropped to the 1000 ms cadence once
    // broadcasting became opt-in and the display timed out.
    if (measurementEnabled || healthScreenEnabled) {
        result = min(result, sensorServiceIntervalMs);
    } else {
        result = min(result, healthPollIntervalMs);
    }

    if (firstTime) {
        // This is the first time the OSThread library has called this function, so do some setup.
        firstTime = false;
        LOG_INFO("Health Telemetry: init");

        // Initialize sensors.
        if (mlx90614Sensor.hasSensor()) {
            mlx90614Sensor.runOnce();
        }
        if (max30102Sensor.hasSensor()) {
            max30102Sensor.runOnce();
        }
    }

    // Keep-awake is disabled so MAX3010x always uses sleep/presence-scan behavior when possible.
    if (max30102Sensor.hasSensor()) {
#ifdef BHV_PPG_DIAG
        // Diagnostic builds hold the sensor in ACTIVE mode so register-level experiments run without a
        // finger present. Costs battery; never enabled in a normal build.
        const bool keepPulseOxAwake = true;
#else
        const bool keepPulseOxAwake = false;
#endif
        max30102Sensor.setStayAwake(keepPulseOxAwake);
        // Service the sensor whenever the health feature is enabled AT ALL - not only while the screen
        // happens to be awake.
        //
        // This used to test pollForScreen, which additionally requires screen->isScreenOn(). That was
        // harmless while health_measurement_enabled defaulted to true, because the first term kept the
        // sensor running regardless. Once broadcasting became opt-in, it became a trap: with the screen
        // timed out, nothing serviced the sensor, so presence scanning stopped, so a finger could never
        // be detected, so the screen could never auto-wake for it. The badge's headline interaction died
        // silently the first time the display slept.
        //
        // Transmission remains gated on measurementEnabled further down, so biometrics still do not leave
        // the device unless the user opts in. This only keeps the low-power presence scan alive, which is
        // exactly what it exists for.
        if (measurementEnabled || healthScreenEnabled || max30102Sensor.isActive()) {
            max30102Sensor.serviceSensor();
        }
    }

    // Poll local cached measurements for the health screen, but not every sensor service tick.
    if (pollForScreen) {
        // In local screen-only mode the OLED reads sensor cache directly, so no packet rebuild needed.
        if (!(healthScreenEnabled && !measurementEnabled)) {
            if ((lastLocalMeasurementAttemptMs == 0) ||
                !Throttle::isWithinTimespanMs(lastLocalMeasurementAttemptMs, healthPollIntervalMs)) {
                lastLocalMeasurementAttemptMs = millis();
                if (updateLocalMeasurement()) {
                    sensor_read_error_count = 0;
                } else {
                    sensor_read_error_count++;
                }
            }
        }
    }

#if HAS_SCREEN
    // HR sensor auto-navigate: switch to health screen when finger detected, restore or sleep when removed.
    if (healthScreenEnabled && max30102Sensor.hasSensor() && screen) {
        const bool engaged = max30102Sensor.isHrEngaged();
        if (!lastHrEngaged && engaged) {
            const bool screenWasOn = screen->isScreenOn();
            if (screenWasOn) {
                previousFrameIndex = screen->getCurrentFrameIndex();
            } else {
                hrAutoWokeScreen = true;
            }
            screen->setOn(true);
            requestFocus();
            UIFrameEvent e;
            e.action = UIFrameEvent::Action::REGENERATE_FRAMESET;
            notifyObservers(&e);
            lastHrEngaged = true;
        } else if (lastHrEngaged && engaged) {
            // Keep refreshing screen wake while finger remains present so the normal screen timeout does not blank the display.
            screen->setOn(true);
        } else if (lastHrEngaged && !engaged) {
            if (hrAutoWokeScreen) {
                screen->setOn(false);
            } else {
                screen->switchToFrameByIndex(previousFrameIndex);
            }
            lastHrEngaged = false;
            hrAutoWokeScreen = false;
            previousFrameIndex = 0;
        }
    }
#endif

    // Keep mesh/phone telemetry transmissions behind measurement_enabled.
    if (measurementEnabled) {
        uint32_t lastTelemetry = transmitHistory ? transmitHistory->getLastSentToMeshMillis(TX_HISTORY_KEY_HEALTH_TELEMETRY) : 0;
        if (((lastTelemetry == 0) ||
             !Throttle::isWithinTimespanMs(lastTelemetry, Default::getConfiguredOrDefaultMsScaled(
                                                              moduleConfig.telemetry.health_update_interval,
                                                              default_telemetry_broadcast_interval_secs, numOnlineNodes))) &&
            airTime->isTxAllowedChannelUtil(config.device.role != meshtastic_Config_DeviceConfig_Role_SENSOR) &&
            airTime->isTxAllowedAirUtil()) {
            sendTelemetry();
            if (transmitHistory) {
                transmitHistory->setLastSentToMesh(TX_HISTORY_KEY_HEALTH_TELEMETRY);
            }
        } else if (((lastSentToPhone == 0) || !Throttle::isWithinTimespanMs(lastSentToPhone, sendToPhoneIntervalMs)) &&
                   (service->isToPhoneQueueEmpty())) {
            // Just send to phone when it's not our time to send to mesh yet.
            sendTelemetry(NODENUM_BROADCAST, true);
            lastSentToPhone = millis();
        }
    }

    return result;
}

bool HealthTelemetryModule::wantUIFrame()
{
    return moduleConfig.telemetry.health_screen_enabled;
}

bool HealthTelemetryModule::getCurrentHeartBpm(uint8_t *bpmOut) const
{
    meshtastic_Telemetry measurement = meshtastic_Telemetry_init_zero;
    if (!max30102Sensor.getMetrics(&measurement) || !measurement.variant.health_metrics.has_heart_bpm) {
        return false;
    }

    if (bpmOut) {
        *bpmOut = measurement.variant.health_metrics.heart_bpm;
    }
    return true;
}

bool HealthTelemetryModule::isHeartRateActive() const
{
    return max30102Sensor.hasSensor() && max30102Sensor.isHrEngaged();
}

bool HealthTelemetryModule::getHeartRateSensitivity(uint8_t *levelOut, uint8_t *maxOut) const
{
    if (!max30102Sensor.hasSensor()) {
        return false;
    }
    if (levelOut) {
        *levelOut = max30102Sensor.getSensitivity();
    }
    if (maxOut) {
        *maxOut = max30102Sensor.getMaxSensitivity();
    }
    return true;
}

bool HealthTelemetryModule::setHeartRateSensitivity(uint8_t level)
{
    if (!max30102Sensor.hasSensor()) {
        return false;
    }
    max30102Sensor.setSensitivity(level);
    return true;
}

void HealthTelemetryModule::drawFrame(OLEDDisplay *display, OLEDDisplayUiState *state, int16_t x, int16_t y)
{
    // Header with battery, time, and title (same as clock)
    graphics::drawCommonHeader(display, x, y, "Heart Rate", true, false);
    const int headerHeight = FONT_HEIGHT_SMALL + 2;
    int contentY = y + headerHeight;

    display->setTextAlignment(TEXT_ALIGN_LEFT);
    display->setFont(FONT_SMALL);
    const int16_t fontHeight = _fontHeight(FONT_SMALL);

    meshtastic_Telemetry lastMeasurement = meshtastic_Telemetry_init_zero;
    bool hasMeasurement = false;

    // Health frame shows local sensor data only (no remote metrics).
    if (max30102Sensor.hasSensor()) {
        lastMeasurement.which_variant = meshtastic_Telemetry_health_metrics_tag;
        lastMeasurement.variant.health_metrics = meshtastic_HealthMetrics_init_zero;
        lastMeasurement.time = getTime();
        hasMeasurement = max30102Sensor.getMetrics(&lastMeasurement);
    }

    if (!hasMeasurement) {
        display->drawString(x, y + fontHeight, "Sensing...");
        return;
    }

    char hrStr[8] = "--";
    char spo2Str[8] = "--";

    if (lastMeasurement.variant.health_metrics.has_heart_bpm) {
        snprintf(hrStr, sizeof(hrStr), "%u", lastMeasurement.variant.health_metrics.heart_bpm);
    }
    if (lastMeasurement.variant.health_metrics.has_spO2) {
        snprintf(spo2Str, sizeof(spo2Str), "%u", lastMeasurement.variant.health_metrics.spO2);
    }
    // Die temperature is shown EXPLICITLY LABELLED and read from a dedicated accessor, never from the
    // health-metrics temperature field. The MAX3010x can only measure its own package temperature, which
    // is not a body temperature; the previous "T:33C" rendering read as hypothermia (see the badge's own
    // promotional artwork). It stays visible because it is a genuinely useful diagnostic - it is the only
    // in-situ measure of the thermal state that shifts the red LED's wavelength - but it may not
    // masquerade as a vital sign.
    char dieTempStr[16] = "";
    float dieTempC = 0.0f;
    if (max30102Sensor.getDieTemperatureC(&dieTempC)) {
        if (moduleConfig.telemetry.environment_display_fahrenheit) {
            snprintf(dieTempStr, sizeof(dieTempStr), " die:%.0fF", UnitConversions::CelsiusToFahrenheit(dieTempC));
        } else {
            snprintf(dieTempStr, sizeof(dieTempStr), " die:%.0fC", dieTempC);
        }
    }

    char metricLine[48];
    snprintf(metricLine, sizeof(metricLine), "HR:%s O2:%s%s", hrStr, spo2Str, dieTempStr);
    display->drawString(x, contentY, metricLine);

    uint32_t irWave[MAX30102_BUFFER_LEN];
    uint32_t redWave[MAX30102_BUFFER_LEN];
    uint16_t waveCount = 0;

    const bool hasLocalWaveform =
        max30102Sensor.getRawWaveformSnapshot(irWave, redWave, MAX30102_BUFFER_LEN, &waveCount);
    if (!hasLocalWaveform) {
        contentY += fontHeight;
        display->drawString(x, contentY, "Place Finger on Sensor...");
        return;
    }

    contentY += fontHeight;
    const int16_t graphTop = contentY + 1;
    const int16_t graphHeight = display->getHeight() - graphTop;
    const int16_t graphWidth = display->getWidth();
    const int16_t laneGap = 2;
    const int16_t laneHeight = (graphHeight - laneGap) / 2;
    const int16_t lane2Height = graphHeight - laneHeight - laneGap;

    if (laneHeight < 6 || lane2Height < 6) {
        display->drawString(x, contentY, "Waveform area too small");
        return;
    }

    drawWaveformLane(display, irWave, waveCount, x, graphTop, graphWidth, laneHeight, "IR");
    drawWaveformLane(display, redWave, waveCount, x, graphTop + laneHeight + laneGap, graphWidth, lane2Height, "RED");

    graphics::drawCommonFooter(display, x, y);
}

bool HealthTelemetryModule::handleReceivedProtobuf(const meshtastic_MeshPacket &mp, meshtastic_Telemetry *t)
{
    if (!t) {
        return false;
    }

    // In screen-only local mode, keep OLED sourced from local sensor cache only.
    if (!moduleConfig.telemetry.health_measurement_enabled && moduleConfig.telemetry.health_screen_enabled) {
        return false;
    }

    if (t->which_variant == meshtastic_Telemetry_health_metrics_tag) {
#if defined(DEBUG_PORT) && !defined(DEBUG_MUTE)
        const char *sender = getSenderShortName(mp);

        LOG_INFO("(Received from %s): temperature=%f, heart_bpm=%d, spO2=%d,", sender, t->variant.health_metrics.temperature,
                 t->variant.health_metrics.heart_bpm, t->variant.health_metrics.spO2);

#endif
        // release previous packet before occupying a new spot
        if (lastMeasurementPacket != nullptr)
            packetPool.release(lastMeasurementPacket);

        lastMeasurementPacket = packetPool.allocCopy(mp);
    }

    return false; // Let others look at this message also if they want
}

bool HealthTelemetryModule::getHealthTelemetry(meshtastic_Telemetry *m)
{
    bool valid = false;
    bool hasSensor = false;
    bool get_metrics;
    m->time = getTime();
    m->which_variant = meshtastic_Telemetry_health_metrics_tag;
    m->variant.health_metrics = meshtastic_HealthMetrics_init_zero;

    if (max30102Sensor.hasSensor()) {
        get_metrics = max30102Sensor.getMetrics(m);
        valid = valid || get_metrics; // avoid short-circuit evaluation rules
        hasSensor = true;
    }
    if (mlx90614Sensor.hasSensor()) {
        get_metrics = mlx90614Sensor.getMetrics(m);
        valid = valid || get_metrics;
        hasSensor = true;
    }

    return valid && hasSensor;
}

bool HealthTelemetryModule::updateLocalMeasurement()
{
    meshtastic_Telemetry m = meshtastic_Telemetry_init_zero;
    m.which_variant = meshtastic_Telemetry_health_metrics_tag;
    m.time = getTime();

    const bool hasFreshMetrics = getHealthTelemetry(&m);

    meshtastic_MeshPacket *p = allocDataProtobuf(m);
    if (!p) {
        return false;
    }
    p->from = nodeDB->getNodeNum();
    p->to = nodeDB->getNodeNum();
    p->rx_time = getTime();

    if (lastMeasurementPacket != nullptr) {
        packetPool.release(lastMeasurementPacket);
    }
    lastMeasurementPacket = packetPool.allocCopy(*p);
    packetPool.release(p);
    return hasFreshMetrics;
}

int HealthTelemetryModule::handleDeepSleep(void *unused)
{
    (void)unused;

    if (max30102Sensor.hasSensor()) {
        max30102Sensor.prepareDeepSleep();
    }

    return 0;
}

meshtastic_MeshPacket *HealthTelemetryModule::allocReply()
{
    if (!moduleConfig.telemetry.health_measurement_enabled) {
        return NULL;
    }

    if (currentRequest) {
        if (isMultiHopBroadcastRequest() && !isSensorOrRouterRole()) {
            ignoreRequest = true;
            return NULL;
        }
        auto req = *currentRequest;
        const auto &p = req.decoded;
        meshtastic_Telemetry scratch;
        meshtastic_Telemetry *decoded = NULL;
        memset(&scratch, 0, sizeof(scratch));
        if (pb_decode_from_bytes(p.payload.bytes, p.payload.size, &meshtastic_Telemetry_msg, &scratch)) {
            decoded = &scratch;
        } else {
            LOG_ERROR("Error decoding HealthTelemetry module!");
            return NULL;
        }
        // Check for a request for health metrics
        if (decoded->which_variant == meshtastic_Telemetry_health_metrics_tag) {
            meshtastic_Telemetry m = meshtastic_Telemetry_init_zero;
            if (getHealthTelemetry(&m)) {
                LOG_INFO("Health telemetry reply to request");
                return allocDataProtobuf(m);
            } else {
                return NULL;
            }
        }
    }
    return NULL;
}

bool HealthTelemetryModule::sendTelemetry(NodeNum dest, bool phoneOnly)
{
    if (!moduleConfig.telemetry.health_measurement_enabled) {
        return false;
    }

    meshtastic_Telemetry m = meshtastic_Telemetry_init_zero;
    m.which_variant = meshtastic_Telemetry_health_metrics_tag;
    m.time = getTime();
    if (getHealthTelemetry(&m)) {
        LOG_INFO("Send: temperature=%f, heart_bpm=%d, spO2=%d", m.variant.health_metrics.temperature,
                 m.variant.health_metrics.heart_bpm, m.variant.health_metrics.spO2);

        sensor_read_error_count = 0;

        meshtastic_MeshPacket *p = allocDataProtobuf(m);
        p->to = dest;
        p->decoded.want_response = false;
        if (config.device.role == meshtastic_Config_DeviceConfig_Role_SENSOR)
            p->priority = meshtastic_MeshPacket_Priority_RELIABLE;
        else
            p->priority = meshtastic_MeshPacket_Priority_BACKGROUND;
        // release previous packet before occupying a new spot
        if (lastMeasurementPacket != nullptr)
            packetPool.release(lastMeasurementPacket);

        lastMeasurementPacket = packetPool.allocCopy(*p);
        if (phoneOnly) {
            LOG_INFO("Send packet to phone");
            service->sendToPhone(p);
        } else {
            LOG_INFO("Send packet to mesh");
            service->sendToMesh(p, RX_SRC_LOCAL, true);

            if (config.device.role == meshtastic_Config_DeviceConfig_Role_SENSOR && config.power.is_power_saving) {
                LOG_DEBUG("Start next execution in 5s, then sleep");
                sleepOnNextExecution = true;
                setIntervalFromNow(5000);
            }
        }
        return true;
    }
    return false;
}

#endif
