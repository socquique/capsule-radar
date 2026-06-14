// AXP2101 battery readout via XPowersLib. Shares the I2C bus with touch/IMU; only
// read it from the LVGL loop (core 1), not the network task.
#include "battery.h"
#include "config.h"
#include "app_log.h"
#include <Arduino.h>
#include <Wire.h>
#define XPOWERS_CHIP_AXP2101
#include "XPowersLib.h"

static XPowersPMU PMU;
static bool s_ok = false;
static uint32_t s_lastSampleMs = 0;
static bool s_present = false;
static int s_percent = -1;
static bool s_charging = false;

static void sample_battery() {
    if (!s_ok) {
        s_present = false;
        s_percent = -1;
        s_charging = false;
        return;
    }
    const uint32_t now = millis();
    if (s_lastSampleMs != 0 && now - s_lastSampleMs < 1000UL) return;
    s_lastSampleMs = now;
    s_present = PMU.isBatteryConnect();
    s_percent = s_present ? PMU.getBatteryPercent() : -1;
    s_charging = PMU.isCharging();
}

bool battery_begin() {
    s_ok = PMU.begin(Wire, AXP2101_SLAVE_ADDRESS, PIN_I2C_SDA, PIN_I2C_SCL);
    Wire.setClock(I2C_BUS_HZ);
    Wire.setTimeOut(20);
    if (s_ok) {
        PMU.enableBattDetection();
        PMU.enableBattVoltageMeasure();
        app_log_println("[batt] AXP2101 ready");
    } else {
        app_log_println("[batt] AXP2101 not found");
    }
    return s_ok;
}

bool battery_present()  { sample_battery(); return s_present; }
int  battery_percent()  { sample_battery(); return s_percent; }
bool battery_charging() { sample_battery(); return s_charging; }

void battery_enable_codec_rail() {
    if (!s_ok) return;
    PMU.setALDO1Voltage(3300);   // ES8311 analog supply (the reference board enables this)
    PMU.enableALDO1();
    app_log_println("[batt] ALDO1 (codec AVDD) enabled @3.3V");
}
