// fox_face.inc — the fox's face: an animated, expressive little vulpine mug
// on the AtomS3R's 128x128 display, with mouth movement driven by audio level
// (FFT lip-sync ported from m5stack-avatar-mic) and captions for clarity.
#pragma once
#include <M5GFX.h>

static M5Canvas* face_canvas = nullptr;
static FoxConfig* face_cfg = nullptr;
static float mouth_open = 0.f;      // 0..1
static int   blink_timer = 0;
static bool  blinking = false;
static String caption;
static uint32_t caption_until = 0;
static uint8_t ear_wiggle = 0;

void face_begin(FoxConfig& c) {
    face_cfg = &c;
    M5.Display.setColorDepth(16);
    M5.Display.setBrightness(90);
    face_canvas = new M5Canvas(&M5.Display);
    face_canvas->createSprite(128, 128);
    face_canvas->setTextDatum(middle_center);
}

void face_wake() { M5.Display.setBrightness(90); }
void face_dim()  { M5.Display.setBrightness(20); }

// Custom boot splash: a fox graphic + the user's text, with an optional effect
// (sparkle / wave / rainbow) configured from the web flasher. Runs ~2.5s.
void face_splash(FoxConfig& c) {
    uint16_t bg = c.color_bg, fur = c.color_primary, acc = c.color_accent;
    String txt = c.splash_text.length() ? c.splash_text : (String("hi, i'm ") + c.name);
    uint32_t t0 = millis();
    int frame = 0;
    while (millis() - t0 < 2500) {
        face_canvas->fillSprite(bg);
        // effect backdrop
        if (c.splash_fx == 1) {            // sparkle
            for (int i = 0; i < 30; ++i)
                face_canvas->drawPixel(random(128), random(128),
                    (random(2) ? acc : 0xFFFF));
        } else if (c.splash_fx == 2) {     // wave
            for (int x = 0; x < 128; ++x) {
                int y = 64 + (int)(20 * sinf(x * 0.1f + frame * 0.2f));
                face_canvas->drawPixel(x, y, acc);
                face_canvas->drawPixel(x, y + 1, fur);
            }
        } else if (c.splash_fx == 3) {     // rainbow sweep bars
            for (int y = 0; y < 128; y += 4) {
                uint8_t R = (uint8_t)(15 + 15 * sinf(y * 0.1f + frame * 0.2f));
                face_canvas->fillRect(0, y, 128, 4, (R << 11) | (y << 3));
            }
        }
        // simple fox head graphic
        face_canvas->fillTriangle(38, 34, 50, 8, 62, 32, fur);
        face_canvas->fillTriangle(90, 34, 78, 8, 66, 32, fur);
        face_canvas->fillEllipse(64, 46, 30, 26, fur);
        face_canvas->fillEllipse(64, 54, 18, 13, acc);
        face_canvas->fillCircle(55, 44, 3, 0x0000);
        face_canvas->fillCircle(73, 44, 3, 0x0000);
        face_canvas->fillTriangle(60, 52, 68, 52, 64, 57, 0x0000);
        // name / splash text
        face_canvas->setTextColor(0xFFFF); face_canvas->setTextDatum(bottom_center);
        face_canvas->drawString(txt.substring(0, 20), 64, 118);
        face_canvas->pushSprite(0, 0);
        frame++;
        delay(40);
    }
}

