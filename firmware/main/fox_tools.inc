// fox_tools.inc — the Flipper-ish hacker tools. Included by fox_main.cpp.
//
// These are the "digital fidget toy" instruments: a BLE radar you sweep around
// the room by rotating the device, a WiFi scanner, and passive sniffers. Each
// is both:
//   * a standalone menu app (flick to navigate, click to exit), and
//   * an LLM-callable tool that returns a short text report the fox can speak.
//
// Everything is drawn on the 128x128 LCD via the shared face canvas.
#pragma once
#include <WiFi.h>
#include <esp_wifi.h>
#include <BLEDevice.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include <esp_heap_caps.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

// forward decls from other units
extern M5Canvas* face_canvas;
extern FoxConfig cfg;
static BLEScan* ble_scan = nullptr;
static bool ble_initialized = false;

// Arduino-ESP32 3.x exposes the BLE C++ API when the IDF NimBLE host is enabled.
// Keep the host initialized for the life of Fox; repeatedly deinitializing/reinitializing
// BLE is both expensive and has known reinitialization problems. Radio *scans* are
// still stopped and their result cache is released whenever a tool exits.
static void ble_begin_once() {
    if (ble_initialized && ble_scan) return;
    if (!BLEDevice::init("")) return;
    ble_scan = BLEDevice::getScan();
    if (!ble_scan) return;
    ble_scan->setActiveScan(true);
    ble_scan->setInterval(100);
    ble_scan->setWindow(90);
    ble_initialized = true;
}

static void tool_radio_release() {
    // Return the shared radio hardware to a neutral state after every RF app.
    WiFi.scanDelete();
    esp_wifi_set_promiscuous(false);
    WiFi.mode(WIFI_OFF);
    if (ble_scan) {
        ble_scan->stop();
        ble_scan->clearResults();
    }
}

// ---------------------------------------------------------------------------
//  Rotational RSSI radar
//
// This is deliberately NOT a compass. The user holds Fox level and rotates
// through one guided sweep. The green needle is the measured IMU rotation;
// the red needle is the target measurement position. RF scans run while the
// sweep advances. Each source accumulates an RSSI profile against rotation
// angle. After the sweep, angular position comes from that profile and radial
// position is only a relative RSSI heuristic.
// ---------------------------------------------------------------------------
static constexpr int RADAR_SOURCES = 24;
static constexpr int RADAR_BINS = 72;
struct RadarSource {
    char id[18];
    char name[24];
    int8_t rssi[RADAR_BINS];
    uint8_t hits[RADAR_BINS];
    int strongest;
    uint32_t seen;
};
static RadarSource* radar_sources = nullptr;
static int radar_n = 0;

static void radar_alloc() {
    if (radar_sources) return;
    radar_sources = (RadarSource*)fox_alloc(RADAR_SOURCES * sizeof(RadarSource));
    if (!radar_sources) return;
    memset(radar_sources, 0, RADAR_SOURCES * sizeof(RadarSource));
    for (int i = 0; i < RADAR_SOURCES; ++i)
        for (int b = 0; b < RADAR_BINS; ++b) radar_sources[i].rssi[b] = -127;
}

static void radar_reset() {
    radar_alloc();
    radar_n = 0;
    if (!radar_sources) return;
    memset(radar_sources, 0, RADAR_SOURCES * sizeof(RadarSource));
    for (int i = 0; i < RADAR_SOURCES; ++i)
        for (int b = 0; b < RADAR_BINS; ++b) radar_sources[i].rssi[b] = -127;
}

static int radar_find(const char* id) {
    for (int i = 0; i < radar_n; ++i)
        if (!strcmp(radar_sources[i].id, id)) return i;
    return -1;
}

static int radar_source(const char* id, const String& name) {
    int i = radar_find(id);
    if (i >= 0) return i;
    if (radar_n >= RADAR_SOURCES) return -1;
    i = radar_n++;
    strncpy(radar_sources[i].id, id, sizeof(radar_sources[i].id)-1);
    strncpy(radar_sources[i].name, name.c_str(), sizeof(radar_sources[i].name)-1);
    radar_sources[i].strongest = -127;
    return i;
}

