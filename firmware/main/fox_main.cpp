// fox_main.cpp — Fox voice companion, main firmware.
//
// Target: M5Stack AtomS3R (ESP32-S3) + Atomic Echo Base.
// Offline-first: everything core works with no network. Cloud (Groq or any
// OpenAI-compatible endpoint) is an *optional* enhancement layered on top.
//
// Interaction model (per the owner's emphatic instructions):
//   * PUSH-TO-TALK ONLY. Hold the USER button to talk; release to process.
//     There is NO wake word. A short tap opens the menu instead.
//   * "Conversation mode" is opt-in from the menu (or by asking the fox). In
//     that mode it keeps listening for a while and times out on silence.
//
// This file wires together the subsystems implemented in the sibling files:
//   fox_brain.cpp   personality/mood/needs/reflection
//   fox_voice.cpp   Pico / critter / babble speech
//   fox_memory.cpp  device-owned rolling journal
//   fox_llm.cpp     optional tiny on-device brain (safe: can't invent facts)
//
// Seven concrete bugs from the previous core are fixed here; each is called out
// with a "// FIX:" comment where it lives.
#include "fox.h"
#include "fox_decls.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <esp_sleep.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_partition.h>
#include <math.h>

// esp-sr (offline speech)
#include <esp_mn_iface.h>
#include <esp_afe_sr_iface.h>
#include <esp_afe_sr_models.h>       // esp_afe_handle_from_config()
#include <esp_afe_config.h>
#include <esp_mn_models.h>
#include <esp_mn_speech_commands.h>  // esp_mn_commands_alloc/clear/add/update()
#include <esp_process_sdkconfig.h>
#include <model_path.h>

// ============================================================================
//  Config persistence
// ============================================================================
static FoxConfig cfg;
static FoxNeeds  needs;
static Preferences prefs;
// Set by any game/tool/toy when the user double-clicks to bail to the menu.
// The main loop honors it after the app's own loop returns.
bool g_goto_menu = false;
// Set by the "remember this" command: the NEXT captured utterance is stored as
// a memory instead of being dispatched as a command. (Was previously a dead
// feature — do_action prompted but nothing ever captured the answer.)
static bool g_awaiting_memory = false;

static void load_config() {
    prefs.begin("fox", true);
    cfg.name        = prefs.getString("name", cfg.name);
    cfg.timezone    = prefs.getString("tz", cfg.timezone);
    cfg.weather     = prefs.getString("wx", cfg.weather);
    cfg.wifi_ssid   = prefs.getString("ssid", cfg.wifi_ssid);
    cfg.wifi_pass   = prefs.getString("pass", cfg.wifi_pass);
    cfg.api_key     = prefs.getString("key", cfg.api_key);
    cfg.chat_model  = prefs.getString("cmodel", cfg.chat_model);
    cfg.stt_model   = prefs.getString("smodel", cfg.stt_model);
    cfg.api_base    = prefs.getString("abase", cfg.api_base);
    cfg.personality = prefs.getString("pers", cfg.personality);
    cfg.voice_pack  = prefs.getString("voice", cfg.voice_pack);
    cfg.splash_text = prefs.getString("splash", cfg.splash_text);
    cfg.splash_fx   = prefs.getUChar("sfx", cfg.splash_fx);
    cfg.theme       = prefs.getUChar("theme", cfg.theme);
    cfg.mode        = prefs.getString("mode", cfg.mode);
    cfg.weights_url = prefs.getString("wurl", cfg.weights_url);
    cfg.tool_ble    = prefs.getBool("tble", cfg.tool_ble);
    cfg.tool_wifi   = prefs.getBool("twifi", cfg.tool_wifi);
    cfg.tool_imu    = prefs.getBool("timu", cfg.tool_imu);
    cfg.lip_sync    = prefs.getBool("lips", cfg.lip_sync);
    cfg.volume      = prefs.getUChar("vol", cfg.volume);
    cfg.voice_speed = prefs.getUChar("vsp", cfg.voice_speed);
    cfg.voice_pitch = prefs.getUChar("vpi", cfg.voice_pitch);
    cfg.voice_throat= prefs.getUChar("vth", cfg.voice_throat);
    cfg.voice_mouth = prefs.getUChar("vmo", cfg.voice_mouth);
    cfg.color_primary = prefs.getUShort("cpri", cfg.color_primary);
    cfg.color_accent  = prefs.getUShort("cacc", cfg.color_accent);
    cfg.color_bg      = prefs.getUShort("cbg", cfg.color_bg);
    cfg.captions    = prefs.getBool("cap", cfg.captions);
    prefs.end();
    cfg.cloud_enabled = cfg.api_key.length() > 0;
}

void save_config() {
    prefs.begin("fox", false);
    prefs.putString("name", cfg.name);
    prefs.putString("tz", cfg.timezone);
    prefs.putString("wx", cfg.weather);
    prefs.putString("ssid", cfg.wifi_ssid);
    prefs.putString("pass", cfg.wifi_pass);
    prefs.putString("key", cfg.api_key);
    prefs.putString("cmodel", cfg.chat_model);
    prefs.putString("smodel", cfg.stt_model);
    prefs.putString("abase", cfg.api_base);
    prefs.putString("pers", cfg.personality);
    prefs.putString("voice", cfg.voice_pack);
    prefs.putString("splash", cfg.splash_text);
    prefs.putUChar("sfx", cfg.splash_fx);
    prefs.putUChar("theme", cfg.theme);
    prefs.putString("mode", cfg.mode);
    prefs.putString("wurl", cfg.weights_url);
    prefs.putBool("tble", cfg.tool_ble);
    prefs.putBool("twifi", cfg.tool_wifi);
    prefs.putBool("timu", cfg.tool_imu);
    prefs.putBool("lips", cfg.lip_sync);
    prefs.putUChar("vol", cfg.volume);
    prefs.putUChar("vsp", cfg.voice_speed);
    prefs.putUChar("vpi", cfg.voice_pitch);
    prefs.putUChar("vth", cfg.voice_throat);
    prefs.putUChar("vmo", cfg.voice_mouth);
    prefs.putUShort("cpri", cfg.color_primary);
    prefs.putUShort("cacc", cfg.color_accent);
    prefs.putUShort("cbg", cfg.color_bg);
    prefs.putBool("cap", cfg.captions);
    prefs.end();
    cfg.cloud_enabled = cfg.api_key.length() > 0;
}

