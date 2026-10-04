// fox_games.inc - digital fidget games. Included by fox_main.cpp.
//
// A mix of IMU tilt games (fly through wormholes, catch falling treats) and
// word games (20 questions). Each runs as a self-contained loop
// on the 128x128 LCD and returns when the button is clicked or the game ends.
// Voice can launch any of these; the fox narrates the result.
#pragma once

extern M5Canvas* face_canvas;
extern FoxConfig cfg;

// Ignore the click that *started* the game (still latched in M5.BtnA).
static bool game_btn_exit(uint32_t started_ms) {
    if (millis() - started_ms < 450) {
        // drain so wasClicked/wasDoubleClicked go false
        (void)M5.BtnA.wasClicked();
        (void)M5.BtnA.wasDoubleClicked();
        return false;
    }
    if (M5.BtnA.wasDoubleClicked()) { g_goto_menu = true; return true; }
    if (M5.BtnA.wasClicked()) return true;
    return false;
}

// small tilt reader: returns roll/pitch in ~[-1,1]
void tilt_xy(float* rx, float* ry) {
    float ax, ay, az;
    M5.Imu.getAccel(&ax, &ay, &az);
    *rx = constrain(ay, -1.f, 1.f);
    *ry = constrain(ax, -1.f, 1.f);
}

// ===========================================================================
//  WORMHOLE - fly your ship through shrinking rings by tilting the device.
// ===========================================================================
void game_wormhole() {
    uint32_t t_start = millis();
    speak("hold tight, into the wormhole!");
    face_caption("tilt to steer - click to exit");
    float ship_x = 64, ship_y = 80;
    int score = 0, lives = 5, best_streak = 0, streak = 0;
    // tunnel rings on a conveyor: evenly spaced in depth so one is always near
    struct Ring { float z, cx, cy; bool judged; };
    const int NR = 5;
    const float ZSTEP = 55.f, ZFAR = 10.f + NR * 55.f;
    Ring rings[NR];
    for (int i = 0; i < NR; ++i)
        rings[i] = { 40.f + i * ZSTEP, 64.f + (int)(esp_random()%61) - 30,
                     64.f + (int)(esp_random()%61) - 30, false };
    float speed = 36.f;
    uint32_t last = millis();
    while (lives > 0 && millis() - t_start < 120000) {
        M5.update();
        if (game_btn_exit(t_start)) break;
        float rx, ry; tilt_xy(&rx, &ry);
        ship_x = constrain(ship_x + rx * 5.5f, 12, 116);
        ship_y = constrain(ship_y + ry * 5.5f, 40, 118);

        float dt = (millis() - last) / 1000.f;
        if (dt < 0.01f) dt = 0.01f;
        last = millis();

        face_canvas->fillSprite(0x0000);
        // starfield parallax
        for (int i = 0; i < 40; ++i) {
            int sx = (i * 97 + (int)(millis() / 8)) % 128;
            int sy = (i * 53 + (int)(millis() / 12)) % 128;
            face_canvas->drawPixel(sx, sy, 0x8410);
        }
        speed += 1.2f * dt;                      // gently accelerates
        bool flash_hit = false, flash_miss = false;
        for (int i = 0; i < NR; ++i) {
            rings[i].z -= speed * dt;
            // JUDGE as the ring reaches the ship plane (z~18), with a fat radius
            if (!rings[i].judged && rings[i].z < 18.f) {
                rings[i].judged = true;
                float dx = ship_x - rings[i].cx, dy = ship_y - rings[i].cy;
                if (dx*dx + dy*dy < 30*30) {         // generous: pass through the ring
                    score++; streak++; if (streak > best_streak) best_streak = streak;
                    flash_hit = true; needs_interact(needs, true);
                } else { lives--; streak = 0; flash_miss = true; }
            }
            if (rings[i].z < 3.f) {                   // recycle to the far end, evenly spaced
                float maxz = 0; for (int j = 0; j < NR; ++j) if (rings[j].z > maxz) maxz = rings[j].z;
                rings[i].z = maxz + ZSTEP;
                rings[i].cx = 64 + (int)(esp_random()%61) - 30;
                rings[i].cy = 64 + (int)(esp_random()%61) - 30;
                rings[i].judged = false;
            }
            float sc = 90.f / rings[i].z;
            int r = (int)(sc * 11); if (r < 3) r = 3; if (r > 64) r = 64;
            int rcx = 64 + (int)((rings[i].cx - 64) * sc * 0.6f);   // converge to center (tunnel)
            int rcy = 64 + (int)((rings[i].cy - 64) * sc * 0.6f);
            uint16_t col = rings[i].z < 24 ? 0x07E0 : (rings[i].z < 60 ? 0xFFE0 : 0x07FF);
            face_canvas->drawCircle(rcx, rcy, r, col);
            face_canvas->drawCircle(rcx, rcy, r - 1, col);
            face_canvas->drawCircle(rcx, rcy, r - 2, col);
        }
        if (flash_hit)  { face_canvas->drawString("+1", (int)ship_x - 6, (int)ship_y - 20); }
        if (flash_miss) { for (int b=0;b<128;b+=8) face_canvas->drawPixel(b,0,0xF800); }
        // ship
        face_canvas->fillTriangle((int)ship_x, (int)ship_y - 6,
                                  (int)ship_x - 6, (int)ship_y + 6,
                                  (int)ship_x + 6, (int)ship_y + 6, 0xFFFF);
        face_canvas->setTextDatum(top_left);
        face_canvas->setTextColor(0xFFFF);
        face_canvas->setTextSize(1);
        face_canvas->drawString(String("rings ") + score + "  hp " + lives + "  x" + streak, 2, 2);
        face_canvas->pushSprite(0, 0);
        delay(16);  // ~60fps eye candy without tearing as bad as unbounded
    }
    face_caption(String("rings cleared: ") + score);
    speak(score >= 15 ? (String("whoa, ") + score + " rings! best streak " + best_streak + "!")
                      : (String("we cleared ") + score + " rings! again?"));
}


