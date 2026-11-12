#pragma once
// ES8311 codec + speaker: short alert "pings". Device-only.
// Boards with an on/off buzzer instead (BOARD_HAS_BUZZER, the LCD 2.1) beep the same cues:
// no pitch or volume there (the web page hides the slider); "Mute alerts" silences it.
//
// Bus discipline: the ES8311 is configured over the shared I2C bus, so audio_begin()
// MUST run on core 1 (setup), like the other I2C devices. Playback afterwards only
// touches the I2S peripheral + the PA GPIO (no I2C), so it runs in its own task.
#include <stdbool.h>

enum AudioCue {
    AUDIO_NEW   = 0,   // new aircraft entered range (soft single beep)
    AUDIO_ALERT = 1,   // emergency / military contact (urgent double beep)
};

bool audio_begin();                 // init ES8311 + I2S + PA + playback task (call on core 1)
bool audio_present();
void audio_set_volume(int pct);     // 0..100 (software amplitude)
void audio_set_muted(bool muted);
void audio_play(AudioCue cue);      // non-blocking: signals the playback task
void audio_selftest();              // ~2 s continuous tone for by-ear verification
void audio_loop();                  // call from loop() (core 1): times a buzzer's beeps; no-op elsewhere
