// fox_face.inc — the fox's face: an animated, expressive little vulpine mug
// on the AtomS3R's 128x128 display, with mouth movement driven by audio level
// (FFT lip-sync ported from m5stack-avatar-mic) and captions for clarity.
#pragma once
#include <M5GFX.h>

static M5Canvas* face_canvas = nullptr;
extern bool g_goto_menu;
static FoxConfig* face_cfg = nullptr;
static float mouth_open = 0.f;      // 0..1
static int   blink_timer = 0;
static bool  blinking = false;
static String caption;
static uint32_t caption_until = 0;
static uint8_t ear_wiggle = 0;
static float face_tilt_x = 0.f, face_tilt_y = 0.f;
static uint32_t face_shake_until = 0;

void face_begin(FoxConfig& c) {
    face_cfg = &c;
    M5.Display.setColorDepth(16);
    /* brightness set once at boot via LP5562 */
    face_canvas = new M5Canvas(&M5.Display);
    face_canvas->createSprite(128, 128);
    face_canvas->setTextDatum(middle_center);
}

void face_wake() { /* brightness set once at boot via LP5562 */ }
void face_dim()  { /* keep backlight steady for AtomS3R LP5562 */ }

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
        // name / splash text — measured wrapping, not a hard 20-character cut.
        face_canvas->setTextColor(0xFFFF); face_canvas->setTextDatum(top_left);
        String rest = txt; int sy = 106;
        for (int line=0; line<2 && rest.length(); ++line) {
            int cut = rest.length();
            while (cut > 1 && face_canvas->textWidth(rest.substring(0,cut)) > 124) --cut;
            if (cut < (int)rest.length()) { int sp = rest.lastIndexOf(' ',cut-1); if(sp>6) cut=sp; }
            String part=rest.substring(0,cut); part.trim(); face_canvas->drawString(part,2,sy); sy+=10;
            rest=rest.substring(cut); rest.trim();
        }
        face_canvas->pushSprite(0, 0);
        frame++;
        delay(40);
    }
}

