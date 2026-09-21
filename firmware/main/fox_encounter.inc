// fox_encounter.inc — "fox encounters" (quick you+the-fox mini-games) and a
// top-down roguelike maze you steer by tilt, where encounters auto-resolve as
// the fox bumps into things. Included by fox_main.cpp. Nothing is persisted.
#pragma once

extern M5Canvas* face_canvas;
extern FoxConfig cfg;
extern FoxNeeds needs;
extern bool g_goto_menu;

// ===========================================================================
//  FOX ENCOUNTERS — short, warm interactions with the fox itself.
// ===========================================================================

// Pet the fox: stroke by tilting side to side a few times; it purrs.
void enc_pet() {
    speak("ooh, pets? tilt me side to side~");
    int strokes = 0; float last = 0; uint32_t t0 = millis();
    while (millis() - t0 < 12000 && strokes < 6) {
        M5.update();
        if (M5.BtnA.wasDoubleClicked()) { g_goto_menu = true; return; }
        if (M5.BtnA.wasClicked()) break;
        float rx, ry; tilt_xy(&rx, &ry);
        if ((rx > 0.4f && last <= 0) || (rx < -0.4f && last >= 0)) { strokes++; last = rx; }
        face_canvas->fillSprite(cfg.color_bg);
        face_set_mouth(0.2f);
        face_draw(MOOD_HAPPY);
        face_canvas->setTextColor(0xFFFF); face_canvas->setTextDatum(bottom_center);
        face_canvas->drawString(String("purr ") + strokes + "/6", 64, 124);
        face_canvas->pushSprite(0, 0);
        delay(20);
    }
    needs_interact(needs, true);
    speak(strokes >= 6 ? "hehe that felt so nice~" : "mmm, thank you");
}

// Feed the fox: flick "up" to toss a treat; time the catch.
void enc_feed() {
    speak("got a treat? flick up to toss it!");
    uint32_t t0 = millis(); int fed = 0;
    while (millis() - t0 < 12000 && fed < 3) {
        M5.update();
        if (M5.BtnA.wasDoubleClicked()) { g_goto_menu = true; return; }
        if (M5.BtnA.wasClicked()) break;
        float ax, ay, az; M5.Imu.getAccel(&ax, &ay, &az);
        if (ax < -0.7f) {   // an upward flick
            // animate a treat arcing up and the fox catching it
            for (int f = 0; f <= 10; ++f) {
                face_canvas->fillSprite(cfg.color_bg);
                int ty = 120 - f * 9;
                face_canvas->fillCircle(64, ty, 4, 0xFD20);
                face_draw(MOOD_EXCITED);
                face_canvas->pushSprite(0, 0);
                delay(24);
            }
            face_set_mouth(1.0f); face_draw(MOOD_HAPPY);
            fed++; needs_interact(needs, true);
            delay(200);
        }
        delay(20);
    }
    speak(fed ? "nom nom, thank you!" : "no treats? that's okay~");
}

// Tug of war: rapidly flick back and forth to out-tug the fox.
void enc_tug() {
    speak("tug of war! flick back and forth fast!");
    int pull = 50; float last = 0; uint32_t t0 = millis();
    while (millis() - t0 < 10000 && pull > 0 && pull < 100) {
        M5.update();
        if (M5.BtnA.wasDoubleClicked()) { g_goto_menu = true; return; }
        float rx, ry; tilt_xy(&rx, &ry);
        if ((ry > 0.5f && last <= 0) || (ry < -0.5f && last >= 0)) { pull += 4; last = ry; }
        pull -= 1;  // the fox pulls back
        pull = constrain(pull, 0, 100);
        face_canvas->fillSprite(cfg.color_bg);
        face_canvas->fillRect(0, 58, (int)(pull * 1.28f), 12, cfg.color_primary);
        face_canvas->drawFastVLine(64, 50, 28, 0xFFFF);   // center line
        face_canvas->setTextColor(0xFFFF); face_canvas->setTextDatum(top_center);
        face_canvas->drawString("TUG!", 64, 20);
        face_canvas->pushSprite(0, 0);
        delay(24);
    }
    needs_interact(needs, true);
    speak(pull >= 100 ? "you win! so strong!" : pull <= 0 ? "hehe i got it!" : "good game~");
}

