// Minimal Arduino.h stub for host-side testing of ReliableJsonStream.
// Stream::timedRead()/readBytes() reproduce the canonical Arduino core semantics verbatim —
// that is the whole point of the test, so do not "improve" them here.
#pragma once
#include <stddef.h>
#include <stdint.h>

// Virtual clock: the mock advances it, so a multi-second stall costs no real time.
extern unsigned long g_virtualMs;
inline unsigned long millis() { return g_virtualMs; }

class Print {
public:
    virtual ~Print() {}
    virtual size_t write(uint8_t) = 0;
    virtual size_t write(const uint8_t *buffer, size_t size) {
        size_t n = 0;
        while (size--) n += write(*buffer++);
        return n;
    }
};

// Enabling ARDUINOJSON_ENABLE_ARDUINO_STREAM also pulls in ArduinoJson's Printable converter.
class Printable {
public:
    virtual ~Printable() {}
    virtual size_t printTo(Print &p) const = 0;
};

class Stream : public Print {
protected:
    unsigned long _timeout = 1000;      // Arduino's default. Deliberately kept.
    unsigned long _startMillis = 0;

    int timedRead() {
        int c;
        _startMillis = millis();
        do {
            c = read();
            if (c >= 0) return c;
        } while (millis() - _startMillis < _timeout);
        return -1;                       // timed out
    }

public:
    virtual int available() = 0;
    virtual int read() = 0;
    virtual int peek() = 0;
    virtual void flush() {}

    void setTimeout(unsigned long t) { _timeout = t; }
    unsigned long getTimeout() const { return _timeout; }

    virtual size_t readBytes(char *buffer, size_t length) {
        size_t count = 0;
        while (count < length) {
            int c = timedRead();
            if (c < 0) break;
            *buffer++ = (char)c;
            count++;
        }
        return count;
    }
};