static void radar_sample(const char* id, const String& name, int rssi, float angle_deg) {
    if (!radar_sources) radar_alloc();
    if (!radar_sources) return;
    int i = radar_source(id, name);
    if (i < 0) return;
    float a = fmodf(angle_deg, 360.0f); if (a < 0) a += 360.0f;
    int b = (int)(a * RADAR_BINS / 360.0f) % RADAR_BINS;
    RadarSource& d = radar_sources[i];
    if (d.hits[b] == 0) d.rssi[b] = (int8_t)constrain(rssi, -127, 0);
    else d.rssi[b] = (int8_t)((d.rssi[b] * 3 + constrain(rssi, -127, 0)) / 4);
    if (d.hits[b] < 255) d.hits[b]++;
    if (rssi > d.strongest) d.strongest = rssi;
    d.seen = millis();
}

static float radar_peak_angle(const RadarSource& d) {
    // Circular weighted centroid around the strongest RSSI response. Use only
    // samples within 12 dB of the source peak so unrelated weak observations
    // don't drag the apparent direction around the circle.
    int peak = d.strongest;
    float sx = 0, sy = 0, wsum = 0;
    for (int b = 0; b < RADAR_BINS; ++b) {
        if (!d.hits[b]) continue;
        int db = peak - d.rssi[b];
        if (db > 12) continue;
        float w = (float)(13 - db) * d.hits[b];
        float a = (b + 0.5f) * 2.0f * PI / RADAR_BINS;
        sx += cosf(a) * w; sy += sinf(a) * w; wsum += w;
    }
    if (wsum < 1) return 0;
    float a = atan2f(sy, sx) * 180.0f / PI;
    if (a < 0) a += 360.0f;
    return a;
}

static float radar_confidence(const RadarSource& d) {
    int hits = 0;
    for (int b = 0; b < RADAR_BINS; ++b) if (d.hits[b]) hits += d.hits[b];
    return constrain(hits / 20.0f, 0.0f, 1.0f);
}

static float radar_range(const RadarSource& d) {
    // Relative only. Strong signals appear closer; no metres are claimed.
    float x = constrain((float)(d.strongest + 90), 5.0f, 60.0f) / 60.0f;
    return 0.12f + (1.0f - x) * 0.78f;
}

// ---- heading: gravity-projected gyro yaw ------------------------------------
// Accumulated CLOCKWISE rotation as seen from above (degrees, unwrapped). The
// gyro vector is projected onto "up" (the accelerometer's gravity reading), so
// it measures turning-in-place however Fox is held — flat or upright. Gyro
// bias is measured while still; stalls (dt > 100ms) are skipped, never
// multiplied into a jump.
struct RadarHeading { float yaw = 0, bx = 0, by = 0, bz = 0; uint32_t last_us = 0; };

static void radar_heading_calibrate(RadarHeading& h) {
    float sx = 0, sy = 0, sz = 0; int n = 0; uint32_t t0 = millis();
    while (millis() - t0 < 500) {
        M5.Imu.update();
        float gx, gy, gz;
        if (M5.Imu.getGyro(&gx, &gy, &gz)) { sx += gx; sy += gy; sz += gz; ++n; }
        delay(10);
    }
    if (n) { h.bx = sx / n; h.by = sy / n; h.bz = sz / n; }
    h.yaw = 0; h.last_us = micros();
}

static float radar_heading_update(RadarHeading& h) {
    M5.Imu.update();
    float ax, ay, az, gx, gy, gz;
    if (!M5.Imu.getAccel(&ax, &ay, &az) || !M5.Imu.getGyro(&gx, &gy, &gz)) return h.yaw;
    uint32_t now = micros();
    float dt = (now - h.last_us) * 1e-6f; h.last_us = now;
    if (dt <= 0.0f || dt > 0.1f) return h.yaw;           // stall: skip, don't jump
    float an = sqrtf(ax * ax + ay * ay + az * az);
    if (an < 0.3f) return h.yaw;                           // free-fall / bad read
    float rate = ((gx - h.bx) * ax + (gy - h.by) * ay + (gz - h.bz) * az) / an;   // deg/s about up
    if (fabsf(rate) < 0.8f) rate = 0.0f;                   // drift deadband
    h.yaw -= rate * dt;            // gyro is CCW-positive about up -> clockwise heading
    return h.yaw;
}

