// AXP2101 battery readout via XPowersLib. Shares the I2C bus with touch/IMU; only
// read it from the LVGL loop (core 1), not the network task.
#include "battery.h"
#include "config.h"
#include <Arduino.h>
#include <Wire.h>
#define XPOWERS_CHIP_AXP2101
#include "XPowersLib.h"

static XPowersPMU PMU;
static bool s_ok = false;

bool battery_begin() {
    s_ok = PMU.begin(Wire, AXP2101_SLAVE_ADDRESS, PIN_I2C_SDA, PIN_I2C_SCL);
    if (s_ok) {
        PMU.enableBattDetection();
        PMU.enableBattVoltageMeasure();
        Serial.println("[batt] AXP2101 ready");
    } else {
        Serial.println("[batt] AXP2101 not found");
    }
    return s_ok;
}

bool battery_present()  { return s_ok && PMU.isBatteryConnect(); }
int  battery_percent()  { return s_ok ? PMU.getBatteryPercent() : -1; }
bool battery_charging() { return s_ok && PMU.isCharging(); }

void battery_enable_codec_rail() {
    if (!s_ok) return;
    PMU.setALDO1Voltage(3300);   // ES8311 analog supply (the reference board enables this)
    PMU.enableALDO1();
    Serial.println("[batt] ALDO1 (codec AVDD) enabled @3.3V");
}
