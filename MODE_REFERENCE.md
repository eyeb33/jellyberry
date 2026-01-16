# JellyBerry Mode Reference Guide

Complete documentation of all LED modes, controls, animations, and voice interactions.

---

## Mode Cycling (Button 2)
Press **Button 2** to cycle through modes in this order:
1. Idle → Ambient VU → Nature Sounds → Pomodoro → Meditation → Clock → Lamp → Sea Gooseberry → Idle

---

## 🔵 IDLE Mode

**Marquee:** "IDLE MODE"

### Visual
- **Animation:** Slow wave pattern traveling up and down the LED matrix
- **Color:** Blue (hue 160)
- **Pattern:** Smooth sine wave with varying brightness

### Controls
- **Button 1 Short Press:** Start Gemini voice recording
- **Button 2:** Switch to Ambient mode

### Voice Commands (via Button 1)
- Set timers, alarms
- Check tide status, moon phase, weather
- Start Pomodoro, meditation
- Play ambient sounds
- Ask general questions

---

## 🎵 AMBIENT Mode

**Marquee:** "RAIN" / "OCEAN" / "RAINFOREST" (depending on sound selected)

### Visual
- **Animation:** VU meter audio visualization
- **Color:** Green spectrum based on audio amplitude
- **Pattern:** Vertical bars synchronized to ambient audio playback

### Controls
- **Button 1 Short Press:** Talk to Gemini (ambient continues in background)
- **Button 2:** Switch to Nature Sounds mode

### Voice Commands
- "Play rain sounds"
- "Play ocean sounds"  
- "Play rainforest sounds"
- "Stop ambient sounds"
- All standard Gemini functions available

### Notes
- Audio continues playing while in other modes until explicitly stopped
- VU meter active only when in Ambient mode LED display

---

## 🍅 POMODORO Mode

**Marquee:** "POMODORO" (on entry)

### Visual
- **Animation:** Countdown/countup timer visualization
- **Focus Sessions (Red):** Countdown from full (top) to empty (bottom)
- **Short Break (Green):** Count up from empty to full
- **Long Break (Blue):** Count up from empty to full
- **Breathing Pulse:** Active LED row pulses (fast when running, slow when paused)
- **Progress Display:** Number of lit LEDs indicates time remaining/elapsed

### Auto-Start Behavior
- Timer automatically starts after marquee completes with zen bell chime
- No manual start required

### Controls
- **Button 1 Short Press:** Talk to Gemini (timer continues in background)
  - Returns to Pomodoro display after 10s conversation window
- **Button 1 Long Press (2 seconds):** Pause/Resume timer
  - No sound on pause/resume (user will source custom sounds)
- **Button 2:** Exit Pomodoro mode (cancels and clears timer)

### Voice Commands (via Button 1)
- "How long is left on the Pomodoro timer?"
- "Which cycle am I in?"
- "Pause the timer" (alternative to long press)
- "Resume the timer" (alternative to long press)
- "Stop the Pomodoro"
- **Start with custom durations:**
  - "Start a Pomodoro with 15 minute work sessions, 5 minute breaks, and 15 minute long break"
  - "Set a Pomodoro timer with 45 minute focus periods"

### Default Durations
- **Focus:** 25 minutes (customizable via voice)
- **Short Break:** 5 minutes (customizable via voice)
- **Long Break:** 15 minutes (customizable via voice)
- **Cycle:** 4 focus sessions, then long break

### Transition Sounds
- **Session Start (Auto):** Zen bell
- **Focus → Break:** Zen bell
- **Break → Focus:** Zen bell
- **Cycle Complete:** Zen bell

### Session Flow
1. Focus (25min) → Short Break (5min)
2. Focus (25min) → Short Break (5min)
3. Focus (25min) → Short Break (5min)
4. Focus (25min) → Long Break (15min)
5. Cycle complete → Return to Focus (paused, press Button 1 long or use voice to start)

---

## 🧘 MEDITATION Mode

**Marquee:** "MEDITATION"

### Visual
- **Animation:** Breathing visualization cycling through 7 chakra colors
- **Breathing Cycle:**
  - Inhale (4s): LEDs fill from bottom to top
  - Hold Top (4s): Full brightness, gentle pulse
  - Exhale (4s): LEDs empty from top to bottom
  - Hold Bottom (4s): Minimum brightness
