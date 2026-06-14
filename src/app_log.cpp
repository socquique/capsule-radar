#include "app_log.h"
#include <stdarg.h>
#include <stdio.h>
#include <time.h>

static void log_timestamp(char *buf, size_t n) {
    if (!buf || n == 0) return;
    const time_t now = time(nullptr);
    if (now > 1700000000L) {
        struct tm local;
        localtime_r(&now, &local);
        strftime(buf, n, "%Y-%m-%d %H:%M:%S", &local);
    } else {
        const unsigned long up = millis() / 1000UL;
        snprintf(buf, n, "uptime %lus", up);
    }
}

static void log_prefix(void) {
    char ts[24];
    log_timestamp(ts, sizeof(ts));
    Serial.printf("[%s] ", ts);
}

class AppLogStream : public Print {
public:
    size_t write(uint8_t c) override {
        if (_lineStart) {
            log_prefix();
            _lineStart = false;
        }
        const size_t n = Serial.write(c);
        if (c == '\n') _lineStart = true;
        return n;
    }

private:
    bool _lineStart = true;
};

void app_log_printf(const char *fmt, ...) {
    log_prefix();
    char line[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt ? fmt : "", ap);
    va_end(ap);
    Serial.print(line);
}

void app_log_println(const char *msg) {
    log_prefix();
    Serial.println(msg ? msg : "");
}

Print &app_log_stream() {
    static AppLogStream s;
    return s;
}
