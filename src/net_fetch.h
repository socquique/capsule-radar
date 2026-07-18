#pragma once
// HTTPS GET into a PSRAM buffer. When the server sends Content-Length the body is
// streamed straight into PSRAM, so large binaries (weather tiles, satellite JPEGs)
// never sit on the scarce internal heap — the contiguous block mbedTLS needs for the
// live-feed TLS handshake lives there, and a 100+ KB String can starve it.
// Chunked responses fall back to getString() (small payloads only reach that path).
// On success the caller owns *out and must release it with heap_caps_free().
#include <stddef.h>
#include <stdint.h>

bool net_fetch_psram(const char *url, const char *userAgent,
                     uint8_t **out, size_t *outLen, size_t maxLen,
                     int connectTimeoutMs, int totalTimeoutMs);