void game_catch() {
    uint32_t t_start = millis();
    speak("catch the treats! tilt to move~");
    float basket = 64; int score = 0, misses = 0;
    struct Drop { float x, y, v; bool live; };
    const int ND = 4; Drop drops[ND];
    for (int i = 0; i < ND; ++i) drops[i] = { (float)(random(10, 118)), (float)(-random(0, 120)), (float)random(2, 4) / 1.0f, true };
    uint32_t t0 = millis();
    while (misses < 5) {
        M5.update();
        if (game_btn_exit(t_start)) break;
        float rx, ry; tilt_xy(&rx, &ry);
        basket = constrain(basket + rx * 8, 12, 116);
        face_canvas->fillSprite(cfg.color_bg);
        for (int i = 0; i < ND; ++i) {
            drops[i].y += drops[i].v;
            face_canvas->fillCircle((int)drops[i].x, (int)drops[i].y, 4, cfg.color_primary);
            if (drops[i].y > 112) {
                bool caught = fabsf(drops[i].x - basket) < 16;
                if (caught) { score++; needs_interact(needs, true); }
                else misses++;
                // a little splash where it landed
                face_canvas->fillCircle((int)drops[i].x, 114, caught ? 6 : 2,
                                        caught ? cfg.color_accent : 0xF800);
                drops[i].x = random(10, 118); drops[i].y = -random(0, 60);
                drops[i].v = (float)random(2, 5);
            }
            else face_canvas->fillCircle((int)drops[i].x, (int)drops[i].y + 2, 2, 0xFD20);  // trail
        }
        // basket
        face_canvas->fillRoundRect((int)basket - 14, 116, 28, 8, 3, cfg.color_accent);
        face_canvas->setTextColor(0xFFFF); face_canvas->setTextDatum(top_left);
        face_canvas->drawString("caught " + String(score), 4, 2);
        face_canvas->setTextDatum(top_right);
        face_canvas->drawString("miss " + String(misses), 124, 2);
        face_canvas->pushSprite(0, 0);
        if (millis() - t0 > 90000) break;
        delay(16);
    }
    speak(String("you caught ") + score + " treats! yum~");
}

// ===========================================================================
//  (20 QUESTIONS lives in fox_bayes.inc - a Bayesian guesser that beats a fixed
//   tree: it updates beliefs and asks the most informative question each turn.)
// ===========================================================================

// ===========================================================================
//  REACTION - wait for the flash, then tap. Measures your reflex.
// ===========================================================================
void game_reaction() {
    speak("wait for green, then tap fast!");
    face_canvas->fillSprite(0xF800); face_canvas->pushSprite(0, 0);   // red
    delay(random(1200, 3500));
    face_canvas->fillSprite(0x07E0); face_canvas->pushSprite(0, 0);   // green
    uint32_t t0 = millis();
    while (millis() - t0 < 2000) {
        M5.update();
        if (M5.BtnA.wasClicked()) { speak(String("whoa, ") + (millis() - t0) + " ms!"); return; }
    }
    speak("too slow~ try again sometime");
}

