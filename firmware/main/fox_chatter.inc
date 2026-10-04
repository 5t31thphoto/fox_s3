// fox_chatter.inc — slot-grammar sentence generator.
//
// Replaces both the degenerate word-Markov (which looped "go go go") and the
// "canned line + hehe" wrapping. A sentence is assembled from interchangeable
// parts chosen by MOOD, so the SAME intent yields thousands of distinct,
// always-grammatical lines. This is the MORF idea done for a fox: the brain
// decides the shape (style/intensity), the grammar fills the slots.
//
//   line  = [opener] core [tag]
//   core  = subject predicate   (agreement handled per subject)
//
// Everything is firmware-owned words, so it can never produce gibberish or
// leak anything unsafe — but it is genuinely generative, not canned.
#pragma once
#include <esp_random.h>

namespace chatter {

template <int N> static const char* pk(const char* const (&a)[N]) { return a[esp_random() % N]; }
static bool chance(int pct) { return (int)(esp_random() % 100) < pct; }

// ---- building blocks, grouped so they recombine sensibly -------------------
// openers set the energy (wider pools)
static const char* OPEN_HAPPY[]   = {"oh!","hey,","ooh,","you know,","guess what,","psst,","oh hey,","listen,","so,","by the way,","","",""};
static const char* OPEN_EXCITED[] = {"ooh ooh!","hey hey!","wow,","okay okay,","listen,","oh oh oh,","quick,","no wait,","","","",""};
static const char* OPEN_SLEEPY[]  = {"mmn,","*yawn*","hey...","soft thought:","mmh,","oh...","*half asleep*","","","",""};
static const char* OPEN_GRUMPY[]  = {"hmph,","ugh,","look,","fine,","honestly,","great,","oh sure,","*sigh*","","","",""};
static const char* OPEN_CALM[]    = {"hmm,","you know,","i was thinking,","so,","funny thing,","quietly now,","between us,","","","",""};

// SUBJECT + matching PREDICATE pools, widened. Each predicate follows its
// subject grammatically. More subjects AND more predicates per subject.
struct SP { const char* subj; const char* preds[10]; int np; };

static const SP SUBJ_HAPPY[] = {
  {"my tail",   {"won't stop wagging","is doing a happy dance","has a mind of its own today","is extra fluffy right now","could sweep the whole floor","is wagging in morse code, it says hi"}, 6},
  {"my ears",   {"just caught something fun","are perked all the way up","are listening for adventures","twitched at a happy sound","are doing their satellite-dish thing"}, 5},
  {"i",         {"feel like chasing something","could boop a hundred snoots","am having the best day","found a warm sunbeam","like it when you're near","want to share a happy little secret","did three spins for no reason","am basically made of good mood right now"}, 8},
  {"the air",   {"smells like good things","feels playful today","is full of little signals","tastes like a good day","has that fun crackle to it"}, 5},
  {"we",        {"should do something fun","make a pretty good team","could go on an adventure","are having a moment, i can tell","should celebrate absolutely nothing"}, 5},
  {"everything",{"seems a little brighter with you here","feels soft and nice","is going my way today","looks more colorful somehow"}, 4},
  {"this moment",{"is a keeper","is exactly where i want to be","feels like a warm blanket"}, 3},
  {"my whiskers",{"are tingling with good vibes","say today is a good one"}, 2},
};
static const SP SUBJ_EXCITED[] = {
  {"i",         {"can't sit still","want to zoom around","caught the zoomies","have so much energy","need an adventure right now","could run up a wall","am vibrating at this point","have forty ideas at once"}, 8},
  {"my paws",   {"are ready to run","are tapping all by themselves","want to go go go","are doing a drumroll"}, 4},
  {"something", {"exciting is about to happen, i can feel it","fun is nearby, i just know it","big is coming, whiskers never lie"}, 3},
  {"we",        {"should play right now","could chase every signal in the room","have to do something, anything!","could set a new record at something"}, 4},
  {"the world", {"is huge and i want to see all of it","is buzzing today","is basically a giant playground"}, 3},
  {"my heart",  {"is doing little backflips","is racing in the fun way"}, 2},
  {"everything",{"is happening at once and i love it","feels turned up to eleven"}, 2},
};
static const SP SUBJ_SLEEPY[] = {
  {"i",         {"found the coziest spot","could nap for a week","am dreaming of treats already","feel soft and slow","keep yawning mid-thought","am about ninety percent blanket right now"}, 6},
  {"my eyes",   {"keep drifting shut","are so heavy","are doing the slow-blink thing","want to take a tiny break"}, 4},
  {"the warm spot",{"is calling my name","is the best spot","has my name on it"}, 3},
  {"everything",{"is quiet and gentle","feels like a soft blanket","is moving in slow motion, nicely"}, 3},
  {"we",        {"could just rest a while","don't have to do anything right now","could have a cozy little nothing-day"}, 3},
  {"my thoughts",{"are drifting like little clouds","are getting all soft around the edges"}, 2},
  {"the quiet", {"feels really nice right now","is my favorite kind of company"}, 2},
};
static const SP SUBJ_GRUMPY[] = {
  {"i",         {"am a little bored","want a snack","could use some attention","am not in the mood","flicked my tail about it","have opinions, and they're grumpy","need something to happen soon"}, 7},
  {"nobody",    {"has played with me in ages","appreciates a good fox","brought snacks, i checked"}, 3},
  {"this",      {"is taking forever","is mildly annoying","is not my favorite","could be going better"}, 4},
  {"my patience",{"is running a bit thin","is a limited resource today","left about an hour ago"}, 3},
  {"everything",{"is too loud","could be more interesting","is conspiring to be dull"}, 3},
  {"the day",   {"owes me a snack","has been deeply uneventful"}, 2},
};
static const SP SUBJ_CALM[] = {
  {"i",         {"was just thinking about you","like these quiet moments","wonder what's out there","feel pretty content","like keeping you company","was counting the little sounds","could stay right here a while"}, 7},
  {"the day",   {"feels calm and easy","is drifting by nicely","has a gentle pace to it"}, 3},
  {"we",        {"make a nice pair","could just hang out","don't need to fill the quiet"}, 3},
  {"it",        {"is peaceful right here","feels good to slow down","is nice to just be"}, 3},
  {"the world", {"is full of little wonders","keeps me curious","is softer than people think"}, 3},
  {"my mind",   {"is wandering somewhere pleasant","keeps circling back to good things"}, 2},
};

// tags close the thought (wider; still often empty so lines can stand alone)
static const char* TAG_HAPPY[]   = {"hehe","isn't that nice?","*wiggle*","just saying!","tee-hee","right?","*happy sigh*","","","","",""};
static const char* TAG_EXCITED[] = {"let's go!","eee!","right?!","*bounce*","can we can we?","woo!","no time to lose!","","","",""};
static const char* TAG_SLEEPY[]  = {"...zzz","mmn","so cozy","*slow blink*","...","","","",""};
static const char* TAG_GRUMPY[]  = {"hmph","...i guess","whatever","if you care","honestly","","","",""};
static const char* TAG_CALM[]    = {"you know?","*soft smile*","mm","i think","anyway","","","",""};

struct Bank { const char* const* open; int no; const SP* sp; int ns;
              const char* const* tag; int nt; };

static Bank bank_for(FoxMood m) {
  switch (m) {
    case MOOD_EXCITED: return {OPEN_EXCITED,12, SUBJ_EXCITED,14, TAG_EXCITED,11};
    case MOOD_SLEEPY:  return {OPEN_SLEEPY,11, SUBJ_SLEEPY,14, TAG_SLEEPY,9};
    case MOOD_GRUMPY:  return {OPEN_GRUMPY,12, SUBJ_GRUMPY,12, TAG_GRUMPY,9};
    case MOOD_HAPPY:   return {OPEN_HAPPY,13, SUBJ_HAPPY,16, TAG_HAPPY,12};
    default:           return {OPEN_CALM,11, SUBJ_CALM,12, TAG_CALM,9};
  }
}

// recent-output memory so we never repeat the same core twice in a row
static uint32_t s_last_hash = 0;
static uint32_t hash_str(const String& s) {
  uint32_t h = 2166136261u;
  for (size_t i = 0; i < s.length(); ++i) { h ^= (uint8_t)s[i]; h *= 16777619u; }
  return h;
}

static String one(const Bank& b, bool strong) {
  // choose subject, then a predicate that fits it
  const SP& sp = b.sp[esp_random() % b.ns];
  String core = String(sp.subj) + " " + sp.preds[esp_random() % sp.np];
  String open = b.open[esp_random() % b.no];
  String tag  = strong ? b.tag[esp_random() % b.nt] : (chance(45) ? b.tag[esp_random() % b.nt] : String(""));
  String out;
  if (open.length()) out += open + " ";
  out += core;
  // punctuation by energy
  char end = strong ? '!' : (chance(25) ? '.' : '\0');
  if (end) out += end;
  if (tag.length()) out += String(end ? " " : (out.endsWith("!")||out.endsWith(".")?" ":", ")) + tag;
  return out;
}

}  // namespace chatter

// A fresh, generated idle/mutter line. `strong` = higher energy (excited moods
// or intensity 3 from the brain).
String fox_generate_line(FoxMood mood, bool strong) {
  using namespace chatter;
  chatter::Bank b = bank_for(mood);
  String out;
  for (int tries = 0; tries < 4; ++tries) {
    out = one(b, strong || mood == MOOD_EXCITED);
    uint32_t h = hash_str(out);
    if (h != s_last_hash) { s_last_hash = h; break; }
  }
  return out;
}
