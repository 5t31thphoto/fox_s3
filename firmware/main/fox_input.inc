// fox_input.inc — buttons, microphone capture, IMU gestures, menu and sleep.
#pragma once
#include <esp_sleep.h>
#include <driver/gpio.h>
#include <math.h>

static uint32_t btn_down_at = 0;
static bool btn_was_down = false;
static bool btn_hold_sent = false;
static bool btn_pending_tap = false;
static uint32_t btn_pending_at = 0;
static constexpr uint32_t DOUBLE_WINDOW_MS = 320;

void input_begin() {
    pinMode((int)USER_GPIO, INPUT_PULLUP);
    M5.Imu.begin();
}

// Button semantics are deliberately split from M5Unified's wasClicked():
// a first short press is held pending for the double-click window. This is the
// only way to make "single click is usable by the app" and "double click exits"
// coexist without the first click prematurely leaving the app.
ButtonEvent input_button_event() {
    const bool down = digitalRead((int)USER_GPIO) == LOW;
    const uint32_t now = millis();

    if (down && !btn_was_down) {
        btn_down_at = now;
        btn_was_down = true;
        btn_hold_sent = false;
    }

    if (down && btn_was_down && !btn_hold_sent &&
        now - btn_down_at >= PTT_MIN_MS) {
        btn_hold_sent = true;
        btn_pending_tap = false;
        return BTN_HOLD_START;
    }

    if (!down && btn_was_down) {
        const uint32_t held = now - btn_down_at;
        btn_was_down = false;
        if (btn_hold_sent) {
            btn_hold_sent = false;
            return BTN_NONE;
        }
        if (held < PTT_MIN_MS) {
            if (btn_pending_tap && now - btn_pending_at <= DOUBLE_WINDOW_MS) {
                btn_pending_tap = false;
                return BTN_DOUBLE;
            }
            btn_pending_tap = true;
            btn_pending_at = now;
        }
    }

    if (!down && btn_pending_tap && now - btn_pending_at > DOUBLE_WINDOW_MS) {
        btn_pending_tap = false;
        return BTN_TAP;
    }
    return BTN_NONE;
}

static bool button_is_down() { return digitalRead((int)USER_GPIO) == LOW; }

// Kept for legacy callers; it is intentionally non-destructive. App loops use
// input_button_event() so a single click remains available to the app.
bool wants_menu() { return false; }

// Simple yes-confirm: single click = yes; double click = no/menu.
bool input_wait_yes(uint32_t timeout_ms) {
    uint32_t t0 = millis();
    while (millis() - t0 < timeout_ms) {
        M5.update();
        ButtonEvent ev = input_button_event();
        if (ev == BTN_DOUBLE) { face_caption("no"); return false; }
        if (ev == BTN_TAP) { face_caption("yes"); return true; }
        float ax, ay, az;
        if (M5.Imu.getAccel(&ax, &ay, &az)) {
            float mag = sqrtf(ax*ax + ay*ay + az*az);
            if (fabsf(mag - 1.0f) > 1.25f) { face_caption("no"); return false; }
        }
        delay(10);
    }
    face_caption("...");
    return false;
}

#include "fox_audio.h"

int16_t* capture_while_held(size_t* out_n) {
    size_t cap = SAMPLE_RATE * (PTT_MAX_MS / 1000) + SAMPLE_RATE;
    int16_t* buf = (int16_t*)heap_caps_malloc(cap * sizeof(int16_t),
                                              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) { *out_n = 0; return nullptr; }
    audio_speaker_end();
    if (!g_echo_ok) { *out_n = 0; return buf; }
    size_t n = 0; uint32_t t0 = millis();
    const size_t CH = 256;
    while (button_is_down() && n + CH < cap && millis() - t0 < PTT_MAX_MS) {
        if (audio_record(buf + n, CH)) {
            long s = 0; for (size_t i = 0; i < CH; ++i) s += abs(buf[n + i]);
            face_set_mouth((s / (float)CH) / 8000.0f);
            n += CH;
        }
        M5.update();
    }
    audio_mic_end();
    *out_n = n;
    return buf;
}

