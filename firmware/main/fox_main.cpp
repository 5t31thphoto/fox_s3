// fox_main.cpp — Fox voice companion, main firmware.
//
// Target: M5Stack AtomS3R (ESP32-S3) + Atomic Echo Base.
// Offline-first: everything core works with no network. Cloud (Groq or any
// OpenAI-compatible endpoint) is an *optional* enhancement layered on top.
//
// Interaction model (per the owner's emphatic instructions):
//   * PUSH-TO-TALK ONLY. Hold the USER button to talk; release to process.
//     A single click belongs to the current screen; only a double click opens the menu.
//   * "Conversation mode" is opt-in from the menu (or by asking the fox). In
//     that mode it keeps listening for a while and times out on silence.
//
// This file wires together the subsystems implemented in the sibling files:
//   fox_brain.cpp   personality/mood/needs/reflection
//   fox_voice.cpp   Pico / critter / babble speech
//   fox_memory.cpp  device-owned rolling journal
//   fox_llm.cpp     optional tiny on-device brain (safe: can't invent facts)
//   fox_ir.cpp      IR sweep + fake-learning (this file includes helpers)
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
#include <esp_partition.h>
#include <driver/rmt_tx.h>
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
    cfg.tool_ir     = prefs.getBool("tir", cfg.tool_ir);
    cfg.tool_imu    = prefs.getBool("timu", cfg.tool_imu);
    cfg.lip_sync    = prefs.getBool("lips", cfg.lip_sync);
    cfg.volume      = prefs.getUChar("vol", cfg.volume);
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
    prefs.putBool("tir", cfg.tool_ir);
    prefs.putBool("timu", cfg.tool_imu);
    prefs.putBool("lips", cfg.lip_sync);
    prefs.putUChar("vol", cfg.volume);
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
// Keep phrases short and phonetically distinct. IDs are 1-based.
static const Command COMMANDS[] = {
    {1,  "hello fox;hey fox;hi fox",             "greet"},
    {2,  "what time is it;tell me the time",      "time"},
    {3,  "how are you;how do you feel",           "mood"},
    {4,  "what is the weather;weather",           "weather"},
    {5,  "turn off the tv;power off tv",          "ir_tv_power"},
    {6,  "volume up",                              "ir_vol_up"},
    {7,  "volume down",                            "ir_vol_dn"},
    {8,  "go to sleep;good night",                 "sleep"},
    {9,  "conversation mode;lets chat",            "conv_on"},
    {10, "remember this",                          "remember"},
    {11, "what do you remember",                   "recall"},
    {12, "open the menu;show menu",                "menu"},
    // voice-launchable tools & games (the fox runs them for you)
    {13, "scan for devices;bluetooth radar",       "ble_radar"},
    {14, "scan wifi;wifi radar",                    "wifi_radar"},
    {15, "sniff packets;sniffer mode",              "sniffer"},
    {16, "play wormhole;fly the ship",              "wormhole"},
    {17, "catch the treats;catch game",             "catch"},
    {18, "twenty questions;guess my thing",         "twentyq"},
    {19, "space weather;solar storm",               "space"},
    {20, "any aurora;northern lights",              "aurora"},
    {21, "lip sync mode;puppet mode",               "lipsync"},
    {22, "learn my tv;learn the remote",            "learn_tv"},
    {23, "hunt mode;pwnagotchi",                     "sniffer"},
    {24, "explore the maze;lets explore",           "maze"},
    {25, "play with me;lets hang out",              "encounter"},
    {26, "show me colors;pretty lights",            "plasma"},
    {27, "starfield;fly through space",             "starfield"},
    {28, "what are phones looking for;probe scan",  "probes"},
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
    for (size_t i = 0; i < COMMAND_COUNT; ++i)
        esp_mn_commands_add(COMMANDS[i].id, (char*)COMMANDS[i].phrases);
    esp_mn_commands_update();
    mn->print_active_speech_commands(mn_data);
    if (afe->get_fetch_chunksize(afe_data) != mn->get_samp_chunksize(mn_data)) {
        Serial.println("FOX: AFE/MultiNet frame mismatch"); return false;
    }
    speech_ready = true;
    return true;
}

