// fox_input.inc — buttons, microphone capture, IMU gestures, on-device menu,
// USB-serial config receiver, and the (fixed) light-sleep path.
#pragma once
#include <esp_sleep.h>
#include <driver/gpio.h>

// (ButtonEvent and Gesture enums are declared in fox_decls.h)

// ---- button: tap / double-tap / hold state machine --------------------------
// Owner's rule: on the fox face, HOLD = talk, DOUBLE-click = menu, SINGLE click
// = a little interaction (never "exit"). A single tap is only reported after the
// double-click window closes, so a double-click never fires a tap first.
static constexpr uint32_t BTN_DEBOUNCE_MS  = 25;
static constexpr uint32_t BTN_DOUBLE_GAP_MS = 320;

void input_begin() {
    pinMode((int)USER_GPIO, INPUT_PULLUP);
    M5.Imu.begin();
}

ButtonEvent input_button_event() {
    static bool     raw_prev    = false;     // debouncer
    static uint32_t raw_t       = 0;
    static bool     down_prev   = false;     // debounced state
    static uint32_t down_at     = 0;
    static uint32_t up_at       = 0;
    static uint8_t  taps        = 0;
    static bool     hold_fired  = false;
    uint32_t now = millis();
    // A level only counts once it has been stable for BTN_DEBOUNCE_MS, so a
    // contact bounce can never be seen as an extra press/release (which would
    // turn one click into a "double-click").
    bool raw = digitalRead((int)USER_GPIO) == LOW;
    if (raw != raw_prev) { raw_prev = raw; raw_t = now; }
    bool down = (now - raw_t >= BTN_DEBOUNCE_MS) ? raw : down_prev;

    if (down && !down_prev) {                       // press
        down_at = now; hold_fired = false;
    }
    if (down && !hold_fired && now - down_at > PTT_MIN_MS) {
        hold_fired = true; taps = 0;                // a hold cancels pending taps
        down_prev = down;
        return BTN_HOLD_START;                      // caller captures until release
    }
    if (!down && down_prev) {                       // release
        up_at = now;
        if (!hold_fired && ++taps >= 2) { taps = 0; down_prev = down; return BTN_DOUBLE; }
    }
    down_prev = down;
    if (!down && taps == 1 && now - up_at > BTN_DOUBLE_GAP_MS) {
        taps = 0; return BTN_TAP;                   // window closed: single tap
    }
    return BTN_NONE;
}

static bool button_is_down() { return digitalRead((int)USER_GPIO) == LOW; }

// Double-click anywhere (including inside a game/tool loop) should jump to the
// menu. Games poll this each frame and return early when it fires; the main
// loop then opens the menu. Returns true once per double-click.
bool wants_menu() {
    return M5.BtnA.wasDoubleClicked();
}

// Simple yes-confirm used by IR narrowing: tap the button within `ms`.
bool input_wait_yes(uint32_t timeout_ms) {
    uint32_t t0 = millis();
    M5.update();
    (void)M5.BtnA.wasClicked();
    (void)M5.BtnA.wasDoubleClicked();
    while (millis() - t0 < timeout_ms) {
        M5.update();
        if (M5.BtnA.wasDoubleClicked()) { face_caption("no"); return false; }
        if (M5.BtnA.wasClicked()) { face_caption("yes"); return true; }
        float ax, ay, az;
        if (M5.Imu.getAccel(&ax, &ay, &az)) {
            float mag = sqrtf(ax*ax + ay*ay + az*az);
            if (fabsf(mag - 1.0f) > 1.2f) { face_caption("no"); return false; }
        }
        delay(15);
    }
    face_caption("...");
    return false;
}


// ---- microphone capture -----------------------------------------------------
// FIX: use fox_audio (EchoBase) instead of M5.Mic — same path as the working
// mic-avatar demo. M5.Mic via atomic_echo hung M5.begin on this hardware.
#include "fox_audio.h"