int16_t* capture_vad_burst(size_t* out_n, uint32_t timeout_ms) {
    size_t cap = SAMPLE_RATE * 12;
    int16_t* buf = (int16_t*)heap_caps_malloc(cap * sizeof(int16_t),
                                              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) { *out_n = 0; return nullptr; }
    audio_speaker_end();
    if (!g_echo_ok) { *out_n = 0; return buf; }
    size_t n = 0; uint32_t t0 = millis(); uint32_t last_voice = 0;
    bool started = false;
    const size_t CH = 256;
    while (n + CH < cap) {
        if (millis() - t0 > timeout_ms && !started) break;
        if (audio_record(buf + n, CH)) {
            long s = 0; for (size_t i = 0; i < CH; ++i) s += abs(buf[n + i]);
            float lvl = s / (float)CH;
            face_set_mouth(lvl / 8000.0f);
            if (lvl > 900) { started = true; last_voice = millis(); }
            n += CH;
            if (started && millis() - last_voice > 900) break;
        }
        M5.update();
        if (button_is_down()) break;
    }
    audio_mic_end();
    *out_n = started ? n : 0;
    return buf;
}

// ---------------------------------------------------------------------------
// IMU helpers. Use gravity-derived roll/pitch rather than raw sensor axes.
// This makes the games behave consistently across the AtomS3R's mounting
// orientation and prevents acceleration magnitude from being mistaken for tilt.
// ---------------------------------------------------------------------------
bool imu_tilt(float* rx, float* ry) {
    float ax, ay, az;
    if (!M5.Imu.getAccel(&ax, &ay, &az)) return false;
    float roll  = atan2f(ay, az);
    float pitch = atan2f(-ax, sqrtf(ay*ay + az*az));
    const float span = 0.55f; // about 31.5 degrees = full travel
    auto norm = [span](float a) {
        float v = a / span;
        if (v > 1) v = 1; if (v < -1) v = -1;
        // small center dead-zone, then smooth response
        if (fabsf(v) < 0.08f) return 0.0f;
        return v;
    };
    *rx = norm(roll);
    *ry = norm(pitch);
    return true;
}

bool imu_rotation_dps(float* gx, float* gy, float* gz) {
    float x, y, z;
    if (!M5.Imu.getGyro(&x, &y, &z)) return false;
    if (gx) *gx = x; if (gy) *gy = y; if (gz) *gz = z;
    return true;
}

Gesture input_gesture() {
    float ax, ay, az;
    if (!M5.Imu.getAccel(&ax, &ay, &az)) return GST_NONE;
    float mag = sqrtf(ax*ax + ay*ay + az*az);
    static uint32_t last = 0;
    uint32_t now = millis();
    if (now - last < 300) return GST_NONE;
    if (mag > 2.25f) { last = now; return GST_SHAKE; }
    return GST_NONE;
}

static bool imu_picked_up() {
    float ax, ay, az;
    if (!M5.Imu.getAccel(&ax, &ay, &az)) return false;
    return fabsf(sqrtf(ax*ax + ay*ay + az*az) - 1.0f) > 0.35f;
}

void enter_light_sleep() {
    face_caption("*curls up*");
    face_dim();
    audio_speaker_end(); audio_mic_end();
    gpio_wakeup_enable(USER_GPIO, GPIO_INTR_LOW_LEVEL);
    esp_sleep_enable_gpio_wakeup();
    esp_sleep_enable_timer_wakeup(3ULL * 1000000ULL);
    for (;;) {
        esp_light_sleep_start();
        esp_sleep_wakeup_cause_t c = esp_sleep_get_wakeup_cause();
        if (c == ESP_SLEEP_WAKEUP_GPIO) break;
        if (c == ESP_SLEEP_WAKEUP_TIMER && imu_picked_up()) break;
    }
    face_wake();
}

enum Flick { FLICK_NONE, FLICK_UP, FLICK_DOWN };
static Flick input_flick() {
    float rx, ry;
    if (!imu_tilt(&rx, &ry)) return FLICK_NONE;
    static float last = 0;
    static uint32_t last_t = 0;
    uint32_t now = millis();
    float d = ry - last;
    last = ry;
    if (now - last_t < 220) return FLICK_NONE;
    if (d > 0.42f) { last_t = now; return FLICK_DOWN; }
    if (d < -0.42f) { last_t = now; return FLICK_UP; }
    return FLICK_NONE;
}

