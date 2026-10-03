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
#include "foxese.h"
#include <esp_random.h>

// forward decl from fox_llm.cpp (weak — may be a stub that returns "")
bool brain_decide(FoxMood mood, const BrainCtx& ctx, Foxese& x);

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

template <size_t N>
static const char* one(const char* const (&arr)[N]) { return arr[pick(N)]; }

// The core: dress a plain fact in the current mood. When the tiny LLM is
// available it gets first crack, but its output is still bounded to the fact.
BrainCtx g_brain_ctx;
uint8_t  g_brain_next = 0;

// Every spoken line passes through here exactly once. The brain decides the
// delivery (and, for an ARMED conversational line, the next move); Foxese
// renders it. One decoration path — nothing is stacked twice.
String fox_dress(const String& fact, FoxMood mood, const FoxConfig& cfg) {
    (void)cfg;
    BrainCtx ctx = g_brain_ctx;
    bool armed = ctx.armed;
    g_brain_ctx = BrainCtx{};                 // one-shot: later lines default to "say"
    Foxese x;
    if (!brain_decide(mood, ctx, x)) return fact;
    if (armed) g_brain_next = x.next;         // only conversational replies steer
    String out = foxese_expand(x, fact);
    return out.length() ? out : fact;
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