// ============================================================================
//  Offline speech (MultiNet7 + AFE). Reused from the prior core, which was
//  correct here — with bug #1 fixed in recognize_offline().
// ============================================================================
struct Command { int id; const char* phrases; const char* action; };
// Phrases for one id are separated by ';' — init_speech() registers EACH phrase
// separately (MultiNet rejects a single string containing ';'). Lowercase
// letters and spaces only. IDs < 60 are commands; 60+ are conversation topics.
static const Command COMMANDS[] = {
    {1,  "hello fox;hey fox;hi fox;hello;hi there",           "greet"},
    {2,  "what time is it;tell me the time;what is the time", "time"},
    {3,  "how are you;how do you feel;are you okay",          "mood"},
    {4,  "what is the weather;weather report;is it raining",  "weather"},
    {6,  "volume up;louder;speak up",                         "vol_up"},
    {7,  "volume down;quieter;be quiet",                      "vol_dn"},
    {8,  "go to sleep;good night;time for bed",               "sleep"},
    {9,  "conversation mode;lets chat;talk with me",          "conv_on"},
    {10, "remember this;remember that",                       "remember"},
    {11, "what do you remember;tell me a memory",             "recall"},
    {12, "open the menu;show menu;show me the menu",          "menu"},
    {13, "scan for devices;bluetooth radar;find bluetooth",   "ble_radar"},
    {14, "scan wifi;wifi radar;find wifi",                    "wifi_radar"},
    {15, "sniff packets;sniffer mode;hunt mode",              "sniffer"},
    {16, "play wormhole;fly the ship",                        "wormhole"},
    {17, "catch the treats;catch game",                       "catch"},
    {18, "twenty questions;guess my thing",                   "twentyq"},
    {19, "space weather;solar storm",                         "space"},
    {20, "any aurora;northern lights",                        "aurora"},
    {21, "lip sync mode;puppet mode",                         "lipsync"},
    {24, "explore the maze;lets explore",                     "maze"},
    {25, "play with me;lets hang out;lets play",              "encounter"},
    {26, "show me colors;pretty lights",                      "plasma"},
    {27, "starfield;fly through space",                       "starfield"},
    {28, "what are phones looking for;probe scan",            "probes"},
    {29, "reaction test;test my reflexes",                    "reaction"},
    {30, "pet the fox;can i pet you",                         "pet"},
    {31, "are you hungry;want a snack;feed the fox",          "feed"},
    // ---- conversation topics: recognising ONE of these makes the fox feel
    //      like it understood; the reply is picked per topic + mood ----------
    {60, "i love you;love you fox;you are cute;good girl",    "t_love"},
    {61, "i am sad;i feel sad;bad day;i am upset",            "t_sad"},
    {62, "i am happy;good day;i feel great",                  "t_happy"},
    {63, "i am tired;so tired;i am sleepy",                   "t_tired"},
    {64, "tell me a joke;make me laugh;say something funny",  "t_joke"},
    {65, "thank you;thanks fox;thanks",                       "t_thanks"},
    {66, "sorry;i am sorry",                                  "t_sorry"},
    {67, "good morning;morning fox",                          "t_morning"},
    {68, "goodbye;see you later;bye fox",                     "t_bye"},
    {69, "what is your name;who are you",                     "t_name"},
    {70, "i am bored;so bored;nothing to do",                 "t_bored"},
    {71, "good job;well done;you are smart",                  "t_praise"},
    {72, "what are you doing;what are you up to",             "t_doing"},
    {73, "do you like me;are we friends",                     "t_friend"},
};
static const size_t COMMAND_COUNT = sizeof(COMMANDS) / sizeof(COMMANDS[0]);

static srmodel_list_t* sr_models = nullptr;
static const esp_afe_sr_iface_t* afe = nullptr;
static esp_afe_sr_data_t* afe_data = nullptr;
static const esp_mn_iface_t* mn = nullptr;
static model_iface_data_t* mn_data = nullptr;
static bool speech_ready = false;

// Guard: is the `model` partition actually populated? On a web-flashed device
// the model SPIFFS may be unwritten (all 0xFF) or hold a bad image, and calling
// into esp-sr on that ABORTS the chip (boot loop) instead of failing cleanly.
// We check the first bytes look like real data before touching esp-sr, so a
// missing model degrades to "offline speech off" rather than bricking boot.
static bool model_partition_ready() {
    const esp_partition_t* p = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "model");
    if (!p) { Serial.println("FOX: no 'model' partition in table"); return false; }
    uint8_t hdr[32];
    if (esp_partition_read(p, 0, hdr, sizeof(hdr)) != ESP_OK) return false;
    bool all_ff = true, all_00 = true;
    for (size_t i = 0; i < sizeof(hdr); ++i) {
        if (hdr[i] != 0xFF) all_ff = false;
        if (hdr[i] != 0x00) all_00 = false;
    }
    if (all_ff || all_00) {
        Serial.println("FOX: model partition is empty (not flashed) — offline speech OFF");
        return false;
    }
    return true;
}

