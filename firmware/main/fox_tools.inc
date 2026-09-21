// fox_tools.inc — the Flipper-ish hacker tools. Included by fox_main.cpp.
//
// These are the "digital fidget toy" instruments: a BLE radar you sweep around
// the room by rotating the device, a WiFi scanner, and passive sniffers. Each
// is both:
//   * a standalone menu app (flick to navigate, double-click to exit), and
//   * an LLM-callable tool that returns a short text report the fox can speak.
//
// Everything is drawn on the 128x128 LCD via the shared face canvas.
#pragma once
#include <WiFi.h>
#include <esp_wifi.h>
#include <BLEDevice.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>

// forward decls from other units
extern M5Canvas* face_canvas;
extern FoxConfig cfg;

// ---------------------------------------------------------------------------
//  Rotational RF radar
//
// This is deliberately NOT a compass. The fox is held level and rotated
// through a sweep. The green needle is the angle actually measured from the
// IMU gyro; the red needle is a slowly advancing target that tells the user
// where/how fast to rotate. Radio scans continue throughout the sweep.
//
// Each source gets an RSSI profile indexed by rotational angle. The strongest
// part of that profile supplies its relative direction; the overall RSSI
// supplies only a relative radial placement. No absolute bearing or metres
// are claimed.
// ---------------------------------------------------------------------------
static const int RADAR_BINS = 72; // 5 degree bins
static const int MAX_BLIPS = 24;
static const float RADAR_SWEEP_DEG = 360.0f;
static const float RADAR_TARGET_DPS = 18.0f;

struct RadarSource {
    String name;
    float rssi_sum[RADAR_BINS];
    uint16_t hits[RADAR_BINS];
    float angle;
    float rssi;
    float confidence;
    uint32_t seen;
};

static RadarSource blips[MAX_BLIPS];
static int blip_n = 0;
static BLEScan* ble_scan = nullptr;
static bool radar_active = false;
static float radar_angle = 0.0f;       // unwrapped, relative rotation
static float radar_target = 0.0f;      // red needle, 0..360
static uint32_t radar_last_ms = 0;
static uint32_t radar_start_ms = 0;
static float radar_total_motion = 0.0f;
static bool radar_complete = false;

static float wrap360(float a) {
    while (a < 0) a += 360.0f;
    while (a >= 360.0f) a -= 360.0f;
    return a;
}

static void radar_reset() {
    blip_n = 0;
    memset(blips, 0, sizeof(blips));
    radar_angle = 0.0f;
    radar_target = 0.0f;
    radar_total_motion = 0.0f;
    radar_last_ms = millis();
    radar_start_ms = radar_last_ms;
    radar_complete = false;
    radar_active = true;
}

static float radar_update_rotation() {
    uint32_t now = millis();
    float dt = (now - radar_last_ms) * 0.001f;
    radar_last_ms = now;
    if (dt <= 0 || dt > 0.25f) return wrap360(radar_angle);

    float gx, gy, gz;
    if (M5.Imu.getGyro(&gx, &gy, &gz)) {
        // Device is held level during the sweep, so the Z gyro is the
        // rotation axis. Keep the sign: the red needle advances in the same
        // direction the operator is expected to rotate.
        float d = gz * dt;
        radar_angle += d;
        radar_total_motion += fabsf(d);
    }
    float elapsed = (now - radar_start_ms) * 0.001f;
    radar_target = fmodf(elapsed * RADAR_TARGET_DPS, RADAR_SWEEP_DEG);
    if (radar_total_motion >= 330.0f && elapsed >= 14.0f) radar_complete = true;
    return wrap360(radar_angle);
}

static int radar_bin(float angle) {
    int b = (int)floorf(wrap360(angle) / (360.0f / RADAR_BINS));
    return constrain(b, 0, RADAR_BINS - 1);
}