- **Chakra Colors (in order):**
  1. Root - Red
  2. Sacral - Orange
  3. Solar Plexus - Yellow
  4. Heart - Green
  5. Throat - Blue
  6. Third Eye - Indigo
  7. Crown - Violet

### Audio
- Chakra-specific Om chants (om001.pcm - om007.pcm)
- Plays continuously during meditation
- Volume automatically reduced to 5% (restored on exit)

### Controls
- **Button 1 Short Press:** Pause/Resume breathing animation
- **Button 1 Long Press (2+ seconds):** Advance to next chakra (Root → Sacral → Solar → Heart → Throat → Third Eye → Crown → Root)
  - Stops current OM audio and starts new chakra's frequency
  - Displays chakra name as marquee
  - Plays volume chime for feedback
- **Button 2 Long Press (2+ seconds):** Return to IDLE mode and start Gemini recording
- **Button 2 Short Press:** Cycle to next mode (Clock)

### Voice Commands
- Not available during meditation (Button 1 controls breathing/chakra)

### Notes
- Volume lowered to 5% for meditation ambiance
- Audio stops and volume restores when exiting mode

---

## 🕐 CLOCK Mode

**Marquee:** "CLOCK"

### Visual
- **Animation:** Digital clock display (hours:minutes)
- **Color:** White/cyan digits
- **Update:** Every minute
- **Layout:** Time displayed across LED matrix in readable format

### Controls
- **Button 1 Short Press:** Talk to Gemini (clock display pauses)
  - Returns to clock after 10s conversation window
- **Button 2:** Switch to Lamp mode

### Voice Commands
- "What time is it?"
- "What's the current time?"
- All standard Gemini functions available

---

## 💡 LAMP Mode

**Marquee:** "LAMP"

### Visual
- **Animation:** Solid color fill, all LEDs same color
- **Colors (cycle with Button 1):**
  1. White (255, 255, 255)
  2. Red (255, 0, 0)
  3. Green (0, 255, 0)
  4. Blue (0, 0, 255)
- **Transition:** Smooth fade between colors (1 second)

### Controls
- **Button 1 Short Press:** Cycle to next color (White → Red → Green → Blue → White)
- **Button 2:** Exit lamp mode (return to Idle)

### Voice Commands
- Not available in Lamp mode (Button 1 cycles colors)

### Notes
- Simple static lighting mode for ambient illumination

---

## 🌊 SEA GOOSEBERRY Mode

**Marquee:** "SEA JELLY"

### Visual
- **Animation:** Living comb jelly drifting in dark water
- **Effect:** Multiple downward-traveling iridescent waves along vertical "comb rows"
- **Colors:** Soft rainbow gradients (blue, cyan, green, magenta)
- **Pattern:** 8 primary bright ribs + 4 dimmer in-between strips
- **Motion:** Organic, phase-shifted waves rotating around the sphere
- **Breathing:** Slow 30-second global brightness modulation

### Biology Inspiration
Mimics real sea gooseberries (Pleurobrachia spp.) - transparent comb jellies with 8 meridional rows of beating cilia. The cilia refract light to create traveling waves of rainbow color flowing from pole to pole.

### Technical Details
- **8 Primary Comb Rows:** Strips 0, 2, 3, 5, 6, 8, 9, 11 (bright, like real comb plates)
- **4 Dimmer Ribs:** Strips 1, 4, 7, 10 (40% brightness, in-between glow)
- **Wave System:** 2 overlapping waves with different speeds for depth
- **Phase Offsets:** Each rib slightly ahead/behind neighbors (rotation effect)
- **Speed Variation:** ±10% per rib for organic, non-mechanical feel
- **Hue Variation:** ±10% per rib for color diversity
- **Wave Speed:** ~1.5 seconds per full downward travel (adjustable)
- **Brightness:** 60% default, ±30% breathing modulation

### Animation Characteristics
- **Non-blocking:** Updates via time deltas, no blocking delay() calls
- **Smooth:** HSV color interpolation with easing curves
- **Relaxing:** Slow, drifting motion like a real creature hovering
- **Never harsh:** No strobing or white flashes, only soft iridescent gradients

### Controls
- **Button 1 Short Press:** Talk to Gemini (animation continues in background)
  - Returns to Sea Gooseberry after 10s conversation window
- **Button 2:** Exit to Idle mode

### Voice Commands
- All standard Gemini functions available (animation plays during responses)

