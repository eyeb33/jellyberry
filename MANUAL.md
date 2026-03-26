# Jellyberry — User Manual

Jellyberry is a voice-first AI companion powered by Google Gemini. Speak to it naturally — ask questions, set timers and alarms, play ambient sounds, stream internet radio, run focus or meditation sessions, or just have a conversation. It responds through a speaker and uses a 144-LED light strip to express its state visually. Jellyberry also remembers things across sessions — your name, preferences, ongoing projects — picking them up silently as you talk.

---

## Quick Start

1. **Power on** — LEDs pulse blue briefly, then settle to a slow blue wave (Idle)
2. **Wait for connection** — a brief cyan flash indicates the server is ready; the blue wave returns once connected
3. **Tap PAD1** (left touch pad) — LEDs turn red, recording starts
4. **Speak** — ask anything; when you pause, the device detects silence and stops recording automatically
5. **Wait** — LEDs go purple (processing), then blue-green bars animate as the response plays back through the speaker

A 10-second follow-up window opens after each response (cyan pulsing LEDs). Speak during this window to continue the conversation — no button needed.

---

## Talking to Jellyberry

### Starting a conversation
Tap PAD1 from Idle or any display mode. LEDs go red immediately.

### How recording stops
You don't need to press anything to stop. After about 1.5 seconds of silence, the device detects you've finished speaking, stops recording, and sends the audio to Gemini. Mid-sentence pauses won't cut you off — it waits for a clear silence.

### The follow-up window
After every response, a 10-second window opens automatically (cyan pulsing LEDs). Speak naturally during this window — the device starts recording on its own when it hears you. If you stay silent, the window closes and Jellyberry returns to its previous mode.

### Interrupting a response
Tap PAD1 while Jellyberry is speaking. Playback stops immediately and recording starts.

### Memory
Jellyberry remembers things across sessions. Mention your name, a preference, or what you're working on — it stores these silently without any special command. You can also ask *"what did we talk about yesterday?"* and it will retrieve a summary of past conversations.

---

## The Buttons

Both pads support a short press and a long press (hold for 2 seconds).

| | PAD1 (left) | PAD2 (right) |
|---|---|---|
| **Short press** | Start recording (context-dependent, see below) | Cycle to next mode |
| **Long press** | Exit any mode → Idle → start recording | Exit any mode → Idle → start recording |

### Mode-specific PAD1 behaviour

| Active mode | PAD1 short press |
|---|---|
| Idle, Sea Gooseberry, Tide, Moon, Timer | Start recording |
| Ambient sounds | Cycle sound: Rain → Ocean → Rainforest → Fire |
| Radio (playing) | Pause stream → start recording |
| Pomodoro | Start recording (timer continues in background) |
| Meditation | Advance to next chakra |
| Lamp | Cycle colour: White → Red → Green → Blue |
| Eyes | — (no action) |

### Mode-specific PAD2 behaviour

| Active mode | PAD2 short press |
|---|---|
| Radio (playing) | Toggle VU visualiser on/off |
| Any other mode | Advance to next mode in cycle |

---

## Modes

PAD2 short presses cycle through modes in this order:

**Idle → Ambient VU → Sea Gooseberry → Ambient Sounds → Radio → Pomodoro → Meditation → Lamp → Eyes → Idle**

---

### Idle
**LEDs:** Slow blue wave

The default standby state. Tap PAD1 to start a conversation.

---

### Ambient VU
**LEDs:** Green→yellow→red VU bars

A transition display mode — no audio, just the visualiser. Press PAD2 to continue to Sea Gooseberry.

---

### Sea Gooseberry
**LEDs:** Downward-travelling rainbow waves (blue → cyan → green → magenta) on 8 glowing comb rows, with a slow 25-second brightness cycle

A bio-accurate comb jelly animation. All voice functions work normally — tap PAD1 to speak.

---

### Ambient Sounds
**LEDs:** VU meter synced to audio

Plays looping ambient audio. PAD1 cycles between four sounds:

**Rain → Ocean → Rainforest → Fire**

All voice commands work while ambient sound plays.

Try: *"Play ocean sounds"*, *"Stop ambient sound"*

---

### Radio
**LEDs:** Teal VU bars during playback

Stream internet radio. Ask Gemini to find a station by genre, mood, or name.

- **PAD1**: Pauses the stream and starts a voice command. Stream resumes after Jellyberry responds.
- **PAD2 (during playback)**: Toggles the VU visualiser.

Try: *"Play jazz radio"*, *"Find a classical music station"*, *"Something more upbeat"*, *"Stop the radio"*

---

### Pomodoro
**LEDs:** Countdown bar — red during focus, green during short break, blue during long break

A productivity timer. Defaults: 25-min focus / 5-min short break / 15-min long break, cycling 4 times before a longer break. A zen bell chimes at each transition.

PAD1 starts a voice command while the timer continues in the background. Jellyberry returns to the Pomodoro display after responding.

Try: *"How long is left?"*, *"Pause the timer"*, *"Resume"*, *"Start a Pomodoro with 30-minute sessions"*, *"Stop the Pomodoro"*

---

### Meditation
**LEDs:** Breathing fill animation in the current chakra colour. 4-second inhale, 4-second hold, 4-second exhale, 4-second hold.
**Audio:** Soft chakra OM tones.

Chakra sequence: Root (red) → Sacral (orange) → Solar Plexus (yellow) → Heart (green) → Throat (blue) → Third Eye (indigo) → Crown (violet) → returns to Idle.

