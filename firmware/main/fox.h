// fox.h — shared types and tunables for the Fox voice companion.
// Target: M5Stack AtomS3R (ESP32-S3, 8MB flash, 8MB PSRAM) + Atomic Echo Base.
#pragma once
#include <Arduino.h>
#include <M5Unified.h>

// ---- hardware ---------------------------------------------------------------
static constexpr gpio_num_t USER_GPIO   = GPIO_NUM_41;  // AtomS3R USER button, active-low
static constexpr gpio_num_t IR_TX_GPIO  = GPIO_NUM_47;  // AtomS3R on-board IR LED
static constexpr int   SAMPLE_RATE      = 16000;        // AFE + MultiNet + PicoTTS all want 16k

// Echo Base I2S / I2C (M5Unified drives these once we set external_speaker.atomic_echo)
static constexpr gpio_num_t ECHO_I2S_DIN = GPIO_NUM_7;
static constexpr gpio_num_t ECHO_I2S_WS  = GPIO_NUM_6;
static constexpr gpio_num_t ECHO_I2S_DO  = GPIO_NUM_5;
static constexpr gpio_num_t ECHO_I2S_BCK = GPIO_NUM_8;

// ---- timing -----------------------------------------------------------------
static constexpr uint32_t IDLE_SLEEP_MS        = 120000;  // light-sleep after 2 min idle
static constexpr uint32_t CONVERSATION_TIMEOUT = 45000;   // conversation mode silence timeout
static constexpr uint32_t PTT_MIN_MS           = 200;     // shorter press = open menu
static constexpr uint32_t PTT_MAX_MS           = 10000;   // hard cap on one utterance
static constexpr float    MIN_COMMAND_PROB     = 0.55f;   // MultiNet acceptance threshold

// ---- personality / memory ---------------------------------------------------
static constexpr size_t   MEM_MAX_BYTES = 6000;   // rolling journal cap (kept in a file)
static constexpr uint8_t  MOOD_LEVELS   = 5;

enum FoxMood : uint8_t { MOOD_SLEEPY, MOOD_CALM, MOOD_HAPPY, MOOD_EXCITED, MOOD_GRUMPY };

// The fox's felt needs drift over time and colour its replies. None of this is
// an LLM — it is cheap accumulated state, the oldest trick in the companion book.
struct FoxNeeds {
    uint8_t play      = 60;  // 0..100, decays; games/interaction raise it
    uint8_t social    = 60;  // decays with silence; talking raises it
    uint8_t energy    = 90;  // decays while awake; sleep restores it
    uint32_t last_tick = 0;
};

struct FoxConfig {
    String name         = "Ember";
    String timezone     = "MST7MDT,M3.2.0,M11.1.0";   // POSIX TZ; portal can override
    String weather      = "";                          // wttr.in location, e.g. "Denver"
    String wifi_ssid    = "";
    String wifi_pass    = "";
    String api_key      = "";                          // Groq (or compatible) key, NVS-only
    String chat_model   = "llama-3.1-8b-instant";
    String stt_model    = "whisper-large-v3-turbo";
    String api_base     = "https://api.groq.com/openai/v1";
    String personality  = "playful, curious, warm, a little mischievous";
    String voice_pack   = "chatterbox";                // "chatterbox" | "critter"
    String splash_text  = "";                          // custom boot splash line
    uint8_t splash_fx   = 0;                            // 0 none,1 sparkle,2 wave,3 rainbow
    uint8_t theme       = 0;                            // preset colour theme index
    // Brain mode, carried over from the original config schema:
    //   "groq"     — cloud chat (default when a key is present)
    //   "ondevice" — tiny on-device brain only
    //   "stream"   — experimental: stream weights from weights_url
    //   "hybrid"   — cloud when online, on-device/templates when not
    String mode          = "groq";
    String weights_url   = "";                          // for "stream" mode
    String context_summary = "";                        // tiny persistent summary
    bool   cloud_enabled = false;                      // derived: key present
    bool   conversation  = false;
    bool   persistence   = true;
    bool   wifi_enabled  = true;
    bool   ble_enabled   = true;
    bool   captions      = true;                        // show text of what fox says
    // Per-tool enables (from the original config_store schema).
    bool   tool_ble      = true;
    bool   tool_wifi     = true;
    bool   tool_ir       = true;
    bool   tool_imu      = true;
    bool   tool_context  = true;
    bool   lip_sync      = true;   // drive the mouth from mic FFT while listening
    uint8_t volume       = 70;
    uint16_t color_primary = 0xFB43;  // rgb565 orange (#ff6b35)
    uint16_t color_accent  = 0xF618;  // cream (#f7c59f)
    uint16_t color_bg      = 0x08A6;  // deep indigo (#0f0f1a)
};

// Fox voice: Pico (real speech) or SAM (retro) chosen by voice_pack, with a
// babble fallback that always works. Implemented in fox_voice.cpp.
void voice_begin(const FoxConfig& cfg);
void voice_say(const String& text, FoxMood mood);
// Face hooks the voice path uses for lip-sync (defined in fox_face.inc):
void face_draw(FoxMood mood);
void face_set_mouth(float level01);
void voice_babble(FoxMood mood, int syllables);
bool voice_is_pico();

// Personality: turns a bare fact into a fox-flavoured line (fox_brain.cpp).
String fox_dress(const String& fact, FoxMood mood, const FoxConfig& cfg);
FoxMood fox_mood(const FoxNeeds& n);
void needs_tick(FoxNeeds& n);
void needs_interact(FoxNeeds& n, bool played);
String fox_idle_line(FoxMood mood);           // fixed idle line (fox_brain.cpp)
String fox_time_greeting(int hour);
String fox_reflect(const String& user_lower);

// Local persistence journal, stored in a small SPIFFS/LittleFS file, not NVS
// (NVS string entries are capped at 4000 bytes and this can grow past that).
void mem_begin();
void mem_append(const String& kind, const String& text);
String mem_tail(size_t max_bytes);
void mem_clear();