static void radar_record(const String& name, int rssi, float angle) {
    if (!name.length()) return;
    int idx = -1;
    for (int i = 0; i < blip_n; ++i) {
        if (blips[i].name == name) { idx = i; break; }
    }
    if (idx < 0) {
        if (blip_n >= MAX_BLIPS) return;
        idx = blip_n++;
        blips[idx].name = name;
        for (int b = 0; b < RADAR_BINS; ++b) {
            blips[idx].rssi_sum[b] = 0;
            blips[idx].hits[b] = 0;
        }
    }
    int b = radar_bin(angle);
    blips[idx].rssi_sum[b] += (float)rssi;
    blips[idx].hits[b]++;
    blips[idx].seen = millis();
}

static void radar_finalize_sources() {
    for (int i = 0; i < blip_n; ++i) {
        int best = 0; float best_r = -127.0f; int total = 0;
        for (int b = 0; b < RADAR_BINS; ++b) {
            if (!blips[i].hits[b]) continue;
            float r = blips[i].rssi_sum[b] / blips[i].hits[b];
            total += blips[i].hits[b];
            if (r > best_r) { best_r = r; best = b; }
        }
        blips[i].angle = (best + 0.5f) * (360.0f / RADAR_BINS);
        blips[i].rssi = best_r;

        // Confidence combines sample count with how much the peak stands out
        // from the source's own angular baseline.
        float base = 0.0f; int nb = 0;
        for (int b = 0; b < RADAR_BINS; ++b) if (blips[i].hits[b]) {
            base += blips[i].rssi_sum[b] / blips[i].hits[b]; nb++;
        }
        if (nb) base /= nb; else base = best_r;
        float peak = max(0.0f, best_r - base);
        float sample_c = constrain(total / 20.0f, 0.0f, 1.0f);
        blips[i].confidence = constrain(0.15f + peak / 25.0f, 0.0f, 1.0f) * sample_c;
    }
}

static void draw_tiny_fox(int cx, int cy) {
    uint16_t fur = cfg.color_primary, acc = cfg.color_accent;
    face_canvas->fillTriangle(cx - 5, cy - 4, cx - 8, cy - 11, cx - 1, cy - 7, fur);
    face_canvas->fillTriangle(cx + 5, cy - 4, cx + 8, cy - 11, cx + 1, cy - 7, fur);
    face_canvas->fillCircle(cx, cy, 6, fur);
    face_canvas->fillCircle(cx - 2, cy - 1, 1, 0x0000);
    face_canvas->fillCircle(cx + 2, cy - 1, 1, 0x0000);
    face_canvas->fillCircle(cx, cy + 2, 1, acc);
}

static void radar_draw(float actual_angle, int count, bool wifi, bool scanning) {
    uint16_t bg = cfg.color_bg, fur = cfg.color_primary, acc = cfg.color_accent;
    face_canvas->fillSprite(bg);
    const int cx = 64, cy = 64, R = 48;
    for (int r = R; r > 0; r -= 16) face_canvas->drawCircle(cx, cy, r, 0x2145);
    face_canvas->drawLine(cx - R, cy, cx + R, cy, 0x2145);
    face_canvas->drawLine(cx, cy - R, cx, cy + R, 0x2145);

    float ga = (actual_angle - 90.0f) * PI / 180.0f;
    float ra = (radar_target - 90.0f) * PI / 180.0f;
    if (scanning) {
        face_canvas->drawLine(cx, cy, cx + R * cosf(ra), cy + R * sinf(ra), 0xF800); // red target
        face_canvas->drawLine(cx, cy, cx + R * cosf(ga), cy + R * sinf(ga), 0x07E0); // green actual
    }

    if (!scanning) {
        radar_finalize_sources();
        for (int i = 0; i < blip_n; ++i) {
            float br = (blips[i].angle - 90.0f) * PI / 180.0f;
            // Relative radial heuristic only: stronger RSSI is nearer to Fox.
            float rr = R - constrain((blips[i].rssi + 90.0f) * 0.70f, 0.0f, (float)(R - 7));
            int x = cx + (int)(rr * cosf(br));
            int y = cy + (int)(rr * sinf(br));
            uint16_t col = blips[i].confidence > 0.45f ? fur : 0x8410;
            face_canvas->fillCircle(x, y, 3, col);
        }
    }
    draw_tiny_fox(cx, cy);

    face_canvas->setTextColor(acc); face_canvas->setTextDatum(top_left);
    face_canvas->drawString(wifi ? "WIFI RADAR" : "BLE RADAR", 3, 2);
    face_canvas->setTextDatum(top_right);
    face_canvas->drawString(String(count), 125, 2);
    face_canvas->setTextDatum(bottom_center);
    face_canvas->setTextColor(0x9CD3);
    if (scanning) face_canvas->drawString("green follows red", 64, 125);
    else face_canvas->drawString("click=rescan  double=exit", 64, 125);
    face_canvas->pushSprite(0, 0);
}

