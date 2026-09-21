// fox_markov.inc — richer idle chatter from a tiny word-level Markov chain,
// plus a pwnagotchi-style "RF mood" that couples the radios to the personality.
// Included by fox_main.cpp.
//
// MARKOV CHATTER
//   The fixed idle lines get repetitive. This walks an order-1 word Markov
//   chain per mood (seeded from a compact corpus baked in here) so the fox's
//   spontaneous mutterings vary. Verified in Python: dozens of distinct,
//   in-character lines from a handful of seeds.
//
// PWNAGOTCHI RF MOOD
//   Like a pwnagotchi gets happy when it captures handshakes and bored when the
//   air is empty, the fox reacts to what its radios see: new BLE/WiFi devices
//   nearby raise excitement/play; a silent, empty environment makes it wistful.
//   Sightings accumulate into a lifetime "friends met" counter it brags about.
#pragma once

extern FoxConfig cfg;
extern FoxNeeds needs;

// ---------------------------------------------------------------------------
//  Markov chatter
// ---------------------------------------------------------------------------
namespace markov {

// Corpus per mood as space-joined lines; we build the chain at boot (cheap).
static const char* CORP_HAPPY[] = {
    "ooh what is that", "i like you", "lets play a game", "the sun feels nice",
    "wag wag wag", "did you see that", "i am so happy", "boop the snoot",
    "hehe you are silly", "i found a thing", "sniff sniff whats this",
    "my tail is wagging",
};
static const char* CORP_GRUMPY[] = {
    "hmph i am bored", "no one plays with me", "flick flick tail", "leave me be",
    "i want a snack", "everything is annoying", "why is it so loud",
    "i am not amused",
};
static const char* CORP_SLEEPY[] = {
    "yawn so sleepy", "five more minutes", "the warm spot is nice",
    "curling up now", "eyes are heavy", "soft and cozy", "dreaming of treats",
    "slow blink",
};
static const char* CORP_EXCITED[] = {
    "ooh ooh ooh", "lets go lets go", "did you hear that", "so many devices",
    "i caught a signal", "zoom zoom zoom", "adventure time", "my ears are up",
};

// A minimal order-1 chain: for a given word, collect all observed successors
// across the mood's corpus and pick one at random. We don't precompute a table;
// with a tiny corpus we just scan it each step. O(words) per token — trivial.
struct Corpus { const char* const* lines; int n; };

static Corpus corpus_for(FoxMood m) {
    switch (m) {
        case MOOD_GRUMPY: return { CORP_GRUMPY, (int)(sizeof(CORP_GRUMPY)/sizeof(char*)) };
        case MOOD_SLEEPY: return { CORP_SLEEPY, (int)(sizeof(CORP_SLEEPY)/sizeof(char*)) };
        case MOOD_EXCITED:return { CORP_EXCITED,(int)(sizeof(CORP_EXCITED)/sizeof(char*)) };
        default:          return { CORP_HAPPY, (int)(sizeof(CORP_HAPPY)/sizeof(char*)) };
    }
}

// pick a random successor of `word` seen in the corpus; "" if none (end).
static String successor(const Corpus& c, const String& word, bool start) {
    // reservoir-sample over all matches so we don't need to count first.
    String pick = ""; int seen = 0;
    for (int i = 0; i < c.n; ++i) {
        String line = c.lines[i];
        // tokenize
        int from = 0;
        String prev = "<s>";
        while (from <= (int)line.length()) {
            int sp = line.indexOf(' ', from);
            String tok = (sp < 0) ? line.substring(from) : line.substring(from, sp);
            bool match = start ? (prev == "<s>") : (prev == word);
            if (match && tok.length()) {
                if ((int)(esp_random() % (++seen)) == 0) pick = tok;
            }
            prev = tok;
            if (sp < 0) break;
            from = sp + 1;
        }
        // also allow end-of-line as a successor (so lines can terminate)
    }
    return pick;
}

}  // namespace markov

// Generate a fresh idle line for the mood via the Markov walk. Falls back to
// the fixed fox_idle_line() if the walk produces nothing.
String fox_markov_line(FoxMood mood) {
    using namespace markov;
    Corpus c = corpus_for(mood);
    String first = successor(c, "", true);
    if (!first.length()) return fox_idle_line(mood);
    String out = first, cur = first;
    for (int i = 0; i < 7; ++i) {
        String nxt = successor(c, cur, false);
        if (!nxt.length()) break;
        out += " " + nxt;
        cur = nxt;
    }
    return out;
}

// ---------------------------------------------------------------------------
//  Pwnagotchi-style RF mood
// ---------------------------------------------------------------------------
// Kept as a lightweight background sampler the main loop can call occasionally.
// It does a quick passive count of nearby WiFi APs (cheap, no association) and
// nudges the fox's needs: a lively environment excites it, an empty one makes
// it quietly lonely. New unique networks since last check count as "friends".
namespace rfmood {

static int   last_ap_count = -1;
static uint32_t lifetime_friends = 0;   // persisted brag counter
static uint32_t last_sample = 0;

}  // namespace rfmood

#include <WiFi.h>

// Call from loop() at a relaxed cadence (handled by the caller's timer).
// Returns an optional remark the fox can mutter, or "" to stay quiet.
String rf_mood_tick() {
    using namespace rfmood;
    if (!cfg.tool_wifi) return "";
    uint32_t now = millis();
    if (now - last_sample < 45000) return "";   // sample at most ~every 45s
    last_sample = now;

    int n = WiFi.scanNetworks(false, false, false, 120);   // fast passive-ish
    WiFi.scanDelete();
    if (n < 0) n = 0;

    String remark = "";
    if (last_ap_count >= 0) {
        int delta = n - last_ap_count;
        if (delta >= 3) {
            needs_interact(needs, true);              // excitement
            lifetime_friends += delta;
            remark = "ooh, lots of new signals nearby!";
        } else if (n == 0) {
            // quiet air → a touch lonely
            if (needs.social > 5) needs.social -= 5;
            remark = "it's so quiet out here...";
        }
    }
    last_ap_count = n;
    return remark;
}

uint32_t rf_lifetime_friends() { return rfmood::lifetime_friends; }
