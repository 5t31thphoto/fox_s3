// fox_net.inc — online reports the fox can fetch and speak. Included by
// fox_main.cpp. All are read-only HTTP(S) GETs; each degrades to a friendly
// "can't reach that right now" when offline.
#pragma once
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>

extern FoxConfig cfg;

// ---- ordinary weather (wttr.in) --------------------------------------------
String net_weather() {
    if (cfg.weather.isEmpty() || !wifi_connect())
        return "i can't check the weather right now";
    WiFiClientSecure c; c.setInsecure(); c.setHandshakeTimeout(10);
    HTTPClient h; h.setConnectTimeout(6000);
    if (!h.begin(c, "https://wttr.in/" + cfg.weather + "?format=%C+%t"))
        return "weather is being shy";
    String out = (h.GET() == 200) ? h.getString() : String("");
    h.end(); out.trim();
    return out.length() ? ("it's " + out) : "weather is being shy";
}

// ---- bitcoin spot price (keyless) -------------------------------------------
// Coinbase public spot price; CoinGecko as a fallback.
String net_bitcoin_price() {
    if (!wifi_connect()) return "i can't check prices without wifi";
    float usd = 0;
    {
        WiFiClientSecure c; c.setInsecure(); c.setHandshakeTimeout(10); HTTPClient h; h.setConnectTimeout(6000);
        if (h.begin(c, "https://api.coinbase.com/v2/prices/BTC-USD/spot")) {
            h.setTimeout(8000);
            if (h.GET() == 200) { JsonDocument d; if (!deserializeJson(d, h.getString())) usd = d["data"]["amount"].as<String>().toFloat(); }
            h.end();
        }
    }
    if (usd <= 0) {
        WiFiClientSecure c; c.setInsecure(); c.setHandshakeTimeout(10); HTTPClient h; h.setConnectTimeout(6000);
        if (h.begin(c, "https://api.coingecko.com/api/v3/simple/price?ids=bitcoin&vs_currencies=usd")) {
            h.setTimeout(8000);
            if (h.GET() == 200) { JsonDocument d; if (!deserializeJson(d, h.getString())) usd = d["bitcoin"]["usd"].as<float>(); }
            h.end();
        }
    }
    if (usd <= 0) return "the coin markets aren't answering right now";
    char b[64]; snprintf(b, sizeof b, "bitcoin is about %ld dollars", (long)lroundf(usd));
    return String(b);
}

// ---- NOAA SWPC Kp (observed/estimated + 3-day forecast) ----------------------
// https://services.swpc.noaa.gov/products/noaa-planetary-k-index-forecast.json
// Current format (verified 2026-10): an array of OBJECTS
//   {"time_tag":"...","kp":2.33,"observed":"observed|estimated|predicted","noaa_scale":"G1"|null}
// Older SWPC products were arrays of arrays with a header row; both are accepted.
struct KpOutlook { bool ok = false; float now = -1; float peak = -1; String peak_scale; };

static float kp_of(JsonVariant row, String* kind, String* scale) {
    if (row.is<JsonObject>()) {
        if (kind)  *kind  = row["observed"].as<String>();
        if (scale) *scale = row["noaa_scale"].isNull() ? String("") : row["noaa_scale"].as<String>();
        if (!row["kp"].isNull()) return row["kp"].as<float>();
        if (!row["Kp"].isNull()) return row["Kp"].as<float>();
        return -1;
    }
    if (row.is<JsonArray>()) {               // legacy: [time_tag, kp, observed, noaa_scale]
        JsonArray r = row.as<JsonArray>();
        String v = r[1].as<String>();
        if (!v.length() || !(isdigit((unsigned char)v[0]) || v[0] == '.')) return -1;   // header row
        if (kind)  *kind  = r[2].as<String>();
        if (scale) *scale = r[3].isNull() ? String("") : r[3].as<String>();
        return v.toFloat();
    }
    return -1;
}

static KpOutlook noaa_kp_outlook() {
    KpOutlook k;
    if (!wifi_connect()) return k;
    WiFiClientSecure c; c.setInsecure(); c.setHandshakeTimeout(10);
    HTTPClient h; h.setConnectTimeout(6000);
    if (!h.begin(c, "https://services.swpc.noaa.gov/products/noaa-planetary-k-index-forecast.json")) return k;
    h.setTimeout(10000);
    int code = h.GET();
    String body = code == 200 ? h.getString() : String("");
    h.end();
    if (!body.length()) { Serial.printf("FOX: NOAA Kp HTTP %d\n", code); return k; }
    JsonDocument d;
    if (deserializeJson(d, body) || !d.is<JsonArray>()) return k;
    int predicted_seen = 0;
    for (JsonVariant row : d.as<JsonArray>()) {
        String kind, scale;
        float kp = kp_of(row, &kind, &scale);
        if (kp < 0) continue;
        if (kind == "predicted") {
            if (predicted_seen++ < 8) {                    // next 8 x 3h = 24 hours
                if (kp > k.peak) { k.peak = kp; k.peak_scale = scale; }
            }
        } else {
            k.now = kp;                                    // latest observed/estimated
        }
    }
    k.ok = k.now >= 0;
    return k;
}