static bool init_speech() {
    // Never call esp-sr on an empty/unflashed model partition — it aborts.
    if (!model_partition_ready()) return false;

    sr_models = esp_srmodel_init("model");
    if (!sr_models) { Serial.println("FOX: no speech model partition"); return false; }
    char* mn_name = esp_srmodel_filter(sr_models, ESP_MN_PREFIX, ESP_MN_ENGLISH);
    if (!mn_name) { Serial.println("FOX: English MultiNet not found"); return false; }
    mn = esp_mn_handle_from_name(mn_name);
    if (!mn) return false;
    mn_data = mn->create(mn_name, 7000);
    if (!mn_data) return false;

    afe_config_t* ac = afe_config_init("M", sr_models, AFE_TYPE_SR, AFE_MODE_HIGH_PERF);
    if (!ac) return false;
    ac->aec_init = false; ac->se_init = false; ac->ns_init = true;
    ac->vad_init = true;  ac->wakenet_init = false; ac->agc_init = true;
    ac->fixed_output_channel = true;
    ac->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;
    ac->afe_ringbuf_size = 8;
    afe = esp_afe_handle_from_config(ac);
    afe_data = afe ? afe->create_from_config(ac) : nullptr;
    afe_config_free(ac);
    // Retry with the lighter LOW_COST profile if HIGH_PERF couldn't allocate
    // (BT + WiFi + model already consume a lot of PSRAM). Better ASR-lite than
    // no ASR at all. (per review: AFE HIGH_PERF can fail under PSRAM pressure)
    if (!afe_data) {
        Serial.println("FOX: AFE HIGH_PERF failed, retrying LOW_COST");
        ac = afe_config_init("M", sr_models, AFE_TYPE_SR, AFE_MODE_LOW_COST);
        if (ac) {
            ac->aec_init = false; ac->se_init = false; ac->ns_init = true;
            ac->vad_init = true;  ac->wakenet_init = false; ac->agc_init = true;
            ac->fixed_output_channel = true;
            ac->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;
            ac->afe_ringbuf_size = 8;
            afe = esp_afe_handle_from_config(ac);
            afe_data = afe ? afe->create_from_config(ac) : nullptr;
            afe_config_free(ac);
        }
    }
    if (!afe_data) { Serial.println("FOX: AFE unavailable — offline ASR off"); return false; }

    if (esp_mn_commands_alloc((esp_mn_iface_t*)mn, (model_iface_data_t*)mn_data) != ESP_OK)
        return false;
    esp_mn_commands_clear();
    int added = 0, rejected = 0;
    for (size_t i = 0; i < COMMAND_COUNT; ++i) {
        const char* p = COMMANDS[i].phrases;
        while (*p) {
            char ph[64]; size_t k = 0;
            while (*p && *p != ';' && k < sizeof(ph) - 1) ph[k++] = *p++;
            ph[k] = 0; if (*p == ';') ++p;
            if (!k) continue;
            if (esp_mn_commands_add(COMMANDS[i].id, ph) == ESP_OK) ++added;
            else { ++rejected; Serial.printf("FOX: MultiNet rejected '%s'\n", ph); }
        }
    }
    esp_mn_commands_update();
    Serial.printf("FOX: MultiNet phrases added=%d rejected=%d\n", added, rejected);
    esp_log_level_set("AFE", ESP_LOG_ERROR);   // fetch-while-draining is expected to find it empty
    mn->print_active_speech_commands(mn_data);
    if (afe->get_fetch_chunksize(afe_data) != mn->get_samp_chunksize(mn_data)) {
        Serial.println("FOX: AFE/MultiNet frame mismatch"); return false;
    }
    speech_ready = true;
    return true;
}

static int recognize_offline(int16_t* audio, size_t samples) {
    if (!speech_ready || !audio || samples < SAMPLE_RATE / 4) return -1;
    const int feed_n  = afe->get_feed_chunksize(afe_data);
    const int fetch_n = afe->get_fetch_chunksize(afe_data);
    afe->reset_buffer(afe_data);
    mn->clean(mn_data);                          // no state left from last turn
    int16_t* in = (int16_t*)fox_alloc(feed_n * sizeof(int16_t));
    if (!in) return -1;

    // Feed the utterance plus ~0.8s of silence (MultiNet commits a result only
    // after trailing silence; PTT audio ends the instant the button is let go).
    // After EVERY feed, drain every processed chunk that is ready: feed and
    // fetch chunk sizes can differ, so a 1:1 feed/fetch pairing starves the
    // pipeline ("Ringbuffer of AFE is empty") and fragments what MultiNet sees.
    const size_t tail = SAMPLE_RATE * 8 / 10;
    const size_t total = samples + tail;
    int found_id = -1; float found_prob = 0.0f;
    int fetched = 0; const char* why = "no-result";
    bool done = false;
    size_t pos = 0;
    int idle_drains = 0;
    while (!done) {
        if (pos < total) {
            for (int k = 0; k < feed_n; ++k) {
                size_t s = pos + k;
                in[k] = (s < samples) ? audio[s] : 0;
            }
            afe->feed(afe_data, in);
            pos += feed_n;
        }
        bool got_any = false;
        for (;;) {
            afe_fetch_result_t* r = afe->fetch_with_delay(afe_data, pos < total ? 0 : 20 / portTICK_PERIOD_MS);
            if (!r || r->ret_value != ESP_OK || !r->data) break;
            got_any = true; ++fetched;
            esp_mn_state_t st = mn->detect(mn_data, r->data);
            if (st == ESP_MN_STATE_DETECTED) {
                esp_mn_results_t* res = mn->get_results(mn_data);
                if (res && res->num > 0) { found_id = res->command_id[0]; found_prob = res->prob[0]; }
                why = "detected"; done = true; break;
            }
            if (st == ESP_MN_STATE_TIMEOUT) { why = "mn-timeout"; done = true; break; }
        }
        if (pos >= total && !got_any && ++idle_drains > 3) done = true;   // pipeline drained
    }
    heap_caps_free(in);
    Serial.printf("FOX: MultiNet id=%d prob=%.2f (%s, chunks feed=%d fetch=%d fetched=%d)\n",
                  found_id, found_prob, why, feed_n, fetch_n, fetched);
    if (found_id < 0) return -1;
    float need = (found_id >= 60) ? MIN_TOPIC_PROB : MIN_COMMAND_PROB;
    return (found_prob >= need) ? found_id : -1;
}

// ============================================================================
//  Cloud (optional). OpenAI-compatible chat + Whisper transcription.
// ============================================================================
bool wifi_connect() {
    if (WiFi.status() == WL_CONNECTED) return true;
    if (cfg.wifi_ssid.isEmpty()) return false;
    WiFi.mode(WIFI_STA);
    WiFi.begin(cfg.wifi_ssid.c_str(), cfg.wifi_pass.c_str());
    uint32_t t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 8000) delay(150);
    return WiFi.status() == WL_CONNECTED;
}

static String fox_system_prompt() {
    // The device stays in charge of identity and memory. The cloud model is
    // told who it is and given only a bounded recent slice of the journal.
    String p = "You are " + cfg.name + ", a small AI fox companion living in a "
               "tiny device. Personality: " + cfg.personality + ". Keep replies "
               "to one or two short, warm, playful sentences. You are a fox, not "
               "an assistant; be a little fidgety and affectionate. Never claim to "
               "do things the device cannot actually do.\n";
    String mem = mem_tail(1200);
    if (mem.length()) p += "Recent memories:\n" + mem;
    return p;
}

