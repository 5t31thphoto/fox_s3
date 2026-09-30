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
#include <ctype.h>

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
// PicoTTS: initialised ONCE and kept alive (it needs ~1.1MB, which lives in
// PSRAM). The sample callback runs on PicoTTS's own task: it only copies the
// samples, updates the mouth level and writes to I2S (the write blocks, which
// paces synthesis to real time). Drawing happens on the main task.
static volatile bool s_tts_done = true;
static volatile bool s_tts_err  = false;
static bool s_pico_up = false;
static bool s_pico_failed = false;   // don't retry a failed init every line

static void pico_cb(int16_t* buf, unsigned count) {
    if (!count) return;
    int16_t* copy = (int16_t*)fox_alloc(count * sizeof(int16_t));   // Pico reuses buf
    if (!copy) return;
    memcpy(copy, buf, count * sizeof(int16_t));
    uint32_t acc = 0;
    for (unsigned i = 0; i < count; ++i) acc += abs(copy[i]);
    face_set_mouth((acc / (float)count) / 6000.0f);
    audio_play_pcm16(copy, count, 16000);
    heap_caps_free(copy);
}
static void pico_idle() { s_tts_done = true; }
static void pico_err()  { s_tts_err = true; s_tts_done = true; }

static bool pico_up() {
    if (s_pico_up) return true;
    if (s_pico_failed) return false;
    if (!picotts_init(5, pico_cb, 1)) {
        Serial.printf("FOX: PicoTTS init failed (free psram=%uKB) — SAM fallback\n",
                      (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
        s_pico_failed = true;
        return false;
    }
    picotts_set_idle_notify(pico_idle);
    picotts_set_error_notify(pico_err);
    s_pico_up = true;
    Serial.println("FOX: PicoTTS ready");
    return true;
}

static bool pico_say(const String& text) {
    if (!g_echo_ok || !pico_up()) return false;
    audio_set_volume(g_cfg.volume > 100 ? 100 : g_cfg.volume);
    String t = text; t.trim();
    if (!t.length()) return false;
    char lc = t[t.length() - 1];
    if (lc != '.' && lc != '!' && lc != '?') t += '.';
    s_tts_err = false; s_tts_done = false;
    picotts_add(t.c_str(), t.length() + 1);      // include the \0: "go"
    uint32_t guard = millis(), budget = 4000 + t.length() * 150;
    while (!s_tts_done && millis() - guard < budget) {
        M5.update(); face_draw(g_speaking_mood); delay(20);
    }
    while (audio_is_playing()) { M5.update(); delay(5); }
    face_set_mouth(0); face_draw(g_speaking_mood);
    if (s_tts_err || !s_tts_done) {
        Serial.println("FOX: PicoTTS error/timeout — SAM fallback for this line");
        if (s_tts_err) { picotts_shutdown(); s_pico_up = false; }   // re-init next time
        return false;
    }
    return true;
}
#endif

// ---- SAM path ---------------------------------------------------------------
// A short "breath" of silence for natural rhythm between phrases.
static void sam_pause(int ms) {
    uint32_t t0 = millis();
    while (millis() - t0 < (uint32_t)ms) { M5.update(); delay(4); }
}

// Speak with SAM, AUTO-CHUNKED into phrases at punctuation. Each phrase renders
// separately (so any length works and every chunk stays under SAM's per-call
// limit) and is voiced with slight expressive variation + a rhythm pause keyed
// to its punctuation — questions lilt up, exclamations get punchy, periods
// settle, commas take a quick breath. Base cute-female voice: 76/46/150/188.
static bool sam_say(const String& text) {
    if (!g_echo_ok) return false;
    String s = text; s.trim();
    if (s.length() == 0) return false;
    audio_set_volume(g_cfg.volume > 100 ? 100 : g_cfg.volume);

    const int L = s.length();
    int start = 0;
    bool spoke = false;
    for (int i = 0; i <= L; ++i) {
        bool eos = (i == L);
        char c = eos ? '.' : s[i];
        bool punct = (c=='.'||c=='!'||c=='?'||c==','||c==';'||c==':');
        bool boundary = eos || punct;
        // SAM's reciter expands text to phonemes in place in a 256-byte buffer
        // (~2x growth), so cap each phrase at ~60 chars, breaking on a space.
        if (!boundary) {
            if ((i - start) < 60 || c != ' ') continue;
        }

        String phrase = s.substring(start, i);
        phrase.trim();
        start = i + 1;
        if (phrase.length() == 0) continue;

        uint8_t bsp=g_cfg.voice_speed?g_cfg.voice_speed:76, bpi=g_cfg.voice_pitch?g_cfg.voice_pitch:46;
        uint8_t bth=g_cfg.voice_throat?g_cfg.voice_throat:150, bmo=g_cfg.voice_mouth?g_cfg.voice_mouth:188;
        uint8_t speed=bsp, pitch=bpi, throat=bth, mouth=bmo;
        int pause = 70;
        if (c == '?')      { pitch = (bpi>4?bpi-4:bpi); mouth = (uint8_t)min(255,bmo+8);  pause = 200; }  // curious lilt
        else if (c == '!') { speed = (bsp>6?bsp-6:bsp); mouth = (uint8_t)min(255,bmo+14); pause = 200; }  // excited punch
        else if (c == '.') { pause = 190; }                           // settle
        else if (c==','||c==';'||c==':') { pause = 110; }             // quick breath

        uint8_t* pcm = nullptr; int len = 0;
        if (sam_render(phrase.c_str(), speed, pitch, throat, mouth, &pcm, &len) && pcm) {
            spoke = true;
            const int CH = 900;   // ~40ms @22050 for smooth lip-sync
            for (int off = 0; off < len; off += CH) {
                int n = min(CH, len - off);
                mouth_from_pcm8(pcm + off, n, g_speaking_mood);
                audio_play_pcm8(pcm + off, n, 22050);
                while (audio_is_playing()) { M5.update(); delay(2); }
            }
            sam_free(pcm);
        }
        face_set_mouth(0); face_draw(g_speaking_mood);
        if (!eos) sam_pause(pause);   // rhythm between phrases
    }
    face_set_mouth(0); face_draw(g_speaking_mood);
    return spoke;
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

// What the TTS engines should actually pronounce: drop *stage directions*
// (those stay on the caption) and characters the reciters choke on.
static String speakable(const String& in) {
    String out; bool in_action = false;
    for (size_t i = 0; i < in.length(); ++i) {
        char c = in[i];
        if (c == '*') { in_action = !in_action; continue; }
        if (in_action) continue;
        if (isalnum((unsigned char)c) || c == ' ' || c == '\'' || c == ',' ||
            c == '.' || c == '!' || c == '?' || c == ':' || c == '-') out += c;
        else if (c == '~' || c == '\n') out += ' ';
    }
    out.trim();
    return out;
}

void voice_say(const String& raw, FoxMood mood) {
    if (!raw.length()) return;
    audio_mic_end();
    g_speaking_mood = mood;
    String text = speakable(raw);
    if (!text.length()) { voice_babble(mood, 2); return; }   // pure *action*: chirp

    bool spoke = false;
    if (g_cfg.voice_pack == "chatterbox") {
#if HAVE_PICO
        spoke = pico_say(text);
#endif
        if (spoke) Serial.println("FOX: voice=picotts");
        if (!spoke) { spoke = sam_say(text); if (spoke) Serial.println("FOX: voice=sam (fallback)"); }
    } else if (g_cfg.voice_pack == "critter") {
        spoke = sam_say(text);
    } else {
        spoke = sam_say(text);
    }
    if (!spoke) {
        Serial.printf("FOX: voice=BABBLE — pico+sam both failed (echo=%d, free psram=%uKB)\n",
                      (int)g_echo_ok, (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
        voice_babble(mood, syllable_estimate(text));
    }
}
