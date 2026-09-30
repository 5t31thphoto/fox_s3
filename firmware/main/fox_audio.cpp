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
static int8_t   s_muted = -1;          // -1 unknown, 0 speaker on, 1 muted
static int8_t   s_mic_slot = -1;       // -1 unknown, 0 left, 1 right

// Only touch the PI4IOE (I2C) when the amp state actually changes.
static void set_mute(bool m) {
    if (!g_echo_ok) return;
    if (s_muted == (m ? 1 : 0)) return;
    g_echo.setMute(m);
    s_muted = m ? 1 : 0;
    if (!m) delay(8);                   // let the class-D amp wake before audio
}

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
    g_echo_ok = true;
    set_mute(false);
    Serial.println("FOX: EchoBase ok (16k stereo I2S)");
    return true;
}

void audio_set_volume(uint8_t volume) {
    if (!g_echo_ok) return;
    if (volume > 100) volume = 100;
    g_echo.setSpeakerVolume(volume);
    set_mute(volume == 0);
}

void audio_speaker_end() {
    set_mute(true);   // amp off while listening (no feedback / hiss)
}

void audio_mic_end() {
}

bool audio_record(int16_t* buf, size_t size_samples) {
    if (!g_echo_ok || !buf || !size_samples) return false;
    // Read the raw interleaved I2S stream directly (this is the read that WORKS
    // on this ES8311 for VAD/FFT/lip-sync). The previous "stereo -> keep LEFT"
    // extraction read the SILENT slot on this codec, which killed the mic when
    // the audio-reactive toys were added.
    return g_echo.record((uint8_t*)buf, (int)(size_samples * sizeof(int16_t)));
}

bool audio_record_mono(int16_t* buf, size_t size_samples) {
    if (!g_echo_ok || !buf || !size_samples) return false;
    static int16_t st[512];                       // 256 stereo frames per read
    size_t done = 0;
    while (done < size_samples) {
        size_t frames = size_samples - done; if (frames > 256) frames = 256;
        if (!g_echo.record((uint8_t*)st, (int)(frames * 2 * sizeof(int16_t))))
            return false;
        if (s_mic_slot < 0) {                     // learn which slot is the mic
            uint32_t eL = 0, eR = 0;
            for (size_t i = 0; i < frames; ++i) { eL += abs(st[2*i]); eR += abs(st[2*i+1]); }
            if (eL + eR > frames * 40) {          // real signal, not idle noise
                s_mic_slot = (eR > eL) ? 1 : 0;
                Serial.printf("FOX: mic on %s slot (L=%u R=%u)\n",
                              s_mic_slot ? "RIGHT" : "LEFT", (unsigned)eL, (unsigned)eR);
            }
        }
        int slot = s_mic_slot;
        if (slot < 0) {                           // until learned: louder slot
            uint32_t eL = 0, eR = 0;
            for (size_t i = 0; i < frames; ++i) { eL += abs(st[2*i]); eR += abs(st[2*i+1]); }
            slot = (eR > eL) ? 1 : 0;
        }
        for (size_t i = 0; i < frames; ++i) buf[done + i] = st[2*i + slot];
        done += frames;
    }
    return true;
}

// Write mono int16 @ SAMPLE_RATE as stereo interleaved to EchoBase.
static bool play_mono16_stereo(const int16_t* mono, size_t n_samples) {
    if (!g_echo_ok || !mono || !n_samples) return false;
    // chunk to limit stack/heap
    static int16_t stereo[512 * 2];
    size_t off = 0;
    set_mute(false);
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

    // Area-average resample to SAMPLE_RATE (box low-pass + decimate) so
    // downsampling doesn't fold harmonics back as harsh aliasing.
    size_t out_n = (size_t)((uint64_t)size_samples * SAMPLE_RATE / sample_rate);
    if (out_n < 1) out_n = 1;
    int16_t* tmp = (int16_t*)fox_alloc(out_n * sizeof(int16_t));
    if (!tmp) return false;
    const float step = (float)sample_rate / SAMPLE_RATE;
    for (size_t i = 0; i < out_n; ++i) {
        size_t a = (size_t)(i * step), b = (size_t)((i + 1) * step);
        if (b <= a) b = a + 1;
        if (b > size_samples) b = size_samples;
        if (a >= size_samples) a = size_samples - 1;
        int32_t acc = 0; for (size_t k = a; k < b; ++k) acc += buf[k];
        tmp[i] = (int16_t)(acc / (int32_t)(b - a));
    }
    bool ok = play_mono16_stereo(tmp, out_n);
    free(tmp);
    return ok;
}

bool audio_play_pcm8(const uint8_t* buf, size_t size_bytes, int sample_rate) {
    if (!g_echo_ok || !buf || !size_bytes) return false;
    if (sample_rate <= 0) sample_rate = 22050;

    // unsigned 8-bit mono (SAM, centred on 128) → int16 mono @ SAMPLE_RATE,
    // area-averaged so SAM's buzzy upper harmonics don't alias into the voice.
    size_t out_n = (size_t)((uint64_t)size_bytes * SAMPLE_RATE / sample_rate);
    if (out_n < 1) out_n = 1;
    int16_t* tmp = (int16_t*)fox_alloc(out_n * sizeof(int16_t));
    if (!tmp) return false;
    const float step = (float)sample_rate / SAMPLE_RATE;
    for (size_t i = 0; i < out_n; ++i) {
        size_t a = (size_t)(i * step), b = (size_t)((i + 1) * step);
        if (b <= a) b = a + 1;
        if (b > size_bytes) b = size_bytes;
        if (a >= size_bytes) a = size_bytes - 1;
        int32_t acc = 0; for (size_t k = a; k < b; ++k) acc += (int32_t)buf[k] - 128;
        int32_t v = (acc * 224) / (int32_t)(b - a);          // ~0.875 FS headroom
        tmp[i] = (int16_t)(v > 32767 ? 32767 : (v < -32768 ? -32768 : v));
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