struct MenuItem { const char* label; const char* id; };
static const MenuItem MENU[] = {
    {"talk to me", "talk"}, {"conversation mode", "conv"}, {"-- tools --", "nop"},
    {"BLE radar", "ble_radar"}, {"WiFi radar", "wifi_radar"}, {"hunt mode", "sniffer"},
    {"probe sniffer", "probes"}, {"learn my TV", "learn_tv"}, {"-- games --", "nop"},
    {"roguelike maze", "maze"}, {"wormhole (tilt)", "wormhole"}, {"catch treats (tilt)", "catch"},
    {"20 questions", "twentyq"}, {"reaction test", "reaction"}, {"guess my paw", "paw"},
    {"-- fox time --", "nop"}, {"pet the fox", "pet"}, {"feed the fox", "feed"},
    {"tug of war", "tug"}, {"surprise me", "encounter"}, {"-- toys --", "nop"},
    {"plasma", "plasma"}, {"starfield", "starfield"}, {"ink sandbox", "ink"},
    {"spirograph", "spiro"}, {"lip-sync puppet", "lipsync"}, {"-- reports --", "nop"},
    {"weather", "weather"}, {"space weather", "space"}, {"aurora tonight?", "aurora"},
    {"-- settings --", "nop"}, {"volume", "volume"}, {"voice pack", "voice"},
    {"forget memories", "forget"}, {"sleep", "sleep"}, {"close", "close"},
};
static const int MENU_N = sizeof(MENU) / sizeof(MENU[0]);

static void menu_render(int sel) {
    face_canvas->fillSprite(face_cfg->color_bg);
    face_canvas->setTextDatum(middle_left);
    int visible = 7, half = visible / 2;
    int start = sel - half; if (start < 0) start = 0;
    if (start > MENU_N - visible) start = max(0, MENU_N - visible);
    for (int row = 0; row < visible && start + row < MENU_N; ++row) {
        int i = start + row, y = 16 + row * 15;
        bool header = !strcmp(MENU[i].id, "nop");
        if (i == sel && !header) {
            face_canvas->fillRoundRect(4, y - 7, 120, 14, 3, face_cfg->color_primary);
            face_canvas->setTextColor(0x0000);
        } else if (header) face_canvas->setTextColor(face_cfg->color_accent);
        else face_canvas->setTextColor(0xFFFF);
        String label = MENU[i].label;
        if (face_canvas->textWidth(label) > 112) label = label.substring(0, 17) + "…";
        face_canvas->drawString(label, header ? 6 : 10, y);
    }
    face_canvas->setTextColor(0x9CD3); face_canvas->setTextDatum(bottom_center);
    face_canvas->drawString("flick = move   click = pick", 64, 126);
    face_canvas->pushSprite(0, 0);
}

// Volume is an actual adjustment screen: flick up/down changes it, single
// click confirms, double click abandons and returns to the menu.
void volume_adjust() {
    int value = cfg.volume;
    face_caption("volume: flick up/down, click to save");
    while (true) {
        M5.update();
        ButtonEvent ev = input_button_event();
        if (ev == BTN_DOUBLE) return;
        if (ev == BTN_TAP) {
            cfg.volume = value;
            audio_set_volume(value);
            save_config();
            speak(String("volume ") + value);
            return;
        }
        Flick f = input_flick();
        if (f == FLICK_UP) value = min(100, value + 5);
        if (f == FLICK_DOWN) value = max(0, value - 5);
        face_canvas->fillSprite(face_cfg->color_bg);
        face_canvas->setTextColor(face_cfg->color_accent); face_canvas->setTextDatum(middle_center);
        face_canvas->drawString("VOLUME", 64, 20);
        face_canvas->drawRoundRect(10, 50, 108, 18, 4, 0x7BEF);
        face_canvas->fillRoundRect(12, 52, (int)(104 * value / 100.0f), 14, 3, face_cfg->color_primary);
        face_canvas->setTextColor(0xFFFF); face_canvas->setTextSize(2);
        face_canvas->drawString(String(value), 64, 82);
        face_canvas->setTextSize(1);
        face_canvas->drawString("flick up/down", 64, 104);
        face_canvas->drawString("click = confirm", 64, 118);
        face_canvas->pushSprite(0, 0);
        delay(16);
    }
}

