// fox_voice.cpp — the fox's mouth.
//
// Three voices, chosen at flash time via cfg.voice_pack, all landing on the
// Echo Base speaker through M5Unified:
//   * "chatterbox": PicoTTS, tuned a little higher/faster to sound cute.
//   * "critter":    SAM (retro Commodore voice), tiny and charming.
//   * babble:       procedural chirps — always available, used when a pack is
//                   missing and for "*thinking*" filler.
//
// A hard lesson baked in here: PicoTTS hands us a pointer to its OWN internal
// buffer in the sample callback and reuses it immediately. M5.Speaker.playRaw
// with copy=false would read freed data. We copy every chunk into a small
// PSRAM ring and play copies. That was a real garble bug in the prior core.
#include "fox.h"
#include "fox_decls.h"
#include <esp_heap_caps.h>

#if __has_include("picotts.h")
#include "picotts.h"
#define HAVE_PICO 1
#else
#define HAVE_PICO 0
#endif

// (sam_render / sam_free are declared in fox_decls.h)

static FoxConfig g_cfg;
static bool g_pico_ok = false;

void voice_begin(const FoxConfig& cfg) {
    g_cfg = cfg;
    g_pico_ok = (HAVE_PICO && cfg.voice_pack == "chatterbox");
}

bool voice_is_pico() { return g_pico_ok; }

// forward from fox_face.inc — lets the talking path drive the mouth + redraw.

// Drive the mouth from a slice of PCM (any bit depth) while the fox speaks, so
// the avatar actually lip-syncs to its OWN voice, not just the mic.
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

// Copy Pico's samples into our own memory before handing them to the speaker.
static void pico_cb(int16_t* buf, unsigned count) {
    if (!count) return;
    int16_t* copy = (int16_t*)heap_caps_malloc(count * sizeof(int16_t),
                                               MALLOC_CAP_8BIT);
    if (!copy) return;
    memcpy(copy, buf, count * sizeof(int16_t));
    // lip-sync: this chunk's amplitude opens the mouth as it plays.
    mouth_from_pcm16(copy, count, g_speaking_mood);
    M5.Speaker.playRaw(copy, count, 16000, false, 1, -1);
    // Wait until this chunk is consumed, then release our copy.
    while (M5.Speaker.isPlaying()) { taskYIELD(); }
    free(copy);
}
static void pico_idle() { s_tts_done = true; }
static void pico_err()  { s_tts_done = true; }

static bool pico_say(const String& text) {
    if (!M5.Speaker.begin()) return false;
    M5.Speaker.setVolume(g_cfg.volume);
    s_tts_done = false;
    if (!picotts_init(5, pico_cb, 1)) return false;
    picotts_set_idle_notify(pico_idle);
    picotts_set_error_notify(pico_err);
    // A cute-fox prosody hack: PicoTTS honours SSML-ish pitch/rate via the
    // engine only crudely, so we lean on short phrasing instead. Keeping lines
    // short is what actually reads as "small animal".
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
    // Higher pitch + smaller throat/mouth -> squeakier, foxier.
    if (!sam_render(text.c_str(), 72, 96, 110, 160, &pcm, &len) || !pcm)
        return false;
    if (!M5.Speaker.begin()) { sam_free(pcm); return false; }
    M5.Speaker.setVolume(g_cfg.volume);
    // Play in ~40ms chunks (22050Hz 8-bit) so the mouth lip-syncs across the
    // whole line instead of opening once for the entire clip.
    const int CH = 900;
    for (int off = 0; off < len; off += CH) {
        int n = min(CH, len - off);
        mouth_from_pcm8(pcm + off, n, g_speaking_mood);
        M5.Speaker.playRaw(pcm + off, n, 22050, false, 1, -1);
        while (M5.Speaker.isPlaying()) { M5.update(); delay(2); }
    }
    face_set_mouth(0); face_draw(g_speaking_mood);
    sam_free(pcm);
    return true;
}

// ---- babble fallback --------------------------------------------------------
// Procedural chirps. Pitch tracks mood; length tracks syllable count. This is
// the Animal-Crossing "animalese" trick and it always works with zero assets.
void voice_babble(FoxMood mood, int syllables) {
    if (!M5.Speaker.begin()) return;
    M5.Speaker.setVolume(g_cfg.volume);
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
        face_set_mouth(0.7f); face_draw(mood);      // open on each chirp
        M5.Speaker.tone(f, 70 + esp_random() % 60);
        while (M5.Speaker.isPlaying()) { delay(2); }
        face_set_mouth(0.0f); face_draw(mood);      // close between
        delay(20 + esp_random() % 30);
    }
}

// Rough syllable estimate so babble length matches the (unspoken) words.
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

// ---- public entry -----------------------------------------------------------
void voice_say(const String& text, FoxMood mood) {
    if (!text.length()) return;
    // Ensure mic is off; speaker and mic share the I2S bus on the Echo Base.
    M5.Mic.end();
    g_speaking_mood = mood;   // lip-sync redraws use this expression

    bool spoke = false;
    if (g_cfg.voice_pack == "chatterbox") {
#if HAVE_PICO
        spoke = pico_say(text);
#endif
    } else if (g_cfg.voice_pack == "critter") {
        spoke = sam_say(text);
    }
    if (!spoke) {
        // Missing pack or engine failure: chirp the line so the fox never goes
        // mute and never pretends words were spoken that weren't.
        voice_babble(mood, syllable_estimate(text));
    }
}