// Capture while the USER button is held. Returns a PSRAM buffer of int16 mono
// @16k; caller frees. Also drives the mouth from live level for lip-sync.
int16_t* capture_while_held(size_t* out_n) {
    size_t cap = SAMPLE_RATE * (PTT_MAX_MS / 1000) + SAMPLE_RATE;
    int16_t* buf = (int16_t*)fox_alloc(cap * sizeof(int16_t));
    if (!buf) { *out_n = 0; return nullptr; }
    audio_speaker_end();
    if (!g_echo_ok) { *out_n = 0; return buf; }
    audio_mic_speech_gain(true);            // speech gain (no clipping)
    size_t n = 0; uint32_t t0 = millis();
    const size_t CH = 256;
    while (button_is_down() && n + CH < cap && millis() - t0 < PTT_MAX_MS) {
        // true 16k mono for MultiNet/STT; the mouth stays in "listening" pose
        // (mic-driven mouth is puppet mode only)
        if (audio_record_mono(buf + n, CH)) n += CH;
        M5.update();
    }
    audio_mic_speech_gain(false);           // back to puppet-mode gain
    audio_mic_end();
    *out_n = n;
    return buf;
}

// Capture a single utterance using level-gated VAD, ending after trailing
// silence or when `timeout_ms` elapses with no speech. Used in conversation mode.
int16_t* capture_vad_burst(size_t* out_n, uint32_t timeout_ms) {
    size_t cap = SAMPLE_RATE * 12;
    int16_t* buf = (int16_t*)fox_alloc(cap * sizeof(int16_t));
    if (!buf) { *out_n = 0; return nullptr; }
    audio_speaker_end();
    if (!g_echo_ok) { *out_n = 0; return buf; }
    audio_mic_speech_gain(true);            // speech gain (no clipping)
    size_t n = 0; uint32_t t0 = millis(); uint32_t last_voice = 0;
    bool started = false;
    const size_t CH = 256;
    float floor_lvl = -1.0f;                 // learned ambient level
    size_t keep_pre = SAMPLE_RATE / 4;       // keep ~250ms before speech onset
    while (n + CH < cap) {
        if (millis() - t0 > timeout_ms && !started) break;
        if (audio_record_mono(buf + n, CH)) {
            long s = 0; for (size_t i = 0; i < CH; ++i) s += abs(buf[n + i]);
            float lvl = (s / (float)CH);
            if (floor_lvl < 0) floor_lvl = lvl;
            if (!started) floor_lvl = floor_lvl * 0.95f + lvl * 0.05f;
            float thresh = max(300.0f, floor_lvl * 3.0f);
            if (lvl > thresh) {
                if (!started) {               // drop leading silence, keep a lead-in
                    size_t lead = n > keep_pre ? n - keep_pre : 0;
                    if (lead) { memmove(buf, buf + lead, (n - lead + CH) * sizeof(int16_t)); n -= lead; }
                }
                started = true; last_voice = millis();
            }
            n += CH;
            if (started && millis() - last_voice > 900) break;
            if (!started && n > SAMPLE_RATE * 2) {  // rolling pre-roll window
                memmove(buf, buf + CH, (n - CH) * sizeof(int16_t)); n -= CH;
            }
        }
        M5.update();
        if (button_is_down()) break;
    }
    audio_mic_speech_gain(false);           // back to puppet-mode gain
    audio_mic_end();
    *out_n = started ? n : 0;
    return buf;
}

// ---- IMU gestures -----------------------------------------------------------
Gesture input_gesture() {
    float ax, ay, az;
    if (!M5.Imu.getAccel(&ax, &ay, &az)) return GST_NONE;
    float mag = sqrtf(ax*ax + ay*ay + az*az);
    static uint32_t last = 0;
    uint32_t now = millis();
    if (now - last < 400) return GST_NONE;
    if (mag > 2.4f) { last = now; return GST_SHAKE; }
    if (mag > 1.6f) { last = now; return GST_TAP; }
    return GST_NONE;
}

