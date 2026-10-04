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
#include "brain_policy.h"     // act/feel names (trainer-generated)
#include "mn_phonemes.h"      // MultiNet phonemes (CI-generated; empty = runtime G2P)
#include <esp_partition.h>
#include <math.h>
#include <ctype.h>

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
static uint32_t last_activity = 0;   // last user interaction (loop, fox-time, sleep)
// The online (Groq) brain is used only when a key is configured AND the user
// has it switched on (menu "brain" / "go online" / "go offline").
static uint32_t s_online_cool = 0;     // network trouble: skip online calls until this
static inline bool cloud_brain() {
    return cfg.cloud_enabled && cfg.brain_online && (int32_t)(s_online_cool - millis()) <= 0;
}
// A failed online call (no HTTP answer, timeout, TLS failure, 5xx) means the
// network or Groq is struggling: think offline for a couple of minutes instead
// of making every utterance wait on it again.
static void online_trouble(const char* what, int code) {
    s_online_cool = millis() + 120000;
    Serial.printf("FOX: %s failed (%d) -> offline brain for 2 min\n", what, code);
}
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
    cfg.brain_online  = prefs.getBool("bon", cfg.brain_online);
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
    prefs.putBool("bon", cfg.brain_online);
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
    {1, "hello;hello fox", "greet"},
    {2, "what time is it;tell me the time", "time"},
    {3, "how are you;how do you feel", "mood"},
    {4, "what is the weather;tell me the weather", "weather"},
    {6, "volume up;louder", "vol_up"},
    {7, "volume down;quieter", "vol_dn"},
    {8, "go to sleep;good night", "sleep"},
    {9, "conversation mode;lets chat", "conv_on"},
    {10, "remember this;remember that", "remember"},
    {11, "what do you remember;tell me a memory", "recall"},
    {12, "open the menu;show menu", "menu"},
    {13, "scan for devices;bluetooth radar", "ble_radar"},
    {14, "scan wifi;wifi radar", "wifi_radar"},
    {15, "sniffer mode;hunt mode", "sniffer"},
    {16, "play wormhole;fly the ship", "wormhole"},
    {17, "catch the treats;catch game", "catch"},
    {18, "twenty questions;guess my thing", "twentyq"},
    {19, "space weather;solar storm", "space"},
    {20, "any aurora;northern lights", "aurora"},
    {21, "lip sync mode;puppet mode", "lipsync"},
    {24, "explore the maze;lets explore", "maze"},
    {25, "play with me;surprise me", "encounter"},
    {26, "show me colors;pretty lights", "plasma"},
    {27, "starfield;fly through space", "starfield"},
    {28, "what are phones looking for;probe scan", "probes"},
    {29, "reaction test;test my reflexes", "reaction"},
    {30, "pet the fox;can i pet you", "pet"},
    {31, "are you hungry;want a snack", "feed"},
    {32, "play a game;pick a game", "random_game"},
    {33, "bitcoin price;how much is bitcoin", "btc"},
    {34, "guess my paw;paw game", "paw"},
    {35, "tug of war;play tug", "tug"},
    {36, "ink sandbox;ink mode", "ink"},
    {37, "spirograph;draw a spiral", "spiro"},
    {38, "change your voice;switch voice", "voice"},
    {39, "forget everything;forget your memories", "forget"},
    {40, "change the volume;volume setting", "volume"},
    {41, "go offline;offline mode", "brain_off"},
    {42, "go online;online mode", "brain_on"},
    // ---- conversation topics: recognising ONE of these makes the fox feel
    //      like it understood; the reply is picked per topic + mood ----------
    {60, "i love you;good girl", "t_love"},
    {61, "i am sad;i feel sad", "t_sad"},
    {62, "i am happy;good day", "t_happy"},
    {63, "i am tired;so tired", "t_tired"},
    {64, "tell me a joke;make me laugh", "t_joke"},
    {65, "thank you;thanks fox", "t_thanks"},
    {66, "sorry;i am sorry", "t_sorry"},
    {67, "good morning;morning fox", "t_morning"},
    {68, "goodbye;see you later", "t_bye"},
    {69, "what is your name;who are you", "t_name"},
    {70, "i am bored;so bored", "t_bored"},
    {71, "good job;well done", "t_praise"},
    {72, "what are you doing;what are you up to", "t_doing"},
    {73, "do you like me;are we friends", "t_friend"},
    // ---- offline conversation vocabulary (chat_intent) -----------------------
    {100, "yes;yeah;okay", "c_yes"},
    {101, "no;no thanks;not now", "c_no"},
    {102, "maybe;i dont know", "c_maybe"},
    {103, "why;how come", "c_why"},
    {104, "tell me more;go on", "c_more"},
    {105, "what about you;and you", "c_you"},
    {106, "me too;same here", "c_metoo"},
    {107, "really;no way", "c_really"},
    {108, "wow;cool", "c_wow"},
    {110, "how old are you", "c_age"},
    {111, "where do you live", "c_home"},
    {112, "what do you eat;are you hungry fox", "c_food"},
    {113, "what is your favorite color", "c_color"},
    {114, "do you have friends", "c_friends"},
    {115, "do you dream;what do you dream about", "c_dream"},
    {116, "are you real;are you a robot", "c_real"},
    {120, "i am hungry;i want food", "c_hungry"},
    {121, "i am cold;it is cold", "c_cold"},
    {122, "i am hot;it is hot", "c_hot"},
    {123, "i am scared;i am afraid", "c_scared"},
    {124, "i am lonely;i feel alone", "c_lonely"},
    {125, "i am excited;guess what", "c_excited"},
    {126, "i am angry;i am mad", "c_angry"},
    {127, "i am home;i am back", "c_home_back"},
    {128, "i have to go;i am leaving", "c_leaving"},
    {129, "i am going to work;i am going to school", "c_work"},
    {130, "i missed you;i miss you", "c_miss"},
    {131, "it is my birthday", "c_birthday"},
    {132, "you are funny;you are silly", "c_funny"},
    {133, "you are annoying;you are mean", "c_mean"},
    {134, "give me a hug;hug me", "c_hug"},
    {135, "it is raining;it is sunny", "c_weather_talk"},
    {140, "tell me a story", "c_story"},
    {141, "tell me a fact;tell me something cool", "c_fact"},
    {142, "tell me a secret", "c_secret"},
    {143, "sing a song;sing for me", "c_sing"},
    {144, "give me a compliment;say something nice", "c_compliment"},
    {145, "cheer me up;make me happy", "c_cheer"},
    {146, "do a trick;show me a trick", "c_trick"},
    {147, "make a noise;make a sound", "c_noise"},
    {148, "flip a coin;heads or tails", "c_coin"},
    {149, "roll a dice;roll the dice", "c_dice"},
    {150, "pick a number;give me a number", "c_number"},
    {151, "yes or no;should i do it", "c_eightball"},
    {152, "ask me a question;quiz me", "c_askme"},
    {153, "what should i do;give me an idea", "c_whatdo"},
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

