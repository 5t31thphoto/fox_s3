// fox_games.inc — digital fidget games. Included by fox_main.cpp.
//
// A mix of IMU tilt games (fly through wormholes, catch falling treats) and
// word games (20 questions, guess-my-paw). Each runs as a self-contained loop
// on the 128x128 LCD and returns when the button is clicked or the game ends.
// Voice can launch any of these; the fox narrates the result.
#pragma once

extern M5Canvas* face_canvas;
extern FoxConfig cfg;

// Single click belongs to the game. Double click is the universal escape/menu.
static ButtonEvent game_button() {
    return input_button_event();
}

void tilt_xy(float* rx, float* ry) {
    if (!imu_tilt(rx, ry)) { *rx = *ry = 0; }
}

// ===========================================================================
//  WORMHOLE — fly your ship through shrinking rings by tilting the device.
// ===========================================================================
void game_wormhole() {
    uint32_t t_start = millis();
    speak("hold tight, into the wormhole!");
    face_caption("tilt to steer — click to exit");
    float ship_x = 64, ship_y = 100;
    int score = 0, lives = 3;
    // tunnel rings: z grows toward camera
    struct Ring { float z; float cx, cy; };
    const int NR = 8;
    Ring rings[NR];
    for (int i = 0; i < NR; ++i) {
        rings[i].z = 20.f + i * 18.f;
        rings[i].cx = 64 + (int)(esp_random() % 21) - 10;
        rings[i].cy = 64 + (int)(esp_random() % 21) - 10;
    }
    uint32_t last = millis();
    while (lives > 0 && millis() - t_start < 90000) {
        M5.update();
        { ButtonEvent bev = game_button(); if (bev == BTN_DOUBLE) { g_goto_menu = true; break; } if (bev == BTN_TAP) { for (int j=0;j<NR;++j) rings[j].z += 5.0f; } }
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
        // rings from far to near
        for (int i = 0; i < NR; ++i) {
            rings[i].z -= 28.f * dt;
            if (rings[i].z < 4.f) {
                // missed / scored
                float dx = ship_x - rings[i].cx;
                float dy = ship_y - rings[i].cy;
                if (dx*dx + dy*dy < 22*22) score += 1;
                else { lives -= 1; }
                rings[i].z = 20.f + (NR - 1) * 18.f;
                rings[i].cx = 64 + (int)(esp_random() % 31) - 15;
                rings[i].cy = 58 + (int)(esp_random() % 25) - 12;
            }
            float sc = 80.f / rings[i].z;
            int r = (int)(sc * 10);
            if (r < 3) r = 3;
            if (r > 60) r = 60;
            uint16_t col = rings[i].z < 12 ? 0xF800 : (rings[i].z < 30 ? 0xFFE0 : 0x07FF);
            face_canvas->drawCircle((int)rings[i].cx, (int)rings[i].cy, r, col);
            face_canvas->drawCircle((int)rings[i].cx, (int)rings[i].cy, r - 1, col);
        }
        // ship
        face_canvas->fillTriangle((int)ship_x, (int)ship_y - 6,
                                  (int)ship_x - 6, (int)ship_y + 6,
                                  (int)ship_x + 6, (int)ship_y + 6, 0xFFFF);
        face_canvas->setTextDatum(top_left);
        face_canvas->setTextColor(0xFFFF);
        face_canvas->setTextSize(1);
        face_canvas->drawString(String("sc ") + score + "  hp " + lives, 2, 2);
        face_canvas->pushSprite(0, 0);
        delay(16);  // ~60fps eye candy without tearing as bad as unbounded
    }
    face_caption(String("wormhole score ") + score);
    speak(String("score ") + score + "!");
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
        { ButtonEvent bev = game_button(); if (bev == BTN_DOUBLE) { g_goto_menu = true; break; } if (bev == BTN_TAP) { for(int j=0;j<ND;++j) if(fabsf(drops[j].x-basket)<24) drops[j].v *= 0.45f; } }
        float rx, ry; tilt_xy(&rx, &ry);
        basket = constrain(basket + rx * 8, 12, 116);
        face_canvas->fillSprite(cfg.color_bg);
        for (int i = 0; i < ND; ++i) {
            drops[i].y += drops[i].v;
            face_canvas->fillCircle((int)drops[i].x, (int)drops[i].y, 4, cfg.color_primary);
            if (drops[i].y > 112) {
                if (fabsf(drops[i].x - basket) < 14) score++; else misses++;
                drops[i].x = random(10, 118); drops[i].y = -random(0, 60);
                drops[i].v = (float)random(2, 5);
            }
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
//  (20 QUESTIONS lives in fox_bayes.inc — a Bayesian guesser that beats a fixed
//   tree: it updates beliefs and asks the most informative question each turn.)
// ===========================================================================

// ===========================================================================
//  REACTION — wait for the flash, then tap. Measures your reflex.
// ===========================================================================
void game_reaction() {
    speak("wait for green, then click fast!");
    face_canvas->fillSprite(0xF800); face_canvas->setTextColor(0xFFFF);
    face_canvas->setTextDatum(middle_center); face_canvas->drawString("WAIT", 64, 64); face_canvas->pushSprite(0, 0);
    delay(random(1200, 3500));
    face_canvas->fillSprite(0x07E0); face_canvas->drawString("CLICK!", 64, 64); face_canvas->pushSprite(0, 0);
    uint32_t t0 = millis();
    while (millis() - t0 < 2000) {
        M5.update();
        ButtonEvent ev = game_button();
        if (ev == BTN_DOUBLE) { g_goto_menu = true; return; }
        if (ev == BTN_TAP) { speak(String("whoa, ") + (millis() - t0) + " ms!"); return; }
    }
    speak("too slow~ try again sometime");
}

void game_guess_paw() {
    int paw = esp_random() & 1;
    speak(paw ? "left or right paw? click for right~" : "left or right paw? click for left~");
    bool tapped = input_wait_yes(2200);
    speak(tapped ? "hehe you got me!" : "missed! again sometime~");
}