// pickup-to-wake used when returning from light sleep
static bool imu_picked_up() {
    float ax, ay, az;
    if (!M5.Imu.getAccel(&ax, &ay, &az)) return false;
    return fabsf(sqrtf(ax*ax + ay*ay + az*az) - 1.0f) > 0.35f;
}

// ---- light sleep (FIX bug #4) ----------------------------------------------
// The old code used ext0 wake on GPIO41. On the ESP32-S3, ext0/ext1 deep-sleep
// wake only supports RTC-capable GPIO0..21 — GPIO41 is not one, so the device
// would sleep forever. We instead use *light* sleep with a GPIO wake source,
// which works on any GPIO, and also wake on a timer to check the IMU.
void enter_light_sleep() {
    // A real, recoverable NAP. No esp_light_sleep: light sleep isolates every
    // GPIO (see boot log "sleep_gpio: isolate all GPIO pins"), which cuts the
    // LCD's control lines -> the panel blanks and nothing re-initialises it,
    // and it also drops the USB link. Instead: panel to sleep, backlight off,
    // amp off, then poll the button / IMU slowly until woken.
    face_caption("*curls up*");
    face_tick(MOOD_SLEEPY);
    delay(600);
    audio_speaker_end(); audio_mic_end();
    M5.Display.sleep();                 // GC9107 sleep-in (keeps its state)
    fox_backlight(0);                   // LP5562 blue channel off
    while (digitalRead((int)USER_GPIO) == LOW) delay(10);   // release first
    for (;;) {
        M5.update();
        if (digitalRead((int)USER_GPIO) == LOW) break;      // button press
        if (imu_picked_up()) break;                         // picked up
        delay(60);
    }
    while (digitalRead((int)USER_GPIO) == LOW) delay(10);   // don't leak a click
    M5.Display.wakeup();
    fox_backlight(200);
    face_caption("*stretches* hi!");
    face_draw(MOOD_HAPPY);
    face_wake();
}

// ---- on-device menu (FIX bug #6) -------------------------------------------
// The menu is navigated by FLICKING the device up or down (per the original
// control scheme); a button click confirms the highlighted item. This keeps a
// one-button device fully drivable without awkward tap-counting.
enum Flick { FLICK_NONE, FLICK_UP, FLICK_DOWN };

static Flick input_flick() {
    float ax, ay, az;
    if (!M5.Imu.getAccel(&ax, &ay, &az)) return FLICK_NONE;
    static uint32_t last = 0;
    uint32_t now = millis();
    if (now - last < 350) return FLICK_NONE;   // debounce between flicks
    // The AtomS3R sits nose-up; a quick pitch flick moves ax past a threshold.
    if (ax >  0.55f) { last = now; return FLICK_DOWN; }
    if (ax < -0.55f) { last = now; return FLICK_UP; }
    return FLICK_NONE;
}

// Menu items are (label, id). Actions are dispatched by the host in fox_main.
struct MenuItem { const char* label; const char* id; };
static const MenuItem MENU[] = {
    {"talk to me",         "talk"},
    {"conversation mode",  "conv"},
    {"-- tools --",        "nop"},
    {"BLE radar",          "ble_radar"},
    {"WiFi radar",         "wifi_radar"},
    {"hunt mode",          "sniffer"},
    {"probe sniffer",      "probes"},
    {"-- games --",        "nop"},
    {"roguelike maze",     "maze"},
    {"wormhole (tilt)",    "wormhole"},
    {"catch treats (tilt)","catch"},
    {"20 questions",       "twentyq"},
    {"reaction test",      "reaction"},
    {"guess my paw",       "paw"},
    {"-- fox time --",     "nop"},
    {"pet the fox",        "pet"},
    {"feed the fox",       "feed"},
    {"tug of war",         "tug"},
    {"surprise me",        "encounter"},
    {"-- toys --",         "nop"},
    {"plasma",             "plasma"},
    {"starfield",          "starfield"},
    {"ink sandbox",        "ink"},
    {"spirograph",         "spiro"},
    {"lip-sync puppet",    "lipsync"},
    {"-- reports --",      "nop"},
    {"weather",            "weather"},
    {"space weather",      "space"},
    {"aurora tonight?",    "aurora"},
    {"bitcoin price",      "btc"},
    {"-- settings --",     "nop"},
    {"volume",             "volume"},
    {"voice pack",         "voice"},
    {"brain online/offline","brain"},
    {"forget memories",    "forget"},
    {"sleep",              "sleep"},
    {"close",              "close"},
};
static const int MENU_N = sizeof(MENU) / sizeof(MENU[0]);