static String kp_words(float kp) {
    if (kp >= 9) return "an extreme G5 storm";
    if (kp >= 8) return "a severe G4 storm";
    if (kp >= 7) return "a strong G3 storm";
    if (kp >= 6) return "a moderate G2 storm";
    if (kp >= 5) return "a minor G1 storm";
    if (kp >= 4) return "unsettled and active";
    return "calm and quiet";
}

static String kp_str(float kp) { char b[8]; snprintf(b, sizeof b, "%.1f", kp); return String(b); }

String net_space_weather() {
    if (!wifi_connect()) return "i can't reach space weather right now";
    KpOutlook k = noaa_kp_outlook();
    if (!k.ok) return "noaa's space weather isn't answering right now";
    String s = "the planetary k index is " + kp_str(k.now) + ", " + kp_words(k.now) + ".";
    if (k.peak >= 0)
        s += " in the next day it may reach " + kp_str(k.peak) + ", " + kp_words(k.peak) + ".";
    return s;
}

// ---- where are we? (lat/lon for the aurora model) ----------------------------
// From the configured weather location via wttr.in (the same service that
// already resolves it for the weather report); if none is set, IP geolocation.
static bool  s_loc_ok = false;
static float s_lat = 0, s_lon = 0;
static String s_loc_name, s_loc_query;

static bool fox_location() {
    if (s_loc_ok && s_loc_query == cfg.weather) return true;
    s_loc_ok = false;
    if (!wifi_connect()) return false;
    if (cfg.weather.length()) {
        WiFiClientSecure c; c.setInsecure(); c.setHandshakeTimeout(10); HTTPClient h; h.setConnectTimeout(6000);
        if (h.begin(c, "https://wttr.in/" + cfg.weather + "?format=j1")) {
            h.setTimeout(12000);
            if (h.GET() == 200) {
                JsonDocument filter;
                filter["nearest_area"][0]["latitude"] = true;
                filter["nearest_area"][0]["longitude"] = true;
                filter["nearest_area"][0]["areaName"][0]["value"] = true;
                JsonDocument d;
                if (!deserializeJson(d, h.getString(), DeserializationOption::Filter(filter))) {
                    JsonVariant a = d["nearest_area"][0];
                    if (!a["latitude"].isNull()) {
                        s_lat = a["latitude"].as<String>().toFloat();
                        s_lon = a["longitude"].as<String>().toFloat();
                        s_loc_name = a["areaName"][0]["value"].as<String>();
                        s_loc_ok = true;
                    }
                }
            }
            h.end();
        }
    }
    if (!s_loc_ok) {                                   // fallback: IP geolocation
        WiFiClient c; HTTPClient h; h.setConnectTimeout(6000);
        if (h.begin(c, "http://ip-api.com/json/?fields=status,lat,lon,city")) {
            h.setTimeout(8000);
            if (h.GET() == 200) {
                JsonDocument d;
                if (!deserializeJson(d, h.getString()) && d["status"] == "success") {
                    s_lat = d["lat"].as<float>(); s_lon = d["lon"].as<float>();
                    s_loc_name = d["city"].as<String>(); s_loc_ok = true;
                }
            }
            h.end();
        }
    }
    if (s_loc_ok) {
        s_loc_query = cfg.weather;
        Serial.printf("FOX: location %s lat=%.2f lon=%.2f\n", s_loc_name.c_str(), s_lat, s_lon);
    }
    return s_loc_ok;
}

// ---- aurora: NOAA OVATION-Prime 30-minute forecast at YOUR location ----------
// https://services.swpc.noaa.gov/json/ovation_aurora_latest.json
// {"Observation Time":..,"Forecast Time":..,"coordinates":[[lon,lat,prob%],...]}
// 360 x 181 grid, lon 0..359, lat -90..90, ~900 KB. Too big to hold, so it is
// stream-parsed: only the cells at your location and poleward (within view on
// the horizon) are kept.
struct AuroraHere { bool ok = false; int here = 0; int view = 0; };