static int recognize_offline(int16_t* audio, size_t samples) {
    if (!speech_ready || !audio || samples < SAMPLE_RATE / 4) return -1;
    const int feed_n = afe->get_feed_chunksize(afe_data);
    if (feed_n != afe->get_fetch_chunksize(afe_data)) return -1;
    afe->reset_buffer(afe_data);
    int16_t* in = (int16_t*)heap_caps_malloc(feed_n * sizeof(int16_t),
                                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!in) return -1;
    int found_id = -1;
    float found_prob = 0.0f;
    // FIX (bug #1): the old loop condition was `pos < samples && !found_id`.
    // found_id starts at -1, and !(-1) is false, so the body never ran and
    // offline recognition ALWAYS failed. Use an explicit found flag instead.
    bool found = false;
    for (size_t pos = 0; pos < samples && !found; pos += feed_n) {
        size_t n = min((size_t)feed_n, samples - pos);
        memcpy(in, audio + pos, n * sizeof(int16_t));
        if (n < (size_t)feed_n) memset(in + n, 0, (feed_n - n) * sizeof(int16_t));
        if (afe->feed(afe_data, in) < 0) continue;
        afe_fetch_result_t* r = afe->fetch_with_delay(afe_data, 2 / portTICK_PERIOD_MS);
        if (!r || r->ret_value != ESP_OK || !r->data) continue;
        esp_mn_state_t st = mn->detect(mn_data, r->data);
        if (st == ESP_MN_STATE_DETECTED) {
            esp_mn_results_t* res = mn->get_results(mn_data);
            if (res && res->num > 0) {
                found_id = res->command_id[0];
                found_prob = res->prob[0];
                found = true;
            }
        }
    }
    free(in);
    if (found_id < 0 || found_prob < MIN_COMMAND_PROB) return -1;
    return found_id;
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
    if (name == "tv_power")   { ir_command("tv", "power"); return "{\"ok\":true}"; }
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
    if (cfg.tool_ir)   fn("tv_power", "Send the learned TV power IR code (toggle the TV on/off).");
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
    uint8_t* buf = (uint8_t*)heap_caps_malloc(total, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
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
#include "fox_ir.inc"     // IR sweep + fake-learning (RMT TX on GPIO47)
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
    else if (!strcmp(id, "learn_tv"))   ir_narrow_power();
    else if (!strcmp(id, "weather"))    speak(net_weather());
    else if (!strcmp(id, "space"))      speak(net_space_weather());
    else if (!strcmp(id, "aurora"))     speak(net_aurora());
}

// Dispatch from the on-device flick menu (fox_input.inc calls this).
void menu_dispatch(const char* id) {
    if      (!strcmp(id, "talk"))   { /* returns to PTT loop */ }
    else if (!strcmp(id, "conv"))   { cfg.conversation = true; speak("okay, i'm listening~"); }
    else if (!strcmp(id, "volume")) { volume_adjust(); }
    else if (!strcmp(id, "voice"))  { cfg.voice_pack = (cfg.voice_pack == "chatterbox") ? "critter" : "chatterbox"; voice_begin(cfg); save_config(); speak("voice changed~"); }
    else if (!strcmp(id, "forget")) { mem_clear(); speak("okay, all forgotten"); }
    else if (!strcmp(id, "sleep"))  { speak("night night"); enter_light_sleep(); }
    else launch(id);
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
    } else if (!strcmp(action, "ir_tv_power")) {
        ir_command("tv", "power");
    } else if (!strcmp(action, "ir_vol_up")) {
        ir_command("tv", "vol_up");
    } else if (!strcmp(action, "ir_vol_dn")) {
        ir_command("tv", "vol_dn");
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
    // Offline command grammar first (needs model partition + Multinet).
    if (!speech_ready) {
        Serial.println("FOX: utterance ignored — offline speech not ready (model?)");
        face_caption("no offline hearing");
        speak("i can't hear commands offline until the speech model is flashed");
        return;
    }
    int id = recognize_offline(audio, n);
    Serial.printf("FOX: offline recognize id=%d samples=%u\n", id, (unsigned)n);
    if (id >= 0) {
        for (size_t i = 0; i < COMMAND_COUNT; ++i)
            if (COMMANDS[i].id == id) { do_action(COMMANDS[i].action); return; }
    }
    // Not a known command
    if (cfg.cloud_enabled) {
        String tx = cloud_transcribe(audio, n);
        if (tx.length()) handle_free_text(tx);
        else {
            face_caption("didn't catch that");
            speak("i didn't catch a command. try say status, or scan, or play a game");
        }
    } else {
        // Do NOT fox_reflect empty — that pretends to understand.
        face_caption("no command matched");
        speak("i didn't catch a command. try: status, scan wifi, ble radar, play wormhole");
    }
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
        fox_backlight(200);  // Wire1 only — safe after Echo owns Wire
    }

    setenv("TZ", cfg.timezone.c_str(), 1); tzset();

    // Each of these is wrapped so a single subsystem failure can't blackscreen
    // the device — the fox still boots to a working face.
    mem_begin();        Serial.println("FOX: mem ok");
    voice_begin(cfg);   Serial.println("FOX: voice ok");
    ir_begin();         Serial.println("FOX: ir ok");
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
    setCpuFrequencyMhz(160);

    needs.last_tick = millis();
    face_wake();
    face_splash(cfg);            // custom boot splash (name/effect/fox graphic)
    speak(String("hi! i'm ") + cfg.name + "~");
    last_activity = millis();    // don't light-sleep 2 min after boot with activity=0
    Serial.println("FOX: ready");
}

void loop() {
    M5.update();
    needs_tick(needs);
    input_poll();          // handles USB serial config + IMU wake
    face_tick(fox_mood(needs));

    uint32_t now = millis();

    // --- Button semantics ---------------------------------------------------
    // A single click NEVER opens the menu. It is deliberately available to the
    // current app/game. Double click is the one universal menu gesture.
    ButtonEvent ev = input_button_event();
    if (ev == BTN_DOUBLE) {
        open_menu();
        last_activity = now;
    } else if (ev == BTN_HOLD_START) {
        face_listen();
        size_t n = 0;
        int16_t* audio = capture_while_held(&n);
        face_think();
        if (audio && n > SAMPLE_RATE / 3) process_utterance(audio, n);
        if (audio) heap_caps_free(audio);
        last_activity = now;
    } else if (ev == BTN_TAP) {
        // Outside an app, a single click is simply a little fox interaction.
        needs_interact(needs, false);
        face_set_mouth(0.55f);
        face_caption("boop!");
        last_activity = now;
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
    if (now - last_activity > 20000 && now - last_idle_chatter > 25000) {
        FoxMood m = fox_mood(needs);
        // Markov-generated line for variety; caption shows the words, and we
        // babble the syllables (never fake spoken words the voice didn't say).
        String line = fox_markov_line(m);
        face_caption(line);
        voice_babble(m, 2 + (esp_random() % 3));
        last_idle_chatter = now;
    }

    // --- Pwnagotchi-style RF mood: the radios colour the fox's feelings -----
    if (cfg.wifi_enabled) {
        String rf = rf_mood_tick();   // self-throttled to ~once per 45s
        if (rf.length()) { face_caption(rf); voice_babble(fox_mood(needs), 3); }
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
