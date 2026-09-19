// fox_voice.cpp — the fox's mouth.
//
// Three voices, chosen at flash time via cfg.voice_pack:
//   * "chatterbox": PicoTTS, tuned a little higher/faster to sound cute.
//   * "critter":    SAM (retro Commodore voice), tiny and charming.
//   * babble:       procedural chirps — always available, used when a pack is
//                   missing and for "*thinking*" filler.
//
// FIX: Playback goes through fox_audio (standalone M5EchoBase) instead of
// M5.Speaker. The atomic_echo path inside M5.begin hung on this hardware;
// the working mic-avatar demo uses the same EchoBase approach.
//
// A hard lesson baked in here: PicoTTS hands us a pointer to its OWN internal
// buffer in the sample callback and reuses it immediately. We copy every chunk
// into our own memory before playing.
#include "fox.h"
#include "fox_decls.h"
#include "fox_audio.h"
#include <esp_heap_caps.h>

#if __has_include("picotts.h")
#include "picotts.h"
#define HAVE_PICO 1
#else
#define HAVE_PICO 0
#endif

static FoxConfig g_cfg;
static bool g_pico_ok = false;

void voice_begin(const FoxConfig& cfg) {
    g_cfg = cfg;
    g_pico_ok = (HAVE_PICO && cfg.voice_pack == "chatterbox");
}

bool voice_is_pico() { return g_pico_ok; }

// Drive the mouth from a slice of PCM while the fox speaks.
__attribute__((unused)) static void mouth_from_pcm16(const int16_t* s, size_t n, FoxMood mood) {
    if (!s || !n) return;
    uint32_t acc = 0;
    for (size_t i = 0; i < n; ++i) acc += abs(s[i]);
    face_set_mouth((acc / (float)n) / 6000.0f);
    face_draw(mood);
}
static void mouth_from_pcm8(const uint8_t* s, size_t n, FoxMood mood) {
    if (!s || !n) return;
    uint32_t acc = 0;
    for (size_t i = 0; i < n; ++i) acc += abs((int)s[i] - 128);
    face_set_mouth((acc / (float)n) / 90.0f);
    face_draw(mood);
}

static FoxMood g_speaking_mood = MOOD_CALM;

// ---- PicoTTS path -----------------------------------------------------------
#if HAVE_PICO
static volatile bool s_tts_done = true;

static void pico_cb(int16_t* buf, unsigned count) {
    if (!count) return;
    int16_t* copy = (int16_t*)heap_caps_malloc(count * sizeof(int16_t),
                                               MALLOC_CAP_8BIT);
    if (!copy) return;
    memcpy(copy, buf, count * sizeof(int16_t));
    mouth_from_pcm16(copy, count, g_speaking_mood);
    audio_play_pcm16(copy, count, 16000);
    while (audio_is_playing()) { taskYIELD(); }
    free(copy);
}
static void pico_idle() { s_tts_done = true; }
static void pico_err()  { s_tts_done = true; }

static bool pico_say(const String& text) {
    if (!g_echo_ok) return false;
    audio_set_volume(g_cfg.volume > 100 ? 100 : g_cfg.volume);
    s_tts_done = false;
    if (!picotts_init(5, pico_cb, 1)) return false;
    picotts_set_idle_notify(pico_idle);
    picotts_set_error_notify(pico_err);
    picotts_add(text.c_str(), text.length() + 1);
    uint32_t guard = millis();
    while (!s_tts_done && millis() - guard < 12000) { M5.update(); delay(4); }
    picotts_shutdown();
    return true;
}
#endif

// ---- SAM path ---------------------------------------------------------------
static bool sam_say(const String& text) {
    uint8_t* pcm = nullptr; int len = 0;
    if (!sam_render(text.c_str(), 72, 96, 110, 160, &pcm, &len) || !pcm)
        return false;
    if (!g_echo_ok) { sam_free(pcm); return false; }
    audio_set_volume(g_cfg.volume > 100 ? 100 : g_cfg.volume);
    // Play in ~40ms chunks (22050Hz 8-bit) so the mouth lip-syncs.
    const int CH = 900;
    for (int off = 0; off < len; off += CH) {
        int n = min(CH, len - off);
        mouth_from_pcm8(pcm + off, n, g_speaking_mood);
        audio_play_pcm8(pcm + off, n, 22050);
        while (audio_is_playing()) { M5.update(); delay(2); }
    }
    face_set_mouth(0); face_draw(g_speaking_mood);
    sam_free(pcm);
    return true;
}

// ---- babble fallback --------------------------------------------------------
void voice_babble(FoxMood mood, int syllables) {
    if (!g_echo_ok) return;
    audio_set_volume(g_cfg.volume > 100 ? 100 : g_cfg.volume);
    int base;
    switch (mood) {
        case MOOD_SLEEPY:  base = 520;  break;
        case MOOD_GRUMPY:  base = 440;  break;
        case MOOD_EXCITED: base = 900;  break;
        case MOOD_HAPPY:   base = 760;  break;
        default:           base = 640;  break;
    }
    if (syllables < 1) syllables = 1;
    if (syllables > 10) syllables = 10;
    for (int i = 0; i < syllables; ++i) {
        int f = base + (int)(esp_random() % 220) - 110;
        face_set_mouth(0.7f); face_draw(mood);
        audio_tone(f, 70 + (int)(esp_random() % 60));
        while (audio_is_playing()) { delay(2); }
        face_set_mouth(0.0f); face_draw(mood);
        delay(20 + (int)(esp_random() % 30));
    }
}

static int syllable_estimate(const String& text) {
    int syl = 0; bool prev_vowel = false;
    for (size_t i = 0; i < text.length(); ++i) {
        char c = tolower(text[i]);
        bool v = (c == 'a' || c == 'e' || c == 'i' || c == 'o' || c == 'u' || c == 'y');
        if (v && !prev_vowel) ++syl;
        prev_vowel = v;
    }
    return syl < 1 ? 1 : syl;
}

void voice_say(const String& text, FoxMood mood) {
    if (!text.length()) return;
    audio_mic_end();
    g_speaking_mood = mood;

    bool spoke = false;
    if (g_cfg.voice_pack == "chatterbox") {
#if HAVE_PICO
        spoke = pico_say(text);
#endif
        if (!spoke) spoke = sam_say(text);
    } else if (g_cfg.voice_pack == "critter") {
        spoke = sam_say(text);
    } else {
        spoke = sam_say(text);
    }
    if (!spoke) {
        voice_babble(mood, syllable_estimate(text));
    }
}
