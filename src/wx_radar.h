#pragma once

#include <stdint.h>

#define WX_RADAR_SIZE 360
#define WX_RADAR_SOURCE_SIZE 512

void wx_radar_begin(void);
uint16_t *wx_radar_back_buffer(void);                    // network/sim: decode here
void wx_radar_commit(uint32_t frameTime, double lat, double lon);
bool wx_radar_front(const uint16_t **pixels, uint32_t *frameTime,
                    double *lat, double *lon, uint32_t *version);
