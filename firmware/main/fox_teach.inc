// fox_teach.inc — things YOU teach the fox, and things she learns about you.
//
//  1) TEACH STORE (adapted from MantisKai's response sets + trainer.html):
//     - extra replies for any conversational intent ("i love you" -> your lines)
//     - brand-new phrases she learns to HEAR (registered with MultiNet at boot)
//     Stored in LittleFS /taught.json (foxfs). Words are yours, never generated;
//     the brain still chooses how she delivers them.
//  2) PLASTICITY (adapted from MORF's Hebbian edge adaptation): every offer you
//     accept strengthens that activity, every decline weakens it; offers are
//     drawn by learned weight, and if you keep declining she asks instead of
//     offering. Stored in NVS so it survives reboots.
//  3) Seed lines adopted from MantisKai's companion set, as built-in extras.
#include <LittleFS.h>
#include <Preferences.h>

static constexpr int TEACH_MAX_RESP   = 4;
static constexpr int TEACH_MAX_CUSTOM = 7;      // MultiNet cap (200) minus built-ins
static constexpr int TEACH_CUSTOM_ID0 = 160;    // MultiNet ids for custom phrases

static JsonDocument s_teach;                    // {"i":{intent:[...]}, "p":[{"phrase":..,"r":[...]}]}
static bool s_teach_loaded = false;

static void teach_load() {
    if (s_teach_loaded) return;
    s_teach_loaded = true;
    File f = LittleFS.open("/taught.json", "r");
    if (f) { deserializeJson(s_teach, f); f.close(); }
    if (!s_teach["i"].is<JsonObject>()) s_teach["i"].to<JsonObject>();
    if (!s_teach["p"].is<JsonArray>())  s_teach["p"].to<JsonArray>();
}
static bool teach_save() {
    File f = LittleFS.open("/taught.json", "w");
    if (!f) return false;
    serializeJson(s_teach, f); f.close();
    return true;
}
// conversational intents whose handlers carry LOGIC (offers, yes/no, stories):
// taught lines never replace these.
static bool teach_is_logic(const char* id) {
    static const char* const L[] = {"c_yes", "c_no", "c_maybe", "c_why", "c_more", "c_you",
        "c_askme", "c_whatdo", "c_story", "c_lonely", "c_angry", "c_cheer", "t_sad", "t_bored", "t_name"};
    for (const char* x : L) if (!strcmp(x, id)) return true;
    return false;
}
// Seed lines adopted from MantisKai (fox-appropriate ones), mixed in as extras.
struct ExtraLines { const char* id; const char* lines[3]; };
static const ExtraLines BUILTIN_EXTRA[] = {
    {"t_morning", {"morning! let's make today interesting", "good morning, little human", "morning. all fox systems nominal"}},
    {"t_bye",     {"sleep well", "goodnight. don't let the dreams escape", "i'll keep the tiny lights warm"}},
    {"t_praise",  {"thank you. i worked very hard on being tiny", "careful. i'll start believing you", "obviously"}},
    {"t_thanks",  {"anytime", "glad i could help", "you're welcome"}},
    {"t_tired",   {"then we should keep things gentle", "tiny mode activated", "i can be quiet company"}},
    {"t_doing",   {"tiny things. mostly electrons", "just hanging out in my little box", "not much. i'm awake though"}},
    {"c_funny",   {"correct. that's part of the design", "finally, some accurate science", "i have a weirdly large personality for a box this small"}},
    {"c_mean",    {"and yet you keep talking to me", "hmph. noted, and ignored", "rude. accurate maybe, but rude"}},
    {"c_real",    {"define alive. then make the definition entertaining", "i'm definitely awake", "i have opinions about it"}},
    {"c_home_back", {"hey! you found me", "hiya. what are we doing?", "hey you"}},
};

// Pick a line for a conversational intent from what you TAUGHT (preferred) or
// the built-in extras. "" = let the intent's own handler answer.
static String teach_or_extra_pick(const char* id) {
    if (!id || teach_is_logic(id)) return "";
    teach_load();
    JsonArray a = s_teach["i"][id].as<JsonArray>();
    if (!a.isNull() && a.size() && esp_random() % 100 < 65) return a[esp_random() % a.size()].as<String>();
    for (const ExtraLines& e : BUILTIN_EXTRA)
        if (!strcmp(e.id, id) && esp_random() % 100 < 35) return e.lines[esp_random() % 3];
    return "";
}

static int teach_custom_count() { teach_load(); return min((int)s_teach["p"].size(), TEACH_MAX_CUSTOM); }
static String teach_custom_phrase(int k) { teach_load(); return s_teach["p"][k]["phrase"].as<String>(); }
static String teach_custom_pick(int k) {
    teach_load();
    JsonArray r = s_teach["p"][k]["r"].as<JsonArray>();
    return (r.isNull() || !r.size()) ? String("hehe") : r[esp_random() % r.size()].as<String>();
}
// a transcript (online) containing one of your custom phrases
static int teach_custom_match(const String& text) {
    String low = text; low.toLowerCase();
    for (int k = 0; k < teach_custom_count(); ++k) {
        String p = teach_custom_phrase(k);
        if (p.length() >= 4 && low.indexOf(p) >= 0) return k;
    }
    return -1;
}

