// fox_sam.c — the "critter" retro voice.
//
// IMPORTANT LICENSING NOTE
// ------------------------
// The obvious way to get a retro Commodore-SAM voice is to lift s-macke's
// reverse-engineered SAM C port. That code's provenance/licensing is murky
// (reverse-engineered 1980s commercial software), so we deliberately DO NOT
// ship it. Instead this is a small, original formant-ish synth written from
// scratch. It sounds like a chirpy cartoon critter rather than SAM exactly,
// which honestly suits a fox better. If you *want* true SAM, drop a licensed
// sam.c here that exports the same two symbols and it will be used instead.
//
// It exposes the C ABI fox_voice.cpp expects:
//   int  sam_render(text, speed, pitch, throat, mouth, out, out_len)
//   void sam_free(p)
// producing 8-bit unsigned mono PCM at ~22050 Hz.
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ctype.h>

#define SR 22050

// crude phoneme -> (formant1, formant2, voiced, duration_ms)
typedef struct { float f1, f2; int voiced; int dur; } Ph;

// Map a letter to a rough formant pair. Vowels get clear formants; consonants
// get noise bursts or short stops. This is intentionally approximate — the
// charm is in the chirpy imperfection.
static Ph phoneme_for(char c) {
    c = tolower((unsigned char)c);
    switch (c) {
        case 'a': return (Ph){ 800, 1200, 1, 90 };
        case 'e': return (Ph){ 500, 1800, 1, 80 };
        case 'i': return (Ph){ 300, 2200, 1, 75 };
        case 'o': return (Ph){ 500,  900, 1, 90 };
        case 'u': return (Ph){ 320,  800, 1, 85 };
        case 'y': return (Ph){ 350, 2000, 1, 70 };
        case 'm': case 'n': return (Ph){ 250, 1000, 1, 60 };
        case 'l': case 'r': return (Ph){ 400, 1300, 1, 55 };
        case 's': case 'f': case 'h': return (Ph){ 0, 0, 0, 55 };  // noise
        case 't': case 'k': case 'p': return (Ph){ 0, 0, 0, 30 };  // stop
        case 'b': case 'd': case 'g': return (Ph){ 200, 900, 1, 45 };
        case ' ': return (Ph){ 0, 0, 0, 45 };  // pause
        default:  return (Ph){ 450, 1400, 1, 55 };
    }
}

int sam_render(const char* text, uint8_t speed, uint8_t pitch,
               uint8_t throat, uint8_t mouth, uint8_t** out, int* out_len) {
    if (!text || !out || !out_len) return 0;
    int n = (int)strlen(text);
    if (n == 0) return 0;

    // speed scales duration; pitch scales base f0; throat/mouth tilt formants.
    float spd = 0.5f + (speed / 128.0f);           // ~0.5..2.5
    float f0  = 110.0f + (pitch);                   // base pitch in Hz
    float throat_g = 0.6f + throat / 255.0f;
    float mouth_g  = 0.6f + mouth / 255.0f;

    // estimate total samples
    long total = 0;
    for (int i = 0; i < n; ++i) total += (long)(phoneme_for(text[i]).dur / spd);
    total = (long)(total * SR / 1000) + SR / 20;
    uint8_t* buf = (uint8_t*)malloc(total);
    if (!buf) return 0;

    long pos = 0;
    float phase = 0.f, ph1 = 0.f, ph2 = 0.f;
    unsigned rng = 0x1234u;
    for (int i = 0; i < n && pos < total; ++i) {
        Ph p = phoneme_for(text[i]);
        int dur_samples = (int)((p.dur / spd) * SR / 1000);
        float f1 = p.f1 * mouth_g;
        float f2 = p.f2 * throat_g;
        for (int s = 0; s < dur_samples && pos < total; ++s, ++pos) {
            float val;
            if (p.voiced && f1 > 0) {
                // simple glottal buzz (sawtooth) shaped by two formant sines
                phase += f0 / SR; if (phase > 1) phase -= 1;
                float glottal = 2.f * phase - 1.f;
                ph1 += f1 / SR; if (ph1 > 1) ph1 -= 1;
                ph2 += f2 / SR; if (ph2 > 1) ph2 -= 1;
                float form = 0.6f * sinf(2 * M_PI * ph1) + 0.4f * sinf(2 * M_PI * ph2);
                val = 0.5f * glottal + 0.5f * form * glottal;
            } else {
                // unvoiced: filtered noise
                rng = rng * 1103515245u + 12345u;
                float nz = ((rng >> 16) & 0xFFFF) / 32768.0f - 1.0f;
                val = 0.4f * nz;
            }
            // envelope: quick attack/decay per phoneme to avoid clicks
            float env = 1.0f;
            int atk = dur_samples / 6 + 1;
            if (s < atk) env = (float)s / atk;
            else if (s > dur_samples - atk) env = (float)(dur_samples - s) / atk;
            val *= env;
            int sample = (int)(128 + val * 110);
            if (sample < 0) sample = 0;
            if (sample > 255) sample = 255;
            buf[pos] = (uint8_t)sample;
        }
    }
    *out = buf;
    *out_len = (int)pos;
    return 1;
}

void sam_free(uint8_t* p) { free(p); }