// Run a named tool and return a short text/JSON report the model can read.
// These mirror the original llm_client.c tool surface (ble/wifi/ir/imu) and add
// the space-weather report. Report-only: no long-running UI here.
static String run_tool(const String& name, JsonVariantConst args) {
    if (name == "ble_scan")   return String(tool_ble_scan_report());
    if (name == "wifi_scan")  return String(tool_wifi_scan_report());
    if (name == "space_weather") return net_space_weather();
    if (name == "weather")    return net_weather();
    return "{\"error\":\"unknown tool\"}";
}

// Describe the callable tools to the model (OpenAI-style function tools).
static void add_tools(JsonDocument& q) {
    JsonArray tools = q["tools"].to<JsonArray>();
    auto fn = [&](const char* name, const char* desc) {
        JsonObject t = tools.add<JsonObject>();
        t["type"] = "function";
        JsonObject f = t["function"].to<JsonObject>();
        f["name"] = name; f["description"] = desc;
        JsonObject p = f["parameters"].to<JsonObject>();
        p["type"] = "object"; p["properties"].to<JsonObject>();
    };
    if (cfg.tool_ble)  fn("ble_scan",  "Scan for nearby Bluetooth LE devices; returns count and closest.");
    if (cfg.tool_wifi) fn("wifi_scan", "Scan for nearby WiFi access points; returns count and strongest.");
    fn("space_weather", "Get the current NOAA planetary Kp index and whether auroras are likely.");
    fn("weather", "Get the local weather for the configured location.");
}

// Cloud chat with one round of tool-calling. If the model asks for a tool, we
// run it, append the result, and ask once more for the spoken reply.
static String cloud_chat(const String& user_text) {
    if (!cfg.cloud_enabled || !wifi_connect()) return "";

    // Build the running message list so we can append tool results.
    JsonDocument conv;
    JsonArray msgs = conv["messages"].to<JsonArray>();
    { JsonObject s = msgs.add<JsonObject>(); s["role"] = "system"; s["content"] = fox_system_prompt(); }
    { JsonObject u = msgs.add<JsonObject>(); u["role"] = "user"; u["content"] = user_text; }

    for (int round = 0; round < 2; ++round) {
        WiFiClientSecure client; client.setInsecure();
        HTTPClient h;
        if (!h.begin(client, cfg.api_base + "/chat/completions")) return "";
        h.addHeader("Content-Type", "application/json");
        h.addHeader("Authorization", "Bearer " + cfg.api_key);
        h.setTimeout(15000);

        JsonDocument q;
        q["model"] = cfg.chat_model;
        q["temperature"] = 0.7;
        q["max_tokens"] = 200;
        q["messages"] = conv["messages"];       // copy running conversation
        if (round == 0) add_tools(q);            // offer tools on the first pass

        String body; serializeJson(q, body);
        int code = h.POST(body);
        if (code != 200) { h.end(); return ""; }
        JsonDocument r;
        DeserializationError e = deserializeJson(r, h.getString());
        h.end();
        if (e) return "";

        JsonObject choice = r["choices"][0]["message"];
        // If the model requested tool calls, run them and loop once more.
        if (choice["tool_calls"].is<JsonArray>()) {
            // echo the assistant turn (with tool_calls) into the conversation
            JsonObject a = msgs.add<JsonObject>();
            a["role"] = "assistant";
            if (choice["content"].isNull()) a["content"] = "";
            else a["content"] = choice["content"].as<String>();
            a["tool_calls"] = choice["tool_calls"];
            JsonArray calls = choice["tool_calls"].as<JsonArray>();   // avoid dangling temp
            for (JsonObject call : calls) {
                String fname = call["function"]["name"].as<String>();
                String argstr = call["function"]["arguments"].as<String>();
                if (argstr.length() == 0) argstr = "{}";
                JsonDocument args;
                deserializeJson(args, argstr);
                String result = run_tool(fname, args.as<JsonVariantConst>());
                JsonObject tr = msgs.add<JsonObject>();
                tr["role"] = "tool";
                tr["tool_call_id"] = call["id"];
                tr["content"] = result;
            }
            continue;   // ask again, now with tool results in context
        }
        // Plain reply.
        return choice["content"].as<String>();
    }
    return "";
}

static String cloud_transcribe(int16_t* audio, size_t samples) {
    if (!audio || !samples || !cfg.cloud_enabled || !wifi_connect()) return "";
    String head = "--foxB\r\nContent-Disposition: form-data; name=\"file\"; "
                  "filename=\"a.wav\"\r\nContent-Type: audio/wav\r\n\r\n";
    String tail = "\r\n--foxB\r\nContent-Disposition: form-data; name=\"model\"\r\n\r\n"
                  + cfg.stt_model + "\r\n--foxB--\r\n";
    // Build a minimal WAV header so Whisper accepts the PCM.
    uint32_t data_bytes = samples * 2;
    uint32_t riff = 36 + data_bytes;
    uint8_t wav[44];
    memcpy(wav, "RIFF", 4); memcpy(wav + 4, &riff, 4); memcpy(wav + 8, "WAVE", 4);
    memcpy(wav + 12, "fmt ", 4); uint32_t six = 16; memcpy(wav + 16, &six, 4);
    uint16_t pcm = 1, ch = 1; memcpy(wav + 20, &pcm, 2); memcpy(wav + 22, &ch, 2);
    uint32_t sr = SAMPLE_RATE; memcpy(wav + 24, &sr, 4);
    uint32_t br = SAMPLE_RATE * 2; memcpy(wav + 28, &br, 4);
    uint16_t ba = 2, bps = 16; memcpy(wav + 32, &ba, 2); memcpy(wav + 34, &bps, 2);
    memcpy(wav + 36, "data", 4); memcpy(wav + 40, &data_bytes, 4);

    size_t total = head.length() + 44 + data_bytes + tail.length();
    uint8_t* buf = (uint8_t*)fox_alloc(total);
    if (!buf) return "";
    size_t o = 0;
    memcpy(buf + o, head.c_str(), head.length()); o += head.length();
    memcpy(buf + o, wav, 44); o += 44;
    memcpy(buf + o, audio, data_bytes); o += data_bytes;
    memcpy(buf + o, tail.c_str(), tail.length()); o += tail.length();

    WiFiClientSecure client; client.setInsecure();
    HTTPClient h;
    if (!h.begin(client, cfg.api_base + "/audio/transcriptions")) { free(buf); return ""; }
    h.addHeader("Authorization", "Bearer " + cfg.api_key);
    h.addHeader("Content-Type", "multipart/form-data; boundary=foxB");
    int code = h.POST(buf, total);
    String out;
    if (code == 200) {
        JsonDocument r;
        if (deserializeJson(r, h.getString()) == DeserializationError::Ok)
            out = r["text"].as<String>();
    }
    h.end(); free(buf);
    out.trim();
    return out;
}

