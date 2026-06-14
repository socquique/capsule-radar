#pragma once
#include <Arduino.h>

void app_log_printf(const char *fmt, ...);
void app_log_println(const char *msg);
Print &app_log_stream();
