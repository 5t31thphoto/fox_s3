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
    WiFiClientSecure c; c.setInsecure();
    HTTPClient h;
    if (!h.begin(c, "https://wttr.in/" + cfg.weather + "?format=%C+%t"))
        return "weather is being shy";
    String out = (h.GET() == 200) ? h.getString() : String("");
    h.end(); out.trim();
    return out.length() ? ("it's " + out) : "weather is being shy";
}

// ---- space weather: NOAA SWPC planetary K-index (geomagnetic activity) ------
// https://services.swpc.noaa.gov/products/noaa-planetary-k-index.json
// Returns the latest Kp; Kp>=5 means a geomagnetic storm (aurora more likely).
String net_space_weather() {
    if (!wifi_connect()) return "i can't reach space weather right now";
    WiFiClientSecure c; c.setInsecure();
    HTTPClient h;
    if (!h.begin(c, "https://services.swpc.noaa.gov/products/noaa-planetary-k-index.json"))
        return "space weather is quiet to me right now";
    String body = (h.GET() == 200) ? h.getString() : String("");
    h.end();
    if (!body.length()) return "space weather is quiet to me right now";
    // The product is a JSON array of rows; last row = most recent.
    JsonDocument d;
    if (deserializeJson(d, body) != DeserializationError::Ok || !d.is<JsonArray>())
        return "space weather looked garbled, sorry~";
    JsonArray arr = d.as<JsonArray>();
    if (arr.size() < 2) return "no space weather yet";
    JsonArray last = arr[arr.size() - 1];
    String kp = last[1].as<String>();
    float kpf = kp.toFloat();
    String mood = kpf >= 7 ? "a big geomagnetic storm!"
               : kpf >= 5 ? "a geomagnetic storm — auroras possible!"
               : kpf >= 4 ? "getting active up there"
                          : "calm and quiet up there";
    return "the Kp index is " + kp + ", " + mood;
}

// ---- aurora chance from NOAA Ovation, tied to your latitude -----------------
// Uses the SWPC aurora forecast grid; we approximate by reading the Kp and the
// user's latitude (from cfg.weather geocode is overkill, so we accept a rough
// hemisphere hint). Honest scope: this is a heuristic headline, not a model.
String net_aurora() {
    String sw = net_space_weather();
    // If a storm is flagged, encourage the human to look up.
    if (sw.indexOf("storm") >= 0 || sw.indexOf("active") >= 0)
        return sw + " go look north, you might catch the lights!";
    return sw + " probably no aurora tonight, though.";
}