PAD1 advances to the next chakra at any point. After Crown, the session ends and Jellyberry returns to Idle.

---

### Lamp
**LEDs:** Solid colour with a smooth spiral transition effect

Ambient room lighting. PAD1 cycles: **White → Red → Green → Blue**

---

### Eyes
**LEDs:** Animated eye expressions

A playful visual mode. PAD2 returns to Idle.

---

## Voice Command Reference

### General
| Say | What happens |
|---|---|
| Any question | Jellyberry answers conversationally |
| *"What time is it?"* | Current time (UK timezone) |
| *"What's the date?"* | Today's date |
| *"What's the weather?"* | Current conditions and forecast |
| *"Tell me a joke"* | Jellyberry tells a joke |

### Timers & Alarms
| Say | What happens |
|---|---|
| *"Set a timer for 10 minutes"* | Countdown starts; LED bar shows progress |
| *"Set an alarm for 7 AM"* | Alarm saved — persists across reboots |
| *"Set an alarm for 8:30 tomorrow"* | Gemini checks the time, then sets it |
| *"What alarms do I have?"* | Lists upcoming alarms |
| *"Cancel my alarm"* | Removes the next alarm |

### Pomodoro
| Say | What happens |
|---|---|
| *"Start a Pomodoro"* | Begins with default settings (25/5/15 min) |
| *"Start a Pomodoro with 45-minute sessions"* | Custom focus length |
| *"How long is left?"* | Reports remaining time in current phase |
| *"Which session am I in?"* | Reports current focus/break phase |
| *"Pause the timer"* / *"Resume"* | Pauses or resumes the timer |
| *"Stop the Pomodoro"* | Ends the session |

### Ambient Sounds
| Say | What happens |
|---|---|
| *"Play rain sounds"* | Starts rain ambient loop |
| *"Play ocean sounds"* | Starts ocean ambient loop |
| *"Play rainforest sounds"* | Starts rainforest ambient loop |
| *"Play fire sounds"* | Starts fire ambient loop |
| *"Stop ambient sound"* | Stops playback |

### Radio
| Say | What happens |
|---|---|
| *"Play jazz radio"* | Searches for and streams a jazz station |
| *"Find a classical music station"* | Searches and queues a station |
| *"Something more upbeat"* | Searches for a livelier station |
| *"Stop the radio"* | Stops the stream and returns to Idle |

### Volume
| Say | What happens |
|---|---|
| *"Turn it up"* / *"Turn it down"* | Adjusts volume one step |
| *"Set volume to 7"* | Sets to level 7 (scale of 1–10; default is 3) |
| *"Louder"* / *"Quieter"* | Same as up/down |

### Information
| Say | What happens |
|---|---|
| *"What's the tide right now?"* | Verbal response + tide level LED display |
| *"Is the tide coming in or going out?"* | Current tide direction and time to next change |
| *"What's the moon phase tonight?"* | Verbal response + moon illumination LED display |
| *"What did we talk about yesterday?"* | Retrieves a summary of past sessions |

---

## LED Colour Guide

| Colour / Pattern | State | Meaning |
|---|---|---|
| Slow blue wave | Idle | Ready — tap PAD1 to speak |
| Red pulse | Recording | Capturing your voice |
| Purple/magenta pulse | Processing | Audio sent, waiting for Gemini |
| Blue-green VU bars | Audio Reactive | Jellyberry is speaking |
| Cyan pulse | Conversation Window | Follow-up window open (10 seconds) |
| Cyan solid | Connected | Server connection active |
| White fade in/out | Reconnecting | Re-establishing server connection |
| Green→yellow→red VU | Ambient / Ambient VU | Ambient sound playing |
| Teal VU bars | Radio | Internet radio streaming |
| Countdown bar (red/green/blue) | Pomodoro | Focus or break timer active |
| Chakra colour, breathing | Meditation | Guided breathing in progress |
| White / Red / Green / Blue solid | Lamp | Ambient lighting active |
| Rainbow downward waves | Sea Gooseberry | Comb jelly animation |
| Blue level fill | Tide | Tide status display |
| White level fill | Moon | Moon phase display |
| Orange countdown bar | Timer | Countdown timer active |
| Yellow outward pulse | Alarm | Alarm triggered — tap PAD1 or PAD2 to dismiss |
| Red fast flash | Error | Connection or initialisation failure |
| Blue pulse | Boot | Starting up |

---

## Tips & Tricks

**Long-press escape**: Hold either pad for 2 seconds to exit any active mode and return to Idle. Works from anywhere — use it if Jellyberry seems stuck or you want to switch topics quickly.

**Volume scale**: 1–10, default is level 3 (~30%). Levels 6–8 work well in most rooms. Say *"set volume to X"* at any time, in any mode.

**Conversation window timing**: The follow-up window ignores the first 1.2 seconds after it opens, to let the speaker stop resonating. If you speak immediately after a response ends and nothing happens, wait a moment and try again.

**Radio and voice**: While radio is playing, PAD1 pauses the stream and opens a recording. Once Jellyberry responds, the stream resumes automatically — useful for changing stations or asking questions mid-listen.

**Memory**: Jellyberry stores facts silently — no special phrase needed. Just mention things naturally: *"My name is Alex"*, *"I prefer ocean sounds when I'm working"*. It will remember for next time.

**If Jellyberry stops responding**: Long-press either pad to reset to Idle, then try again. White pulsing LEDs mean it is reconnecting and will be ready within a few seconds.