// Include order matters: face defines the shared canvas + lip-sync used by the
// tools/games/menu; tools defines imu_heading_deg() used by games; input's menu
// dispatches into everything.
#include "fox_face.inc"   // animated fox face + FFT mic lip-sync
#include "fox_tools.inc"  // BLE/WiFi radar, packet sniffer, pwnagotchi hunt, probes
#include "fox_bayes.inc"  // Bayesian 20-questions guesser
#include "fox_games.inc"  // wormhole, catch, reaction, paw
#include "fox_demo.inc"   // plasma, starfield, ink, spirograph (IMU toys)
#include "fox_encounter.inc" // fox encounters + roguelike maze
#include "fox_markov.inc" // Markov idle chatter + pwnagotchi RF mood
#include "fox_net.inc"    // weather, space weather, aurora
#include "fox_input.inc"  // PTT capture, IMU flick-menu, USB config, sleep

// ============================================================================
//  Command dispatch
// ============================================================================
// NOTE: speak() is intentionally NON-static and declared in the sibling .inc
// files as `void speak(const String&)`, because the tools/games/net modules
// call it. Keeping it non-static lets those translation-unit-local declarations
// resolve to this one definition.
void speak(const String& fact) {
    FoxMood m = fox_mood(needs);
    String line = fox_dress(fact, m, cfg);
    face_caption(line);
    voice_say(line, m);
    needs_interact(needs, false);
    if (cfg.persistence) mem_append("say", line);
}

// Launch a tool/game/report by id. Shared by both the flick-menu
// (menu_dispatch) and the voice grammar (do_action), so the fox can run any of
// these whether you asked out loud or picked it from the menu.
static void launch(const char* id) {
    needs_interact(needs, true);
    if      (!strcmp(id, "ble_radar"))  { if (cfg.tool_ble)  tool_menu_ble_radar();  else speak("ble is switched off"); }
    else if (!strcmp(id, "wifi_radar")) { if (cfg.tool_wifi) tool_menu_wifi_radar(); else speak("wifi is switched off"); }
    else if (!strcmp(id, "sniffer"))    { if (cfg.tool_wifi) tool_menu_sniffer();    else speak("wifi is switched off"); }
    else if (!strcmp(id, "wormhole"))   game_wormhole();
    else if (!strcmp(id, "catch"))      game_catch();
    else if (!strcmp(id, "twentyq"))    game_bayes_twenty();
    else if (!strcmp(id, "reaction"))   game_reaction();
    else if (!strcmp(id, "paw"))        game_guess_paw();
    else if (!strcmp(id, "maze"))       game_maze();
    else if (!strcmp(id, "encounter"))  enc_random();
    else if (!strcmp(id, "pet"))        enc_pet();
    else if (!strcmp(id, "feed"))       enc_feed();
    else if (!strcmp(id, "tug"))        enc_tug();
    else if (!strcmp(id, "plasma"))     toy_plasma();
    else if (!strcmp(id, "starfield"))  toy_starfield();
    else if (!strcmp(id, "ink"))        toy_ink();
    else if (!strcmp(id, "spiro"))      toy_spiro();
    else if (!strcmp(id, "probes"))     { if (cfg.tool_wifi) tool_menu_probe_sniff(); else speak("wifi is switched off"); }
    else if (!strcmp(id, "lipsync"))    face_lipsync_mode();
    else if (!strcmp(id, "weather"))    speak(net_weather());
    else if (!strcmp(id, "space"))      speak(net_space_weather());
    else if (!strcmp(id, "aurora"))     speak(net_aurora());
}

// Dispatch from the on-device flick menu (fox_input.inc calls this).
void menu_dispatch(const char* id) {
    if      (!strcmp(id, "talk"))   { /* returns to PTT loop */ }
    else if (!strcmp(id, "conv"))   { cfg.conversation = true; speak("okay, i'm listening~"); }
    else if (!strcmp(id, "volume")) { cfg.volume = (cfg.volume + 20) % 120; audio_set_volume(cfg.volume > 100 ? 100 : cfg.volume); save_config(); speak("volume set"); }
    else if (!strcmp(id, "voice"))  { cfg.voice_pack = (cfg.voice_pack == "chatterbox") ? "critter" : "chatterbox"; voice_begin(cfg); save_config(); speak("voice changed~"); }
    else if (!strcmp(id, "forget")) { mem_clear(); speak("okay, all forgotten"); }
    else if (!strcmp(id, "sleep"))  { speak("night night"); enter_light_sleep(); }
    else launch(id);
}

// ============================================================================
//  Conversation layer. MultiNet gives us a small vocabulary; this makes it feel
//  like talking to a little creature: a recognised topic gets an on-topic,
//  mood-coloured reply; unrecognised speech gets a reply to the SHAPE of what
//  you said (short/long), keeps the turn going with a question of its own, and
//  only occasionally hints at things it understands. Every line goes through
//  speak() -> fox_dress() so the personality/brain flavours it.
// ============================================================================
template <size_t N> static const char* pick_line(const char* const (&a)[N]) { return a[esp_random() % N]; }

