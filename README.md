# 🦊 Fox Voice Companion

A cute, fidgety anime-fox companion that lives on an **M5Stack AtomS3R** with an
**Atomic Echo Base**. It listens, talks, makes faces, remembers you, blasts IR at
your TV, and plays little games — **entirely offline by default**. If you give it
wifi and a free API key, it grows an online brain too. But the online part is a
*bonus*, never a requirement.

There is **no wake word**. You talk to the fox by holding its button
(push-to-talk). A quick tap opens its menu. It can also enter a hands-free
"conversation mode" that listens for a while and times out on silence.

---

## What it does

Offline-first, cloud-as-a-bonus. Everything below works with **no network**; wifi
+ a free API key only *adds* to it.

**Voice control (offline).** MultiNet7 on-device speech recognition maps ~22
spoken commands to actions — no wake word, push-to-talk only. The fox can launch
any tool or game for you and give a spoken report ("scan for devices", "any
aurora tonight?", "play wormhole").

**Flipper-style hacker tools.**
- **BLE radar** — rotate the device and nearby Bluetooth devices plot around a
  sweep by bearing (IMU magnetometer), blip radius ~ signal strength.
- **WiFi radar** — same, for access points.
- **Passive packet sniffer** — a receive-only channel hopper with a live
  mgmt/data/ctrl frame scope. Transmits nothing.
- **IR blaster** — a TV-B-Gone-style code library with "fake learning": the fox
  sweeps power codes, you tap the instant your TV turns off, and it
  binary-searches down to *your* code and remembers it.

**Digital fidget games.**
- **Wormhole** — fly a ship through rings by tilting (IMU).
- **Catch** — tilt a basket to catch falling treats.
- **20 Questions** — a real **Bayesian guesser** (Akinator-style): it keeps a
  belief distribution over candidates and asks the question with the highest
  expected information gain each turn (verified: identifies all entities with
  honest answers, ~93% robust under noisy answers, ~5 questions average).
- **Reaction test**, **guess-my-paw**, and a **lip-sync puppet** mode that opens
  the fox's mouth in time with sound via a real FFT on the mic.

**Alive, emotive, pwnagotchi/furby-flavoured.**
- **Moods** from decaying play/social/energy needs colour its face and phrasing.
- **Markov chatter** — idle mutterings are generated from a small per-mood word
  Markov chain, so they vary instead of repeating fixed lines.
- **RF mood (pwnagotchi-style)** — the radios feed the feelings: lots of new
  devices nearby excite it and rack up a lifetime "friends met" counter; empty,
  silent air makes it wistful.
- **IMU reactions** — boop it, shake it, pick it up to wake it.
- **Tiny on-device brain** — an optional ~200K–1M param transformer (trained from
  scratch, int8, runs from a flash partition) rephrases lines to be cuter. It can
  never invent facts (safety gate).

**Talks** with two swappable voices (PicoTTS "chatterbox" / SAM-ish "critter")
plus a babble fallback so it is *never* mute. Captions always show on screen.

**Remembers** in a device-owned rolling journal on its own flash partition; the
cloud only ever sees a bounded recent slice.

**Optional online brain.** With wifi + an OpenAI-compatible key (Groq's free tier
works great), it transcribes free speech (Whisper) and holds richer
conversations — and can **call its own tools mid-conversation** (BLE/WiFi scan,
space weather, TV power) via function-calling, then speak the result.

**Website-configurable, web-flashed.** Flash from the browser with esp-web-tools;
a configurator sends name/personality/wifi/key/colours **and per-tool toggles**
to the device over USB. Secrets live only on the device — never in CI or the
binary.

---

## Hardware

| Part | Notes |
|------|-------|
| M5Stack **AtomS3R** | ESP32-S3, 8MB flash, 8MB octal PSRAM, 128×128 LCD, BMI270+BMM150 IMU, IR LED on **GPIO47**, USER button on **GPIO41** |
| **Atomic Echo Base** | ES8311 codec, mic + speaker over I2S (DIN 7 / WS 6 / DOUT 5 / BCK 8; I2C SDA 38 / SCL 39; amp via PI4IOE @0x43) |

### Audio path (important)

**Do not** set `M5.config().external_speaker.atomic_echo = true` before
`M5.begin()` on AtomS3R + Atomic Echo Base. That path hangs inside
`M5.begin()` on this hardware (blank screen, no serial after
“Returned from app_main()”).

The working mic-avatar demo from M5 avoids that path. This firmware does the
same:

1. Plain `M5.begin()` → display comes up first
2. Standalone **M5Atomic-EchoBase** library (vendored under `firmware/main/echobase/`)
3. All mic/speaker I/O goes through `fox_audio.*`

See `fox_audio.h` / `fox_audio.cpp` and the comments in `setup()`.

---

## Flash it (no toolchain needed)

1. Open the **web flasher** (GitHub Pages deploy of `web/`) in Chrome, Edge, or
   Opera on a desktop.
2. Pick a **brain pack** (Chatterbox or Critter).
3. Click **Flash my fox** and follow the prompt. ~7 MB, about a minute.
4. In step 3, fill in a name/personality (and optionally wifi + API key) and
   click **Send to fox** — this streams the settings to the device over USB.
   **Your wifi password and API key are written only to the device.** They are
   never uploaded and never compiled into the firmware or CI.

Prefer the command line? Build with ESP-IDF v5.5.2 (`firmware/`), then flash the
app plus the data partitions at the offsets in `firmware/partitions.csv`.

---

## Brain packs (swappable at flash time)

| Pack | Voice | On-device brain | Feel |
|------|-------|-----------------|------|
| **A — Chatterbox** (default) | PicoTTS, pitched up | ~200K params | Speaks clear words; friendliest default |
| **B — Critter** | procedural formant synth | ~700K params | Chirpy little creature; more character |

Both are flashed; you can switch from the fox's on-device menu. Captions show
either way.

---

## The on-device brain, honestly

The brain is a real llama-style transformer (RMSNorm + RoPE + attention +
SwiGLU, int8 weights) trained **from scratch in pure numpy** — no PyTorch — by
`tools/train_brain.py`, so it builds on a plain CI runner in ~20–30 s. It reaches
cross-entropy ~0.2 and generates lines like:

```
[happy]   it is sunny        ->  ooh it is sunny.
[sleepy]  it is night        ->  it is night... *yawn*
[excited] found your remote  ->  found your remote! wag!
[grumpy]  battery is low     ->  battery is low. hmph.
```

It is intentionally tiny and "barely able to speak" — that's the charm. It runs
straight out of a flash partition with no RAM copy of the weights.

### Safety: the brain can't lie

The firmware computes every real fact (the time, a scan result, a tool action).
The brain only ever *rewraps* that fact. Its output is accepted **only if the
fact's key word survives** in what it generated; otherwise it's discarded and the
deterministic template layer speaks instead. So an undertrained or hallucinating
micro-model can never make the fox claim something false — worst case, it's quiet
and the templates carry the line. The fox also never fakes an action or speech it
didn't actually perform.

---

## Build pipeline

GitHub Actions (`.github/workflows/build-firmware.yml`):

1. **data job** — `pip install numpy`, then `build_ir.py` compiles the Flipper
   `.ir` libraries into a compact `FOXI` blob, and `train_brain.py` trains both
   brain packs into `FOXB` blobs.
2. **firmware job** — builds with `espressif/esp-idf-ci-action` (IDF v5.5.2,
   target esp32s3), assembles `dist/` (bootloader + partition table + app + data
   bins + `SHA256SUMS`), and emits an **esp-web-tools `manifest.json`**.
3. **pages** — publishes `web/` + `dist/` so the flasher works from a URL.

No secrets are ever needed to build; configuration happens on-device post-flash.

---

## Repo layout

```
firmware/                ESP-IDF (Arduino-as-component) app
  main/
    fox.h                shared config/types/tunables
    fox_main.cpp         wiring: speech, cloud, dispatch, PTT loop
    fox_brain.cpp        personality: moods, needs, reflection, templates
    fox_voice.cpp        PicoTTS / critter / babble speech
    fox_memory.cpp       device-owned rolling journal (LittleFS)
    fox_llm.cpp          tiny on-device brain loader + safety gate
    fox_llm_forward.inc  transformer forward pass (int8, mmap weights)
    fox_sam.c            original procedural "critter" voice
    fox_ir.inc           IR sweep + fake-learning (RMT TX, GPIO47)
    fox_face.inc         animated face + caption + lip-sync hooks
    fox_input.inc        PTT capture, IMU gestures, menu, USB config, sleep
    data/                built FOXB/FOXI blobs land here
  partitions.csv         8MB layout (app, models, PicoTTS, foxbrain, foxdata, foxfs)
  sdkconfig.defaults     esp32s3, PSRAM, MultiNet7, no wakenet, USB-CDC
tools/
  build_ir.py            Flipper .ir  -> FOXI binary
  train_brain.py         from-scratch numpy transformer -> FOXB binary
assets/ir/               source .ir libraries (tv/audio/projector/ac)
web/index.html           esp-web-tools flasher + USB config sender
tests/                   build-artifact validation (no toolchain needed)
docs/                    architecture, partition map, limitations
```

## Run the tests

```
python tests/test_build_artifacts.py
```

Validates the IR converter, the brain byte-contract (trainer ↔ firmware must
agree exactly), and the command registry — all without an ESP32.

---

## Known limitations & honest notes

See `docs/LIMITATIONS.md`. Highlights: PicoTTS "cuteness" is prosody-limited and
best confirmed on real hardware; the "critter" voice is an **original** synth,
not the licensing-murky reverse-engineered SAM; MultiNet phrases may need tuning
for your accent; the on-device brain is a toy by design.
