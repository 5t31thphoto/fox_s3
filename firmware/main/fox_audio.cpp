// fox_audio.cpp — Echo Base ownership layer (see fox_audio.h).
//
// IMPORTANT: Do NOT include driver/i2s.h here. M5EchoBase (IDF >= 5) uses
// Arduino ESP_I2S.h; including the legacy IDF driver/i2s.h causes:
//   error: conflicting declaration 'typedef enum i2s_mode_t i2s_mode_t'
// and breaks the CI build.
#include "fox_audio.h"
#include "fox.h"
#include <math.h>
#include <string.h>
#include <stdlib.h>

#if (ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0))
M5EchoBase g_echo;
#else
M5EchoBase g_echo(I2S_NUM_0);
#endif

bool g_echo_ok = false;
static uint32_t s_play_until = 0;

bool audio_begin(uint8_t volume) {
    // Match the working mic-avatar: take the I2S bus away from M5Unified first.
    M5.Speaker.end();
    M5.Mic.end();

    // AtomS3 / AtomS3R pin map (identical for the Echo Base).
    if (!g_echo.init(SAMPLE_RATE /*16k*/, 38 /*SDA*/, 39 /*SCL*/,
                     7 /*DIN*/, 6 /*WS*/, 5 /*DOUT*/, 8 /*BCK*/, Wire)) {
        Serial.println("FOX: EchoBase init FAILED — audio off");
        g_echo_ok = false;
        return false;
    }

    // I2S is owned by M5EchoBase (ESP_I2S on IDF 5.x). No legacy i2s_set_clk.

    g_echo.setMicGain(ES8311_MIC_GAIN_6DB);
    g_echo.setSpeakerVolume(volume > 100 ? 100 : volume);
    g_echo.setMute(false);
    g_echo_ok = true;
    Serial.println("FOX: EchoBase ok");
    return true;
}

void audio_set_volume(uint8_t volume) {
    if (!g_echo_ok) return;
    if (volume > 100) volume = 100;
    g_echo.setSpeakerVolume(volume);
    g_echo.setMute(volume == 0);
}

void audio_speaker_end() {
    if (g_echo_ok) g_echo.setMute(true);
}

void audio_mic_end() {
    // EchoBase keeps the driver; next record() reuses the same I2S.
}

bool audio_record(int16_t* buf, size_t size_samples) {
    if (!g_echo_ok || !buf || !size_samples) return false;
    return g_echo.record((uint8_t*)buf, (int)(size_samples * sizeof(int16_t)));
}

bool audio_play_pcm16(const int16_t* buf, size_t size_samples, int sample_rate) {
    if (!g_echo_ok || !buf || !size_samples) return false;
    g_echo.setMute(false);
    uint32_t ms = (uint32_t)((size_samples * 1000ULL) / (sample_rate > 0 ? sample_rate : SAMPLE_RATE));
    s_play_until = millis() + ms + 20;
    return g_echo.play((uint8_t*)buf, (int)(size_samples * sizeof(int16_t)));
}

bool audio_play_pcm8(const uint8_t* buf, size_t size_bytes, int sample_rate) {
    if (!g_echo_ok || !buf || !size_bytes) return false;
    static int16_t chunk[512];
    g_echo.setMute(false);
    size_t off = 0;
    while (off < size_bytes) {
        size_t n = size_bytes - off;
        if (n > 512) n = 512;
        for (size_t i = 0; i < n; ++i)
            chunk[i] = ((int16_t)buf[off + i] - 128) << 8;
        if (!g_echo.play((uint8_t*)chunk, (int)(n * sizeof(int16_t))))
            return false;
        off += n;
    }
    uint32_t ms = (uint32_t)((size_bytes * 1000ULL) / (sample_rate > 0 ? sample_rate : 22050));
    s_play_until = millis() + ms + 20;
    return true;
}

void audio_tone(int freq_hz, int duration_ms) {
    if (!g_echo_ok || duration_ms <= 0) return;
    const int sr = SAMPLE_RATE;
    const int n = (sr * duration_ms) / 1000;
    if (n <= 0) return;
    const int MAX_N = sr / 5;  // 200 ms
    int samples = n > MAX_N ? MAX_N : n;
    int16_t* buf = (int16_t*)malloc(samples * sizeof(int16_t));
    if (!buf) return;
    for (int i = 0; i < samples; ++i) {
        float t = (float)i / (float)sr;
        float s = sinf(2.0f * 3.14159265f * (float)freq_hz * t);
        float env = 1.0f;
        if (i < 32) env = (float)i / 32.0f;
        else if (i > samples - 32) env = (float)(samples - i) / 32.0f;
        buf[i] = (int16_t)(s * env * 12000.0f);
    }
    g_echo.setMute(false);
    g_echo.play((uint8_t*)buf, samples * (int)sizeof(int16_t));
    s_play_until = millis() + (uint32_t)duration_ms + 20;
    free(buf);
}

bool audio_is_playing() {
    return millis() < s_play_until;
}