static void radar_prompt(bool wifi) {
    face_canvas->fillSprite(cfg.color_bg);
    face_canvas->setTextDatum(middle_center);
    face_canvas->setTextColor(cfg.color_accent);
    face_canvas->drawString(wifi ? "WIFI RADAR" : "BLE RADAR", 64, 22);
    face_canvas->setTextColor(0xFFFF);
    face_canvas->drawString("hold level + steady", 64, 47);
    face_canvas->drawString("rotate 360°", 64, 63);
    face_canvas->setTextColor(0x07E0); face_canvas->drawString("green = you", 64, 80);
    face_canvas->setTextColor(0xF800); face_canvas->drawString("red = target", 64, 94);
    face_canvas->setTextColor(0x9CD3); face_canvas->drawString("follow red with green", 64, 111);
    face_canvas->pushSprite(0, 0);
    delay(900);
}

static bool radar_should_exit() {
    M5.update();
    return M5.BtnA.wasDoubleClicked();
}

// ---------------------------------------------------------------------------
// BLE RADAR
// ---------------------------------------------------------------------------
static void ble_begin_once() {
    static bool inited = false;
    if (inited && ble_scan) return;
    BLEDevice::init("");
    ble_scan = BLEDevice::getScan();
    ble_scan->setActiveScan(true);
    ble_scan->setInterval(80);
    ble_scan->setWindow(60);
    inited = true;
}

void tool_menu_ble_radar() {
    WiFi.mode(WIFI_OFF);
    delay(50);
    ble_begin_once();
    for (;;) {
        radar_prompt(false);
        radar_reset();
        speak("follow my red needle. spin me around~");
        while (!radar_complete && millis() - radar_start_ms < 45000) {
            if (radar_should_exit()) { tool_radio_release(); return; }
            float actual = radar_update_rotation();
            BLEScanResults* res = ble_scan->start(1, false);
            int n = res ? res->getCount() : 0;
            for (int i = 0; i < n; ++i) {
                BLEAdvertisedDevice d = res->getDevice(i);
                String nm = d.haveName() ? String(d.getName().c_str())
                                         : String(d.getAddress().toString().c_str()).substring(0, 8);
                radar_record(nm, d.getRSSI(), actual);
            }
            ble_scan->clearResults();
            radar_draw(actual, blip_n, false, true);
        }
        radar_complete = true;
        radar_finalize_sources();
        radar_draw(wrap360(radar_angle), blip_n, false, false);

        // The finished map stays up. Single click means "measure again";
        // double-click is the only exit gesture.
        while (true) {
            M5.update();
            if (M5.BtnA.wasDoubleClicked()) { tool_radio_release(); return; }
            if (M5.BtnA.wasClicked()) break;
            radar_target = fmodf((millis() - radar_start_ms) * 24.0f / 1000.0f, 360.0f);
            radar_draw(wrap360(radar_angle), blip_n, false, true);
            delay(25);
        }
    }
}

