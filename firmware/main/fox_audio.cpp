// fox_audio.cpp — Echo Base ownership layer (see fox_audio.h).
//
// I2S is configured STEREO 16-bit @ SAMPLE_RATE (16 kHz). All playback must
// expand mono → interleaved L/R or it sounds like pure crunch/noise.
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
    M5.Speaker.end();
    M5.Mic.end();

    if (!g_echo.init(SAMPLE_RATE /*16k*/, 38 /*SDA*/, 39 /*SCL*/,
                     7 /*DIN*/, 6 /*WS*/, 5 /*DOUT*/, 8 /*BCK*/, Wire)) {
        Serial.println("FOX: EchoBase init FAILED — audio off");
        g_echo_ok = false;
        return false;
    }

    g_echo.setMicGain(ES8311_MIC_GAIN_6DB);
    g_echo.setSpeakerVolume(volume > 100 ? 100 : volume);
    g_echo.setMute(false);
    g_echo_ok = true;
    Serial.println("FOX: EchoBase ok (16k stereo I2S)");
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
}

bool audio_record(int16_t* buf, size_t size_samples) {
    if (!g_echo_ok || !buf || !size_samples) return false;
    // EchoBase record is raw I2S bytes (stereo 16-bit). Read stereo frames and
    // keep left channel for mono ASR consumers.
    size_t bytes = size_samples * 4;  // 2 ch * 2 bytes
    uint8_t* raw = (uint8_t*)malloc(bytes);
    if (!raw) return false;
    bool ok = g_echo.record(raw, (int)bytes);
    if (ok) {
        const int16_t* s = (const int16_t*)raw;
        for (size_t i = 0; i < size_samples; ++i)
            buf[i] = s[i * 2];  // left
    }
    free(raw);
    return ok;
}

// Write mono int16 @ SAMPLE_RATE as stereo interleaved to EchoBase.
static bool play_mono16_stereo(const int16_t* mono, size_t n_samples) {
    if (!g_echo_ok || !mono || !n_samples) return false;
    // chunk to limit stack/heap
    static int16_t stereo[512 * 2];
    size_t off = 0;
    g_echo.setMute(false);
    while (off < n_samples) {
        size_t n = n_samples - off;
        if (n > 512) n = 512;
        for (size_t i = 0; i < n; ++i) {
            int16_t s = mono[off + i];
            stereo[i * 2]     = s;
            stereo[i * 2 + 1] = s;
        }
        if (!g_echo.play((uint8_t*)stereo, (int)(n * 2 * sizeof(int16_t))))
            return false;
        off += n;
    }
    uint32_t ms = (uint32_t)((n_samples * 1000ULL) / SAMPLE_RATE);
    s_play_until = millis() + ms + 30;
    return true;
}

bool audio_play_pcm16(const int16_t* buf, size_t size_samples, int sample_rate) {
    if (!g_echo_ok || !buf || !size_samples) return false;
    if (sample_rate <= 0) sample_rate = SAMPLE_RATE;

    if (sample_rate == SAMPLE_RATE) {
        return play_mono16_stereo(buf, size_samples);
    }

    // Linear resample to SAMPLE_RATE then stereo
    size_t out_n = (size_t)((uint64_t)size_samples * SAMPLE_RATE / sample_rate);
    if (out_n < 1) out_n = 1;
    int16_t* tmp = (int16_t*)malloc(out_n * sizeof(int16_t));
    if (!tmp) return false;
    for (size_t i = 0; i < out_n; ++i) {
        size_t src = (size_t)((uint64_t)i * sample_rate / SAMPLE_RATE);
        if (src >= size_samples) src = size_samples - 1;
        tmp[i] = buf[src];
    }
    bool ok = play_mono16_stereo(tmp, out_n);
    free(tmp);
    return ok;
}

bool audio_play_pcm8(const uint8_t* buf, size_t size_bytes, int sample_rate) {
    if (!g_echo_ok || !buf || !size_bytes) return false;
    if (sample_rate <= 0) sample_rate = 22050;

    // unsigned 8-bit mono → int16 mono @ SAMPLE_RATE
    size_t out_n = (size_t)((uint64_t)size_bytes * SAMPLE_RATE / sample_rate);
    if (out_n < 1) out_n = 1;
    int16_t* tmp = (int16_t*)malloc(out_n * sizeof(int16_t));
    if (!tmp) return false;
    for (size_t i = 0; i < out_n; ++i) {
        size_t src = (size_t)((uint64_t)i * sample_rate / SAMPLE_RATE);
        if (src >= size_bytes) src = size_bytes - 1;
        // SAM: unsigned 8-bit centered at 128
        tmp[i] = ((int16_t)buf[src] - 128) << 8;
    }
    bool ok = play_mono16_stereo(tmp, out_n);
    free(tmp);
    return ok;
}

void audio_tone(int freq_hz, int duration_ms) {
    if (!g_echo_ok || duration_ms <= 0) return;
    const int sr = SAMPLE_RATE;
    int n = (sr * duration_ms) / 1000;
    if (n <= 0) return;
    const int MAX_N = sr / 5;  // 200 ms
    int samples = n > MAX_N ? MAX_N : n;
    int16_t* buf = (int16_t*)malloc(samples * sizeof(int16_t));
    if (!buf) return;
    for (int i = 0; i < samples; ++i) {
        float t = (float)i / (float)sr;
        float env = 1.0f;
        if (i < samples / 10) env = (float)i / (samples / 10);
        else if (i > samples * 9 / 10) env = (float)(samples - i) / (samples / 10);
        buf[i] = (int16_t)(sinf(2.0f * 3.14159265f * freq_hz * t) * 12000.0f * env);
    }
    play_mono16_stereo(buf, (size_t)samples);
    free(buf);
}

bool audio_is_playing() {
    return (int32_t)(millis() - s_play_until) < 0;
}