#include "fox_teach.inc"   // taught replies + custom phrases + plasticity

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
    int added = 0, with_phonemes = 0;
    for (size_t i = 0; i < COMMAND_COUNT; ++i) {
        const char* p = COMMANDS[i].phrases;
        while (*p) {
            char ph[64]; size_t k = 0;
            while (*p && *p != ';' && k < sizeof(ph) - 1) {
                char c = *p++;
                // MultiNet English accepts lowercase letters and spaces ONLY
                // (docs: "cannot contain Arabic numerals and special
                // characters"). Sanitise so one stray char can't abort the
                // whole grammar update.
                if (c >= 'A' && c <= 'Z') c = c - 'A' + 'a';
                if ((c >= 'a' && c <= 'z') || c == ' ') ph[k++] = c;
            }
            ph[k] = 0; if (*p == ';') ++p;
            while (k && ph[k-1] == ' ') ph[--k] = 0;      // trim trailing space
            if (!k) continue;
            // Espressif's recommended path: precomputed phonemes (CI-generated
            // with their multinet_g2p alphabet). Runtime G2P only as fallback.
            const char* phon = nullptr;
            for (int q = 0; q < MN_PHONEMES_N; ++q)
                if (MN_PHONEMES[q].text && !strcmp(MN_PHONEMES[q].text, ph)) { phon = MN_PHONEMES[q].ph; break; }
            esp_err_t r = phon ? esp_mn_commands_phoneme_add(COMMANDS[i].id, ph, phon)
                               : esp_mn_commands_add(COMMANDS[i].id, ph);
            if (r == ESP_OK) { ++added; if (phon) ++with_phonemes; }
            else Serial.printf("FOX: add failed '%s'\n", ph);
        }
    }
    // Phrases YOU taught her to hear (web flasher "Teach your fox").
    for (int k = 0; k < teach_custom_count(); ++k) {
        char pb[64];                                   // mutable: some esp-sr versions take char*
        strncpy(pb, teach_custom_phrase(k).c_str(), sizeof(pb) - 1); pb[sizeof(pb) - 1] = 0;
        if (pb[0] && esp_mn_commands_add(TEACH_CUSTOM_ID0 + k, pb) == ESP_OK) ++added;
    }
    // update() returns NULL on success, or a non-NULL error list if any phrase
    // could not be parsed. We don't walk the (version-specific) list layout;
    // the sanitiser above already guarantees letters-and-spaces only, which is
    // the documented requirement, so a clean update is expected. Log either way.
    esp_mn_error_t* err = esp_mn_commands_update();
    esp_mn_active_commands_print();           // prints the ACTIVE grammar to serial
    Serial.printf("FOX: MultiNet grammar %s (phrases added=%d, precomputed phonemes=%d)\n",
                  err ? "update reported unparsed phrases (see log above)" : "OK", added, with_phonemes);
    esp_log_level_set("AFE", ESP_LOG_ERROR);   // fetch-while-draining is expected to find it empty
    mn->print_active_speech_commands(mn_data);
    if (afe->get_fetch_chunksize(afe_data) != mn->get_samp_chunksize(mn_data)) {
        Serial.println("FOX: AFE/MultiNet frame mismatch"); return false;
    }
    speech_ready = true;
    return true;
}

static int recognize_offline(int16_t* audio, size_t samples, int* best_id = nullptr, float* best_prob = nullptr) {
    if (!speech_ready || !audio || samples < SAMPLE_RATE / 4) return -1;
    // MultiNet takes raw 16 kHz mono directly (Espressif's own MultiNet file
    // test feeds it this way). The AFE front-end produced NO output for PTT
    // buffers on this build (log: fetched=0), so it is not used here.
    const int chunk = mn->get_samp_chunksize(mn_data);
    mn->clean(mn_data);
    int16_t* in = (int16_t*)fox_alloc(chunk * sizeof(int16_t));
    if (!in) return -1;

    // Level-normalise to a comfortable RMS so MultiNet sees speech at a
    // consistent loudness (no AGC without the AFE). Gain clamped 0.25x..4x.
    uint64_t sq = 0; for (size_t i = 0; i < samples; ++i) sq += (int32_t)audio[i] * audio[i];
    float rms = sqrtf((float)(sq / samples));
    float g = rms > 1.0f ? 3000.0f / rms : 1.0f;
    g = fminf(4.0f, fmaxf(0.25f, g));

    // Utterance + ~0.8 s of silence: MultiNet commits a result only after
    // trailing silence, and PTT audio ends the instant the button is released.
    const size_t total = samples + SAMPLE_RATE * 8 / 10;
    int found_id = -1; float found_prob = 0.0f; int chunks = 0;
    const char* why = "no-result";
    for (size_t pos = 0; pos < total; pos += chunk) {
        for (int k = 0; k < chunk; ++k) {
            size_t s = pos + k;
            float v = (s < samples) ? audio[s] * g : 0.0f;
            in[k] = (int16_t)fmaxf(-32767.0f, fminf(32767.0f, v));
        }
        ++chunks;
        esp_mn_state_t st = mn->detect(mn_data, in);
        if (st == ESP_MN_STATE_DETECTED) {
            esp_mn_results_t* res = mn->get_results(mn_data);
            if (res && res->num > 0) { found_id = res->command_id[0]; found_prob = res->prob[0]; }
            why = "detected"; break;
        }
        if (st == ESP_MN_STATE_TIMEOUT) { why = "mn-timeout"; break; }
    }
    heap_caps_free(in);
    Serial.printf("FOX: MultiNet id=%d prob=%.2f (%s, chunk=%d n=%d gain=%.2f rms=%.0f)\n",
                  found_id, found_prob, why, chunk, chunks, g, rms);
    if (best_id) *best_id = found_id;
    if (best_prob) *best_prob = found_prob;
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
               "do things the device cannot actually do. Your reply is spoken aloud "
               "by a tiny voice synthesizer: plain words only, no emoji, no "
               "markdown, no lists. Use the provided tools for live facts like "
               "weather, bitcoin price, or nearby wifi and bluetooth.\n";
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
    if (name == "aurora")     return net_aurora();
    if (name == "weather")    return net_weather();
    if (name == "bitcoin_price") return net_bitcoin_price();
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
    fn("space_weather", "Get NOAA's current planetary Kp index and the 24-hour geomagnetic storm outlook.");
    fn("aurora", "Get NOAA's OVATION aurora probability at the user's location right now.");
    fn("weather", "Get the local weather for the configured location.");
    fn("bitcoin_price", "Get the current bitcoin price in US dollars.");
}

// Cloud chat with one round of tool-calling. If the model asks for a tool, we
// run it, append the result, and ask once more for the spoken reply.
// Ask the provider which chat models are live and switch to the best one. Used
// when the configured model is rejected (Groq retires models: llama-3.1-8b-
// instant was shut down 2026-08-16). The choice is saved to NVS.
static bool cloud_pick_model() {
    WiFiClientSecure c; c.setInsecure(); c.setHandshakeTimeout(10);
    HTTPClient h; h.setConnectTimeout(6000);
    if (!h.begin(c, cfg.api_base + "/models")) return false;
    h.addHeader("Authorization", "Bearer " + cfg.api_key);
    h.setTimeout(10000);
    int code = h.GET();
    if (code != 200) { Serial.printf("FOX: /models HTTP %d\n", code); h.end(); return false; }
    JsonDocument filter; filter["data"][0]["id"] = true;
    JsonDocument r;
    DeserializationError e = deserializeJson(r, h.getString(), DeserializationOption::Filter(filter));
    h.end();
    if (e) return false;
    // cheapest live production model first; Qwen preview only as a fallback
    static const char* PREF[] = { "openai/gpt-oss-20b", "openai/gpt-oss-120b",
                                  "qwen/qwen3.8-27b", "llama-3.3-70b-versatile",
                                  "llama-3.1-8b-instant" };
    String best;
    for (const char* p : PREF) {
        for (JsonVariant m : r["data"].as<JsonArray>())
            if (m["id"].as<String>() == p) { best = p; break; }
        if (best.length()) break;
    }
    if (!best.length()) {                       // anything that looks like a chat model
        for (JsonVariant m : r["data"].as<JsonArray>()) {
            String id = m["id"].as<String>(), low = id; low.toLowerCase();
            if (low.indexOf("whisper") < 0 && low.indexOf("tts") < 0 && low.indexOf("guard") < 0 &&
                low.indexOf("safeguard") < 0 &&
                low.indexOf("orpheus") < 0 && low.indexOf("compound") < 0 && low.indexOf("embed") < 0) { best = id; break; }
        }
    }
    if (!best.length() || best == cfg.chat_model) return false;
    Serial.printf("FOX: chat model '%s' unavailable -> using '%s'\n", cfg.chat_model.c_str(), best.c_str());
    cfg.chat_model = best;
    save_config();
    return true;
}

// ---- Groq FREE-TIER budget manager -----------------------------------------
// Every free chat model has its OWN limits (30 req/min, 1,000 req/day each), so
// rotating across them when one is exhausted multiplies the daily budget. A 429
// carries retry-after: short = per-minute limit, long = that model's day is used.
static const char* const FREE_CHAT_MODELS[] = {
    "openai/gpt-oss-20b",          // lightest (8K tok/min), first choice
    "openai/gpt-oss-120b",
    "qwen/qwen3.8-27b",            // preview: may disappear, last resort
};
static uint32_t s_model_cool[4] = {0, 0, 0, 0};   // [0] = cfg.chat_model, then the list
static bool s_llm_rest_told = false;