### Notes
- Most scientifically accurate mode - closely models real comb jelly bioluminescence
- Optimized for ESP32-S3 performance with minimal CPU overhead
- Perfect for meditation, relaxation, or just watching nature's light show

---

## 🔴 RECORDING Mode (Transient)

**Not a selectable mode** - Activated automatically when Button 1 pressed in eligible modes

### Visual
- **Animation:** Pulsing red pattern
- **Color:** Red (indicates voice recording active)
- **Pattern:** Breathing effect while listening

### Active Recording States
1. **Initial Recording:** Waiting for voice activity (VAD threshold: 700)
2. **Voice Detected:** Recording in progress (30s max, or until silence)
3. **Processing:** Uploading to Gemini

### Timeout
- **Max Duration:** 30 seconds
- **Silence Timeout:** ~2 seconds of silence ends recording

### Exit
- Returns to previous mode after Gemini response completes

---

## ⏳ PROCESSING Mode (Transient)

**Not a selectable mode** - Shows after recording while waiting for Gemini response

### Visual
- **Animation:** "Thinking" animation with rotating/pulsing pattern
- **Color:** Purple/magenta
- **Delay:** Shows after 3.5 seconds if no response received

### Timeout
- **Duration:** 10 seconds max
- **Recovery:** Returns to Pomodoro/Timer/Moon/Tide if active, otherwise Idle

---

## 🔊 AUDIO_REACTIVE Mode (Transient)

**Not a selectable mode** - Shows during Gemini audio playback

### Visual
- **Animation:** VU meter synchronized to voice response
- **Color:** Blue-green spectrum based on audio amplitude
- **Pattern:** Vertical bars showing real-time audio levels
- **Sync Delay:** ~240ms buffer for smooth synchronization
- **Decay:** Fast fade (0.60x per frame) when audio drops

### Behavior
- Active during Gemini voice responses
- Smoothed audio levels prevent jitter
- Rapid fade when response completes

---

## 💬 CONVERSATION WINDOW Mode (Transient)

**Not a selectable mode** - Opens after Gemini responses for follow-up questions

### Visual
- **Animation:** Gentle pulsing cyan
- **Color:** Cyan/light blue
- **Pattern:** Breathing effect inviting interaction

### Duration
- **Window:** 10 seconds
- **Purpose:** Listen for follow-up questions

### Behavior
- Opens automatically after every Gemini response (turnComplete=true)
- If voice detected (VAD threshold: 650), starts new recording
- If no voice after 10s, returns to previous mode:
  - Priority: Pomodoro → Timer → Moon → Tide → Idle

---

## ⏱️ TIMER Mode (Transient)

**Not a selectable mode** - Activated via voice command "set a timer for X minutes"

### Visual
- **Animation:** Countdown visualization
- **Color:** Orange/amber
- **Pattern:** Vertical bar graph showing time remaining

### Controls
- **Button 1 Short Press:** Talk to Gemini (timer continues in background)
  - Returns to timer display after conversation
- **Voice Commands:**
  - "How much time is left?"
  - "Cancel the timer"

### Alarm Behavior
- **Sound:** alarm_sound.pcm loops continuously
- **Visual:** Red flashing LEDs
- **Dismissal:** Press Button 1 to stop alarm and clear timer

### Notes
- Timer runs in background during conversations
- Returns to timer display after 10s conversation window

---

## 🌊 TIDE Mode (Transient)

**Not a selectable mode** - Activated via voice command "what's the tide status"

### Visual
- **Animation:** Water level visualization
- **Rising Tide:** LEDs fill from bottom
- **Falling Tide:** LEDs drain from top
- **High/Low Tide:** Static level with gentle shimmer
- **Color:** Blue (ocean theme)

### Display Duration
- **Automatic:** 15 seconds, then returns to Idle
- **Persistent if queried:** Stays visible during conversation

### Voice Commands
- "What's the tide status?"
- "When is high tide?"
- "Is the tide rising or falling?"

### Data Source
- Live data from StormGlass API
- Updates dynamically based on location

---

## 🌙 MOON PHASE Mode (Transient)

**Not a selectable mode** - Activated via voice command "what's the moon phase"

### Visual
- **Animation:** Moon phase visualization
- **Full Moon:** All LEDs lit (white)
- **New Moon:** All LEDs dark
- **Waxing/Waning:** Partial fill showing current illumination percentage
- **Color:** White/silver (moonlight theme)