// LLM-callable: quick BLE sweep — not the directional radar.
const char* tool_ble_scan_report() {
    static char buf[220];
    ble_begin_once();
    BLEScanResults* res = ble_scan->start(3, false);
    int n = res ? res->getCount() : 0, strongest = -999; String who = "";
    for (int i = 0; i < n; ++i) {
        BLEAdvertisedDevice d = res->getDevice(i);
        if (d.getRSSI() > strongest) {
            strongest = d.getRSSI();
            who = d.haveName() ? String(d.getName().c_str())
                               : String(d.getAddress().toString().c_str());
        }
    }
    ble_scan->clearResults();
    snprintf(buf, sizeof(buf),
             "{\"ble_devices\":%d,\"closest\":\"%s\",\"rssi\":%d}",
             n, who.c_str(), strongest);
    return buf;
}

// ---------------------------------------------------------------------------
// WIFI RADAR
// ---------------------------------------------------------------------------
void tool_menu_wifi_radar() {
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
    delay(50);
    for (;;) {
        radar_prompt(true);
        radar_reset();
        speak("follow my red needle. spin me around~");
        while (!radar_complete && millis() - radar_start_ms < 45000) {
            if (radar_should_exit()) { tool_radio_release(); return; }
            float actual = radar_update_rotation();
            int n = WiFi.scanNetworks(false, true, true, 180);
            for (int i = 0; i < n; ++i)
                radar_record(WiFi.SSID(i), WiFi.RSSI(i), actual);
            WiFi.scanDelete();
            radar_draw(actual, blip_n, true, true);
        }
        radar_complete = true;
        radar_finalize_sources();
        radar_draw(wrap360(radar_angle), blip_n, true, false);
        while (true) {
            M5.update();
            if (M5.BtnA.wasDoubleClicked()) { tool_radio_release(); return; }
            if (M5.BtnA.wasClicked()) break;
            radar_target = fmodf((millis() - radar_start_ms) * 24.0f / 1000.0f, 360.0f);
            radar_draw(wrap360(radar_angle), blip_n, true, true);
            delay(25);
        }
    }
}

const char* tool_wifi_scan_report() {
    static char buf[220];
    WiFi.mode(WIFI_STA);
    int n = WiFi.scanNetworks();
    int best = -999; String who = "";
    for (int i = 0; i < n; ++i)
        if (WiFi.RSSI(i) > best) { best = WiFi.RSSI(i); who = WiFi.SSID(i); }
    WiFi.scanDelete();
    snprintf(buf, sizeof(buf),
             "{\"wifi_aps\":%d,\"strongest\":\"%s\",\"rssi\":%d}",
             n, who.c_str(), best);
    return buf;
}

// Release every radio resource owned by the tool layer. Never erase saved
// WiFi credentials: disconnect(false,false) is intentionally avoided here.
void tool_radio_release() {
    WiFi.scanDelete();
    esp_wifi_set_promiscuous(false);
    WiFi.mode(WIFI_OFF);
    if (ble_scan) {
        ble_scan->clearResults();
        BLEDevice::deinit(true);
        ble_scan = nullptr;
    }
}

// ===========================================================================
//  PASSIVE WIFI SNIFFER + PWNAGOTCHI-STYLE HUNT
//  Promiscuous mode counts frames per type AND passively spots the things a
//  pwnagotchi cares about — open networks, EAPOL handshake frames, and PMKID
//  material — purely to TELL YOU and to feed the fox's score/level/mood. It
//  transmits nothing and it PERSISTS nothing: every count is in RAM and resets
//  on reboot, exactly as asked.
// ===========================================================================
static uint32_t sniff_mgmt = 0, sniff_data = 0, sniff_ctrl = 0;
// pwnagotchi-ish tallies (RAM only, never saved)
static uint32_t pwn_open_beacons = 0;   // beacons advertising no crypto
static uint32_t pwn_eapol = 0;          // EAPOL (handshake) frames seen
static uint32_t pwn_pmkid = 0;          // EAPOL msg1 carrying PMKID
static uint32_t pwn_probes = 0;         // probe requests (devices looking)
static uint32_t pwn_score = 0, pwn_level = 1;

static inline uint16_t le16(const uint8_t* p) { return p[0] | (p[1] << 8); }