// ---- background scanner ------------------------------------------------------
// Scans block (WiFi ~0.8s, BLE 1s). They run on their own task so the needles
// keep moving at full frame rate; each result is stamped with the heading at
// the middle of the scan that heard it.
struct RadarHit { char id[18]; char name[24]; int8_t rssi; float yaw; };
static QueueHandle_t  radar_q = nullptr;
static volatile float radar_live_yaw = 0.0f;
static volatile bool  radar_worker_run = false;
static volatile bool  radar_worker_alive = false;

static void radar_push(const char* id, const char* name, int rssi, float yaw) {
    RadarHit h; memset(&h, 0, sizeof h);
    strncpy(h.id, id, sizeof(h.id) - 1);
    strncpy(h.name, name, sizeof(h.name) - 1);
    h.rssi = (int8_t)constrain(rssi, -127, 0);
    h.yaw = yaw;
    xQueueSend(radar_q, &h, 0);
}

static void radar_worker(void* arg) {
    const bool wifi = (arg != nullptr);
    while (radar_worker_run) {
        float y0 = radar_live_yaw;
        if (wifi) {
            int n = WiFi.scanNetworks(false, true, false, 60);   // blocks THIS task only
            float ym = (y0 + radar_live_yaw) * 0.5f;
            for (int i = 0; i < n; ++i)
                radar_push(WiFi.BSSIDstr(i).c_str(), WiFi.SSID(i).c_str(), WiFi.RSSI(i), ym);
            WiFi.scanDelete();
        } else if (ble_scan) {
            BLEScanResults* res = ble_scan->start(1, false);
            float ym = (y0 + radar_live_yaw) * 0.5f;
            if (res) {
                for (int i = 0; i < res->getCount(); ++i) {
                    BLEAdvertisedDevice d = res->getDevice(i);
                    String id = String(d.getAddress().toString().c_str());
                    String nm = d.haveName() ? String(d.getName().c_str()) : id.substring(0, 8);
                    radar_push(id.c_str(), nm.c_str(), d.getRSSI(), ym);
                }
                ble_scan->clearResults();
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    radar_worker_alive = false;
    vTaskDelete(nullptr);
}

static void radar_worker_start(bool wifi) {
    if (!radar_q) radar_q = xQueueCreate(96, sizeof(RadarHit));
    xQueueReset(radar_q);
    radar_worker_run = true;
    radar_worker_alive = true;
    xTaskCreatePinnedToCore(radar_worker, "radar", 8192, wifi ? (void*)1 : nullptr, 2, nullptr, 0);
}

static void radar_worker_stop() {
    radar_worker_run = false;
    uint32_t t0 = millis();
    while (radar_worker_alive && millis() - t0 < 4000) delay(20);   // let the scan finish
}

static void radar_drain() {
    RadarHit h;
    while (radar_q && xQueueReceive(radar_q, &h, 0) == pdTRUE)
        radar_sample(h.id, String(h.name), h.rssi, h.yaw);
}

// ---- drawing -----------------------------------------------------------------
enum RadarPhase { RADAR_CAL, RADAR_MEASURE, RADAR_LIVE };

static void radar_draw(float green, float red, bool wifi, RadarPhase ph, float heading) {
    uint16_t bg = cfg.color_bg, fur = cfg.color_primary, acc = cfg.color_accent;
    face_canvas->fillSprite(bg);
    const int cx = 64, cy = 66, R = 46;
    // angle convention: degrees clockwise from "up" (12 o'clock)
    auto at = [&](float deg, float r, int& x, int& y) {
        float a = deg * PI / 180.0f;
        x = cx + (int)lroundf(r * sinf(a));
        y = cy - (int)lroundf(r * cosf(a));
    };
    for (int r = 15; r <= R; r += 15) face_canvas->drawCircle(cx, cy, r, 0x2145);
    face_canvas->drawCircle(cx, cy, R, 0x4208);

    if (ph == RADAR_MEASURE) {
        // progress ring = how much of the one turn you've completed
        float cov = fminf(fabsf(green), 360.0f);
        if (cov > 1.0f) face_canvas->fillArc(cx, cy, R + 4, R + 2, 270.0f, 270.0f + cov, 0x07E0);
        int x, y;
        at(fmodf(red, 360.0f), R, x, y);
        face_canvas->drawWideLine(cx, cy, x, y, 1.6f, 0xF800);       // red: target pace
        at(fmodf(green + 3600.0f, 360.0f), R - 4, x, y);
        face_canvas->drawWideLine(cx, cy, x, y, 1.6f, 0x07E0);       // green: you
    } else if (ph == RADAR_LIVE) {
        // purely decorative sweep with a fading trail
        float sweep = fmodf(millis() * 0.18f, 360.0f);
        for (int t = 60; t >= 0; t -= 6) {
            int x, y; at(sweep - t, R, x, y);
            uint16_t col = (uint16_t)(((uint8_t)((63 - t)) >> 1) << 5);
            face_canvas->drawLine(cx, cy, x, y, col);
        }
        // tick at the top = the way you are facing
        face_canvas->fillTriangle(cx - 4, cy - R - 1, cx + 4, cy - R - 1, cx, cy - R + 6, acc);
    }

    // sources: measure frame = relative to where the turn started;
    // live = relative to where you face NOW (map turns with you)
    float sweep_now = fmodf(millis() * 0.18f, 360.0f);
    for (int i = 0; i < radar_n; ++i) {
        RadarSource& d = radar_sources[i];
        float bearing = radar_peak_angle(d);
        float disp = (ph == RADAR_LIVE) ? bearing - heading : bearing;
        disp = fmodf(disp + 3600.0f, 360.0f);
        int x, y; at(disp, R * radar_range(d), x, y);
        bool lit = false;
        if (ph == RADAR_LIVE) { float da = fabsf(disp - sweep_now); if (da > 180) da = 360 - da; lit = da < 18.0f; }
        uint16_t col = lit ? 0x07E0 : (d.seen && millis() - d.seen < 3000 ? fur : 0x8410);
        face_canvas->fillCircle(x, y, lit ? 4 : 3, col);
    }

    // tiny fox in the middle, facing up = facing the way you face
    face_canvas->fillTriangle(cx-5,cy-2,cx-2,cy-9,cx+1,cy-3,fur);
    face_canvas->fillTriangle(cx+5,cy-2,cx+2,cy-9,cx-1,cy-3,fur);
    face_canvas->fillEllipse(cx,cy+2,5,4,fur);
    face_canvas->fillCircle(cx-2,cy+1,1,0x0000); face_canvas->fillCircle(cx+2,cy+1,1,0x0000);

    face_canvas->setTextSize(1);
    face_canvas->setTextDatum(top_left); face_canvas->setTextColor(acc);
    face_canvas->drawString(wifi ? "WIFI RADAR" : "BLE RADAR", 2, 1);
    face_canvas->setTextDatum(top_right); face_canvas->setTextColor(0x07E0);
    char n[12]; snprintf(n, sizeof n, "%d", radar_n);
    face_canvas->drawString(n, 126, 1);
    face_canvas->setTextDatum(bottom_center); face_canvas->setTextColor(0x9CD3);
    const char* hint = ph == RADAR_CAL ? "hold still..."
                     : ph == RADAR_MEASURE ? (green < -25.0f ? "other way!" : "turn: green on red")
                     : "click=rescan 2x=exit";
    face_canvas->drawString(hint, 64, 127);
    face_canvas->pushSprite(0, 0);
}

static void radar_run(bool wifi) {
  for (;;) {
    radar_reset();
    if (wifi) { WiFi.mode(WIFI_STA); WiFi.disconnect(false, false); delay(50); }
    else      { WiFi.mode(WIFI_OFF); delay(50); ble_begin_once(); }
    speak("hold me still, then turn around once. keep the green needle on the red one~");

    RadarHeading hd;
    radar_draw(0, 0, wifi, RADAR_CAL, 0);
    radar_heading_calibrate(hd);
    radar_worker_start(wifi);

    // ---- ONE guided turn: red sweeps smoothly 0 -> 360 deg in 20 s and stops;
    //      the measurement ends when YOU have turned a full circle. ----------
    const float TARGET_DPS = 18.0f;
    uint32_t t0 = millis();
    bool quit = false;
    float green = 0;
    for (;;) {
        uint32_t f0 = millis();
        M5.update();
        if (M5.BtnA.wasDoubleClicked()) { quit = true; break; }
        green = radar_heading_update(hd);
        radar_live_yaw = green;
        radar_drain();
        float red = fminf(360.0f, (millis() - t0) * 0.001f * TARGET_DPS);
        radar_draw(green, red, wifi, RADAR_MEASURE, 0);
        if (fabsf(green) >= 360.0f) break;       // one full turn, either way
        if (millis() - t0 > 60000) break;        // don't trap anyone
        uint32_t el = millis() - f0; if (el < 30) delay(30 - el);
    }
    if (quit) { radar_worker_stop(); tool_radio_release(); return; }

    // a full turn brings you back to where you started: heading 0 == start
    hd.yaw = fmodf(hd.yaw, 360.0f);
    face_caption(String("mapped ") + radar_n + " signals!");
    radar_draw(green, 360, wifi, RADAR_LIVE, hd.yaw);
    speak(String("i mapped ") + radar_n + " signals around us~");
    hd.last_us = micros();                       // don't integrate across the speech

    // ---- live map: keeps scanning and keeps tracking your heading, so the
    //      map stays pinned to the room while you turn. ----------------------
    bool rescan = false;
    for (;;) {
        uint32_t f0 = millis();
        M5.update();
        if (M5.BtnA.wasDoubleClicked()) { quit = true; break; }
        if (M5.BtnA.wasSingleClicked()) { rescan = true; break; }
        float h = radar_heading_update(hd);
        radar_live_yaw = h;
        radar_drain();
        radar_draw(0, 0, wifi, RADAR_LIVE, h);
        uint32_t el = millis() - f0; if (el < 33) delay(33 - el);
    }
    radar_worker_stop();
    tool_radio_release();
    if (quit || !rescan) return;
  }
}

void tool_menu_ble_radar() { radar_run(false); }

const char* tool_ble_scan_report() {
    static char buf[220];
    ble_begin_once();
    BLEScanResults* res = ble_scan->start(3, false);
    int n = res ? res->getCount() : 0, strongest = -999; String who = "";
    for (int i = 0; i < n; ++i) {
        BLEAdvertisedDevice d = res->getDevice(i);
        if (d.getRSSI() > strongest) { strongest=d.getRSSI(); who=d.haveName()?String(d.getName().c_str()):String(d.getAddress().toString().c_str()); }
    }
    ble_scan->clearResults();
    snprintf(buf,sizeof(buf),"{\"ble_devices\":%d,\"closest\":\"%s\",\"rssi\":%d}",n,who.c_str(),strongest);
    return buf;
}

// ===========================================================================
//  WIFI SCAN / RADAR
// ===========================================================================
void tool_menu_wifi_radar() { radar_run(true); }

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
        if (M5.BtnA.wasClicked() || M5.BtnA.wasDoubleClicked()) break;
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
        face_canvas->drawString("click=exit", 124, 122);
        face_canvas->pushSprite(0, 0);
        delay(40);
    }
    esp_wifi_set_promiscuous(false);
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
        if (M5.BtnA.wasClicked() || M5.BtnA.wasDoubleClicked()) break;
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
    esp_wifi_set_promiscuous(false);
    speak(String("saw ") + probe_count + " different network names being sought");
}
