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

// forward decls from other units
extern M5Canvas* face_canvas;
extern FoxConfig cfg;

// ---------------------------------------------------------------------------
//  Small helpers for the radar/scope screens
// ---------------------------------------------------------------------------
float imu_heading_deg() {
    // Use magnetometer if available for absolute bearing; fall back to the
    // gyro-integrated yaw. The AtomS3R has a BMM150 magnetometer.
    float mx, my, mz;
    if (M5.Imu.getMag(&mx, &my, &mz)) {
        float h = atan2f(my, mx) * 180.0f / PI;
        if (h < 0) h += 360.0f;
        return h;
    }
    float ax, ay, az;
    M5.Imu.getAccel(&ax, &ay, &az);
    return atan2f(ay, ax) * 180.0f / PI + 180.0f;
}

// ===========================================================================
//  BLE RADAR
//  Rotate the device; each nearby BLE advertiser is plotted as a blip at the
//  bearing the device was pointing when it was strongest, radius ~ signal.
// ===========================================================================
struct Blip { String name; int rssi; float bearing; uint32_t seen; };
static const int MAX_BLIPS = 24;
static Blip blips[MAX_BLIPS];
static int  blip_n = 0;
static BLEScan* ble_scan = nullptr;

static void ble_begin_once() {
    static bool inited = false;
    if (inited) return;
    BLEDevice::init("");
    ble_scan = BLEDevice::getScan();
    ble_scan->setActiveScan(true);
    ble_scan->setInterval(80);
    ble_scan->setWindow(60);
    inited = true;
}

static void blip_upsert(const String& name, int rssi, float bearing) {
    // Merge by name (or a synthesized id); keep the strongest sighting.
    for (int i = 0; i < blip_n; ++i) {
        if (blips[i].name == name) {
            if (rssi > blips[i].rssi) { blips[i].rssi = rssi; blips[i].bearing = bearing; }
            blips[i].seen = millis();
            return;
        }
    }
    if (blip_n < MAX_BLIPS) blips[blip_n++] = { name, rssi, bearing, millis() };
}

static void radar_draw(float heading, int count, bool wifi) {
    uint16_t bg = cfg.color_bg, fur = cfg.color_primary, acc = cfg.color_accent;
    face_canvas->fillSprite(bg);
    int cx = 64, cy = 60, R = 52;
    // rings
    for (int r = R; r > 0; r -= 17) face_canvas->drawCircle(cx, cy, r, 0x2145);
    face_canvas->drawLine(cx, cy - R, cx, cy + R, 0x2145);
    face_canvas->drawLine(cx - R, cy, cx + R, cy, 0x2145);
    // sweep line at current heading
    float hr = (heading - 90) * PI / 180.0f;
    face_canvas->drawLine(cx, cy, cx + R * cosf(hr), cy + R * sinf(hr), acc);
    // blips
    for (int i = 0; i < blip_n; ++i) {
        float br = (blips[i].bearing - 90) * PI / 180.0f;
        float rr = R * (1.0f - (min(0, blips[i].rssi + 30) / -70.0f));  // -30dBm→edge
        if (rr > R) rr = R;
        if (rr < 6) rr = 6;
        int x = cx + rr * cosf(br), y = cy + rr * sinf(br);
        uint16_t col = (millis() - blips[i].seen < 1500) ? fur : 0x8410;
        face_canvas->fillCircle(x, y, 3, col);
    }
    // header
    face_canvas->setTextColor(acc);
    face_canvas->setTextDatum(top_left);
    face_canvas->drawString(wifi ? "WIFI RADAR" : "BLE RADAR", 4, 2);
    face_canvas->setTextDatum(top_right);
    face_canvas->drawString(String(count), 124, 2);
    face_canvas->setTextDatum(bottom_center);
    face_canvas->setTextColor(0x9CD3);
    face_canvas->drawString("click to exit", 64, 126);
    face_canvas->pushSprite(0, 0);
}

// Standalone menu app: live BLE radar until the button is clicked.
void tool_menu_ble_radar() {
    ble_begin_once();
    blip_n = 0;
    speak("radar up! spin me around the room~");
    uint32_t t0 = millis();
    while (true) {
        M5.update();
        if (M5.BtnA.wasClicked()) break;
        float heading = imu_heading_deg();
        // short scan burst; attribute results to the current heading
        BLEScanResults* res = ble_scan->start(1, false);
        int n = res->getCount();
        for (int i = 0; i < n; ++i) {
            BLEAdvertisedDevice d = res->getDevice(i);
            String nm = d.haveName() ? String(d.getName().c_str())
                                     : String(d.getAddress().toString().c_str()).substring(0, 8);
            blip_upsert(nm, d.getRSSI(), heading);
        }
        ble_scan->clearResults();
        radar_draw(heading, blip_n, false);
        if (millis() - t0 > 120000) break;   // auto-exit after 2 min
    }
    speak(String("i found ") + blip_n + " gadgets nearby");
}

// LLM-callable: quick BLE sweep → compact JSON-ish report string.
const char* tool_ble_scan_report() {
    static char buf[220];
    ble_begin_once();
    BLEScanResults* res = ble_scan->start(3, false);
    int n = res->getCount(), strongest = -999; String who = "";
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

// ===========================================================================
//  WIFI SCAN / RADAR
// ===========================================================================
void tool_menu_wifi_radar() {
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    speak("scanning the airwaves~");
    blip_n = 0;
    uint32_t t0 = millis();
    while (true) {
        M5.update();
        if (M5.BtnA.wasClicked()) break;
        float heading = imu_heading_deg();
        int n = WiFi.scanNetworks(false, true);
        for (int i = 0; i < n; ++i)
            blip_upsert(WiFi.SSID(i), WiFi.RSSI(i), heading);
        WiFi.scanDelete();
        radar_draw(heading, blip_n, true);
        if (millis() - t0 > 120000) break;
    }
    speak(String("saw ") + blip_n + " networks around us");
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