static void menu_render(int sel) {
    face_canvas->fillSprite(face_cfg->color_bg);
    face_canvas->setTextDatum(middle_left);
    // show a window of items around the selection (list scrolls)
    int visible = 7, half = visible / 2;
    int start = sel - half; if (start < 0) start = 0;
    if (start > MENU_N - visible) start = max(0, MENU_N - visible);
    for (int row = 0; row < visible && start + row < MENU_N; ++row) {
        int i = start + row, y = 16 + row * 15;
        bool header = !strcmp(MENU[i].id, "nop");
        if (i == sel && !header) {
            face_canvas->fillRoundRect(4, y - 7, 120, 14, 3, face_cfg->color_primary);
            face_canvas->setTextColor(0x0000);
        } else if (header) {
            face_canvas->setTextColor(face_cfg->color_accent);
        } else face_canvas->setTextColor(0xFFFF);
        face_canvas->drawString(MENU[i].label, header ? 6 : 10, y);
    }
    // scrollbar hint
    face_canvas->setTextColor(0x9CD3); face_canvas->setTextDatum(bottom_center);
    face_canvas->drawString("flick to move . click to pick", 64, 126);
    face_canvas->pushSprite(0, 0);
}

void open_menu() {
    int sel = 0;
    // The double-click that opened us is still in M5's button history; wait for
    // release and drain it so it can't instantly "pick" the first item.
    while (digitalRead((int)USER_GPIO) == LOW) { M5.update(); delay(5); }
    M5.update(); (void)M5.BtnA.wasClicked(); (void)M5.BtnA.wasDoubleClicked();
    uint32_t idle = millis();
    menu_render(sel);
    for (;;) {
        M5.update();
        Flick f = input_flick();
        if (f == FLICK_DOWN) {
            do { sel = (sel + 1) % MENU_N; } while (!strcmp(MENU[sel].id, "nop"));
            menu_render(sel); idle = millis();
        } else if (f == FLICK_UP) {
            do { sel = (sel - 1 + MENU_N) % MENU_N; } while (!strcmp(MENU[sel].id, "nop"));
            menu_render(sel); idle = millis();
        }
        if (M5.BtnA.wasClicked()) {
            const char* id = MENU[sel].id;
            if (!strcmp(id, "nop")) { idle = millis(); continue; }  // header row
            if (!strcmp(id, "close")) return;
            menu_dispatch(id);
            return;
        }
        if (millis() - idle > 12000) return;   // auto-close
        delay(15);
    }
}

// (weather / space-weather / aurora now live in fox_net.inc)