### Display Duration
- **Automatic:** 15 seconds, then returns to Idle
- **Persistent if queried:** Stays visible during conversation

### Voice Commands
- "What's the moon phase?"
- "How full is the moon?"
- "What phase is the moon in?"

### Data Calculation
- Astronomical calculation based on current date
- Shows illumination percentage and phase name

---

## 🚨 ALARM Mode (Transient)

**Not a selectable mode** - Triggered when alarm time reached

### Visual
- **Animation:** Fast flashing red
- **Color:** Red (urgent)
- **Pattern:** Alternating on/off at high frequency

### Audio
- **Sound:** alarm_sound.pcm loops continuously
- **Volume:** Full volume

### Controls
- **Button 1 Press:** Dismiss alarm
  - Stops sound
  - Clears alarm
  - Restores previous mode

### Voice Setup
- "Set an alarm for 7:30 AM"
- "Set an alarm for 15 minutes from now"
- "Cancel all alarms"

### Notes
- Alarm sound continues until dismissed
- LEDs flash throughout alarm duration
- Previous mode state preserved and restored

---

## 🎭 MARQUEE Mode (Transient)

**Not a selectable mode** - Plays when entering certain modes

### Visual
- **Animation:** Scrolling text clockwise around full circumference
- **Display:** Uses all 12 LED strips (full 360° shell)
- **Font:** Custom 3×8 chunky glyphs, centered at rows 2-9
- **Spacing:** 4 columns per character (3 for glyph + 1 column spacing)
- **Color:** Mode-specific (Pomodoro=Orange, Meditation=Purple, etc.)
- **Speed:** ~4 columns/second (250ms update interval default)
- **Direction:** Right-to-left scroll (appears to move clockwise)

### Marquee Messages
- "POMODORO" - Orange
- "MEDITATION" - Purple  
- "CLOCK" - White
- "LAMP" - White
- "IDLE MODE" - Blue
- "FOCUS TIME" - Red (Pomodoro transition)
- "SHORT BREAK" - Green (Pomodoro transition)
- "LONG BREAK" - Blue (Pomodoro transition)
- "CYCLE DONE" - Yellow (Pomodoro complete)
- "RAIN" / "OCEAN" / "RAINFOREST" - Green (Ambient modes)

### Behavior
- Scrolls text once
- Automatically switches to target mode when complete
- Cannot be interrupted (except by Button 2 long press emergency exit)

---

## 🆘 Emergency Actions

### Button 2 Long Press (2 seconds)
**Available in:** Ambient, Pomodoro, Meditation modes

**Effect:** Return to IDLE + start Gemini recording
- Stops all audio
- Clears all mode states
- Immediately opens voice recording
- Useful for urgent interruptions

---

## Voice Assistant (Gemini) Functions

### Available Across All Modes (via Button 1 in most modes)

#### Time & Alarms
- "What time is it?"
- "Set an alarm for [time]"
- "Cancel all alarms"
- "List my alarms"

#### Timers
- "Set a timer for [X] minutes"
- "How much time is left?"
- "Cancel the timer"

#### Weather & Nature
- "What's the weather?"
- "What's the tide status?"
- "What's the moon phase?"

#### Focus & Productivity
- "Start a Pomodoro"
- "Start Pomodoro with [custom durations]"
- "How long is left on the Pomodoro?"
- "Which cycle am I in?"
- "Pause/Resume the timer"
- "Stop the Pomodoro"

#### Ambient Sounds
- "Play rain sounds"
- "Play ocean sounds"
- "Play rainforest sounds"
- "Stop ambient sounds"

#### General
- Ask any question
- Get information
- Have conversations

---

## Technical Notes

### Button Debouncing
- **Hardware:** TTP223 touch sensors with built-in debounce
- **Software:** 10ms debounce delay
- **Long Press Detection:** 2000ms threshold

### Audio Specifications
- **Sample Rate:** 24kHz
- **Format:** 16-bit PCM mono
- **I2S Output:** MAX98357A amplifier
- **Volume Control:** Software multiplier (5-100%)

### Voice Activity Detection (VAD)
- **Normal Recording:** 700 amplitude threshold
- **Conversation Window:** 650 amplitude threshold (slightly more sensitive)
- **Processing:** Real-time amplitude analysis on 640-byte chunks

