# Fox user guide

*Generated from the firmware's own command and menu tables (`tools/` script). Every phrase listed here is exactly what the fox registers with her offline speech model at boot.*

## Buttons

| On the face | Does |
|---|---|
| **Hold** the button and talk, then release | she listens and answers |
| **Double-click** | opens the menu |
| **Single click** | a boop / little reaction |
| **Picking her up or a button press** | wakes her from a nap |

In the menu, single-click selects. In any game, tool or toy, **double-click** goes back to the menu.

## How she understands you

1. **Offline first.** Her on-device speech model knows the phrases below. A known command always runs on the device, instantly, even with no internet.
2. **Online brain (optional).** With a Groq API key and WiFi, anything that isn't one of her phrases goes to Groq's free speech-to-text and a free chat model. If one free model's daily budget runs out, she switches to the next free one; if all are resting, she tells you once and uses her offline brain.
3. **Reports and tools never need Groq.** Weather, space weather, aurora and bitcoin only need WiFi.
4. **If she half-hears you**, she asks *"did you mean …?"*. Say **yes** to run it.
5. **If the internet or Groq is struggling**, she stops waiting on it for two minutes and answers with her offline brain, so she never goes silent.

Say **"go offline"** or **"go online"** (or use **Settings → brain online/offline**) to switch. It's saved.

## Voice commands

Hold the button and say any of these. Alternate phrasings are separated by " / ".

### Everyday

| Say | What happens |
|---|---|
| hello / hello fox | says hello back |
| what time is it / tell me the time | tells the time |
| how are you / how do you feel | how she's feeling |
| what is the weather / tell me the weather | live weather for your location |
| open the menu / show menu | opens the menu |
| conversation mode / lets chat | conversation mode (talk without holding the button) |
| remember this / remember that | remembers the next thing you say |
| what do you remember / tell me a memory | tells you a memory |

### Reports (need WiFi, not Groq)

| Say | What happens |
|---|---|
| space weather / solar storm | NOAA space weather (Kp index + 24h storm outlook) |
| any aurora / northern lights | NOAA aurora chance at your location |
| bitcoin price / how much is bitcoin | live bitcoin price |

### Games & encounters

| Say | What happens |
|---|---|
| play a game / pick a game | she picks a game for you |
| play wormhole / fly the ship | wormhole flying game (tilt) |
| catch the treats / catch game | catch-the-treats game (tilt) |
| twenty questions / guess my thing | twenty questions (click = yes, shake = no) |
| explore the maze / lets explore | roguelike maze (tilt) |
| reaction test / test my reflexes | reaction-time test |
| guess my paw / paw game | guess-the-paw game |
| play with me / surprise me | a surprise fox encounter |
| pet the fox / can i pet you | pet her (tilt side to side) |
| are you hungry / want a snack | give her a treat (flick up) |
| tug of war / play tug | tug of war (flick back and forth) |

### Toys

| Say | What happens |
|---|---|
| show me colors / pretty lights | pretty lights |
| starfield / fly through space | starfield |
| ink sandbox / ink mode | ink sandbox |
| spirograph / draw a spiral | spirograph |
| lip sync mode / puppet mode | puppet mode: her mouth follows your voice |

### Radios

| Say | What happens |
|---|---|
| scan for devices / bluetooth radar | Bluetooth radar (turn around once) |
| scan wifi / wifi radar | WiFi radar (turn around once) |
| sniffer mode / hunt mode | hunt mode: passive WiFi listening, levels up |
| what are phones looking for / probe scan | shows network names nearby phones are looking for |

### Settings

| Say | What happens |
|---|---|
| volume up / louder | louder |
| volume down / quieter | quieter |
| change the volume / volume setting | steps the volume |
| change your voice / switch voice | switches voice pack |
| go online / online mode | online brain (Groq free tier) |
| go offline / offline mode | offline brain (nothing leaves the device) |
| forget everything / forget your memories | clears her memories |
| go to sleep / good night | naps (screen off; button or pick-up wakes her) |

## Talking with her (offline conversation)

These aren't commands — they're conversation. Her on-device brain decides *how* she answers (tone, gesture, energy) and *what she does next*: ask you something back, offer an activity, share a fact, or check in on you later.

**About her:** *how old are you* · *where do you live* · *what do you eat* / *are you hungry fox* · *what is your favorite color* · *do you have friends* · *do you dream* / *what do you dream about* · *are you real* / *are you a robot* · *what is your name* / *who are you* · *what are you doing* / *what are you up to*

**Tell her how you feel:** *i am sad* / *i feel sad* · *i am happy* / *good day* · *i am tired* / *so tired* · *i am hungry* / *i want food* · *i am cold* / *it is cold* · *i am hot* / *it is hot* · *i am scared* / *i am afraid* · *i am lonely* / *i feel alone* · *i am excited* / *guess what* · *i am angry* / *i am mad*