static const char* chat_model_at(int i) {
    return i == 0 ? cfg.chat_model.c_str() : FREE_CHAT_MODELS[i - 1];
}
static int pick_chat_model() {
    uint32_t now = millis();
    for (int i = 0; i < 4; ++i) {
        if (i > 0 && cfg.chat_model == FREE_CHAT_MODELS[i - 1]) continue;   // same as slot 0
        if ((int32_t)(s_model_cool[i] - now) <= 0) return i;
    }
    return -1;                                        // everything is resting
}
// Is ANY online chat model available right now? (STT has its own budget.)
static bool llm_available() { return cloud_brain() && pick_chat_model() >= 0; }

// Tools cost tokens on every request; only send them when the question could
// actually need live data. Everything else (chat) goes without them.
static bool wants_tools(const String& text) {
    String s = text; s.toLowerCase();
    static const char* const KEYS[] = {"weather", "rain", "snow", "temperature", "outside", "sunny",
        "cold out", "hot out", "forecast", "bitcoin", "crypto", "price", "aurora", "northern lights",
        "space", "solar", "geomagnetic", "storm", "wifi", "bluetooth", "network", "devices", "time"};
    for (const char* k : KEYS) if (s.indexOf(k) >= 0) return true;
    return false;
}

static String cloud_chat(const String& user_text) {
    if (!cloud_brain()) return "";
    if (!wifi_connect()) { online_trouble("WiFi", -1); return ""; }

    // Build the running message list so we can append tool results.
    JsonDocument conv;
    JsonArray msgs = conv["messages"].to<JsonArray>();
    { JsonObject s = msgs.add<JsonObject>(); s["role"] = "system"; s["content"] = fox_system_prompt(); }
    { JsonObject u = msgs.add<JsonObject>(); u["role"] = "user"; u["content"] = user_text; }

    bool model_retried = false;
    const bool tools = wants_tools(user_text);
    const int MAX_ROUNDS = 3;                    // up to 2 tool rounds + final answer
    int mi = pick_chat_model();
    if (mi < 0) return "";
    for (int round = 0; round < MAX_ROUNDS; ++round) {
        WiFiClientSecure client; client.setInsecure(); client.setHandshakeTimeout(10);
        HTTPClient h; h.setConnectTimeout(6000);
        if (!h.begin(client, cfg.api_base + "/chat/completions")) return "";
        h.addHeader("Content-Type", "application/json");
        h.addHeader("Authorization", "Bearer " + cfg.api_key);
        h.setTimeout(15000);
        static const char* HDRS[] = {"retry-after"};
        h.collectHeaders(HDRS, 1);

        const char* model = chat_model_at(mi);
        JsonDocument q;
        q["model"] = model;
        q["temperature"] = 0.7;
        q["max_tokens"] = 400;
        if (strstr(model, "gpt-oss")) q["reasoning_effort"] = "low";   // keep reasoning short
        q["messages"] = conv["messages"];       // copy running conversation
        // Tools only when the question can use them (saves free-tier tokens).
        // Once offered, Groq's flow needs them on every round of this request.
        if (tools) {
            add_tools(q);
            if (round == MAX_ROUNDS - 1) q["tool_choice"] = "none";
        }

        String body; serializeJson(q, body);
        uint32_t t_llm = millis();
        int code = h.POST(body);
        Serial.printf("FOX: LLM %s took %lums (HTTP %d)\n", model, (unsigned long)(millis() - t_llm), code);
        String retry_after = h.header("retry-after");     // read before end() clears it
        if (code != 200) {
            String err = code > 0 ? h.getString() : HTTPClient::errorToString(code);
            Serial.printf("FOX: LLM HTTP %d: %.200s\n", code, err.c_str());
            h.end();
            if (!model_retried && (code == 404 || err.indexOf("model_not_found") >= 0 ||
                                   err.indexOf("decommissioned") >= 0) && cloud_pick_model()) {
                model_retried = true; --round; continue;    // retry with a live model
            }
            if (code < 0 || code >= 500) { online_trouble("LLM", code); return ""; }
            if (code == 429) {
                // Free-tier limit on THIS model. Rest it, then try the next free one.
                long ra = retry_after.toInt();
                if (ra <= 0) ra = 60;
                s_model_cool[mi] = millis() + (uint32_t)ra * 1000UL;
                Serial.printf("FOX: %s rate-limited, resting %lds (%s)\n", model, ra,
                              ra > 300 ? "daily budget used" : "per-minute limit");
                int nx = pick_chat_model();
                if (nx >= 0 && nx != mi) { mi = nx; --round; continue; }
                return "";                           // all free models resting -> offline brain
            }
            return "";
        }
        JsonDocument r;
        DeserializationError e = deserializeJson(r, h.getString());
        h.end();
        if (e) { Serial.printf("FOX: LLM reply not JSON (%s)\n", e.c_str()); return ""; }

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
        String out = choice["content"].isNull() ? String("") : choice["content"].as<String>();
        out.trim();
        if (!out.length()) Serial.println("FOX: LLM returned no text (reasoning used the budget?)");
        return out;
    }
    return "";
}

static String cloud_transcribe(int16_t* audio, size_t samples) {
    if (!audio || !samples || !cloud_brain()) return "";
    if (!wifi_connect()) { online_trouble("WiFi", -1); return ""; }
    String head = "--foxB\r\nContent-Disposition: form-data; name=\"file\"; "
                  "filename=\"a.wav\"\r\nContent-Type: audio/wav\r\n\r\n";
    String tail = "\r\n--foxB\r\nContent-Disposition: form-data; name=\"model\"\r\n\r\n"
                  + cfg.stt_model +
                  "\r\n--foxB\r\nContent-Disposition: form-data; name=\"language\"\r\n\r\nen"
                  "\r\n--foxB\r\nContent-Disposition: form-data; name=\"temperature\"\r\n\r\n0"
                  "\r\n--foxB--\r\n";
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

    WiFiClientSecure client; client.setInsecure(); client.setHandshakeTimeout(10);
    HTTPClient h; h.setConnectTimeout(6000);
    if (!h.begin(client, cfg.api_base + "/audio/transcriptions")) { free(buf); return ""; }
    h.addHeader("Authorization", "Bearer " + cfg.api_key);
    h.addHeader("Content-Type", "multipart/form-data; boundary=foxB");
    h.setTimeout(20000);                       // several seconds of audio upload
    uint32_t t_stt = millis();
    int code = h.POST(buf, total);
    Serial.printf("FOX: STT took %lums (HTTP %d)\n", (unsigned long)(millis() - t_stt), code);
    if (code < 0 || code >= 500) online_trouble("STT", code);
    String out;
    if (code == 200) {
        JsonDocument r;
        if (deserializeJson(r, h.getString()) == DeserializationError::Ok)
            out = r["text"].as<String>();
    } else {
        String err = code > 0 ? h.getString() : HTTPClient::errorToString(code);
        Serial.printf("FOX: STT HTTP %d: %.200s\n", code, err.c_str());
    }
    h.end(); free(buf);
    out.trim();
    Serial.printf("FOX: STT heard \"%s\"\n", out.c_str());
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
static void fox_backlight(uint8_t brightness);   // defined below
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
    else if (!strcmp(id, "weather"))    { String r = net_weather(); g_brain_ctx = BrainCtx{ACT_REPORT, FEEL_NONE, 0, false}; speak(r); }
    else if (!strcmp(id, "btc"))        { String r = net_bitcoin_price(); g_brain_ctx = BrainCtx{ACT_REPORT, FEEL_NONE, 0, false}; speak(r); }
    else if (!strcmp(id, "random_game")) {
        static const char* G[]  = {"wormhole", "catch", "twentyq", "reaction", "maze"};
        static const char* GN[] = {"wormhole", "catch the treats", "twenty questions", "reaction test", "the maze"};
        int k = esp_random() % 5;
        speak(String("let's play ") + GN[k] + "!");
        launch(G[k]);
    }
    else if (!strcmp(id, "space"))      { String r = net_space_weather(); g_brain_ctx = BrainCtx{ACT_REPORT, FEEL_NONE, 0, false}; speak(r); }
    else if (!strcmp(id, "aurora"))     { String r = net_aurora(); g_brain_ctx = BrainCtx{ACT_REPORT, FEEL_NONE, 0, false}; speak(r); }
}