// A tiny random-encounter picker so "fox encounter" (voice/menu) is a surprise.
void enc_random() {
    switch (esp_random() % 3) {
        case 0: enc_pet(); break;
        case 1: enc_feed(); break;
        default: enc_tug(); break;
    }
}

// ===========================================================================
//  ROGUELIKE MAZE — a top-down maze you steer with tilt. The fox auto-fights /
//  auto-collects whatever it walks into (a light auto-play roguelike). Reach
//  the exit; grab treats, dodge or bump critters. Regenerates each run.
// ===========================================================================
#define MAZ_W 16
#define MAZ_H 16
static uint8_t maze[MAZ_H][MAZ_W];   // 0 floor, 1 wall, 2 treat, 3 critter, 4 exit

static void maze_gen() {
    // simple randomized walls with a guaranteed border; carve a rough path.
    for (int y = 0; y < MAZ_H; ++y)
        for (int x = 0; x < MAZ_W; ++x)
            maze[y][x] = (x == 0 || y == 0 || x == MAZ_W-1 || y == MAZ_H-1) ? 1
                        : (esp_random() % 100 < 22 ? 1 : 0);
    // carve a diagonal-ish corridor so it's always solvable-ish
    int cx = 1, cy = 1;
    while (cx < MAZ_W-2 || cy < MAZ_H-2) {
        maze[cy][cx] = 0;
        if (cx < MAZ_W-2 && (esp_random() & 1)) cx++;
        else if (cy < MAZ_H-2) cy++;
    }
    maze[1][1] = 0;
    maze[MAZ_H-2][MAZ_W-2] = 4;   // exit
    // sprinkle treats and critters on floor tiles
    for (int i = 0; i < 10; ++i) {
        int x = 1 + esp_random() % (MAZ_W-2), y = 1 + esp_random() % (MAZ_H-2);
        if (maze[y][x] == 0) maze[y][x] = (esp_random() & 1) ? 2 : 3;
    }
}

void game_maze() {
    speak("into the maze! tilt to move, i'll handle what we meet~");
    maze_gen();
    int px = 1, py = 1, hp = 5, treats = 0;
    uint32_t lastmove = 0;
    while (true) {
        M5.update();
        if (M5.BtnA.wasDoubleClicked()) { g_goto_menu = true; return; }
        if (M5.BtnA.wasClicked()) break;
        float rx, ry; tilt_xy(&rx, &ry);
        // step on a throttle so tilt = steady movement
        if (millis() - lastmove > 180) {
            int nx = px, ny = py;
            if (rx > 0.35f) nx++; else if (rx < -0.35f) nx--;
            if (ry > 0.35f) ny++; else if (ry < -0.35f) ny--;
            if (nx >= 0 && nx < MAZ_W && ny >= 0 && ny < MAZ_H && maze[ny][nx] != 1) {
                px = nx; py = ny;
                uint8_t& cell = maze[py][px];
                if (cell == 2) { treats++; cell = 0; needs_interact(needs, true); }
                else if (cell == 3) {           // auto-resolved "fight"
                    cell = 0;
                    if (esp_random() % 100 < 65) { /* fox wins */ }
                    else { hp--; }              // took a scratch
                }
                else if (cell == 4) {
                    speak(String("we made it! ") + treats + " treats, hp " + hp + "~");
                    return;
                }
            }
            lastmove = millis();
        }
        if (hp <= 0) { speak("oof, we got worn out. let's rest~"); return; }
        // render: scale the 16x16 grid to 128x128 (8px cells)
        face_canvas->fillSprite(0x0000);
        for (int y = 0; y < MAZ_H; ++y)
            for (int x = 0; x < MAZ_W; ++x) {
                uint16_t c = 0x0000;
                switch (maze[y][x]) {
                    case 1: c = 0x39C7; break;               // wall
                    case 2: c = 0xFD20; break;               // treat
                    case 3: c = 0xF800; break;               // critter
                    case 4: c = 0x07E0; break;               // exit
                    default: c = 0x0000;
                }
                if (c) face_canvas->fillRect(x * 8, y * 8, 8, 8, c);
            }
        face_canvas->fillCircle(px * 8 + 4, py * 8 + 4, 3, cfg.color_primary);  // fox
        face_canvas->setTextColor(0xFFFF); face_canvas->setTextDatum(top_left);
        face_canvas->drawString(String("hp") + hp + " t" + treats, 2, 0);
        face_canvas->pushSprite(0, 0);
        delay(16);
    }
}
