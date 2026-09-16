// fox_games.inc — digital fidget games. Included by fox_main.cpp.
//
// A mix of IMU tilt games (fly through wormholes, catch falling treats) and
// word games (20 questions, guess-my-paw). Each runs as a self-contained loop
// on the 128x128 LCD and returns when the button is clicked or the game ends.
// Voice can launch any of these; the fox narrates the result.
#pragma once

extern M5Canvas* face_canvas;
extern FoxConfig cfg;

// small tilt reader: returns roll/pitch in ~[-1,1]
void tilt_xy(float* rx, float* ry) {
    float ax, ay, az;
    M5.Imu.getAccel(&ax, &ay, &az);
    *rx = constrain(ay, -1.f, 1.f);
    *ry = constrain(ax, -1.f, 1.f);
}

// ===========================================================================
//  WORMHOLE — fly your ship through shrinking rings by tilting the device.
// ===========================================================================
void game_wormhole() {
    speak("hold tight, into the wormhole!");
    int score = 0; float ship_x = 64, ship_y = 64;
    // rings approach from the center outward (illusion of travel)
    struct Ring { float r; float cx, cy; bool passed; };
    const int NR = 5; Ring rings[NR];
    for (int i = 0; i < NR; ++i) rings[i] = { 8.0f + i * 22, 64, 64, false };
    uint32_t t0 = millis(); int lives = 3;
    while (lives > 0) {
        M5.update();
        if (M5.BtnA.wasDoubleClicked()) { g_goto_menu = true; break; }
        if (M5.BtnA.wasClicked()) break;
        float rx, ry; tilt_xy(&rx, &ry);
        ship_x = constrain(ship_x + rx * 6, 8, 120);
        ship_y = constrain(ship_y + ry * 6, 8, 120);
        face_canvas->fillSprite(0x0000);
        // starfield
        for (int i = 0; i < 24; ++i) {
            int sx = (i * 53 + (millis() / 20)) % 128;
            int sy = (i * 31) % 128;
            face_canvas->drawPixel(sx, sy, 0x39C7);
        }
        // rings grow toward viewer; passing plane at r>54 scores/misses
        for (int i = 0; i < NR; ++i) {
            rings[i].r += 0.9f;
            uint16_t c = cfg.color_primary;
            face_canvas->drawCircle(64, 64, (int)rings[i].r, c);
            face_canvas->drawCircle(64, 64, (int)rings[i].r + 1, c);
            if (!rings[i].passed && rings[i].r > 52) {
                rings[i].passed = true;
                // did the ship sit inside the ring's gap (near center-ish)?
                float d = hypotf(ship_x - 64, ship_y - 64);
                if (d < 26) { score++; }
                else { lives--; }
                rings[i].r = 8; rings[i].passed = false;   // recycle
            }
        }
        // ship
        face_canvas->fillTriangle(ship_x, ship_y - 6, ship_x - 5, ship_y + 5,
                                  ship_x + 5, ship_y + 5, cfg.color_accent);
        // hud
        face_canvas->setTextColor(0xFFFF); face_canvas->setTextDatum(top_left);
        face_canvas->drawString("score " + String(score), 4, 2);
        face_canvas->setTextDatum(top_right);
        face_canvas->drawString(String(lives) + " <3", 124, 2);
        face_canvas->pushSprite(0, 0);
        if (millis() - t0 > 60000) break;
        delay(16);
    }
    speak(String("nice flying! ") + score + " rings cleared~");
}

// ===========================================================================
//  CATCH — tilt to move a basket and catch falling treats.
// ===========================================================================
void game_catch() {
    speak("catch the treats! tilt to move~");
    float basket = 64; int score = 0, misses = 0;
    struct Drop { float x, y, v; bool live; };
    const int ND = 4; Drop drops[ND];
    for (int i = 0; i < ND; ++i) drops[i] = { (float)(random(10, 118)), (float)(-random(0, 120)), (float)random(2, 4) / 1.0f, true };
    uint32_t t0 = millis();
    while (misses < 5) {
        M5.update();
        if (M5.BtnA.wasDoubleClicked()) { g_goto_menu = true; break; }
        if (M5.BtnA.wasClicked()) break;
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

// guess-my-paw (kept from before, quick fidget)
void game_guess_paw() {
    int paw = esp_random() & 1;
    speak(paw ? "left or right paw? tap for right~" : "left or right paw? tap for left~");
    bool tapped = input_wait_yes(2200);
    speak(tapped ? "hehe you got me!" : "missed! again sometime~");
}
