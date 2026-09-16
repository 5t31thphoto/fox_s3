// fox_brain.cpp — the accumulated "fake AI" personality layer.
//
// This is deliberately NOT a neural net. It is the pile of cheap tricks that
// have made toys feel alive for forty years: mood state, need drives, template
// banks with slot filling, ELIZA-style reflection, and time-of-day awareness.
// It runs in microseconds and never needs the network.
//
// The optional on-device tiny LLM (fox_llm.cpp) plugs in *above* this: when it
// is present and confident it may rewrite a line, but it can only ever wrap a
// fact the firmware already computed. It cannot invent facts. If it is absent
// or unsure, these templates are the voice of the fox.
#include "fox.h"
#include "fox_decls.h"
#include <esp_random.h>

// forward decl from fox_llm.cpp (weak — may be a stub that returns "")
String llm_flavour(const String& fact, FoxMood mood, const FoxConfig& cfg);

static uint32_t pick(uint32_t n) { return n ? (esp_random() % n) : 0; }

// ---- mood derivation --------------------------------------------------------
FoxMood fox_mood(const FoxNeeds& n) {
    if (n.energy < 20) return MOOD_SLEEPY;
    if (n.play  < 25 && n.social < 25) return MOOD_GRUMPY;   // neglected
    if (n.play  > 75 || n.social > 80) return MOOD_EXCITED;
    if (n.social > 55) return MOOD_HAPPY;
    return MOOD_CALM;
}

void needs_tick(FoxNeeds& n) {
    uint32_t now = millis();
    if (!n.last_tick) { n.last_tick = now; return; }
    uint32_t dt = (now - n.last_tick) / 1000;  // seconds
    if (dt < 5) return;
    n.last_tick = now;
    auto dec = [](uint8_t& v, uint32_t amt) { v = v > amt ? v - amt : 0; };
    dec(n.play,   dt / 30);
    dec(n.social, dt / 20);
    dec(n.energy, dt / 60);
}

void needs_interact(FoxNeeds& n, bool played) {
    auto add = [](uint8_t& v, int amt) { int r = v + amt; v = r > 100 ? 100 : r; };
    add(n.social, 18);
    if (played) add(n.play, 22);
    n.last_tick = millis();
}

// ---- mood-flavoured decoration ---------------------------------------------
// Prefixes / suffixes that carry emotion without changing the fact.
static const char* PRE_SLEEPY[]  = {"*yawn* ", "mmn... ", "*stretches* "};
static const char* PRE_HAPPY[]   = {"", "ooh! ", "hehe, ", "okay~ "};
static const char* PRE_EXCITED[] = {"yes yes! ", "ooh ooh! ", "*ears perk* ", "!! "};
static const char* PRE_GRUMPY[]  = {"*hmf* ", "fine. ", "...", "*flicks tail* "};
static const char* SUF_HAPPY[]   = {"", " ^^", " ~", ""};
static const char* SUF_EXCITED[] = {" !!", " hehe~", "!"};
static const char* SUF_SLEEPY[]  = {" ...", " *nods off*", "..."};
static const char* SUF_GRUMPY[]  = {".", " hmph.", ""};

template <size_t N>
static const char* one(const char* const (&arr)[N]) { return arr[pick(N)]; }

// The core: dress a plain fact in the current mood. When the tiny LLM is
// available it gets first crack, but its output is still bounded to the fact.
String fox_dress(const String& fact, FoxMood mood, const FoxConfig& cfg) {
    // Give the on-device model a chance (returns "" if absent/unsure).
    String flav = llm_flavour(fact, mood, cfg);
    String body = flav.length() ? flav : fact;

    const char* pre = "";
    const char* suf = "";
    switch (mood) {
        case MOOD_SLEEPY:  pre = one(PRE_SLEEPY);  suf = one(SUF_SLEEPY);  break;
        case MOOD_HAPPY:   pre = one(PRE_HAPPY);   suf = one(SUF_HAPPY);   break;
        case MOOD_EXCITED: pre = one(PRE_EXCITED); suf = one(SUF_EXCITED); break;
        case MOOD_GRUMPY:  pre = one(PRE_GRUMPY);  suf = one(SUF_GRUMPY);  break;
        default: break;
    }
    String out = String(pre) + body + String(suf);
    return out;
}

// ---- ELIZA-style reflection for offline "conversation" ----------------------
// When cloud is off and the user says something we can transcribe locally-ish
// (or that arrives as a menu-driven free response), we reflect it back. This is
// the classic Rogerian therapist trick and it is astonishingly convincing on a
// cute animal.
struct Reflect { const char* pat; const char* reply; };
static const Reflect REFLECTS[] = {
    {"i feel",     "why do you feel that way?"},
    {"i am",       "how long have you been?"},
    {"i'm",        "and how does that sit with you?"},
    {"i think",    "what makes you think so?"},
    {"i want",     "what would having it change?"},
    {"i need",     "what happens if you don't get it?"},
    {"because",    "is that the whole reason?"},
    {"you",        "we were talking about you, though~"},
    {"why",        "what do *you* think?"},
    {"no",         "not even a little?"},
    {"yes",        "hehe, tell me more?"},
    {"sad",        "aw. want to sit together a while?"},
    {"happy",      "yay! that makes my tail wag."},
    {"tired",      "mm, me too. we can rest."},
};
static const char* GENERIC[] = {
    "mmhm, go on?", "ooh, and then?", "*tilts head* tell me more?",
    "i'm listening~", "and how did that feel?", "*ears forward* really?",
};

String fox_reflect(const String& user_lower) {
    for (auto& r : REFLECTS) {
        if (user_lower.indexOf(r.pat) >= 0) return String(r.reply);
    }
    return String(GENERIC[pick(sizeof(GENERIC) / sizeof(GENERIC[0]))]);
}

// ---- time-of-day flavour ----------------------------------------------------
String fox_time_greeting(int hour) {
    if (hour < 5)  return "it's so late... but i'm up if you are.";
    if (hour < 11) return "morning! did you sleep okay?";
    if (hour < 17) return "afternoon~ what are we doing?";
    if (hour < 22) return "evening! cozy time.";
    return "getting late. i might curl up soon.";
}

// ---- idle chatter the fox emits on its own (fidgety companion) --------------
static const char* IDLE_HAPPY[] = {
    "*chases own tail*", "poke me, i'm bored~", "did you know i can wiggle?",
    "*sniffs the air*", "hehe.", "boop?",
};
static const char* IDLE_GRUMPY[] = {
    "*sulks*", "hmph. no one plays with me.", "*flicks tail*",
};
static const char* IDLE_SLEEPY[] = {
    "*yawn*", "sleepy...", "five more minutes...",
};
String fox_idle_line(FoxMood mood) {
    switch (mood) {
        case MOOD_GRUMPY: return IDLE_GRUMPY[pick(3)];
        case MOOD_SLEEPY: return IDLE_SLEEPY[pick(3)];
        default:          return IDLE_HAPPY[pick(6)];
    }
}
