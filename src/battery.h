#pragma once
// AXP2101 PMIC battery readout (via XPowersLib). Device-only.
// All calls must run on the same core as the other I2C users (the LVGL loop),
// never from the network task, to avoid bus contention.
bool battery_begin();      // init; false if the PMIC isn't found
bool battery_present();    // a LiPo is connected
int  battery_percent();    // 0..100, or -1 if unknown
bool battery_charging();   // currently charging
void battery_enable_codec_rail();  // enable ALDO1 3.3V (ES8311 analog AVDD) for audio
