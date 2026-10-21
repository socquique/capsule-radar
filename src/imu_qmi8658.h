#pragma once
// Minimal QMI8658 (6-axis IMU) driver over I2C. Device-only. We only use the
// accelerometer's Z axis to detect "face-down" (screen toward the ground); the decision
// itself lives in facedown_sleep.h.
#include <stdint.h>

bool imu_begin();                 // init; false if the chip isn't found
bool imu_read_az(int16_t *az);    // accelerometer Z (±2 g, 16384 LSB/g); false if unavailable
