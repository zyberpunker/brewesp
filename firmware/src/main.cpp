#include <Arduino.h>
#include <Preferences.h>
#include <esp_system.h>

#include "App.h"

namespace {
App app;
constexpr uint32_t kPowerOnRestartMagic = 0xB007C0DEUL;
constexpr uint32_t kPowerOnStabilizationDelayMs = 1500UL;
RTC_DATA_ATTR uint32_t g_power_on_restart_magic = 0;
constexpr char kBootTraceNamespace[] = "boottrace";
constexpr char kBootTraceKey[] = "current";
constexpr size_t kBootTraceMaxChars = 220;

const char* resetReasonName(esp_reset_reason_t reason) {
    switch (reason) {
        case ESP_RST_UNKNOWN:
            return "unknown";
        case ESP_RST_POWERON:
            return "power_on";
        case ESP_RST_EXT:
            return "external";
        case ESP_RST_SW:
            return "software";
        case ESP_RST_PANIC:
            return "panic";
        case ESP_RST_INT_WDT:
            return "interrupt_watchdog";
        case ESP_RST_TASK_WDT:
            return "task_watchdog";
        case ESP_RST_WDT:
            return "watchdog";
        case ESP_RST_DEEPSLEEP:
            return "deep_sleep";
        case ESP_RST_BROWNOUT:
            return "brownout";
        case ESP_RST_SDIO:
            return "sdio";
        default:
            return "other";
    }
}

String readBootTrace() {
    Preferences prefs;
    if (!prefs.begin(kBootTraceNamespace, true)) {
        return String();
    }
    const String trace = prefs.getString(kBootTraceKey, "");
    prefs.end();
    return trace;
}

void writeBootTrace(const String& trace) {
    Preferences prefs;
    if (!prefs.begin(kBootTraceNamespace, false)) {
        return;
    }
    prefs.putString(kBootTraceKey, trace.substring(0, kBootTraceMaxChars));
    prefs.end();
}

void appendBootTrace(const String& step) {
    String trace = readBootTrace();
    if (trace.isEmpty()) {
        trace = step;
    } else {
        trace += " > " + step;
    }
    writeBootTrace(trace);
}
}

void setup() {
    Serial.begin(115200);
    delay(250);

    const esp_reset_reason_t resetReason = esp_reset_reason();
    const String previousTrace = readBootTrace();
    if (!previousTrace.isEmpty()) {
        Serial.printf("[boot] previous_trace=%s\r\n", previousTrace.c_str());
    }

    writeBootTrace("reset=" + String(resetReasonName(resetReason)));
    Serial.printf("[boot] reset_reason=%s\r\n", resetReasonName(resetReason));

    const bool firstColdBootAttempt =
        resetReason == ESP_RST_POWERON && g_power_on_restart_magic != kPowerOnRestartMagic;
    if (firstColdBootAttempt) {
        g_power_on_restart_magic = kPowerOnRestartMagic;
        appendBootTrace("power_on_delay");
        Serial.printf(
            "[boot] power-on stabilization: waiting %lu ms then self-restarting\r\n",
            static_cast<unsigned long>(kPowerOnStabilizationDelayMs));
        delay(kPowerOnStabilizationDelayMs);
        appendBootTrace("self_restart");
        ESP.restart();
    }

    if (g_power_on_restart_magic == kPowerOnRestartMagic && resetReason == ESP_RST_SW) {
        appendBootTrace("after_poweron_restart");
        Serial.println("[boot] continuing after power-on stabilization restart");
    }
    g_power_on_restart_magic = 0;

    appendBootTrace("app_begin");
    app.begin();
    appendBootTrace("app_begin_ok");
}

void loop() {
    static bool bootLoopRecorded = false;
    if (!bootLoopRecorded) {
        appendBootTrace("loop_entered");
        bootLoopRecorded = true;
    }
    app.update();
    delay(50);
}