**Things you say:** *i love you* / *good girl* · *thank you* / *thanks fox* · *sorry* / *i am sorry* · *good morning* / *morning fox* · *goodbye* / *see you later* · *good job* / *well done* · *do you like me* / *are we friends* · *i am bored* / *so bored* · *i am home* / *i am back* · *i have to go* / *i am leaving* · *i am going to work* / *i am going to school* · *i missed you* / *i miss you* · *it is my birthday* · *you are funny* / *you are silly* · *you are annoying* / *you are mean* · *give me a hug* / *hug me* · *it is raining* / *it is sunny*

**Ask her for something:** *tell me a joke* / *make me laugh* · *tell me a story* · *tell me a fact* / *tell me something cool* · *tell me a secret* · *sing a song* / *sing for me* · *give me a compliment* / *say something nice* · *cheer me up* / *make me happy* · *do a trick* / *show me a trick* · *make a noise* / *make a sound* · *flip a coin* / *heads or tails* · *roll a dice* / *roll the dice* · *pick a number* / *give me a number* · *yes or no* / *should i do it* · *ask me a question* / *quiz me* · *what should i do* / *give me an idea*

**Replying to her:** *yes* / *yeah* / *okay* · *no* / *no thanks* / *not now* · *maybe* / *i dont know* · *why* / *how come* · *tell me more* / *go on* · *what about you* / *and you* · *me too* / *same here* · *really* / *no way* · *wow* / *cool*

**How replies work:** when she offers something (*"want to watch some pretty lights?"*), **yes** starts it, **no** declines, **why** gets her reason. When she asks a question, your yes/no answers *that* question. **Tell me more** continues a story. **What about you** gets her side of what you just said.

## Fox time

Every few minutes when you're not busy, she may ask to do something — a treat, pets, tug of war, a game, a story. Say **yes** within about 15 seconds, or ignore it and she'll shrug it off.

## Menu

| Menu item | What it is |
|---|---|
| talk to me | listen once (same as holding the button) |
| conversation mode | conversation mode |
| -- tools -- | — |
| BLE radar | Bluetooth radar (turn around once) |
| WiFi radar | WiFi radar (turn around once) |
| hunt mode | hunt mode: passive WiFi listening, levels up |
| probe sniffer | shows network names nearby phones are looking for |
| -- games -- | — |
| roguelike maze | roguelike maze (tilt) |
| wormhole (tilt) | wormhole flying game (tilt) |
| catch treats (tilt) | catch-the-treats game (tilt) |
| 20 questions | twenty questions (click = yes, shake = no) |
| reaction test | reaction-time test |
| guess my paw | guess-the-paw game |
| -- fox time -- | — |
| pet the fox | pet her (tilt side to side) |
| feed the fox | give her a treat (flick up) |
| tug of war | tug of war (flick back and forth) |
| surprise me | a surprise fox encounter |
| -- toys -- | — |
| plasma | pretty lights |
| starfield | starfield |
| ink sandbox | ink sandbox |
| spirograph | spirograph |
| lip-sync puppet | puppet mode: her mouth follows your voice |
| -- reports -- | — |
| weather | live weather for your location |
| space weather | NOAA space weather (Kp index + 24h storm outlook) |
| aurora tonight? | NOAA aurora chance at your location |
| bitcoin price | live bitcoin price |
| -- settings -- | — |
| volume | steps the volume |
| voice pack | switches voice pack |
| brain online/offline | toggle online/offline brain |
| forget memories | clears her memories |
| sleep | naps (screen off; button or pick-up wakes her) |
| close | close the menu |

## Radios: what to expect

- **Radars:** hold her still, then turn around **once**, keeping the green needle on the red one. After that the map stays pinned to the room and turns with you; the arrow at the top is the way you're facing. Click to re-measure, double-click to exit.
- **Hunt mode** and **probe sniffer** only *listen*. Nothing is transmitted and nothing is saved.

## Brains and voices

- **Chatterbox** (flasher brain A): talks more, asks follow-up questions.
- **Critter** (flasher brain B): sillier, offers more games.
- **Voice:** *chatterbox* uses PicoTTS with SAM as a fallback; *critter* uses SAM. Tune SAM's pitch, speed, throat and mouth in the web flasher.

## Online setup (optional)

Fill in WiFi, your Groq API key and your weather location in the web flasher before you click Flash. The default chat model is `openai/gpt-oss-20b`; she rotates to `openai/gpt-oss-120b` and `qwen/qwen3.8-27b` if one runs out. On Groq's free tier each chat model allows about **30 requests a minute and 1,000 a day**, and speech-to-text has its own separate budget.