// ---- USB serial config receiver --------------------------------------------
// The web flasher sends a single JSON line over USB CDC after flashing. We
// parse it and persist. Secrets (wifi/key) live only in NVS on the device;
// they are never compiled into the binary or CI.
static String serial_line;
static void apply_config_json(const String& js) {
    JsonDocument d;
    if (deserializeJson(d, js) != DeserializationError::Ok) {
        Serial.println("FOX_CONFIG_ERR");
        Serial.flush();
        return;
    }
    if (teach_apply_json(d)) return;          // a "teach" line, not a config line
    if (d["name"].is<const char*>())  cfg.name = d["name"].as<String>();
    if (d["tz"].is<const char*>())    cfg.timezone = d["tz"].as<String>();
    if (d["weather"].is<const char*>()) cfg.weather = d["weather"].as<String>();
    if (d["voice_speed"].is<int>())  cfg.voice_speed  = (uint8_t)constrain((int)d["voice_speed"],  40, 130);
    if (d["voice_pitch"].is<int>())  cfg.voice_pitch  = (uint8_t)constrain((int)d["voice_pitch"],  16, 100);
    if (d["voice_throat"].is<int>()) cfg.voice_throat = (uint8_t)constrain((int)d["voice_throat"], 90, 255);
    if (d["voice_mouth"].is<int>())  cfg.voice_mouth  = (uint8_t)constrain((int)d["voice_mouth"],  90, 255);
    if (d["ssid"].is<const char*>())  cfg.wifi_ssid = d["ssid"].as<String>();
    if (d["pass"].is<const char*>())  cfg.wifi_pass = d["pass"].as<String>();
    if (d["key"].is<const char*>())   cfg.api_key = d["key"].as<String>();
    if (d["chat_model"].is<const char*>()) cfg.chat_model = d["chat_model"].as<String>();
    if (d["brain"].is<const char*>()) cfg.brain_online = (d["brain"].as<String>() != "offline");
    if (d["stt_model"].is<const char*>())  cfg.stt_model = d["stt_model"].as<String>();
    if (d["api_base"].is<const char*>())   cfg.api_base = d["api_base"].as<String>();
    if (d["personality"].is<const char*>()) cfg.personality = d["personality"].as<String>();
    if (d["voice"].is<const char*>()) cfg.voice_pack = d["voice"].as<String>();
    if (d["mode"].is<const char*>())  cfg.mode = d["mode"].as<String>();
    if (d["splash"].is<const char*>()) cfg.splash_text = d["splash"].as<String>();
    if (d["splash_fx"].is<int>())     cfg.splash_fx = d["splash_fx"].as<int>();
    if (d["theme"].is<int>())         cfg.theme = d["theme"].as<int>();
    if (d["weights_url"].is<const char*>()) cfg.weights_url = d["weights_url"].as<String>();
    if (d["volume"].is<int>())        cfg.volume = d["volume"].as<int>();
    if (d["cpri"].is<int>())          cfg.color_primary = d["cpri"].as<int>();
    if (d["cacc"].is<int>())          cfg.color_accent = d["cacc"].as<int>();
    if (d["cbg"].is<int>())           cfg.color_bg = d["cbg"].as<int>();
    // per-tool enables + lip-sync
    if (d["tool_ble"].is<bool>())   cfg.tool_ble  = d["tool_ble"].as<bool>();
    if (d["tool_wifi"].is<bool>())  cfg.tool_wifi = d["tool_wifi"].as<bool>();
    if (d["tool_imu"].is<bool>())   cfg.tool_imu  = d["tool_imu"].as<bool>();
    if (d["lip_sync"].is<bool>())   cfg.lip_sync  = d["lip_sync"].as<bool>();
    save_config();
    voice_begin(cfg);
    // ACK before speak so the web UI unblocks even if audio is slow/broken
    Serial.println("FOX_CONFIG_OK");
    Serial.flush();
    speak(String("thanks! i'm ") + cfg.name + " now~");
}

void input_poll() {
    while (Serial.available()) {
        char c = (char)Serial.read();
        if (c == '\n') {
            serial_line.trim();
            if (serial_line == "FOX?") { Serial.println("FOX_READY"); }   // handshake
            else if (serial_line.startsWith("{")) apply_config_json(serial_line);
            serial_line = "";
        } else if (c != '\r' && serial_line.length() < 4096) serial_line += c;
    }
}
