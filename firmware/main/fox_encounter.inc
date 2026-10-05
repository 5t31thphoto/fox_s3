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
        if (ax < -0.7f) {   // an upward flick = toss
            // treat arcs from the hand (bottom) up to the fox's mouth (center),
            // the mouth opens as it arrives, then a happy chomp.
            int sx = 20 + esp_random() % 88;
            for (int f = 0; f <= 14; ++f) {
                float t = f / 14.0f;
                int tx = sx + (int)((64 - sx) * t);
                int ty = 120 - (int)(150 * t * (1.2f - t));    // parabola toward the mouth
                face_canvas->fillSprite(cfg.color_bg);
                face_set_mouth(t > 0.7f ? 1.0f : 0.15f);        // mouth opens near arrival
                face_draw(t > 0.7f ? MOOD_EXCITED : MOOD_HAPPY);
                face_canvas->fillCircle(tx, ty, 4, 0xFD20);
                face_canvas->fillCircle(tx, ty, 2, 0xFEA0);     // treat highlight
                face_canvas->pushSprite(0, 0);
                delay(22);
            }
            // chomp: mouth snaps shut, little sparkle
            for (int c = 0; c < 3; ++c) {
                face_canvas->fillSprite(cfg.color_bg);
                face_set_mouth(c == 1 ? 0.0f : 0.6f);
                face_draw(MOOD_HAPPY);
                if (c == 1) { face_canvas->drawString("nom!", 54, 40); }
                face_canvas->pushSprite(0, 0); delay(90);
            }
            fed++; needs_interact(needs, true);
            delay(150);
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
// ---------------------------------------------------------------------------
//  ROGUELIKE MAZE - a real little dungeon run.
//  Progression: the fox gains XP from treats/wins; levelling raises max HP and
//  attack. Items drop and have real effects. Critters are a risk/reward CHOICE
//  (fight for XP, or sneak past). The run ends when HP hits 0 OR you choose to
//  rest at a den; score = depth x treats x level. Deeper = better loot + tougher
//  foes, so there's a reason to push on.
// ---------------------------------------------------------------------------
#define MAZ_W 16
#define MAZ_H 16
// cells: 0 floor 1 wall 2 treat 3 critter 4 stairs-down 5 item 6 den(rest/bank)
static uint8_t maze[MAZ_H][MAZ_W];

static void maze_carve(int floor) {
    for (int y = 0; y < MAZ_H; ++y)
        for (int x = 0; x < MAZ_W; ++x)
            maze[y][x] = (x==0||y==0||x==MAZ_W-1||y==MAZ_H-1) ? 1
                        : (esp_random() % 100 < 20 + floor) ? 1 : 0;   // deeper = denser
    // guarantee a path corner-to-corner (drunkard's walk clearing)
    int cx = 1, cy = 1;
    while (cx < MAZ_W-2 || cy < MAZ_H-2) {
        maze[cy][cx] = 0;
        if (cx < MAZ_W-2 && (esp_random() & 1)) cx++;
        else if (cy < MAZ_H-2) cy++;
        else if (cx < MAZ_W-2) cx++;
    }
    maze[1][1] = 0;
    maze[MAZ_H-2][MAZ_W-2] = 4;                 // stairs down
    int treats = 5, crits = 2 + floor, items = (floor % 2) ? 1 : 2, dens = (floor % 3 == 0) ? 1 : 0;
    for (int i = 0; i < treats + crits + items + dens; ++i) {
        int x = 1 + esp_random() % (MAZ_W-2), y = 1 + esp_random() % (MAZ_H-2);
        if (maze[y][x] != 0 || (x==1&&y==1) || (x==MAZ_W-2&&y==MAZ_H-2)) continue;
        maze[y][x] = i < treats ? 2 : i < treats+crits ? 3 : i < treats+crits+items ? 5 : 6;
    }
}

struct Hero { int hp, maxhp, atk, lvl, xp, treats, depth, potions, charms; };
static void hero_levelcheck(Hero& h) {
    int need = h.lvl * 5;
    while (h.xp >= need) { h.xp -= need; h.lvl++; h.maxhp += 2; h.hp = h.maxhp; h.atk++; need = h.lvl * 5;
        face_caption(String("level up! now level ") + h.lvl); }
}
static const char* const CRITTERS[] = {"a grumpy badger","a sleepy owl","a thorn-sprite",
    "a cave bat","a mushroom goblin","a shadow fox","a stone golem","a will-o-wisp"};
static const char* const ITEMS[] = {"a berry potion","a lucky charm","an old map","a shiny acorn"};

void game_maze() {
    Hero h = {6,6,2,1,0,0,1,0,0};
    speak("a dungeon! tilt to explore. find the stairs to go deeper~");
    maze_carve(h.depth);
    int px = 1, py = 1; uint32_t lastmove = 0; uint32_t msg_until = 0; String msg;
    auto show_msg = [&](const String& m){ msg = m; msg_until = millis() + 1800; };

    while (true) {
        M5.update();
        if (M5.BtnA.wasDoubleClicked()) { g_goto_menu = true; return; }
        if (M5.BtnA.wasClicked()) {
            // click = rest/leave if on a den, else a quick status chirp
            if (maze[py][px] == 6) break;
            show_msg(String("lvl ") + h.lvl + "  atk " + h.atk + "  potions " + h.potions);
        }
        float rx, ry; tilt_xy(&rx, &ry);
        if (millis() - lastmove > 170) {
            int nx = px, ny = py;
            if (rx > 0.35f) nx++; else if (rx < -0.35f) nx--;
            if (ry > 0.35f) ny++; else if (ry < -0.35f) ny--;
            if (nx>=0 && nx<MAZ_W && ny>=0 && ny<MAZ_H && maze[ny][nx] != 1) {
                px = nx; py = ny;
                uint8_t& cell = maze[py][px];
                if (cell == 2) { cell = 0; h.treats++; h.xp += 1; needs_interact(needs, true);
                    if (esp_random()%100 < 30) show_msg("yum, a treat! (+1 xp)");
                    hero_levelcheck(h); }
                else if (cell == 5) { cell = 0; int it = esp_random()%4;
                    if (it==0) { h.potions++; show_msg("found a berry potion!"); }
                    else if (it==1) { h.charms++; h.atk++; show_msg("a lucky charm! atk up"); }
                    else if (it==2) { h.xp += 3; show_msg("an old map! (+3 xp)"); hero_levelcheck(h); }
                    else { h.treats += 3; show_msg("a shiny acorn! +3 treats"); }
                    needs_interact(needs, true); }
                else if (cell == 3) {                       // CRITTER: fight or flee
                    cell = 0;
                    const char* who = CRITTERS[esp_random() % 8];
                    int foe = 2 + h.depth + esp_random() % 3;    // foe strength scales with depth
                    // auto-resolve as a quick turn-based skirmish using atk vs foe
                    int roll = h.atk * 2 + (esp_random()%6) - foe;
                    if (roll >= 0) { int xp = 2 + h.depth; h.xp += xp;
                        show_msg(String("beat ") + who + "! +" + xp + " xp"); hero_levelcheck(h); }
                    else { int dmg = 1 + esp_random()%2;
                        if (h.potions > 0 && h.hp - dmg <= 1) { h.potions--; h.hp = min(h.maxhp, h.hp+3);
                            show_msg(String(who) + " hit hard, drank a potion!"); }
                        else { h.hp -= dmg; show_msg(String(who) + " scratched you (-" + dmg + " hp)"); } }
                }
                else if (cell == 6) {                       // DEN: heal a bit, offer to bank out
                    h.hp = min(h.maxhp, h.hp + 2);
                    show_msg("a cozy den. click here to rest and finish the run");
                }
                else if (cell == 4) {                       // STAIRS DOWN
                    h.depth++; h.hp = min(h.maxhp, h.hp + 1);
                    face_canvas->fillSprite(0x0000);
                    face_canvas->setTextColor(0x07E0); face_canvas->setTextDatum(middle_center);
                    face_canvas->setTextSize(2); face_canvas->drawString(String("floor ") + h.depth, 64, 48);
                    face_canvas->setTextSize(1); face_canvas->setTextColor(0xFFFF);
                    face_canvas->drawString(String("lvl ") + h.lvl + "  hp " + h.hp + "/" + h.maxhp, 64, 72);
                    face_canvas->drawString(String(h.treats) + " treats  " + h.potions + " potions", 64, 88);
                    face_canvas->pushSprite(0, 0);
                    speak(String("floor ") + h.depth + "! it's getting tougher~");
                    delay(1300);
                    maze_carve(h.depth); px = 1; py = 1; lastmove = millis(); continue;
                }
            }
            lastmove = millis();
        }
        if (h.hp <= 0) {
            int score = h.depth * (h.treats + 1) * h.lvl;
            face_canvas->fillSprite(0x0000);
            face_canvas->setTextColor(0xF800); face_canvas->setTextDatum(middle_center);
            face_canvas->setTextSize(2); face_canvas->drawString("worn out!", 64, 42);
            face_canvas->setTextSize(1); face_canvas->setTextColor(0xFFFF);
            face_canvas->drawString(String("floor ") + h.depth + "  lvl " + h.lvl, 64, 70);
            face_canvas->drawString(String("score ") + score, 64, 86);
            face_canvas->pushSprite(0, 0);
            speak(String("we fell on floor ") + h.depth + " at level " + h.lvl + ". score " + score + "! good run~");
            delay(2800); return;
        }
        // render
        face_canvas->fillSprite(0x0000);
        for (int y = 0; y < MAZ_H; ++y) for (int x = 0; x < MAZ_W; ++x) {
            uint16_t c = 0;
            switch (maze[y][x]) { case 1:c=0x39C7;break; case 2:c=0xFD20;break; case 3:c=0xF800;break;
                case 4:c=0x07E0;break; case 5:c=0x07FF;break; case 6:c=0xFFE0;break; default:c=0; }
            if (c) face_canvas->fillRect(x*8, y*8, 8, 8, c);
        }
        face_canvas->fillCircle(px*8+4, py*8+4, 3, cfg.color_primary);
        // HUD: hp bar + stats
        face_canvas->fillRect(0, 0, 128, 9, 0x0000);
        int hpw = (int)(h.hp / (float)h.maxhp * 40); if (hpw < 0) hpw = 0;
        face_canvas->fillRect(2, 2, 40, 5, 0x3186);
        face_canvas->fillRect(2, 2, hpw, 5, h.hp > h.maxhp/3 ? 0x07E0 : 0xF800);
        face_canvas->setTextColor(0xFFFF); face_canvas->setTextDatum(top_right); face_canvas->setTextSize(1);
        face_canvas->drawString(String("F") + h.depth + " L" + h.lvl + " t" + h.treats, 126, 1);
        if (msg.length() && millis() < msg_until) {
            face_canvas->setTextDatum(bottom_center); face_canvas->setTextColor(0xFFE0);
            face_canvas->drawString(msg, 64, 127);
        }
        face_canvas->pushSprite(0, 0);
        delay(16);
    }
    // rested at a den: bank the run
    int score = h.depth * (h.treats + 1) * h.lvl;
    speak(String("we rested safe on floor ") + h.depth + " at level " + h.lvl +
          " with " + h.treats + " treats. score " + score + "! nicely done~");
}