// Draw the fox at the current expression. Eyes/ears/mouth respond to mood and
// to the live mouth_open value while speaking.
void face_draw(FoxMood mood) {
    if (!face_canvas || !face_cfg) return;   // never draw before face_begin()
    uint16_t bg = face_cfg->color_bg;
    uint16_t fur = face_cfg->color_primary;
    uint16_t cream = face_cfg->color_accent;
    face_canvas->fillSprite(bg);

    // head
    face_canvas->fillEllipse(64, 66, 44, 40, fur);
    // cheeks / muzzle
    face_canvas->fillEllipse(64, 82, 26, 20, cream);
    // ears (triangles), wiggle offset
    int ew = (ear_wiggle % 8 < 4) ? 0 : 2;
    face_canvas->fillTriangle(30, 40, 44, 8 - ew, 56, 38, fur);
    face_canvas->fillTriangle(98, 40, 84, 8 - ew, 72, 38, fur);
    face_canvas->fillTriangle(36, 34, 45, 16, 52, 34, bg);
    face_canvas->fillTriangle(92, 34, 83, 16, 76, 34, bg);

    // eyes — shape depends on mood
    uint16_t eye = 0x0000;
    if (blinking) {
        face_canvas->drawFastHLine(44, 60, 14, eye);
        face_canvas->drawFastHLine(70, 60, 14, eye);
    } else {
        switch (mood) {
            case MOOD_SLEEPY:
                face_canvas->drawArc(51, 60, 8, 8, 200, 340, eye);
                face_canvas->drawArc(77, 60, 8, 8, 200, 340, eye);
                break;
            case MOOD_EXCITED:
                face_canvas->fillCircle(51, 58, 7, eye);
                face_canvas->fillCircle(77, 58, 7, eye);
                face_canvas->fillCircle(53, 56, 2, 0xFFFF);
                face_canvas->fillCircle(79, 56, 2, 0xFFFF);
                break;
            case MOOD_GRUMPY:
                face_canvas->drawLine(44, 56, 58, 60, eye);
                face_canvas->drawLine(84, 56, 70, 60, eye);
                face_canvas->fillCircle(51, 60, 4, eye);
                face_canvas->fillCircle(77, 60, 4, eye);
                break;
            default:
                face_canvas->fillCircle(51, 59, 6, eye);
                face_canvas->fillCircle(77, 59, 6, eye);
                face_canvas->fillCircle(53, 57, 2, 0xFFFF);
                face_canvas->fillCircle(79, 57, 2, 0xFFFF);
        }
    }
    // nose
    face_canvas->fillTriangle(60, 76, 68, 76, 64, 82, eye);
    // mouth — opens with mouth_open while speaking
    int mo = (int)(mouth_open * 12);
    if (mo < 1) {
        face_canvas->drawArc(64, 84, 8, 8, 20, 160, eye);
    } else {
        face_canvas->fillEllipse(64, 88, 7, 2 + mo, eye);
    }

    // caption bar
    if (caption.length() && millis() < caption_until && face_cfg->captions) {
        face_canvas->fillRect(0, 112, 128, 16, 0x0000);
        face_canvas->setTextColor(0xFFFF);
        face_canvas->setTextSize(1);
        face_canvas->drawString(caption.substring(0, 20), 64, 120);
    }
    face_canvas->pushSprite(0, 0);
}

// Called every loop to animate idle motion (blink, ear wiggle).
void face_tick(FoxMood mood) {
    if (++blink_timer > 220) { blinking = true; }
    if (blink_timer > 230)   { blinking = false; blink_timer = 0; }
    if ((blink_timer & 7) == 0) ear_wiggle++;
    // decay mouth toward closed when not actively driven
    mouth_open *= 0.7f;
    face_draw(mood);
}

void face_caption(const String& text) {
    caption = text;
    caption_until = millis() + 4000;
}

void face_listen() { face_caption("*listening*"); }
void face_think()  { face_caption("*thinking*"); }

// The speaker path and mic path both poke this to drive the mouth.
void face_set_mouth(float level01) {
    if (level01 < 0) level01 = 0;
    if (level01 > 1) level01 = 1;
    mouth_open = level01;
}

// ---- real FFT mic lip-sync (ported from m5stack-avatar-mic) -----------------
// A dedicated mode: the fox opens its mouth in time with sound it hears through
// the Echo Base mic, using the same FFT-bin-sum → ratio trick as the reference
// avatar. Runs until the button is clicked. Great as a "puppet" / reactive mode.
#include "fft.hpp"
static fft_t s_fft;
static int16_t* s_wave = nullptr;
static float s_lip_max = 10.0f;
static uint32_t s_last_loud = 0;

#include "fox_audio.h"

static void face_lipsync_from_mic_once() {
    static constexpr size_t WAVE = 256 * 2;
    if (!s_wave) s_wave = (int16_t*)heap_caps_malloc(WAVE * sizeof(int16_t),
                                                     MALLOC_CAP_8BIT);
    if (!s_wave) return;
    uint64_t level = 0;
    if (g_echo_ok && audio_record(s_wave, WAVE)) {
        s_fft.exec(s_wave);
        for (size_t bx = 5; bx <= 60; ++bx) { int32_t f = s_fft.get(bx); level += (f < 0 ? -f : f); }
    }
    uint32_t tl = level >> 11;                    // shift → mouth scale
    float ratio = (float)tl / s_lip_max;
    if (ratio <= 0.01f) {
        ratio = 0.0f;
        if (millis() - s_last_loud > 500) s_lip_max = 10.0f;   // decay ceiling
    } else {
        if (ratio > 1.3f) { if (ratio > 1.5f) s_lip_max += 10.0f; ratio = 1.3f; }
        s_last_loud = millis();
    }
    face_set_mouth(ratio > 1.0f ? 1.0f : ratio);
}

// Standalone "reactive puppet" app.
void face_lipsync_mode() {
    audio_speaker_end();
    if (!g_echo_ok) return;
    while (true) {
        M5.update();
        if (M5.BtnA.wasClicked()) break;
        face_lipsync_from_mic_once();
        face_draw(MOOD_HAPPY);
        delay(1);
    }
    audio_mic_end();
}