static AuroraHere noaa_ovation_at(float lat, float lon) {
    AuroraHere r;
    const int tlon = ((int)lroundf(lon) % 360 + 360) % 360;
    const int tlat = (int)lroundf(lat);
    const int pole = lat >= 0 ? 1 : -1;                // look toward your pole
    WiFiClientSecure c; c.setInsecure(); c.setHandshakeTimeout(10); HTTPClient h; h.setConnectTimeout(6000);
    h.useHTTP10(true);                                  // plain (non-chunked) body for streaming
    if (!h.begin(c, "https://services.swpc.noaa.gov/json/ovation_aurora_latest.json")) return r;
    h.setTimeout(15000);
    int code = h.GET();
    if (code != 200) { Serial.printf("FOX: OVATION HTTP %d\n", code); h.end(); return r; }
    WiFiClient* s = h.getStreamPtr();
    int left = h.getSize();                             // -1 if unknown
    // tiny parser: wait for "coordinates", then read [a,b,c] triples
    const char* key = "\"coordinates\""; int km = 0; bool in_coords = false;
    bool in_triple = false; int idx = 0; char num[16]; int nl = 0; float v[3] = {0, 0, 0};
    uint8_t buf[512]; uint32_t last_data = millis(); long cells = 0;
    while (millis() - last_data < 8000 && (left > 0 || left == -1)) {
        int av = s->available();
        if (av <= 0) { if (!h.connected() && !s->available()) break; delay(2); continue; }
        int got = s->readBytes(buf, av > (int)sizeof buf ? sizeof buf : av);
        if (got <= 0) continue;
        last_data = millis();
        if (left > 0) left -= got;
        for (int i = 0; i < got; ++i) {
            char ch = (char)buf[i];
            if (!in_coords) {
                km = (ch == key[km]) ? km + 1 : (ch == key[0] ? 1 : 0);
                if (key[km] == 0) in_coords = true;
                continue;
            }
            if (!in_triple) { if (ch == '[' ) { in_triple = true; idx = 0; nl = 0; } continue; }
            if ((ch >= '0' && ch <= '9') || ch == '-' || ch == '.') { if (nl < 15) num[nl++] = ch; continue; }
            if (ch == ',' || ch == ']') {
                if (nl && idx < 3) { num[nl] = 0; v[idx++] = atof(num); }
                nl = 0;
                if (ch == ']') {
                    in_triple = false;
                    if (idx == 3) {
                        ++cells;
                        int lo = (int)v[0], la = (int)v[1], p = (int)v[2];
                        int dlo = abs(lo - tlon); if (dlo > 180) dlo = 360 - dlo;
                        if (lo == tlon && la == tlat) r.here = p;
                        int dla = (la - tlat) * pole;          // degrees poleward of you
                        if (dlo <= 2 && dla >= 0 && dla <= 8 && p > r.view) r.view = p;
                    }
                }
            }
        }
    }
    h.end();
    r.ok = cells > 60000;                               // full 360x181 grid parsed
    Serial.printf("FOX: OVATION cells=%ld here=%d view=%d\n", cells, r.here, r.view);
    return r;
}

String net_aurora() {
    if (!wifi_connect()) return "i can't check the aurora without wifi";
    face_caption("checking noaa's aurora forecast...");
    if (!fox_location()) return "i couldn't figure out where we are for the aurora forecast";
    AuroraHere a = noaa_ovation_at(s_lat, s_lon);
    KpOutlook k = noaa_kp_outlook();
    if (!a.ok) return "noaa's aurora forecast isn't answering right now";
    String where = s_loc_name.length() ? s_loc_name : String("here");
    String s;
    if (a.here >= 30)      s = "aurora alert! noaa gives " + where + " a " + String(a.here) + " percent chance overhead right now.";
    else if (a.view >= 20) s = "maybe! low on the horizon toward the pole there's up to a " + String(a.view) + " percent chance from " + where + ".";
    else if (a.view >= 5)  s = "slim. only about " + String(a.view) + " percent near " + where + " right now.";
    else                   s = "no aurora near " + where + " right now.";
    if (k.ok && k.peak >= 5) s += " but a storm is forecast, kp up to " + kp_str(k.peak) + ", so check again tonight.";
    else if (k.ok)           s += " kp is " + kp_str(k.now) + ".";
    struct tm tmv;
    if (getLocalTime(&tmv, 50) && tmv.tm_hour >= 7 && tmv.tm_hour < 18) s += " it's daytime though.";
    return s;
}