// Inspect a raw 802.11 frame for the pwnagotchi-relevant bits. Best-effort and
// tolerant: if the layout isn't what we expect we just skip it.
static void pwn_inspect(const uint8_t* pkt, int len) {
    if (len < 24) return;
    uint16_t fc = le16(pkt);
    uint8_t ftype = (fc >> 2) & 0x3;
    uint8_t fsub  = (fc >> 4) & 0xF;

    if (ftype == 0 && fsub == 4) { pwn_probes++; return; }      // probe request
    if (ftype == 0 && fsub == 8) {                              // beacon
        // fixed params (12) then tagged params; privacy bit in capability info
        if (len >= 24 + 12) {
            uint16_t cap = le16(pkt + 24 + 10);
            bool privacy = cap & 0x10;
            if (!privacy) pwn_open_beacons++;                  // open network!
        }
        return;
    }
    // EAPOL is a data frame (type 2) with LLC/SNAP ethertype 0x888E. Scan a
    // little into the payload for the SNAP header rather than parse every
    // variant precisely.
    if (ftype == 2) {
        for (int i = 24; i + 8 < len && i < 40; ++i) {
            if (pkt[i] == 0xAA && pkt[i+1] == 0xAA && pkt[i+2] == 0x03 &&
                pkt[i+6] == 0x88 && pkt[i+7] == 0x8E) {
                pwn_eapol++;
                // EAPOL-Key message 1 of 4 often carries a PMKID in the KDE.
                // Heuristic: key-info field with the pairwise+ack bits and no
                // MIC bit → message 1; PMKID presence is common on WPA2.
                int k = i + 8;                 // start of EAPOL header
                if (k + 5 < len && pkt[k+1] == 0x03 /* key */) pwn_pmkid++;
                return;
            }
        }
    }
}

static void sniffer_cb(void* buf, wifi_promiscuous_pkt_type_t type) {
    wifi_promiscuous_pkt_t* p = (wifi_promiscuous_pkt_t*)buf;
    switch (type) {
        case WIFI_PKT_MGMT: sniff_mgmt++; break;
        case WIFI_PKT_DATA: sniff_data++; break;
        case WIFI_PKT_CTRL: sniff_ctrl++; break;
        default: break;
    }
    if (p && p->rx_ctrl.sig_len > 0)
        pwn_inspect(p->payload, p->rx_ctrl.sig_len);
}

// Level up as the fox "catches" interesting things (open nets + handshakes).
static void pwn_update_level() {
    pwn_score = pwn_open_beacons * 2 + pwn_eapol * 5 + pwn_pmkid * 10;
    uint32_t lvl = 1 + (uint32_t)(sqrtf((float)pwn_score) / 3.0f);
    if (lvl != pwn_level) {
        pwn_level = lvl;
        speak(String("level ") + pwn_level + "! i'm getting good at this~");
    }
}