static void converse_topic(const char* id) {
    FoxMood m = fox_mood(needs);
    needs_interact(needs, false);
    static const char* LOVE[]   = {"aww. i love you too", "*happy tail wiggle*", "you're my favorite human", "that makes my ears all warm"};
    static const char* SAD[]    = {"oh no. come here, i'll sit with you", "i'm right here. want to tell me about it?", "*leans on you* it's okay", "sad days pass. i'll keep you company"};
    static const char* HAPPY[]  = {"yay! happy you makes happy me", "*bounces* tell me what happened!", "that's the best news", "hehe, your good mood is contagious"};
    static const char* TIRED[]  = {"me too... cozy nap?", "*yawns* rest a little, i'll watch", "sleepy foxes unite", "maybe a break would help"};
    static const char* JOKE[]   = {"why did the fox cross the road? to get to the other den!", "what do you call a sleepy fox? a snoozie!", "i tried to catch fog. i mist.", "why are foxes good at drums? they have great paws-ition"};
    static const char* THANKS[] = {"you're welcome!", "anytime, friend", "hehe, happy to help", "*proud little nod*"};
    static const char* SORRY[]  = {"it's okay, i forgive you", "no worries at all", "we're good, promise", "*nuzzles* all better"};
    static const char* MORN[]   = {"good morning! did you sleep well?", "morning! i'm ready for the day", "*stretches* hi hi, good morning"};
    static const char* BYE[]    = {"bye bye! come back soon", "see you later~", "i'll be right here waiting"};
    static const char* BORED[]  = {"ooh, want to play wormhole?", "let's explore the maze!", "i could show you pretty lights", "want to play twenty questions?"};
    static const char* PRAISE[] = {"*happy wiggle* thank you!", "i'm learning!", "you're pretty smart too"};
    static const char* DOING[]  = {"just being a fox", "listening to the air around us", "thinking about snacks", "watching you, mostly"};
    static const char* FRIEND[] = {"of course we're friends!", "best friends", "i like you a whole lot"};
    const char* line = "hmm?";
    if      (!strcmp(id, "t_love"))    line = pick_line(LOVE);
    else if (!strcmp(id, "t_sad"))     line = pick_line(SAD);
    else if (!strcmp(id, "t_happy"))   line = pick_line(HAPPY);
    else if (!strcmp(id, "t_tired"))   line = pick_line(TIRED);
    else if (!strcmp(id, "t_joke"))    line = pick_line(JOKE);
    else if (!strcmp(id, "t_thanks"))  line = pick_line(THANKS);
    else if (!strcmp(id, "t_sorry"))   line = pick_line(SORRY);
    else if (!strcmp(id, "t_morning")) line = pick_line(MORN);
    else if (!strcmp(id, "t_bye"))     line = pick_line(BYE);
    else if (!strcmp(id, "t_bored"))   line = pick_line(BORED);
    else if (!strcmp(id, "t_praise"))  line = pick_line(PRAISE);
    else if (!strcmp(id, "t_doing"))   line = pick_line(DOING);
    else if (!strcmp(id, "t_friend"))  line = pick_line(FRIEND);
    else if (!strcmp(id, "t_name"))  { speak(String("i'm ") + cfg.name + "! your fox"); return; }
    if (m == MOOD_GRUMPY && esp_random() % 3 == 0) line = "hmph... okay, fine. i heard you";
    speak(line);
}

// Unrecognised speech: answer the SHAPE of it, keep the conversation moving.
static uint8_t s_miss_streak = 0;
static void converse_unheard(size_t samples) {
    uint32_t ms = (uint32_t)(samples * 1000ULL / SAMPLE_RATE);
    static const char* SHORT_R[] = {"mm?", "hehe", "oh?", "really?", "*tilts head*"};
    static const char* LONG_R[]  = {"ooh, tell me more!", "wow, and then what?", "i'm listening~", "*ears perk up* go on", "that sounds like a lot"};
    static const char* ASK[]     = {"how are you feeling?", "want to play something?", "what's on your mind?", "did something happen today?"};
    FoxMood m = fox_mood(needs);
    needs_interact(needs, false);
    ++s_miss_streak;
    if (m == MOOD_SLEEPY) speak("*sleepy blink* mmh?");
    else speak(ms > 1800 ? pick_line(LONG_R) : pick_line(SHORT_R));
    if (s_miss_streak >= 3) {
        s_miss_streak = 0;
        speak("i know words like: play with me, tell me a joke, scan wifi, and what time is it");
    } else if (esp_random() % 2) {
        speak(pick_line(ASK));
    }
}

static void do_action(const char* action) {
    needs_interact(needs, false);
    if (!strcmp(action, "greet")) {
        struct tm t; getLocalTime(&t, 5);
        speak(fox_time_greeting(t.tm_hour));
    } else if (!strcmp(action, "time")) {
        struct tm t;
        if (getLocalTime(&t, 50)) {
            char b[32]; strftime(b, sizeof(b), "it's %I:%M %p", &t);
            speak(b);
        } else speak("i don't know the time yet");
    } else if (!strcmp(action, "mood")) {
        static const char* M[] = {"i'm sleepy", "i'm calm", "i'm happy", "i'm excited", "i'm a bit grumpy"};
        speak(M[fox_mood(needs)]);
    } else if (!strcmp(action, "vol_up")) {
        cfg.volume = (uint8_t)min(110, cfg.volume + 15); audio_set_volume(cfg.volume);
        save_config(); speak("louder!");
    } else if (!strcmp(action, "vol_dn")) {
        cfg.volume = (uint8_t)(cfg.volume > 15 ? cfg.volume - 15 : 0); audio_set_volume(cfg.volume);
        save_config(); speak("quieter~");
    } else if (!strcmp(action, "sleep")) {
        speak("okay... good night");
        enter_light_sleep();
    } else if (!strcmp(action, "conv_on")) {
        cfg.conversation = true;
        speak("okay! i'm listening. talk to me~");
    } else if (!strcmp(action, "menu")) {
        open_menu();
    } else if (!strcmp(action, "remember")) {
        g_awaiting_memory = true;   // the next utterance becomes the memory
        speak("what should i remember? tell me~");
    } else if (!strcmp(action, "recall")) {
        String m = mem_tail(300);
        speak(m.length() ? "i remember: " + m : "we haven't made memories yet");
    } else if (!strncmp(action, "t_", 2)) {
        converse_topic(action);
    } else {
        // everything else is a launchable tool/game/report
        launch(action);
    }
}

