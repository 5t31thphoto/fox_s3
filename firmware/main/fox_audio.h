// fox_audio.h — thin wrapper around the official M5Atomic-EchoBase library.
//
// Why this exists:
//   Setting M5.config().external_speaker.atomic_echo = true before M5.begin()
//   caused a hang on AtomS3R + Atomic Echo Base (display never initialized,
//   no serial after "Returned from app_main()"). The working mic-avatar demo
//   from M5 deliberately avoids that path and uses the standalone EchoBase
//   library instead ("mic doesn't work well with M5Unified").
//
// Pattern (matches the working demo):
//   1. Plain M5.begin()  → display comes up
//   2. M5.Speaker.end() / M5.Mic.end() so M5Unified does not own I2S
//   3. echobase.init(16000, 38, 39, 7, 6, 5, 8, Wire)
//   4. All record/play goes through this wrapper
#pragma once
#include <Arduino.h>
#include <M5Unified.h>
#include "M5EchoBase.h"

// Global instance (defined in fox_main.cpp)
extern M5EchoBase g_echo;
extern bool       g_echo_ok;

// Call once after plain M5.begin(). Returns true on success.
bool audio_begin(uint8_t volume);

// Volume 0..100 (EchoBase scale). Also updates mute state.
void audio_set_volume(uint8_t volume);

// Stop any ongoing playback / release bus for recording.
void audio_speaker_end();
void audio_mic_end();

// Record int16 mono @ 16 kHz into buf. Returns true on success.
// size_samples is number of int16 samples (not bytes).
bool audio_record(int16_t* buf, size_t size_samples);

// Play int16 mono PCM. sample_rate is typically 16000 or 22050.
// Blocks until the chunk has been written (EchoBase play is synchronous-ish).
bool audio_play_pcm16(const int16_t* buf, size_t size_samples, int sample_rate);

// Play 8-bit unsigned PCM (SAM output). Converts on the fly into a small stack
// buffer in chunks so we don't need a huge temp allocation.
bool audio_play_pcm8(const uint8_t* buf, size_t size_bytes, int sample_rate);

// Simple blocking tone (for babble fallback). Generates a short sine burst.
void audio_tone(int freq_hz, int duration_ms);

// True while the last play is still draining (best-effort; EchoBase has no
// isPlaying, so we track a simple deadline).
bool audio_is_playing();