void tool_menu_sniffer() {
    speak("passive sniff mode. i only listen~");
    WiFi.mode(WIFI_STA); WiFi.disconnect();
    esp_wifi_set_promiscuous(true);
    esp_wifi_set_promiscuous_rx_cb(&sniffer_cb);
    int ch = 1;
    uint32_t last = 0;
    while (true) {
        M5.update();
        if (M5.BtnA.wasDoubleClicked()) break;
        if (millis() - last > 250) {   // hop channels ~4/sec
            ch = (ch % 13) + 1;
            esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
            last = millis();
            pwn_update_level();
        }
        face_canvas->fillSprite(cfg.color_bg);
        face_canvas->setTextColor(cfg.color_accent); face_canvas->setTextDatum(top_left);
        face_canvas->drawString("HUNT ch" + String(ch) + "  Lv" + String(pwn_level), 4, 2);
        auto bar = [&](int y, const String& lbl, uint32_t v, uint16_t c) {
            int w = (int)min(v, (uint32_t)116);
            face_canvas->fillRect(4, y + 9, w, 7, c);
            face_canvas->setTextColor(0xFFFF); face_canvas->drawString(lbl, 6, y);
        };
        bar(18, "open nets " + String(pwn_open_beacons), pwn_open_beacons * 3, 0x5E11);
        bar(40, "handshakes " + String(pwn_eapol), pwn_eapol * 8, 0xFB43);
        bar(62, "pmkid " + String(pwn_pmkid), pwn_pmkid * 12, 0xC24F);
        bar(84, "probes " + String(pwn_probes), pwn_probes / 2, 0x9CD3);
        face_canvas->setTextColor(0xFFFF); face_canvas->setTextDatum(bottom_left);
        face_canvas->drawString("score " + String(pwn_score), 4, 122);
        face_canvas->setTextColor(0x9CD3); face_canvas->setTextDatum(bottom_right);
        face_canvas->drawString("double=exit", 124, 122);
        face_canvas->pushSprite(0, 0);
        delay(40);
    }
    tool_radio_release();
    // report — and forget: nothing is written to flash.
    speak(String("i heard ") + pwn_eapol + " handshakes and " +
          pwn_open_beacons + " open nets. all in my head, nothing saved~");
    sniff_mgmt = sniff_data = sniff_ctrl = 0;
    pwn_open_beacons = pwn_eapol = pwn_pmkid = pwn_probes = 0;
}

// ===========================================================================
//  PROBE-REQUEST SNIFFER (Marauder-style, non-destructive)
//  Shows the network names nearby devices are actively searching for — a fun,
//  read-only "what are the phones around me looking for" scope.
// ===========================================================================
static char probe_names[8][33];
static int  probe_count = 0;

static void probe_cb(void* buf, wifi_promiscuous_pkt_type_t type) {
    if (type != WIFI_PKT_MGMT) return;
    wifi_promiscuous_pkt_t* p = (wifi_promiscuous_pkt_t*)buf;
    const uint8_t* pkt = p->payload; int len = p->rx_ctrl.sig_len;
    if (len < 26) return;
    uint16_t fc = le16(pkt);
    if (((fc >> 2) & 0x3) != 0 || ((fc >> 4) & 0xF) != 4) return;  // probe req
    // first tagged element at offset 24 should be SSID (id 0)
    if (pkt[24] != 0) return;
    int slen = pkt[25];
    if (slen <= 0 || slen > 32 || 26 + slen > len) return;
    char ssid[33]; memcpy(ssid, pkt + 26, slen); ssid[slen] = 0;
    for (int i = 0; i < probe_count; ++i)
        if (!strcmp(probe_names[i], ssid)) return;   // dedup
    if (probe_count < 8) { strncpy(probe_names[probe_count++], ssid, 32); }
}

void tool_menu_probe_sniff() {
    speak("listening for what phones are looking for~");
    probe_count = 0;
    WiFi.mode(WIFI_STA); WiFi.disconnect();
    esp_wifi_set_promiscuous(true);
    esp_wifi_set_promiscuous_rx_cb(&probe_cb);
    int ch = 1; uint32_t last = 0;
    while (true) {
        M5.update();
        if (M5.BtnA.wasDoubleClicked()) break;
        if (millis() - last > 300) { ch = (ch % 13) + 1; esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE); last = millis(); }
        face_canvas->fillSprite(cfg.color_bg);
        face_canvas->setTextColor(cfg.color_accent); face_canvas->setTextDatum(top_left);
        face_canvas->drawString("PROBES ch" + String(ch), 4, 2);
        for (int i = 0; i < probe_count; ++i) {
            face_canvas->setTextColor(0xFFFF);
            face_canvas->drawString(String(probe_names[i]).substring(0, 18), 6, 20 + i * 12);
        }
        if (!probe_count) { face_canvas->setTextColor(0x9CD3); face_canvas->drawString("(listening...)", 6, 24); }
        face_canvas->pushSprite(0, 0);
        delay(40);
    }
    tool_radio_release();
    speak(String("saw ") + probe_count + " different network names being sought");
}