### LED Specifications
- **Matrix:** 12 rows × 21 columns = 252 LEDs
- **Controller:** FastLED library
- **Refresh Rate:** ~33 FPS (expect ~990 updates per 30s)
- **Color Depth:** 24-bit RGB

### Network
- **WiFi:** 2.4GHz, WPA2
- **WebSocket:** Persistent connection to server
- **Health Check:** Every 5 seconds
- **Timeout:** 120 seconds idle

---

## Mode Transition Summary

```
IDLE (Button 2) → AMBIENT VU (Button 2) → NATURE SOUNDS (Button 2) → POMODORO (Button 2) → MEDITATION (Button 2) → CLOCK (Button 2) → LAMP (Button 2) → IDLE

Any Mode + Button 1 Short Press → RECORDING → PROCESSING → AUDIO_REACTIVE → CONVERSATION_WINDOW → Return to Original Mode

Voice "set timer" → TIMER → Return to IDLE after alarm

Voice "tide/moon" → TIDE/MOON → Return to IDLE after 15s

Alarm Time → ALARM → (Button 1 dismiss) → Return to Previous Mode
```

---

## UX Testing Checklist

### IDLE Mode
- [ ] Blue wave animation plays smoothly
- [ ] Button 1 starts recording
- [ ] Button 2 cycles to Ambient

### AMBIENT Mode  
- [ ] Audio plays correctly (rain/ocean/rainforest)
- [ ] VU meter syncs with audio
- [ ] Button 1 opens Gemini (audio continues)
- [ ] Returns to ambient after conversation
- [ ] Button 2 exits to Pomodoro

### POMODORO Mode
- [ ] Auto-starts after marquee with zen bell
- [ ] Countdown displays correctly (red bars decreasing)
- [ ] Button 1 short press opens Gemini (timer continues)
- [ ] Returns to Pomodoro after 10s window
- [ ] Button 1 long press pauses/resumes (no sound currently)
- [ ] Voice query "how long is left" works
- [ ] Voice query "which cycle" works
- [ ] Button 2 exits and clears timer
- [ ] Transitions play zen bell
- [ ] Short breaks display green (count up)
- [ ] Long break displays blue (count up)
- [ ] Cycle complete shows yellow marquee

### MEDITATION Mode
- [ ] Breathing animation cycles correctly (4s phases)
- [ ] Chakra colors display in order
- [ ] Om audio plays at reduced volume (5%)
- [ ] Button 1 short press pauses/resumes breathing
- [ ] Button 1 long press cycles chakras
- [ ] Audio changes with chakra
- [ ] Button 2 exits and restores volume

### CLOCK Mode
- [ ] Time displays correctly
- [ ] Updates every minute
- [ ] Button 1 opens Gemini
- [ ] Returns to clock after conversation
- [ ] Button 2 cycles to Lamp

### LAMP Mode
- [ ] Starts in white
- [ ] Button 1 cycles colors (white→red→green→blue→white)
- [ ] Smooth fade transitions
- [ ] Button 2 exits to Idle

### Voice Commands
- [ ] Timer setting works
- [ ] Alarm setting works
- [ ] Pomodoro status queries accurate
- [ ] Custom Pomodoro durations work
- [ ] Tide status displays correctly
- [ ] Moon phase displays correctly
- [ ] Weather queries work
- [ ] Ambient sound requests work

### Conversation Flow
- [ ] 10s window opens after every response
- [ ] Voice detection triggers new recording
- [ ] Returns to correct mode after window expires
- [ ] Works during Pomodoro (timer continues)
- [ ] Works during Timer (timer continues)

### Audio & Sync
- [ ] VU meter starts ~240ms after speech
- [ ] VU meter fades cleanly at end
- [ ] No lingering LEDs after audio stops
- [ ] Alarm sound loops correctly
- [ ] Alarm dismissal doesn't crash
- [ ] Zen bell plays at Pomodoro transitions

### Edge Cases
- [ ] Processing timeout returns to correct mode
- [ ] WebSocket disconnection recovery
- [ ] False trigger prevention (keyboard noise)
- [ ] Long press vs short press detection reliable
- [ ] Mode exit clears state properly
- [ ] Background timers persist during conversations

---

**Document Version:** 1.0  
**Last Updated:** 2026-01-13  
**Hardware:** ESP32-S3-DevKitC-1  
**Firmware:** JellyBerry v1.0