void open_menu() {
    int sel = 0;
    uint32_t idle = millis();
    menu_render(sel);
    for (;;) {
        M5.update();
        Flick f = input_flick();
        if (f == FLICK_DOWN) { do { sel = (sel + 1) % MENU_N; } while (!strcmp(MENU[sel].id, "nop")); menu_render(sel); idle = millis(); }
        else if (f == FLICK_UP) { do { sel = (sel - 1 + MENU_N) % MENU_N; } while (!strcmp(MENU[sel].id, "nop")); menu_render(sel); idle = millis(); }
        ButtonEvent ev = input_button_event();
        if (ev == BTN_TAP) {
            const char* id = MENU[sel].id;
            if (!strcmp(id, "nop")) { idle = millis(); continue; }
            if (!strcmp(id, "close")) return;
            menu_dispatch(id);
            return;
        }
        if (ev == BTN_DOUBLE) return;
        if (millis() - idle > 12000) return;
        delay(15);
    }
}

static String serial_line;
static void apply_config_json(const String& js) {
    JsonDocument d;
    if (deserializeJson(d, js) != DeserializationError::Ok) { Serial.println("FOX_CONFIG_ERR"); Serial.flush(); return; }
    if (d["name"].is<const char*>()) cfg.name = d["name"].as<String>();
    if (d["tz"].is<const char*>()) cfg.timezone = d["tz"].as<String>();
    if (d["weather"].is<const char*>()) cfg.weather = d["weather"].as<String>();
    if (d["ssid"].is<const char*>()) cfg.wifi_ssid = d["ssid"].as<String>();
    if (d["pass"].is<const char*>()) cfg.wifi_pass = d["pass"].as<String>();
    if (d["key"].is<const char*>()) cfg.api_key = d["key"].as<String>();
    if (d["chat_model"].is<const char*>()) cfg.chat_model = d["chat_model"].as<String>();
    if (d["stt_model"].is<const char*>()) cfg.stt_model = d["stt_model"].as<String>();
    if (d["api_base"].is<const char*>()) cfg.api_base = d["api_base"].as<String>();
    if (d["personality"].is<const char*>()) cfg.personality = d["personality"].as<String>();
    if (d["voice"].is<const char*>()) cfg.voice_pack = d["voice"].as<String>();
    if (d["mode"].is<const char*>()) cfg.mode = d["mode"].as<String>();
    if (d["splash"].is<const char*>()) cfg.splash_text = d["splash"].as<String>();
    if (d["splash_fx"].is<int>()) cfg.splash_fx = d["splash_fx"].as<int>();
    if (d["theme"].is<int>()) cfg.theme = d["theme"].as<int>();
    if (d["weights_url"].is<const char*>()) cfg.weights_url = d["weights_url"].as<String>();
    if (d["volume"].is<int>()) cfg.volume = d["volume"].as<int>();
    if (d["cpri"].is<int>()) cfg.color_primary = d["cpri"].as<int>();
    if (d["cacc"].is<int>()) cfg.color_accent = d["cacc"].as<int>();
    if (d["cbg"].is<int>()) cfg.color_bg = d["cbg"].as<int>();
    if (d["tool_ble"].is<bool>()) cfg.tool_ble = d["tool_ble"].as<bool>();
    if (d["tool_wifi"].is<bool>()) cfg.tool_wifi = d["tool_wifi"].as<bool>();
    if (d["tool_ir"].is<bool>()) cfg.tool_ir = d["tool_ir"].as<bool>();
    if (d["tool_imu"].is<bool>()) cfg.tool_imu = d["tool_imu"].as<bool>();
    if (d["lip_sync"].is<bool>()) cfg.lip_sync = d["lip_sync"].as<bool>();
    save_config(); voice_begin(cfg);
    Serial.println("FOX_CONFIG_OK"); Serial.flush();
    speak(String("thanks! i'm ") + cfg.name + " now~");
}

void input_poll() {
    while (Serial.available()) {
        char c = (char)Serial.read();
        if (c == '\n') { apply_config_json(serial_line); serial_line = ""; }
        else if (c != '\r' && serial_line.length() < 1024) serial_line += c;
    }
}
