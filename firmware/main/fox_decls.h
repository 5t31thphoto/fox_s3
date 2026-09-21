// fox_decls.h — ONE place that forward-declares every function shared across
// the fox source files. fox_main.cpp includes this BEFORE any of the .inc
// modules, and the standalone .cpp translation units include it too. With every
// cross-file symbol declared here, include ORDER never matters and we never get
// "not declared in this scope" or link errors from a missing forward decl.
//
// Rule for this project: any function called from a file other than the one it
// is defined in is declared here and defined WITHOUT `static` (external
// linkage). Purely file-local helpers (hsv565, le16, syllable_estimate, …) are
// not listed here and stay static in their own file.
#pragma once
#include "fox.h"
#include <ArduinoJson.h>

// ---- personality / brain (fox_brain.cpp) -----------------------------------
// (declared in fox.h: fox_dress, fox_mood, needs_tick, needs_interact,
//  fox_idle_line, fox_time_greeting, fox_reflect)

// ---- tiny on-device LLM (fox_llm.cpp) --------------------------------------
String llm_flavour(const String& fact, FoxMood mood, const FoxConfig& cfg);

// ---- voice (fox_voice.cpp) --------------------------------------------------
// (declared in fox.h: voice_begin, voice_say, voice_babble, voice_is_pico)

// ---- SAM critter synth (fox_sam.c, C ABI) ----------------------------------
extern "C" {
int  sam_render(const char* text, unsigned char speed, unsigned char pitch,
                unsigned char throat, unsigned char mouth,
                unsigned char** out, int* out_len);
void sam_free(unsigned char* p);
}

// ---- face (fox_face.inc) ----------------------------------------------------
void face_begin(FoxConfig& c);
void face_wake();
void face_dim();
void face_splash(FoxConfig& c);
void face_draw(FoxMood mood);
void face_tick(FoxMood mood);
void face_caption(const String& text);
void face_listen();
void face_think();
void face_set_mouth(float level01);
void face_lipsync_mode();

// ---- IR (fox_ir.inc) --------------------------------------------------------
void ir_begin();
void ir_command(const char* category, const char* button);
void ir_narrow_power();

// ---- tools: radar / sniffer / pwnagotchi (fox_tools.inc) -------------------
float imu_heading_deg();
void  tool_menu_ble_radar();
void  tool_menu_wifi_radar();
void  tool_menu_sniffer();
void  tool_menu_probe_sniff();
const char* tool_ble_scan_report();
const char* tool_wifi_scan_report();

// ---- games (fox_games.inc) --------------------------------------------------
void  tilt_xy(float* rx, float* ry);
void  game_wormhole();
void  game_catch();
void  game_reaction();
void  game_guess_paw();
bool  input_wait_yes(uint32_t ms);   // defined in fox_input.inc, used widely

// ---- Bayesian 20 questions (fox_bayes.inc) ---------------------------------
void  game_bayes_twenty();

// ---- demoscene toys (fox_demo.inc) -----------------------------------------
void  toy_plasma();
void  toy_starfield();
void  toy_ink();
void  toy_spiro();

// ---- fox encounters + roguelike (fox_encounter.inc) ------------------------
void  enc_pet();
void  enc_feed();
void  enc_tug();
void  enc_random();
void  game_maze();

// ---- markov chatter + pwnagotchi RF mood (fox_markov.inc) ------------------
String   fox_markov_line(FoxMood mood);
String   rf_mood_tick();
uint32_t rf_lifetime_friends();

// ---- network reports (fox_net.inc) -----------------------------------------
String net_weather();
String net_space_weather();
String net_aurora();

// ---- input: buttons / capture / menu / sleep (fox_input.inc) ---------------
enum ButtonEvent : uint8_t { BTN_NONE, BTN_TAP, BTN_HOLD_START };
enum Gesture     : uint8_t { GST_NONE, GST_TAP, GST_SHAKE, GST_TILT };
void        input_begin();
void        input_poll();
ButtonEvent input_button_event();
Gesture     input_gesture();
bool        wants_menu();
int16_t*    capture_while_held(size_t* out_n);
int16_t*    capture_vad_burst(size_t* out_n, uint32_t timeout_ms);
void        enter_light_sleep();
void        open_menu();

// ---- host glue defined in fox_main.cpp, used by the .inc modules -----------
void speak(const String& fact);
void menu_dispatch(const char* id);
void save_config();
bool wifi_connect();
