// fox_demo.inc — demoscene eye-candy and IMU-reactive tactile fidget toys.
// Included by fox_main.cpp. These are the "just fun to hold and move" pieces:
// tight visual loops that respond to tilt, rotation, and taps. Each runs until
// a click (or double-click → menu). Nothing is saved.
#pragma once

extern M5Canvas* face_canvas;
extern FoxConfig cfg;

// small helper: HSV->rgb565 for smooth colour cycling
static uint16_t hsv565(float h, float s, float v) {
    h = fmodf(h, 360); if (h < 0) h += 360;
    float c = v * s, x = c * (1 - fabsf(fmodf(h / 60.0f, 2) - 1)), m = v - c;
    float r, g, b;
    if (h < 60)      { r = c; g = x; b = 0; }
    else if (h < 120){ r = x; g = c; b = 0; }
    else if (h < 180){ r = 0; g = c; b = x; }
    else if (h < 240){ r = 0; g = x; b = c; }
    else if (h < 300){ r = x; g = 0; b = c; }
    else             { r = c; g = 0; b = x; }
    uint8_t R = (r + m) * 31, G = (g + m) * 63, B = (b + m) * 31;
    return (R << 11) | (G << 5) | B;
}

// A frame helper: returns false when the toy should exit.
static bool toy_running(bool& to_menu) {
    M5.update();
    if (wants_menu()) { to_menu = true; g_goto_menu = true; return false; }
    if (M5.BtnA.wasClicked()) return false;
    return true;
}

// ===========================================================================
//  PLASMA — classic demoscene plasma, hue shifted by device tilt.
// ===========================================================================
void toy_plasma() {
    speak("oooh pretty~ tilt me!");
    float t = 0; bool to_menu = false;
    // low-res buffer scaled up for speed (32x32 → 128x128)
    while (toy_running(to_menu)) {
        float rx, ry; tilt_xy(&rx, &ry);
        float hue = 180 + rx * 180;
        for (int y = 0; y < 32; ++y)
            for (int x = 0; x < 32; ++x) {
                float v = sinf(x * 0.4f + t) + sinf(y * 0.5f - t)
                        + sinf((x + y) * 0.3f + t * 1.3f) + sinf(hypotf(x-16, y-16) * 0.4f - t);
                uint16_t col = hsv565(hue + v * 40, 0.9f, 0.5f + 0.25f * v);
                face_canvas->fillRect(x * 4, y * 4, 4, 4, col);
            }
        face_canvas->pushSprite(0, 0);
        t += 0.15f + fabsf(ry) * 0.3f;   // tilt speeds it up
    }

}

// ===========================================================================
//  STARFIELD — fly through stars; tilt steers, rotation twists.
// ===========================================================================
void toy_starfield() {
    speak("to infinity~");
    const int N = 90;
    static float sx[N], sy[N], sz[N];
    for (int i = 0; i < N; ++i) { sx[i] = random(-64, 64); sy[i] = random(-64, 64); sz[i] = random(1, 64); }
    bool to_menu = false;
    while (toy_running(to_menu)) {
        float rx, ry; tilt_xy(&rx, &ry);
        face_canvas->fillSprite(0x0000);
        for (int i = 0; i < N; ++i) {
            sz[i] -= 1.2f + fabsf(ry) * 2;
            sx[i] += rx * 2; 
            if (sz[i] <= 1) { sx[i] = random(-64, 64); sy[i] = random(-64, 64); sz[i] = 64; }
            int px = 64 + (int)(sx[i] / sz[i] * 64);
            int py = 64 + (int)(sy[i] / sz[i] * 64);
            int b = (int)(255 * (1 - sz[i] / 64));
            uint16_t col = hsv565(200, 0.2f, b / 255.0f);
            int szp = (int)(3 * (1 - sz[i] / 64)) + 1;
            if (px >= 0 && px < 128 && py >= 0 && py < 128)
                face_canvas->fillCircle(px, py, szp, col);
        }
        face_canvas->pushSprite(0, 0);
    }

}

// ===========================================================================
//  INK — a fluid-ish tilt sandbox: a blob of "ink" rolls with gravity and
//  leaves a fading trail. Pure tactile fidget.
// ===========================================================================
void toy_ink() {
    speak("roll me around~");
    float x = 64, y = 64, vx = 0, vy = 0, hue = 0;
    bool to_menu = false;
    // persistent dim canvas for trails
    face_canvas->fillSprite(0x0000);
    while (toy_running(to_menu)) {
        float rx, ry; tilt_xy(&rx, &ry);
        vx += rx * 0.8f; vy += ry * 0.8f; vx *= 0.92f; vy *= 0.92f;
        x += vx; y += vy;
        if (x < 4 || x > 124) { vx = -vx * 0.6f; x = constrain(x, 4, 124); }
        if (y < 4 || y > 124) { vy = -vy * 0.6f; y = constrain(y, 4, 124); }
        // fade existing frame slightly by overlaying translucent black-ish grid
        for (int i = 0; i < 40; ++i) face_canvas->drawPixel(random(128), random(128), 0x0000);
        hue += 4;
        face_canvas->fillCircle((int)x, (int)y, 5, hsv565(hue, 0.9f, 0.9f));
        face_canvas->pushSprite(0, 0);
    }

}

// ===========================================================================
//  SPIROGRAPH — rotate the device to draw hypnotic curves; button changes gears
// ===========================================================================
void toy_spiro() {
    speak("spin me to draw~");
    float t = 0; int gear = 3; bool to_menu = false;
    face_canvas->fillSprite(cfg.color_bg);
    while (toy_running(to_menu)) {
        if (M5.BtnA.wasPressed()) gear = 2 + (gear % 6);
        float rx, ry; tilt_xy(&rx, &ry);
        float R = 40, r = 40.0f / gear, d = 20 + rx * 15;
        for (int k = 0; k < 6; ++k) {
            float a = t + k * 0.02f;
            float x = 64 + (R - r) * cosf(a) + d * cosf((R - r) / r * a);
            float y = 64 + (R - r) * sinf(a) - d * sinf((R - r) / r * a);
            face_canvas->fillCircle((int)x, (int)y, 1, hsv565(t * 20, 0.8f, 1));
        }
        face_canvas->pushSprite(0, 0);
        t += 0.15f + fabsf(ry) * 0.2f;
    }

}
