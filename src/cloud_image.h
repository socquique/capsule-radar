#pragma once

#include <stdint.h>
#include "wx_radar.h"

void cloud_image_begin(void);
uint16_t *cloud_image_back_buffer(void);
void cloud_image_commit(uint32_t frameTime, double lat, double lon);
bool cloud_image_front(const uint16_t **pixels, uint32_t *frameTime,
                       double *lat, double *lon, uint32_t *version);