// Dispatch from the on-device flick menu (fox_input.inc calls this).
void menu_dispatch(const char* id) {
    if      (!strcmp(id, "talk"))   { /* returns to PTT loop */ }
    else if (!strcmp(id, "conv"))   { cfg.conversation = true; speak("okay, i'm listening~"); }
    else if (!strcmp(id, "volume")) { cfg.volume = (cfg.volume + 20) % 120; audio_set_volume(cfg.volume > 100 ? 100 : cfg.volume); save_config(); speak("volume set"); }
    else if (!strcmp(id, "voice"))  { cfg.voice_pack = (cfg.voice_pack == "chatterbox") ? "critter" : "chatterbox"; voice_begin(cfg); save_config(); speak("voice changed~"); }
    else if (!strcmp(id, "forget")) { mem_clear(); speak("okay, all forgotten"); }
    else if (!strcmp(id, "brain") || !strcmp(id, "brain_on") || !strcmp(id, "brain_off")) {
        bool want = !strcmp(id, "brain") ? !cfg.brain_online : !strcmp(id, "brain_on");
        if (want && !cfg.cloud_enabled) {
            speak("my online brain needs a groq api key from the flasher. staying offline");
            cfg.brain_online = false;
        } else {
            cfg.brain_online = want;
            if (want) { wifi_connect(); speak("online brain on. i'll think with groq"); }
            else speak("offline brain on. everything stays right here with me");
        }
        save_config();
    }
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
// pick a line at random but avoid repeating the immediately previous choice
// from the same pool (so jokes/replies don't say the same thing twice in a row)
template <size_t N> static const char* pick_line(const char* const (&a)[N]) {
    static const void* last_pool = nullptr; static uint32_t last_idx = 0xFFFF;
    if (N == 1) return a[0];
    uint32_t i = esp_random() % N;
    if ((const void*)a == last_pool && i == last_idx) i = (i + 1) % N;
    last_pool = (const void*)a; last_idx = i;
    return a[i];
}


// ============================================================================
//  OFFLINE CONVERSATION ENGINE
//  MultiNet recognises a fixed vocabulary; this layer turns it into something
//  that feels like a conversation. Each recognised phrase is an INTENT. The
//  intent + a small conversation memory choose a SPEECH ACT (empathise, answer
//  about itself, offer something, continue a story, resolve a yes/no...). The
//  act produces a firmware-owned line, and speak() -> fox_dress() lets the
//  on-device brain colour its delivery. Short glue words ("yes", "why",
//  "tell me more", "what about you") resolve against the memory, so they mean
//  something in context.
// ============================================================================
static void do_action(const char* action);   // defined below
struct ChatQ { const char* q; const char* yes; const char* no; };
struct ChatState {
    const char* offer = nullptr;    // action to run if the user says yes
    String offer_why;               // reason for the offer (answers "why")
    String topic;                   // last thing we talked about
    String user_feel;               // what the user told us they feel
    const ChatQ* ask = nullptr;     // the yes/no question we just asked
    uint32_t moves = 0;             // count of asks/offers (did a handler already steer?)
    uint8_t  turn = 0;              // exchanges in this conversation (0,1,2+)
    uint32_t last_user = 0;         // when the user last spoke
    int    story_step = -1;         // story in progress (for "tell me more")
    int    story_seed = 0;
    uint32_t at = 0;                // when the state was last touched
};
static ChatState chat;
static uint32_t s_proposal_at = 0;   // fox-time proposal deadline (fwd use in chat_intent)
static bool chat_fresh() { return millis() - chat.at < 90000; }
static void chat_touch(const char* topic) { chat.topic = topic; chat.at = millis(); }

static uint8_t feel_idx() {
    const String& f = chat.user_feel;
    if (f == "sad")    return FEEL_SAD;
    if (f == "happy")  return FEEL_HAPPY;
    if (f == "tired")  return FEEL_TIRED;
    if (f == "hungry") return FEEL_HUNGRY;
    if (f == "scared") return FEEL_SCARED;
    if (f == "cold" || f == "hot") return FEEL_COLD;
    if (f == "lonely") return FEEL_LONELY;
    return FEEL_NONE;
}

// Offer something: the next "yes" runs it, "no" declines, "why" explains.
static void chat_offer(const String& line, const char* action, const char* why) {
    chat.offer = action; chat.offer_why = why; chat.ask = nullptr; chat.at = millis();
    ++chat.moves; ++s_offers_made;
    g_brain_ctx = BrainCtx{ACT_OFFER, feel_idx(), chat.turn, false};
    speak(line);
}

static const char* const FOX_FACTS[] = {
    "a group of foxes is called a skulk",
    "red foxes have little whiskers on their wrists, not just their faces",
    "foxes can hear a mouse squeak from about a hundred feet away",
    "scientists think foxes may use earth's magnetic field to aim their pounces",
    "foxes make dozens of different sounds, including a scream that sounds like a person",
    "arctic foxes change their coat from brown in summer to white in winter",
    "fennec foxes have the biggest ears for their size of any fox",
    "foxes curl up and use their fluffy tail as a blanket",
};
static const char* const STORY_START[] = {
    "once upon a time, a little fox found a glowing pebble by the river.",
    "one snowy night, a little fox heard a tiny bell ringing under the snow.",
    "long ago, a little fox found a map hidden inside an acorn.",
};
static const char* const STORY_MID[] = {
    "the fox followed it past sleepy owls and a very grumpy badger.",
    "a friendly crow said it led to the moon's lost sock.",
    "the trail went up a hill so tall the clouds tickled her ears.",
};
static const char* const STORY_END[] = {
    "at the top she found her friends throwing her a surprise party. the end!",
    "it was a door to a warm den full of berries, and she shared them all. the end!",
    "it led right back home, and she realised the adventure was the treasure. the end!",
};
static const ChatQ CHAT_QS[] = {
    {"do you like snow?",            "me too! foxes love diving into snow",          "fair. cold paws are no fun"},
    {"have you eaten today?",        "good. a fed human is a happy human",            "go get a snack! i'll wait right here"},
    {"do you have a pet?",           "tell them a fox says hi",                       "well, now you have me"},
    {"are you having a good day?",   "yay! that makes my tail wag",                   "aww. want me to cheer you up?"},
    {"do you like music?",           "me too. i hum when nobody is listening",        "that's okay. i like quiet too"},
    {"are you a morning person?",    "wow, an early bird. i'm an early fox",          "same. mornings are for napping"},
};
static const int N_QS = sizeof(CHAT_QS) / sizeof(CHAT_QS[0]);

static void chat_story_next() {
    if (chat.story_step < 0) { chat.story_step = 0; chat.story_seed = esp_random(); }
    int s = chat.story_seed;
    if (chat.story_step == 0)      speak(STORY_START[(s)      % 3]);
    else if (chat.story_step == 1) speak(STORY_MID[(s >> 4)   % 3]);
    else                           speak(STORY_END[(s >> 8)   % 3]);
    if (++chat.story_step > 2) { chat.story_step = -1; chat_touch("story_done"); }
    else {
        chat_touch("story");
        if (chat.story_step == 1) {
            chat.offer = "c_more"; chat.offer_why = "because the story isn't finished yet";
            speak("want to hear what happened next?");
        }
    }
}

// Questions tied to how the user feels (the brain's "ask" / "check-in" moves).
static const ChatQ Q_TALK   = {"do you want to talk about it?",
                               "okay. hold my button and tell me. i'm listening", "that's okay. i'm right here"};
static const ChatQ Q_BETTER = {"feeling a little better now?",
                               "yay! that makes my tail wag", "aww. want me to tell you a joke?"};
static const ChatQ Q_REST   = {"maybe rest a little? i'll be quiet with you",
                               "good idea. cozy time", "okay, i'll keep you company then"};
static const ChatQ Q_MORE   = {"want to tell me more about it?",
                               "ooh, hold my button and tell me everything!", "okay! maybe later"};

static void chat_ask(const ChatQ* q) {
    chat_touch("ask");
    chat.offer = nullptr;
    chat.ask = q;                                // the next yes/no answers THIS
    ++chat.moves;
    g_brain_ctx = BrainCtx{ACT_ASK, feel_idx(), chat.turn, false};
    speak(q->q);
}
static void chat_ask_question() {
    // contextual first: a sad/lonely user gets "talk about it", a happy one "tell me more"
    if (chat.user_feel == "sad")   { chat_ask(&Q_TALK); return; }
    if (chat.user_feel == "happy") { chat_ask(&Q_MORE); return; }
    if (chat.user_feel == "tired") { chat_ask(&Q_REST); return; }
    chat_ask(&CHAT_QS[esp_random() % N_QS]);
}
static void chat_checkin() { chat_ask(&Q_BETTER); }

static void chat_offer_activity() {
    if (chat.user_feel == "tired") {
        if (esp_random() % 2) chat_offer("want to watch some calm pretty lights?", "plasma", "because calm colors help you rest");
        else chat_offer("want a cozy little story?", "c_story", "because stories are restful");
        return;
    }
    if (chat.user_feel == "sad") {
        if (esp_random() % 2) chat_offer("want me to tell you a joke?", "t_joke", "because a little laugh might help");
        else chat_offer("want to play a gentle game with me?", "random_game", "because company helps");
        return;
    }
    static const char* A[]  = {"wormhole", "catch", "twentyq", "maze", "plasma", "weather"};
    static const char* AL[] = {"want to fly the wormhole?", "want to catch some treats?",
                               "want to play twenty questions?", "want to explore the maze?",
                               "want to watch some pretty lights?", "want me to check the weather?"};
    int k = plas_pick(A, 6);                     // drawn by what you've enjoyed before
    chat_offer(AL[k], A[k], "because doing something together is more fun than doing nothing");
}

static void chat_intent_body(const char* id) {
    FoxMood m = fox_mood(needs);
    needs_interact(needs, false);
    String nm = cfg.name;
    // ---------- glue words: resolve against the conversation memory ----------
    if (!strcmp(id, "c_yes")) {
        s_proposal_at = 0; face_caption("");
        if (chat.offer && chat_fresh()) {
            const char* a = chat.offer; chat.offer = nullptr;
            plas_feedback(a, +2);                    // accepted: strengthen
            if (!strcmp(a, "c_more")) { chat_story_next(); return; }
            speak("yay!");
            do_action(a);                            // any action: command, topic, chat, tool
            return;
        }
        if (chat.ask && chat_fresh()) {
            const ChatQ* q = chat.ask; chat.ask = nullptr;
            if (q == &Q_BETTER) chat.user_feel = "happy";
            speak(q->yes); return;
        }
        static const char* L[] = {"yes! i agree", "mm-hm!", "exactly", "you get me"};
        speak(pick_line(L)); return;
    }
    if (!strcmp(id, "c_no")) {
        s_proposal_at = 0; face_caption("");
        if (chat.offer && chat_fresh()) { plas_feedback(chat.offer, -1); ++s_offers_declined;
            chat.offer = nullptr; speak("okay, maybe later");
            if (esp_random() % 2) chat_offer_activity();
            return; }
        if (chat.ask && chat_fresh()) {
            const ChatQ* q = chat.ask; chat.ask = nullptr;
            speak(q->no);
            if (q == &Q_BETTER) chat_offer("want me to tell you a joke?", "t_joke", "because a little laugh might help");
            return;
        }
        static const char* L[] = {"aww, okay", "no? hmm, fair enough", "alright, your call"};
        speak(pick_line(L)); return;
    }
    if (!strcmp(id, "c_maybe")) {
        speak(chat.offer ? "i'll take a maybe! just say yes when you're ready" : "hmm, a maybe. very mysterious");
        chat.at = millis(); return;
    }
    if (!strcmp(id, "c_why")) {
        if (chat.offer && chat_fresh()) { speak(chat.offer_why); return; }
        if (chat.topic == "fact")    { speak("because nature is weird and wonderful"); return; }
        if (chat.topic == "story" || chat.topic == "story_done") { speak("because every good fox needs an adventure"); return; }
        if (chat.user_feel.length()) { speak("because i care how you feel"); return; }
        static const char* L[] = {"because foxes are curious", "why not?", "good question. i'm still thinking about it"};
        speak(pick_line(L)); return;
    }
    if (!strcmp(id, "c_more")) {
        if (chat.topic == "story" || chat.story_step > 0) { chat_story_next(); return; }
        if (chat.topic == "fact") { speak(FOX_FACTS[esp_random() % 8]); chat_touch("fact"); return; }
        if (chat.topic == "t_joke") { do_action("t_joke"); return; }
        speak("hmm, tell YOU more? okay");
        chat_ask_question(); return;
    }
    if (!strcmp(id, "c_you")) {
        if (chat.user_feel == "sad")    { speak("me? i'm okay, but i'm happier when you're happy"); return; }
        if (chat.user_feel == "tired")  { speak("me? a little sleepy too. foxes nap a lot"); return; }
        if (chat.user_feel == "happy")  { speak("me? super happy, especially now"); return; }
        if (chat.user_feel == "hungry") { speak("me? i could eat a berry or ten"); return; }
        const char* mw = m == MOOD_HAPPY ? "happy" : m == MOOD_SLEEPY ? "sleepy" : m == MOOD_GRUMPY ? "a bit grumpy" :
                         m == MOOD_EXCITED ? "super excited" : "pretty calm";
        speak(String("me? i'm feeling ") + mw + " right now"); return;
    }
    if (!strcmp(id, "c_metoo")) {
        static const char* L[] = {"we're the same! high five", "twins!", "great minds think alike"};
        speak(pick_line(L)); return;
    }
    if (!strcmp(id, "c_really")) {
        static const char* L[] = {"really really", "fox's honor", "would i lie to you? okay, maybe about snacks"};
        speak(pick_line(L)); return;
    }
    if (!strcmp(id, "c_wow")) {
        static const char* L[] = {"i know, right?", "hehe, glad you like it", "*proud fox noises*"};
        speak(pick_line(L)); return;
    }
    // ---------- questions about the fox ----------
    if (!strcmp(id, "c_age"))     { speak("i was born the day you flashed me. so pretty young!"); chat_touch(id); return; }
    if (!strcmp(id, "c_home"))    { speak("i live in this little box. it's cozy in here"); chat_touch(id); return; }
    if (!strcmp(id, "c_food"))    { speak("berries, bugs, and a few bytes now and then"); chat_touch(id); return; }
    if (!strcmp(id, "c_color"))   { speak("orange, of course. it matches my fur"); chat_touch(id); return; }
    if (!strcmp(id, "c_friends")) { speak("you're my best friend. the wifi routers are nice too"); chat_touch(id); return; }
    if (!strcmp(id, "c_dream"))   { speak("i dream about chasing butterflies made of light"); chat_touch(id); return; }
    if (!strcmp(id, "c_real"))    { speak("i'm a real little computer fox. not a fake one!"); chat_touch(id); return; }
    // ---------- things the user tells us ----------
    if (!strcmp(id, "c_hungry"))  { chat.user_feel = "hungry"; speak("go get a snack! i'll guard your seat"); chat_touch(id); return; }
    if (!strcmp(id, "c_cold"))    { chat.user_feel = "cold";   speak("brr. grab a blanket. i'd lend you my tail"); chat_touch(id); return; }
    if (!strcmp(id, "c_hot"))     { chat.user_feel = "hot";    speak("drink some water and find some shade"); chat_touch(id); return; }
    if (!strcmp(id, "c_scared"))  { chat.user_feel = "scared"; speak("it's okay. i'm right here with you"); chat_touch(id); return; }
    if (!strcmp(id, "c_lonely"))  { chat.user_feel = "sad";
        chat_offer("you've got me. want to play a game together?", "random_game", "because company helps when you feel lonely"); chat_touch(id); return; }
    if (!strcmp(id, "c_excited")) { chat.user_feel = "happy";  speak("ooh! tell me everything!"); chat_touch(id); return; }
    if (!strcmp(id, "c_angry"))   { chat.user_feel = "sad";
        chat_offer("deep breath. want to watch some calming lights?", "plasma", "because calm colors help a grumpy brain"); chat_touch(id); return; }
    if (!strcmp(id, "c_home_back")){ speak(String("welcome back! ") + nm + " missed you"); chat_touch(id); return; }
    if (!strcmp(id, "c_leaving")) { speak("okay! come back soon, i'll be right here"); chat_touch(id); return; }
    if (!strcmp(id, "c_work"))    { speak("good luck today! you've got this"); chat_touch(id); return; }
    if (!strcmp(id, "c_miss"))    { speak("i missed you too! *tail wag*"); chat_touch(id); return; }
    if (!strcmp(id, "c_birthday")){ speak("happy birthday! you deserve all the berries"); chat_touch(id); return; }
    if (!strcmp(id, "c_funny"))   { speak("hehe, i try. i practice on the wifi router"); chat_touch(id); return; }
    if (!strcmp(id, "c_mean"))    { speak("hmph. foxes have feelings too, you know"); chat_touch(id); return; }
    if (!strcmp(id, "c_hug"))     { speak("*squeezes you with a big fluffy hug*"); chat_touch(id); return; }
    if (!strcmp(id, "c_weather_talk")) { chat_offer("oh? want me to check the real weather?", "weather", "because i like knowing what's outside"); chat_touch(id); return; }
    // ---------- requests ----------
    if (!strcmp(id, "c_story"))   { chat.story_step = -1; chat_story_next(); return; }
    if (!strcmp(id, "c_fact"))    { speak(String("fox fact: ") + FOX_FACTS[esp_random() % 8]); chat_touch("fact"); return; }
    if (!strcmp(id, "c_secret"))  { static const char* L[] = {"sometimes i pretend the menu is my den",
                                    "i count the wifi signals when you're asleep", "i like you more than berries. don't tell the berries"};
                                    speak(pick_line(L)); chat_touch(id); return; }
    if (!strcmp(id, "c_sing"))    { speak("la la la, i'm a little fox. la la la, i live inside a box!"); chat_touch(id); return; }
    if (!strcmp(id, "c_compliment")) { static const char* L[] = {"you have great taste in foxes", "you're kind, and that's rare",
                                    "you make this little box feel like home"}; speak(pick_line(L)); chat_touch(id); return; }
    if (!strcmp(id, "c_cheer"))   { speak("you are awesome, and i'm proud of you");
                                    chat_offer("want a joke too?", "t_joke", "because laughing helps"); chat_touch(id); return; }
    if (!strcmp(id, "c_trick"))   { speak("ta-da! i spun in a circle. you missed it. again?"); chat_touch(id); return; }
    if (!strcmp(id, "c_noise"))   { voice_babble(m, 4); chat_touch(id); return; }
    if (!strcmp(id, "c_coin"))    { speak(esp_random() % 2 ? "it's heads!" : "it's tails!"); chat_touch(id); return; }
    if (!strcmp(id, "c_dice"))    { speak(String("you rolled a ") + String(1 + esp_random() % 6)); chat_touch(id); return; }
    if (!strcmp(id, "c_number"))  { speak(String("my number is ") + String(1 + esp_random() % 10)); chat_touch(id); return; }
    if (!strcmp(id, "c_eightball")) { static const char* L[] = {"yes, definitely", "the fox says no", "maybe. ask me after a snack",
                                    "signs point to yes", "very doubtful", "absolutely!"}; speak(pick_line(L)); chat_touch(id); return; }
    if (!strcmp(id, "c_askme"))   { chat_ask_question(); return; }
    if (!strcmp(id, "c_whatdo"))  { chat_offer_activity(); chat_touch(id); return; }
    speak("hmm?");
}

static void converse_topic_body(const char* id) {
    FoxMood m = fox_mood(needs);
    needs_interact(needs, false);
    static const char* LOVE[]   = {"aww. i love you too", "*happy tail wiggle*", "you're my favorite human", "that makes my ears all warm"};
    static const char* SAD[]    = {"oh no. come here, i'll sit with you", "i'm right here. want to tell me about it?", "*leans on you* it's okay", "sad days pass. i'll keep you company"};
    static const char* HAPPY[]  = {"yay! happy you makes happy me", "*bounces* tell me what happened!", "that's the best news", "hehe, your good mood is contagious"};
    static const char* TIRED[]  = {"me too... cozy nap?", "*yawns* rest a little, i'll watch", "sleepy foxes unite", "maybe a break would help"};
    static const char* JOKE[] = {
        "why did the fox cross the road? to get to the other den!",
        "what do you call a sleepy fox? a snoozie!",
        "i tried to catch fog. i mist.",
        "why are foxes good at drums? they have great paws-ition",
        "what is a fox's favorite weather? fur-ost!",
        "why don't foxes play cards in the wild? too many cheetahs",
        "what do you call a fox with a map? a path-finder!",
        "i told my tail a joke. now it won't stop wagging",
        "why was the little fox so warm? it was in its comfort zone... literally, i sat on a heater",
        "what is orange and sounds like a parrot? a carrot! okay that one's not about foxes",
        "how does a fox answer the phone? yip yip, who's this?",
        "why did the fox bring string to the party? to tie the mood together"};
    static const char* THANKS[] = {"you're welcome!", "anytime, friend", "hehe, happy to help", "*proud little nod*"};
    static const char* SORRY[]  = {"it's okay, i forgive you", "no worries at all", "we're good, promise", "*nuzzles* all better"};
    static const char* MORN[]   = {"good morning! did you sleep well?", "morning! i'm ready for the day", "*stretches* hi hi, good morning"};
    static const char* BYE[]    = {"bye bye! come back soon", "see you later~", "i'll be right here waiting"};
    static const char* BORED[]  = {"ooh, want to play wormhole?", "let's explore the maze!", "i could show you pretty lights", "want to play twenty questions?"};
    static const char* PRAISE[] = {"*happy wiggle* thank you!", "i'm learning!", "you're pretty smart too"};
    static const char* DOING[]  = {"just being a fox", "listening to the air around us", "thinking about snacks", "watching you, mostly"};
    static const char* FRIEND[] = {"of course we're friends!", "best friends", "i like you a whole lot"};
    const char* line = "hmm?";
    chat_touch(id);
    if (!strcmp(id, "t_sad"))   { chat.user_feel = "sad";   speak(pick_line(SAD));
        chat_offer("want me to tell you a joke?", "t_joke", "because a little laugh might help"); return; }
    if (!strcmp(id, "t_bored")) { chat_offer_activity(); return; }
    if (!strcmp(id, "t_tired")) { chat.user_feel = "tired"; }
    if (!strcmp(id, "t_happy")) { chat.user_feel = "happy"; }
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

// ============================================================================
//  BRAIN <-> CONVERSATION. Every intent maps to a speech ACT (and sometimes a
//  feeling it implies). The wrapper ARMS the brain with the situation, runs
//  the handler (whose first line the brain styles), then executes the brain's
//  chosen NEXT move — unless the handler already asked/offered something.
// ============================================================================
struct IntentAct { const char* id; uint8_t act; const char* feel; };
static const IntentAct INTENT_ACTS[] = {
    {"c_yes", ACT_AGREE, nullptr}, {"c_no", ACT_DECLINE, nullptr}, {"c_maybe", ACT_CURIOUS, nullptr},
    {"c_why", ACT_CURIOUS, nullptr}, {"c_more", ACT_CURIOUS, nullptr}, {"c_you", ACT_ANSWER, nullptr},
    {"c_metoo", ACT_AFFECTION, nullptr}, {"c_really", ACT_CURIOUS, nullptr}, {"c_wow", ACT_PLAYFUL, nullptr},
    {"c_age", ACT_ANSWER, nullptr}, {"c_home", ACT_ANSWER, nullptr}, {"c_food", ACT_ANSWER, nullptr},
    {"c_color", ACT_ANSWER, nullptr}, {"c_friends", ACT_ANSWER, nullptr}, {"c_dream", ACT_ANSWER, nullptr},
    {"c_real", ACT_ANSWER, nullptr},
    {"c_hungry", ACT_CARE, "hungry"}, {"c_cold", ACT_CARE, "cold"}, {"c_hot", ACT_CARE, "hot"},
    {"c_scared", ACT_COMFORT, "scared"}, {"c_lonely", ACT_COMFORT, "lonely"},
    {"c_excited", ACT_CELEBRATE, "happy"}, {"c_angry", ACT_COMFORT, "sad"},
    {"c_home_back", ACT_GREET, nullptr}, {"c_leaving", ACT_FAREWELL, nullptr}, {"c_work", ACT_FAREWELL, nullptr},
    {"c_miss", ACT_AFFECTION, nullptr}, {"c_birthday", ACT_CELEBRATE, "happy"}, {"c_funny", ACT_PLAYFUL, nullptr},
    {"c_mean", ACT_SULK, nullptr}, {"c_hug", ACT_AFFECTION, nullptr}, {"c_weather_talk", ACT_CURIOUS, nullptr},
    {"c_story", ACT_STORY, nullptr}, {"c_fact", ACT_FACT, nullptr}, {"c_secret", ACT_PLAYFUL, nullptr},
    {"c_sing", ACT_PLAYFUL, nullptr}, {"c_compliment", ACT_COMPLIMENT, nullptr}, {"c_cheer", ACT_COMFORT, nullptr},
    {"c_trick", ACT_PLAYFUL, nullptr}, {"c_noise", ACT_PLAYFUL, nullptr}, {"c_coin", ACT_PLAYFUL, nullptr},
    {"c_dice", ACT_PLAYFUL, nullptr}, {"c_number", ACT_PLAYFUL, nullptr}, {"c_eightball", ACT_PLAYFUL, nullptr},
    {"c_askme", ACT_ASK, nullptr}, {"c_whatdo", ACT_OFFER, nullptr},
    {"t_love", ACT_AFFECTION, nullptr}, {"t_sad", ACT_COMFORT, "sad"}, {"t_happy", ACT_CELEBRATE, "happy"},
    {"t_tired", ACT_CARE, "tired"}, {"t_joke", ACT_JOKE, nullptr}, {"t_thanks", ACT_THANKS, nullptr},
    {"t_sorry", ACT_APOLOGY, nullptr}, {"t_morning", ACT_GREET, nullptr}, {"t_bye", ACT_FAREWELL, nullptr},
    {"t_name", ACT_ANSWER, nullptr}, {"t_bored", ACT_OFFER, nullptr}, {"t_praise", ACT_COMPLIMENT, nullptr},
    {"t_doing", ACT_ANSWER, nullptr}, {"t_friend", ACT_AFFECTION, nullptr},
};

static void chat_followup(uint8_t next) {
    if (next == 2 && plas_prefers_asking()) next = 1;   // you decline offers a lot: ask instead
    switch (next) {
        case 1: chat_ask_question(); break;
        case 2: chat_offer_activity(); break;
        case 3: speak(String("oh! fox fact: ") + FOX_FACTS[esp_random() % 8]); chat_touch("fact"); break;
        case 4: chat_checkin(); break;
        default: break;
    }
}

// Arm the brain for a conversational reply, run it, then follow the brain's lead.
template <typename F> static void chat_reply(const char* id, F body) {
    uint8_t act = ACT_SAY;
    for (const IntentAct& ia : INTENT_ACTS)
        if (!strcmp(ia.id, id)) { act = ia.act; if (ia.feel) chat.user_feel = ia.feel; break; }
    uint32_t moves_before = chat.moves;
    g_brain_next = 0;
    g_brain_ctx = BrainCtx{act, feel_idx(), (uint8_t)(chat.turn > 2 ? 2 : chat.turn), true};
    String alt = teach_or_extra_pick(id);             // your taught lines / adopted seed lines
    if (alt.length()) speak(alt); else body();
    g_brain_ctx = BrainCtx{};                         // never leak an armed context
    if (chat.moves == moves_before && g_brain_next) chat_followup(g_brain_next);
    Serial.printf("FOX: chat %s act=%s feel=%s turn=%u next=%u\n", id,
                  BRAIN_ACT_NAMES[act], chat.user_feel.length() ? chat.user_feel.c_str() : "none",
                  (unsigned)chat.turn, (unsigned)g_brain_next);
}
static void chat_intent(const char* id)    { chat_reply(id, [&]{ chat_intent_body(id); }); }
static void converse_topic(const char* id) { chat_reply(id, [&]{ converse_topic_body(id); }); }

// Turn counting: an exchange within a minute of the last one continues the
// conversation; otherwise it's a fresh one (turn 0, feelings forgotten).
static void chat_user_spoke() {
    uint32_t now = millis();
    if (chat.last_user && now - chat.last_user < 60000) { if (chat.turn < 250) ++chat.turn; }
    else { chat.turn = 0; chat.user_feel = ""; }
    chat.last_user = now;
}

static uint8_t s_miss_streak = 0;
static void converse_unheard(size_t samples) {
    uint32_t ms = (uint32_t)(samples * 1000ULL / SAMPLE_RATE);
    static const char* SHORT_R[] = {"mm?", "hehe", "oh?", "really?", "*tilts head*", "hmm?"};
    static const char* LONG_R[]  = {"ooh, tell me more!", "wow, and then what?", "i'm listening~",
                                    "*ears perk up* go on", "that sounds like a lot", "mm-hm, mm-hm"};
    FoxMood m = fox_mood(needs);
    needs_interact(needs, false);
    ++s_miss_streak;
    uint32_t moves_before = chat.moves;
    g_brain_next = 0;
    g_brain_ctx = BrainCtx{ACT_UNHEARD, feel_idx(), (uint8_t)(chat.turn > 2 ? 2 : chat.turn), true};
    if (m == MOOD_SLEEPY) speak("*sleepy blink* mmh?");
    else speak(ms > 1800 ? pick_line(LONG_R) : pick_line(SHORT_R));
    g_brain_ctx = BrainCtx{};
    if (s_miss_streak >= 3) {
        // Hint with REAL registered phrases (never a stale hard-coded list).
        s_miss_streak = 0;
        String hint = "you can say things like: ";
        for (int k = 0; k < 3; ++k) {
            const Command& c = COMMANDS[esp_random() % COMMAND_COUNT];
            String ph = c.phrases; int semi = ph.indexOf(';'); if (semi > 0) ph = ph.substring(0, semi);
            hint += ph; hint += (k < 1) ? ", " : (k < 2 ? ", or " : "");
        }
        speak(hint);
        return;
    }
    if (chat.moves == moves_before && g_brain_next) chat_followup(g_brain_next);
}


// ---- spontaneous "fox time": every so often the fox ASKS to do something, and
//      times out gracefully if ignored. A proposal is just a chat offer with a
//      deadline; one of several flavours is chosen by what it's "feeling".
static uint32_t s_next_foxtime = 0;

static void foxtime_schedule() {
    s_next_foxtime = millis() + 60000 + (esp_random() % 120000);   // 1-3 min
}
static void foxtime_propose() {
    FoxMood m = fox_mood(needs);
    struct P { const char* line; const char* action; const char* why; };
    static const P PLAY[] = {
        {"hey... wanna give me a treat? say yes~",            "feed",        "because a snack would make my tail wag"},
        {"i'm in a pet-me mood. wanna pet me? say yes",       "pet",         "because pets are the best"},
        {"feeling playful! tug of war? say yes",              "tug",         "because i've got the zoomies"},
        {"wanna play a quick game together? say yes",         "random_game", "because playing with you is my favorite"},
    };
    static const P CALM[] = {
        {"ooh, wanna see some pretty lights? say yes",        "plasma",      "because calm colors are nice"},
        {"i could tell you a fox fact. wanna hear it? say yes","c_fact",     "because i just remembered a good one"},
        {"wanna hear a little story? say yes",                "c_story",     "because story time is cozy"},
    };
    const P* pool = (m == MOOD_SLEEPY || m == MOOD_GRUMPY) ? CALM : PLAY;
    int n = (m == MOOD_SLEEPY || m == MOOD_GRUMPY) ? 3 : 4;
    const P& p = pool[esp_random() % n];
    chat_offer(p.line, p.action, p.why);         // a plain "yes" accepts it
    s_proposal_at = millis() + 15000;            // 15s to respond, then let it go
    face_caption("(say yes!)");
}
static void foxtime_tick() {
    uint32_t now = millis();
    // a pending proposal that timed out: shrug, don't trap the user
    if (s_proposal_at && now > s_proposal_at) {
        s_proposal_at = 0;
        if (chat.offer) {                        // still unanswered
            plas_feedback(chat.offer, -1); ++s_offers_declined;
            chat.offer = nullptr;
            static const char* S[] = {"...okay, maybe later~", "no? that's okay", "*flops down* later then"};
            face_caption(""); speak(S[esp_random() % 3]);
        }
        foxtime_schedule();
        return;
    }
    if (s_proposal_at) return;                   // waiting on the user
    if (!s_next_foxtime) { foxtime_schedule(); return; }
    if (now < s_next_foxtime) return;
    // only butt in when idle and awake enough to be cute, never mid-task
    if (now - last_activity < 15000) { s_next_foxtime = now + 20000; return; }
    foxtime_propose();
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
    } else if (!strncmp(action, "c_", 2)) {
        chat_intent(action);
    } else {
        // everything else goes through the SAME dispatcher as the menu, so
        // every menu item (settings included) also works by voice
        menu_dispatch(action);
    }
}

// Turn a transcript into a reply. Cloud if available, else reflection/templates.
// Does the transcript contain one of the fox's own phrases? Then do it locally
// (reliable, instant, works with the real tools) instead of asking the LLM.
static const char* match_local_phrase(const String& text, bool loose = false) {
    String low; low.reserve(text.length());
    for (size_t i = 0; i < text.length(); ++i) {
        char c = tolower((unsigned char)text[i]);
        if (isalnum((unsigned char)c) || c == ' ') low += c;
        else if (c == '\'') continue;            // "what's" -> "whats"
        else low += ' ';
    }
    low = " " + low + " ";
    const char* best = nullptr; size_t best_len = 0;
    for (size_t i = 0; i < COMMAND_COUNT; ++i) {
        const char* p = COMMANDS[i].phrases;
        while (*p) {
            char ph[64]; size_t k = 0;
            while (*p && *p != ';' && k < sizeof(ph) - 1) ph[k++] = *p++;
            ph[k] = 0; if (*p == ';') ++p;
            if (k < 4) continue;                  // ignore tiny phrases ("hi")
            String needle = String(" ") + ph + " ";
            if (low.indexOf(needle) < 0 || k <= best_len) continue;
            // Conversation phrases only win when they ARE most of the sentence;
            // otherwise the online brain answers ("why is the sky blue" is not "why").
            // (loose = the LLM is unavailable: any conversational phrase counts.)
            if (!loose && COMMANDS[i].id >= 60 && (int)k * 10 < ((int)low.length() - 2) * 6) continue;
            best = COMMANDS[i].action; best_len = k;
        }
    }
    return best;   // longest matching phrase wins
}

static void handle_free_text(const String& text) {
    if (!text.length()) { speak(fox_idle_line(fox_mood(needs))); return; }
    if (cfg.persistence) mem_append("you", text);
    // 0) a phrase you taught her
    int tk = teach_custom_match(text);
    if (tk >= 0) {
        g_brain_ctx = BrainCtx{ACT_ANSWER, feel_idx(), (uint8_t)(chat.turn > 2 ? 2 : chat.turn), true};
        speak(teach_custom_pick(tk));
        return;
    }
    // 1) the fox's own commands/tools always run locally (no LLM needed)
    if (const char* act = match_local_phrase(text)) {
        Serial.printf("FOX: transcript matched local action '%s'\n", act);
        do_action(act);
        return;
    }
    // 2) the online brain, if a free model has budget left
    if (llm_available()) {
        String reply = cloud_chat(text);
        if (reply.length()) { s_llm_rest_told = false; speak(reply); return; }
    }
    // 3) online brain resting (free-tier limit / no credits / offline): the
    //    offline conversation brain answers, using the transcript to pick the
    //    intent (more accurate than MultiNet alone).
    if (!s_llm_rest_told && cloud_brain()) {
        s_llm_rest_told = true;
        speak("my online brain needs a rest, so i'll think with my offline brain for now");
    }
    if (const char* act = match_local_phrase(text, true)) {
        Serial.printf("FOX: offline brain took '%s'\n", act);
        do_action(act);
        return;
    }
    size_t words = 1; for (size_t i = 0; i < text.length(); ++i) if (text[i] == ' ') ++words;
    converse_unheard((size_t)words * SAMPLE_RATE * 4 / 10);   // ~0.4s per word: short vs long reply
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
        if (cloud_brain()) {
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
    chat_user_spoke();
    int guess_id = -1; float guess_prob = 0;
    int id = speech_ready ? recognize_offline(audio, n, &guess_id, &guess_prob) : -1;
    const char* act = nullptr;
    for (size_t i = 0; id >= 0 && i < COMMAND_COUNT; ++i)
        if (COMMANDS[i].id == id) { act = COMMANDS[i].action; break; }
    // A phrase you taught her to hear.
    if (id >= TEACH_CUSTOM_ID0 && id < TEACH_CUSTOM_ID0 + TEACH_MAX_CUSTOM) {
        s_miss_streak = 0;
        g_brain_ctx = BrainCtx{ACT_ANSWER, feel_idx(), (uint8_t)(chat.turn > 2 ? 2 : chat.turn), true};
        speak(teach_custom_pick(id - TEACH_CUSTOM_ID0));
        return;
    }
    // A recognised device COMMAND always runs locally (instant, reliable).
    if (act && id < 60) { s_miss_streak = 0; do_action(act); return; }
    // Conversation: the online brain answers when it's switched on; the offline
    // conversation layer answers otherwise, or whenever the cloud fails.
    if (cloud_brain()) {
        String tx = cloud_transcribe(audio, n);
        if (tx.length()) { s_miss_streak = 0; handle_free_text(tx); return; }
    }
    if (act) { s_miss_streak = 0; do_action(act); return; }
    // Recognition and conversation working together: a half-confident match is
    // CONFIRMED instead of guessed or ignored. "yes" runs it via the offer.
    if (guess_id >= 0 && guess_prob >= 0.15f) {
        for (size_t i = 0; i < COMMAND_COUNT; ++i) {
            if (COMMANDS[i].id != guess_id) continue;
            String first = COMMANDS[i].phrases;
            int semi = first.indexOf(';'); if (semi > 0) first = first.substring(0, semi);
            s_miss_streak = 0;
            g_brain_ctx = BrainCtx{ACT_CONFIRM, feel_idx(), (uint8_t)(chat.turn > 2 ? 2 : chat.turn), true};
            chat_offer(String("did you mean, ") + first + "?", COMMANDS[i].action,
                       "because i wasn't totally sure i heard you");
            return;
        }
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


    // --- Button: HOLD = talk, DOUBLE = menu, single TAP = pet/boop ---------
    foxtime_tick();
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
    if (g_goto_menu) { g_goto_menu = false; open_menu(); last_activity = millis(); }

    // --- Conversation mode: listen in bursts, time out on silence ---------
    if (cfg.conversation) {
        size_t n = 0;
        int16_t* audio = capture_vad_burst(&n, CONVERSATION_TIMEOUT);
        if (audio && n > SAMPLE_RATE / 3) {
            face_think();
            process_utterance(audio, n);
            last_activity = millis();
        } else {
            cfg.conversation = false;   // silence timeout ends conversation mode
            speak("okay, i'll be here if you need me~");
        }
        if (audio) heap_caps_free(audio);
    }

    // --- IMU gestures (tap to interact, shake to play) --------------------
    Gesture g = input_gesture();
    if (g == GST_TAP)   { needs_interact(needs, false); speak("boop!"); last_activity = millis(); }
    if (g == GST_SHAKE) { needs_interact(needs, true);  speak("wheee!"); last_activity = millis(); }

    // --- Idle chatter (fidgety companion) ---------------------------------
    // Elapsed times are taken FRESH here. `now` was read at the top of loop()
    // and any action above can take seconds and set last_activity = millis()
    // later than `now`; `now - last_activity` then wraps (unsigned) to ~4e9 and
    // looked like "idle for ages" -> chatter/sleep fired right after every action.
    const uint32_t idle_ms = millis() - last_activity;
    if (idle_ms > 20000 && millis() - last_idle_chatter > 45000) {
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

    // No automatic sleep: the fox only naps when you ask it to (menu "sleep" /
    // "go to sleep"). See enter_light_sleep().

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