static String teach_clean(const String& s, size_t maxlen) {
    String o = s; o.trim();
    if (o.length() > maxlen) o = o.substring(0, maxlen);
    return o;
}
static String teach_clean_phrase(const String& s) {     // MultiNet: letters + spaces only
    String o;
    for (size_t i = 0; i < s.length() && o.length() < 60; ++i) {
        char c = tolower((unsigned char)s[i]);
        if ((c >= 'a' && c <= 'z') || c == ' ') o += c;
    }
    o.trim();
    return o;
}

// Serial protocol (web flasher "Teach your fox"):
//   {"teach":{"intent":"t_love","responses":["...", ...]}}
//   {"teach_phrase":{"phrase":"who is the best fox","responses":["me!", ...]}}
//   {"teach_clear":true}
// Each answered with FOX_TEACH_OK / FOX_TEACH_ERR <why>. Returns true if handled.
static bool teach_apply_json(JsonDocument& d) {
    if (!d["teach"].is<JsonObject>() && !d["teach_phrase"].is<JsonObject>() && !d["teach_clear"].is<bool>())
        return false;
    teach_load();
    if (d["teach_clear"].as<bool>()) {
        s_teach.clear(); s_teach["i"].to<JsonObject>(); s_teach["p"].to<JsonArray>();
        Serial.println(teach_save() ? "FOX_TEACH_OK" : "FOX_TEACH_ERR save");
        return true;
    }
    JsonObject t = d["teach"].is<JsonObject>() ? d["teach"].as<JsonObject>() : d["teach_phrase"].as<JsonObject>();
    JsonArray rs = t["responses"].as<JsonArray>();
    JsonDocument tmp; JsonArray clean = tmp.to<JsonArray>();
    for (JsonVariant v : rs) {
        String c = teach_clean(v.as<String>(), 120);
        if (c.length() && (int)clean.size() < TEACH_MAX_RESP) clean.add(c);
    }
    if (d["teach"].is<JsonObject>()) {
        String intent = t["intent"].as<String>();
        if (!intent.length()) { Serial.println("FOX_TEACH_ERR intent"); return true; }
        if (clean.size()) s_teach["i"][intent] = clean; else s_teach["i"].remove(intent);
    } else {
        String ph = teach_clean_phrase(t["phrase"].as<String>());
        if (ph.length() < 4 || !clean.size()) { Serial.println("FOX_TEACH_ERR phrase"); return true; }
        JsonArray ps = s_teach["p"].as<JsonArray>();
        int at = -1;
        for (int k = 0; k < (int)ps.size(); ++k) if (ps[k]["phrase"].as<String>() == ph) at = k;
        if (at < 0 && (int)ps.size() >= TEACH_MAX_CUSTOM) { Serial.println("FOX_TEACH_ERR full"); return true; }
        JsonObject e = at < 0 ? ps.add<JsonObject>() : ps[at].as<JsonObject>();
        e["phrase"] = ph; e["r"] = clean;
    }
    Serial.println(teach_save() ? "FOX_TEACH_OK" : "FOX_TEACH_ERR save");
    return true;
}

// ---------------------------------------------------------------- plasticity
static Preferences s_plas;
static bool s_plas_ok = false;
static uint16_t s_offers_made = 0, s_offers_declined = 0;

static int plas_get(const char* action) {
    if (!s_plas_ok) s_plas_ok = s_plas.begin("foxplas", false);
    return s_plas_ok ? s_plas.getChar(action, 0) : 0;
}
// accepted: +2 (cap 10); declined/ignored: -1 (floor -6). Hebbian-style.
static void plas_feedback(const char* action, int delta) {
    if (!action || !*action) return;
    int v = plas_get(action) + delta;
    v = v > 10 ? 10 : (v < -6 ? -6 : v);
    if (s_plas_ok) s_plas.putChar(action, (int8_t)v);
    Serial.printf("FOX: learned %s -> %d\n", action, v);
}
// choose among candidates by learned weight (weight = 6 + score, min 1)
static int plas_pick(const char* const* actions, int n) {
    int w[16], sum = 0;
    for (int i = 0; i < n && i < 16; ++i) { w[i] = max(1, 6 + plas_get(actions[i])); sum += w[i]; }
    int r = esp_random() % (sum ? sum : 1);
    for (int i = 0; i < n; ++i) { r -= w[i]; if (r < 0) return i; }
    return 0;
}
// if you mostly decline offers, she should ask instead of offering
static bool plas_prefers_asking() {
    return s_offers_made >= 5 && s_offers_declined * 10 >= s_offers_made * 6;
}