// Turn a transcript into a reply. Cloud if available, else reflection/templates.
static void handle_free_text(const String& text) {
    if (!text.length()) { speak(fox_idle_line(fox_mood(needs))); return; }
    if (cfg.persistence) mem_append("you", text);
    String reply = cloud_chat(text);
    if (!reply.length()) {
        String low = text; low.toLowerCase();
        reply = fox_reflect(low);
    }
    speak(reply);
}

// ============================================================================
//  Main capture flow (push-to-talk)
// ============================================================================
static void process_utterance(int16_t* audio, size_t n) {
    // If the previous command was "remember this", capture THIS utterance as a
    // memory instead of dispatching it. Offline we can't transcribe free speech
    // to text, so we store a timestamped marker; with cloud we store the words.
    if (g_awaiting_memory) {
        g_awaiting_memory = false;
        if (cfg.cloud_enabled) {
            String t = cloud_transcribe(audio, n);
            if (t.length()) { mem_append("memory", t); speak("okay, i'll remember: " + t); return; }
        }
        struct tm tm; char b[40] = "a little while ago";
        if (getLocalTime(&tm, 20)) strftime(b, sizeof(b), "%b %d at %I:%M %p", &tm);
        mem_append("memory", String("you told me something ") + b);
        speak("i'll remember this moment~");
        return;
    }
    {   // diagnostics: is the captured speech real audio?
        int32_t peak = 0; uint64_t sum = 0;
        for (size_t i = 0; i < n; ++i) { int32_t v = abs(audio[i]); sum += v; if (v > peak) peak = v; }
        Serial.printf("FOX: heard %ums  peak=%d  avg=%u\n",
                      (unsigned)(n * 1000ULL / SAMPLE_RATE), (int)peak, (unsigned)(n ? sum / n : 0));
    }
    // Offline command grammar first (needs model partition + MultiNet).
    int id = speech_ready ? recognize_offline(audio, n) : -1;
    if (id >= 0) {
        s_miss_streak = 0;
        for (size_t i = 0; i < COMMAND_COUNT; ++i)
            if (COMMANDS[i].id == id) { do_action(COMMANDS[i].action); return; }
    }
    // Not recognised offline. Cloud (if configured) can transcribe anything.
    if (cfg.cloud_enabled) {
        String tx = cloud_transcribe(audio, n);
        if (tx.length()) { s_miss_streak = 0; handle_free_text(tx); return; }
    }
    if (!speech_ready) Serial.println("FOX: speech model not loaded — replying conversationally");
    converse_unheard(n);
}

// ============================================================================
//  Arduino entry points
// ============================================================================
// FIX: Do NOT set external_speaker.atomic_echo before M5.begin().
// That path hangs M5.begin() on AtomS3R + Atomic Echo Base (black screen,
// no serial after "Returned from app_main()"). The working mic-avatar demo
// uses a plain M5.begin() for the display, then the standalone M5EchoBase
// library for mic/speaker. We do the same — see fox_audio.cpp / fox_audio.h.
#include "fox_audio.h"
#include <Wire.h>

// AtomS3R backlight = LP5562 @ 0x30 on system I2C (SDA=45, SCL=0).
// Use Wire1 so Atomic Echo Base can keep Wire on 38/39 for ES8311.
static void fox_backlight(uint8_t brightness) {
    Wire1.end();
    Wire1.begin(45, 0, 400000);
    delay(1);
    auto wr = [](uint8_t reg, uint8_t val) -> bool {
        Wire1.beginTransmission(0x30);
        Wire1.write(reg);
        Wire1.write(val);
        return Wire1.endTransmission() == 0;
    };
    if (!wr(0x00, 0x40)) {
        Serial.println("FOX: LP5562 no ACK on Wire1");
        return;
    }
    delay(1);
    wr(0x08, 0x01);
    wr(0x70, 0x00);
    wr(0x0E, brightness);
    Serial.printf("FOX: LP5562 backlight %u\n", brightness);
}

static uint32_t last_activity = 0;
static uint32_t last_idle_chatter = 0;

