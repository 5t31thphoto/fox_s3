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

static const char* const PRE[] = {
    "", "ooh! ", "hehe, ", "okay~ "
};
static const char* const SUF[] = {
    ".", "!", " ~", " hehe!"
};
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
                     uint8_t gesture, uint8_t intensity) {
    String s = "F1M"; s += hexc(mood & 0x0F); s += "F";
    s += hexc((fact_id >> 4) & 0x0F); s += hexc(fact_id & 0x0F);
    s += "S"; s += hexc(style & 0x0F);
    s += "G"; s += hexc(gesture & 0x0F);
    s += "E"; s += hexc(intensity & 0x0F);
    return s;
}

bool foxese_parse(const String& packet, Foxese& out) {
    out = Foxese{};
    String s = packet; s.trim();
    // Exactly 13 printable bytes: F1 Mx Fxx Sx Gx Ex.
    if (s.length() != 13 || s[0] != 'F' || s[1] != '1' ||
        s[2] != 'M' || s[4] != 'F' || s[7] != 'S' ||
        s[9] != 'G' || s[11] != 'E') return false;
    uint8_t m = nibble_at(s,3), f = 0, st = nibble_at(s,8);
    uint8_t g = nibble_at(s,10), e = nibble_at(s,12);
    if (m == 0xFF || st == 0xFF || g == 0xFF || e == 0xFF || m > 4 || st > 3 || g > 3 || e > 3) return false;
    if (!byte_at(s,5,f)) return false;
    out.version = 1; out.mood=m; out.fact_id=f; out.style=st;
    out.gesture=g; out.intensity=e; out.valid=true; return true;
}

String foxese_expand(const Foxese& x, const String& fact) {
    if (!x.valid || !fact.length()) return "";
    // The model is not allowed to supply the fact text.  This is always the
    // firmware-owned source of truth. Every semantic field affects expansion.
    String body;
    switch (x.style & 3) {
        case 1: body = String(PRE[1]) + fact; break;
        case 2: body = fact + SUF[2]; break;
        case 3: body = String(PRE[2]) + fact + SUF[3]; break;
        default: body = fact + SUF[0]; break;
    }
    if (x.gesture == 1) body = "*ears perk* " + body;
    else if (x.gesture == 2) body = "*tail wag* " + body;
    else if (x.gesture == 3) body = "*tilts head* " + body;
    if (x.intensity >= 3) body += "!";
    return body;
}