// Draw the fox at the current expression. Eyes/ears/mouth respond to mood and
// to the live mouth_open value while speaking.
void face_draw(FoxMood mood) {
    if (!face_canvas || !face_cfg) return;

    // Read the IMU here too, so idle animations and every app-rendered fox face
    // react immediately even when the main loop is not calling face_tick().
    float ax, ay, az;
    if (M5.Imu.getAccel(&ax, &ay, &az)) {
        float roll = atan2f(ay, az);
        float pitch = atan2f(-ax, sqrtf(ay*ay + az*az));
        face_tilt_x = constrain(roll / 0.65f, -1.0f, 1.0f);
        face_tilt_y = constrain(pitch / 0.65f, -1.0f, 1.0f);
        float mag = sqrtf(ax*ax + ay*ay + az*az);
        if (mag > 2.2f) face_shake_until = millis() + 420;
    }
    const bool shaken = millis() < face_shake_until;

    uint16_t bg = face_cfg->color_bg, fur = face_cfg->color_primary, cream = face_cfg->color_accent;
    face_canvas->fillSprite(bg);

    int bob = shaken ? ((millis() / 45) & 1 ? 2 : -2) : (int)(face_tilt_y * 2.0f);
    int ear = shaken ? ((millis() / 35) % 5 - 2) : (int)(face_tilt_x * 3.0f);
    face_canvas->fillEllipse(64, 66 + bob, 44, 40, fur);
    face_canvas->fillEllipse(64, 82 + bob, 26, 20, cream);
    face_canvas->fillTriangle(30,40, 44,8-ear, 56,38, fur);
    face_canvas->fillTriangle(98,40, 84,8+ear, 72,38, fur);
    face_canvas->fillTriangle(36,34,45,16-ear,52,34,bg);
    face_canvas->fillTriangle(92,34,83,16+ear,76,34,bg);

    int px = (int)(face_tilt_x * 5.0f);
    int py = (int)(face_tilt_y * 3.0f);
    uint16_t eye = 0x0000;
    if (blinking) {
        face_canvas->drawFastHLine(44+px, 60+py, 14, eye);
        face_canvas->drawFastHLine(70+px, 60+py, 14, eye);
    } else {
        switch (mood) {
            case MOOD_SLEEPY:
                face_canvas->drawArc(51+px,60+py,8,8,200,340,eye);
                face_canvas->drawArc(77+px,60+py,8,8,200,340,eye);
                break;
            case MOOD_EXCITED:
                face_canvas->fillCircle(51+px,58+py,7,eye); face_canvas->fillCircle(77+px,58+py,7,eye);
                face_canvas->fillCircle(53+px,56+py,2,0xFFFF); face_canvas->fillCircle(79+px,56+py,2,0xFFFF);
                break;
            case MOOD_GRUMPY:
                face_canvas->drawLine(44+px,56+py,58+px,60+py,eye); face_canvas->drawLine(84+px,56+py,70+px,60+py,eye);
                face_canvas->fillCircle(51+px,60+py,4,eye); face_canvas->fillCircle(77+px,60+py,4,eye);
                break;
            default:
                face_canvas->fillCircle(51+px,59+py,6,eye); face_canvas->fillCircle(77+px,59+py,6,eye);
                face_canvas->fillCircle(53+px,57+py,2,0xFFFF); face_canvas->fillCircle(79+px,57+py,2,0xFFFF);
        }
    }
    face_canvas->fillTriangle(60,76+bob,68,76+bob,64,82+bob,eye);

    // More expressive lipsync geometry: jaw opening, dark mouth cavity,
    // tongue/highlight and a small upper-lip curve instead of one ellipse.
    float mo = constrain(mouth_open, 0.f, 1.f);
    if (mo < 0.08f) {
        face_canvas->drawArc(64,84+bob,8,7,18,162,eye);
    } else {
        int h = 3 + (int)(mo * 13);
        face_canvas->fillRoundRect(55,82+bob,18, h, 7, eye);
        if (mo > 0.38f) {
            int th = max(2, (int)(h * 0.28f));
            face_canvas->fillRoundRect(58,82+bob+h-th,12,th,3,0xF800);
        }
        face_canvas->drawFastHLine(57,82+bob,14,0xFFFF);
    }

    // Caption uses measured pixel width rather than a fixed character count;
    // proportional glyphs were the reason long text was being clipped.
    if (caption.length() && millis() < caption_until && face_cfg->captions) {
        face_canvas->fillRect(0, 94, 128, 34, 0x0000);
        face_canvas->setTextColor(0xFFFF); face_canvas->setTextSize(1); face_canvas->setTextDatum(top_left);
        String rest = caption; int y = 95;
        for (int line=0; line<3 && rest.length(); ++line) {
            int cut = rest.length();
            while (cut > 1 && face_canvas->textWidth(rest.substring(0,cut)) > 124) --cut;
            if (cut < (int)rest.length()) {
                int sp = rest.lastIndexOf(' ', cut-1);
                if (sp > 8) cut = sp;
            }
            String part = rest.substring(0,cut); part.trim();
            face_canvas->drawString(part,2,y); y += 10;
            rest = rest.substring(cut); rest.trim();
        }
        face_canvas->setTextDatum(middle_center);
    }
    face_canvas->pushSprite(0,0);
}

// Called every loop to animate idle motion (blink, ear wiggle).
// Throttle redraws: full pushSprite every loop tears on AtomS3R (flicker).
void face_tick(FoxMood mood) {
    static uint32_t last_draw = 0;
    static float last_mouth = -1;
    static int last_blink = -1;
    static int last_ear = -1;
    static FoxMood last_mood = (FoxMood)-1;
    uint32_t now = millis();

    if (++blink_timer > 220) { blinking = true; }
    if (blink_timer > 230)   { blinking = false; blink_timer = 0; }
    if ((blink_timer & 7) == 0) ear_wiggle++;
    if (millis() < face_shake_until) ear_wiggle += 3;
    // decay mouth toward closed when not actively driven
    mouth_open *= 0.85f;

    bool due = (now - last_draw) >= 66;  // ~15 fps max
    bool changed = (blinking != last_blink) || (ear_wiggle != last_ear)
                || (mood != last_mood) || ((mouth_open > last_mouth ? mouth_open - last_mouth : last_mouth - mouth_open) > 0.04f)
                || (caption.length() && millis() < caption_until);
    if (!due && !changed) return;

    face_draw(mood);
    last_draw = now;
    last_mouth = mouth_open;
    last_blink = blinking;
    last_ear = ear_wiggle;
    last_mood = mood;
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
        ButtonEvent ev = input_button_event();
        if (ev == BTN_DOUBLE) { g_goto_menu = true; break; }
        // A single click toggles between a smile and a surprised mouth while
        // still leaving the mode running; double click is the exit gesture.
        static bool grin = false;
        if (ev == BTN_TAP) grin = !grin;
        face_lipsync_from_mic_once();
        if (grin) face_set_mouth(min(1.0f, mouth_open + 0.18f));
        face_draw(MOOD_HAPPY);
        delay(1);
    }
    audio_mic_end();
}
