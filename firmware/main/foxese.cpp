#include "foxese.h"

namespace {
static uint8_t hexv(char c) {
    if (c >= '0' && c <= '9') return (uint8_t)(c - '0');
    if (c >= 'A' && c <= 'F') return (uint8_t)(c - 'A' + 10);
    if (c >= 'a' && c <= 'f') return (uint8_t)(c - 'a' + 10);
    return 0xFF;
}
static char hexc(uint8_t v) { return v < 10 ? char('0' + v) : char('A' + v - 10); }
static bool byte_at(const String& s, int p, uint8_t& v) {
    if (p + 1 >= (int)s.length()) return false;
    uint8_t a = hexv(s[p]), b = hexv(s[p + 1]);
    if (a == 0xFF || b == 0xFF) return false;
    v = (uint8_t)((a << 4) | b); return true;
}
static uint8_t nibble_at(const String& s, int p) {
    return p < (int)s.length() ? hexv(s[p]) : 0xFF;
}

// Stable FNV-1a gives arbitrary runtime facts a compact identity without
// storing the prose in the neural packet.  Known training facts occupy IDs
// 0..25; runtime facts use the high range and are never mistaken for a known
// training fact during expansion.
static uint8_t hash_fact(const String& fact) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < fact.length(); ++i) {
        char c = fact[i];
        if (c >= 'A' && c <= 'Z') c = char(c + ('a' - 'A'));
        h ^= (uint8_t)c; h *= 16777619u;
    }
    return (uint8_t)(0x80u | ((h >> 1) & 0x7Fu));
}

// ---- FOXESE v2 rendering banks ---------------------------------------------
// style:   0 plain 1 bubbly 2 tender 3 silly 4 shy 5 dramatic 6 drowsy 7 sassy
// gesture: 0 still 1 wag 2 perk 3 tilt 4 nuzzle 5 bounce 6 flop 7 squint
// The brain picks the style (already mood-aware); this is the ONLY place a line
// gets decorated, so nothing is stacked twice.
static const char* const STY_PRE[8][5] = {
    {"", "", "so, ", "okay, ", "well, "},
    {"ooh! ", "yay, ", "oh oh! ", "hehe, ", ""},
    {"aww, ", "hey... ", "", "oh, ", "*softly* "},
    {"hehe ", "heehee, ", "*snorts* ", "pfft, ", ""},
    {"um... ", "*blushes* ", "oh... ", "*hides behind tail* ", ""},
    {"behold! ", "oh my! ", "gasp! ", "*dramatic pose* ", "listen! "},
    {"*yawn* ", "mmn... ", "*sleepy blink* ", "zzz... oh, ", ""},
    {"*hmf* ", "fine. ", "obviously, ", "*flicks tail* ", "hmph, "},
};
static const char* const STY_SUF[8][5] = {
    {".", "", ".", "", "."},
    {"!", " ~", "!", " hehe", "!!"},
    {" ~", ".", "", " *soft*", " mm"},
    {" hehe", "!", " *wiggle*", "~", "!"},
    {"...", " *blush*", " hehe...", "~", ""},
    {"!", "!!", " *ta-da*", "!", ""},
    {"...", " *yawn*", " zzz", "...", ""},
    {".", " i guess", " *hmf*", ".", ""},
};
static const char* const GESTURE[8] = {
    "", "*tail wag* ", "*ears perk* ", "*tilts head* ", "*nuzzles* ",
    "*bounces* ", "*flops down* ", "*squints* "
};
static uint32_t rnd5() { return esp_random() % 5; }
static bool is_punct(char ch) { return ch == '.' || ch == '!' || ch == '?' || ch == '~'; }
}

uint8_t foxese_fact_id(const String& fact) {
    static const char* const known[] = {
        "the time is now", "it is sunny", "it is raining", "it is cloudy",
        "battery is low", "battery is full", "the tv is off", "the tv is on",
        "found your remote", "message from a friend", "it is morning",
        "it is night", "you have been away", "the volume is up",
        "the volume is down", "the light is on", "the light is off",
        "a brand new day", "time to rest", "i saved that", "i remember you",
        "let us play", "i missed you", "all done", "ready to go"
    };
    String x = fact; x.toLowerCase(); x.trim();
    for (uint8_t i = 0; i < sizeof(known)/sizeof(known[0]); ++i)
        if (x == known[i]) return i;
    return hash_fact(x);
}

String foxese_encode(uint8_t mood, uint8_t fact_id, uint8_t style,
                     uint8_t gesture, uint8_t intensity, uint8_t next) {
    String s = "F2M"; s += hexc(mood & 0x0F); s += "F";
    s += hexc((fact_id >> 4) & 0x0F); s += hexc(fact_id & 0x0F);
    s += "S"; s += hexc(style & 0x0F);
    s += "G"; s += hexc(gesture & 0x0F);
    s += "E"; s += hexc(intensity & 0x0F);
    s += "N"; s += hexc(next & 0x0F);
    return s;
}

bool foxese_parse(const String& packet, Foxese& out) {
    out = Foxese{};
    String s = packet; s.trim();
    // v2: exactly 15 printable bytes: F2 Mx Fxx Sx Gx Ex Nx.
    if (s.length() != 15 || s[0] != 'F' || s[1] != '2' ||
        s[2] != 'M' || s[4] != 'F' || s[7] != 'S' ||
        s[9] != 'G' || s[11] != 'E' || s[13] != 'N') return false;
    uint8_t m = nibble_at(s,3), f = 0, st = nibble_at(s,8);
    uint8_t g = nibble_at(s,10), e = nibble_at(s,12), n = nibble_at(s,14);
    if (m > 4 || st > 7 || g > 7 || e > 3 || n > 4) return false;
    if (!byte_at(s,5,f)) return false;
    out.version = 2; out.mood = m; out.fact_id = f; out.style = st;
    out.gesture = g; out.intensity = e; out.next = n; out.valid = true;
    return true;
}

String foxese_expand(const Foxese& x, const String& fact) {
    if (!x.valid || !fact.length()) return "";
    // The model never supplies the words: `fact` is always firmware-owned.
    String body = fact; body.trim();
    String pre = STY_PRE[x.style & 7][rnd5()];
    String suf = STY_SUF[x.style & 7][rnd5()];
    bool own_action = body.length() && body[0] == '*';
    if (own_action && pre.length() && pre[0] == '*') pre = "";      // one *action* up front
    char last = body.length() ? body[body.length() - 1] : ' ';
    if (is_punct(last) && suf.length() && is_punct(suf[0])) suf = "";   // no "!." pile-ups
    String out = (own_action ? String("") : String(GESTURE[x.gesture & 7])) + pre + body + suf;
    // intensity: 3 = emphatic, 0 = soft
    out.trim();
    if (x.intensity >= 3 && out.length() && out[out.length() - 1] == '.') out.setCharAt(out.length() - 1, '!');
    if (x.intensity == 0 && out.length() && out.endsWith("!!")) out.remove(out.length() - 1);
    return out;
}