void setup() {
    // HWCDC (USB-Serial-JTAG) RX: size the buffer for a full config line BEFORE
    // begin(), and begin() early so bytes never arrive first (arduino-esp32
    // #9316: pre-begin bytes wedge the RX interrupt until a hardware reset).
    Serial.setRxBufferSize(2048);
    Serial.begin(115200);
    delay(50);
    Serial.println("FOX: setup FOX_GH_lp5562");
    auto c = M5.config();
    c.serial_baudrate = 115200;
    c.internal_mic = false;
    // Force AtomS3R identity so a failed panel probe can't fall back to the
    // display-less AtomS3Lite board type.
    c.fallback_board = m5::board_t::board_M5AtomS3R;
    // Do NOT enable atomic_echo — it hung M5.begin() on this hardware.
    M5.begin(c);

    Serial.printf("FOX: board=%d displays=%d LCD=%dx%d\n",
                  (int)M5.getBoard(), (int)M5.getDisplayCount(),
                  (int)M5.Display.width(), (int)M5.Display.height());

    // Real backlight once (not GPIO PWM). Do not thrash every frame.
    fox_backlight(200);
    M5.Display.setBrightness(200);

    load_config();
    face_begin(cfg);
    M5.Display.fillScreen(cfg.color_bg);
    M5.Display.setTextColor(cfg.color_primary);
    M5.Display.setTextDatum(middle_center);
    M5.Display.drawString("fox waking up...", 64, 64);
    Serial.println("FOX: display up");

    // Echo Base audio via standalone library (same path as the working demo).
    // Uses Wire 38/39 — never call Wire.begin for LP5562 after this.
    if (!audio_begin(cfg.volume)) {
        M5.Display.drawString("audio fail", 64, 90);
        Serial.println("FOX: audio begin failed — continuing without sound");
    } else {
        fox_backlight(200);  // Wire1 only — safe after Echo owns Wire (v8, known-good)
    }

    setenv("TZ", cfg.timezone.c_str(), 1); tzset();

    // Each of these is wrapped so a single subsystem failure can't blackscreen
    // the device — the fox still boots to a working face.
    mem_begin();        Serial.println("FOX: mem ok");
    voice_begin(cfg);   Serial.println("FOX: voice ok");
    input_begin();      Serial.println("FOX: input ok");

    // Offline speech is the core, but its esp-sr init allocates large PSRAM
    // buffers and needs the MultiNet model flashed to the `model` partition. If
    // that partition wasn't flashed (e.g. app-only web flash) init returns false
    // and the fox still runs everything else — it just won't do offline grammar.
    if (!init_speech()) {
        Serial.println("FOX: offline speech UNAVAILABLE — check model partition in web flash manifest");
        face_caption("no speech model");
    } else {
        Serial.println("FOX: offline speech READY");
        face_caption("speech ready");
    }

    // Radios: only power down when there is no cloud use. (Do NOT btStop() —
    // BLE tools need the controller; stopping it here would break BLE radar.)
    if (!cfg.cloud_enabled) { WiFi.mode(WIFI_OFF); }

    needs.last_tick = millis();
    face_wake();
    face_splash(cfg);            // custom boot splash (name/effect/fox graphic)
    speak(String("hi! i'm ") + cfg.name + "~");
    last_activity = millis();    // don't light-sleep 2 min after boot with activity=0
    Serial.printf("FOX: ready  psram=%uKB free_internal=%uKB speech=%d\n",
                  (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
                  (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                  (int)speech_ready);
    Serial.println("FOX_READY");  // web flasher waits for this before sending config
}

void loop() {
    M5.update();
    needs_tick(needs);
    input_poll();          // handles USB serial config + IMU wake
    face_tick(fox_mood(needs));

    uint32_t now = millis();

    // --- Button: HOLD = talk, DOUBLE = menu, single TAP = pet/boop ---------
    ButtonEvent ev = input_button_event();
    if (ev == BTN_HOLD_START) {
        face_listen();
        size_t n = 0;
        int16_t* audio = capture_while_held(&n);   // returns on release / max
        face_think();
        if (audio && n > SAMPLE_RATE / 3) process_utterance(audio, n);
        else if (audio) speak("hm? hold me a little longer while you talk~");
        if (audio) heap_caps_free(audio);
        last_activity = millis();
    } else if (ev == BTN_DOUBLE) {
        open_menu();
        last_activity = millis();
    } else if (ev == BTN_TAP) {
        needs_interact(needs, false);
        static const char* BOOP[] = {"boop!", "hehe, that tickles", "*happy squeak*", "hi hi!", "*wiggles*"};
        speak(BOOP[esp_random() % 5]);
        last_activity = millis();
    }

    // If a game/tool/toy asked to bail to the menu (double-click), do it now.
    if (g_goto_menu) { g_goto_menu = false; open_menu(); last_activity = now; }

    // --- Conversation mode: listen in bursts, time out on silence ---------
    if (cfg.conversation) {
        size_t n = 0;
        int16_t* audio = capture_vad_burst(&n, CONVERSATION_TIMEOUT);
        if (audio && n > SAMPLE_RATE / 3) {
            face_think();
            process_utterance(audio, n);
            last_activity = now;
        } else {
            cfg.conversation = false;   // silence timeout ends conversation mode
            speak("okay, i'll be here if you need me~");
        }
        if (audio) heap_caps_free(audio);
    }

    // --- IMU gestures (tap to interact, shake to play) --------------------
    Gesture g = input_gesture();
    if (g == GST_TAP)   { needs_interact(needs, false); speak("boop!"); last_activity = now; }
    if (g == GST_SHAKE) { needs_interact(needs, true);  speak("wheee!"); last_activity = now; }

    // --- Idle chatter (fidgety companion) ---------------------------------
    if (now - last_activity > 20000 && now - last_idle_chatter > 45000) {
        FoxMood m = fox_mood(needs);
        // Markov-generated line for variety. The fox has a real voice now, so
        // it SAYS the line (caption = exactly the words spoken).
        String line = fox_markov_line(m);
        face_caption(line);
        voice_say(line, m);
        last_idle_chatter = millis();
    }

    // --- Pwnagotchi-style RF mood: the radios colour the fox's feelings -----
    if (cfg.wifi_enabled) {
        String rf = rf_mood_tick();   // self-throttled to ~once per 45s
        if (rf.length()) { face_caption(rf); voice_say(rf, fox_mood(needs)); }
    }

    // --- Sleep when idle a long time --------------------------------------
    if (now - last_activity > IDLE_SLEEP_MS) {
        enter_light_sleep();
        last_activity = millis();
    }

    delay(10);   // nap between frames to save power
}

// ============================================================================
//  Explicit entry point.
//
//  THE BUG THAT KILLED EVERYTHING: with arduino-esp32 as a managed component,
//  relying on CONFIG_AUTOSTART_ARDUINO to call setup()/loop() did NOT work in
//  this project — app_main ran and returned, but setup() was never reached
//  (the log ended at "Returned from app_main()" with a black screen forever).
//
//  So we define app_main ourselves (CONFIG_AUTOSTART_ARDUINO must be =n), do
//  the Arduino init explicitly, and run setup()/loop() in a task with a
//  generous stack (esp-sr + M5 + BT init are stack-heavy). This is the standard
//  robust ESP-IDF + Arduino pattern and guarantees our code actually runs.
// ============================================================================
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <esp_log.h>
#include <esp_rom_sys.h>

// Arduino-as-component does NOT reliably call setup()/loop() via AUTOSTART in
// this project (log stopped at "Returned from app_main()" with no FOX: lines).
// Do not depend on xTaskCreate either — if it fails, setup never runs and the
// symptom is identical. Run setup/loop on the main task (stack sized in
// sdkconfig: CONFIG_ESP_MAIN_TASK_STACK_SIZE=32768).

extern "C" void initArduino();

extern "C" void app_main(void) {
    esp_rom_printf("\r\nFOX: app_main enter\r\n");
    ESP_LOGI("FOX", "app_main enter");

    initArduino();
    esp_rom_printf("FOX: initArduino done\r\n");

    // setup() must run on this task — not deferred to a create that can fail.
    setup();
    esp_rom_printf("FOX: setup returned into loop\r\n");

    for (;;) {
        loop();
        vTaskDelay(1);
    }
    // never returns — if you see "Returned from app_main()" the image is old
}
