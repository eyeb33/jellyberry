# JellyBerry - Conversational AI Device Firmware

**A production-ready Gemini 2.5 Flash conversational AI device for ESP32-S3 with real-time audio I/O, WebSocket communication, and FastLED visualization.**

*Powered by `gemini-2.5-flash-native-audio-dialog` for fast, low-latency audio generation.*

## Features

- **Real-Time Conversational AI** - Powered by Google Gemini 2.5 Realtime API
- **Native Audio Streaming** - 16kHz PCM audio via I2S (no Whisper transcription overhead)
- **Dual-Core Processing** - FreeRTOS tasks optimized for ESP32-S3 cores
- **144 WS2812B LED Strip** - 20+ LED animation modes including audio-reactive VU, ambient, tide, moon, pomodoro, meditation, and more
- **Touch-Based Interface** - PAD1 starts recording (silence auto-stops via VAD), PAD2 cycles modes; long-press either pad to escape any mode
- **Low Latency** - 8-10 second end-to-end response time (realistic minimum)
- **Production Ready** - Error handling, WiFi reconnection, graceful degradation

## Hardware Requirements

- **ESP32-S3** microcontroller
- **INMP441** omnidirectional MEMS microphone (I2S)
- **MAX98357A** I2S Class-D amplifier with 8Ω 5W speaker
- **144x WS2812B** addressable RGB LEDs (single data line)
- **2x TTP223** capacitive touch pads
- **5V power supply** (3A minimum; LEDs at operating brightness draw ~2.5A)

## Bill of Materials

| Component | Part | Qty | Notes |
|---|---|---|---|
| Microcontroller | ESP32-S3-DevKitC-1 | 1 | PSRAM variant recommended |
| Microphone | INMP441 MEMS I2S | 1 | 3.3V, omnidirectional |
| Amplifier | MAX98357A I2S Class-D | 1 | 3W/4Ω or 1.4W/8Ω output |
| Speaker | 8Ω 5W full-range | 1 | |
| LED Strip | WS2812B 144-LED, 1m | 1 | 5V, single data line |
| Touch Pads | TTP223 capacitive module | 2 | |
| Power Supply | 5V 3A+ regulated | 1 | USB-C or barrel jack |

## GPIO Pin Assignments

```
Microphone (INMP441):
  SCK (Clock)   → GPIO 8
  WS (Word Sel) → GPIO 9
  SD (Data)     → GPIO 10
  GND           → ESP32 GND
  VCC           → 3.3V

Speaker (MAX98357A):
  LRC (Sync)    → GPIO 4
  BCLK (Clock)  → GPIO 5
  DIN (Data)    → GPIO 6
  GND           → ESP32 GND
  VIN           → 5V

LED Strip (WS2812B):
  DATA          → GPIO 1
  GND           → ESP32 GND (CRITICAL: must share ground with ESP32)
  VCC/5V        → 5V power supply

Touch Pads (TTP223):
  PAD1 (Start)  → GPIO 2
  PAD2 (Stop)   → GPIO 3
  GND           → ESP32 GND
  VCC           → 3.3V

⚠️  IMPORTANT: All devices must share a common ground (GND) with the ESP32.
    Without shared ground, LED data signals will be corrupted.
```

## Setup Instructions

### 1. Install PlatformIO
```bash
pip install platformio
```

### 2. Clone Repository
```bash
git clone https://github.com/eyeb33/jellyberry.git
cd jellyberry
```

### 3. Configure WiFi & API Keys

Edit `src/Config.h` and update:

```cpp
#define WIFI_SSID "YOUR_WIFI_SSID"
#define WIFI_PASSWORD "YOUR_WIFI_PASSWORD"
#define GEMINI_API_KEY "YOUR_GEMINI_API_KEY_HERE"
```

### 4. Install Dependencies

PlatformIO will auto-install required libraries:
- **WebSocketsClient** (Markus Sattler v2.16+)
- **ArduinoJson** (v7.0+)
- **FastLED** (v3.6+)

### 5. Compile & Upload

```bash
# Connect ESP32-S3 via USB
platformio run --target upload --environment esp32-s3-devkitc-1
```

### 6. Monitor Serial Output

```bash
platformio device monitor
```

Expected output:
```
=== JellyBerry Conversational AI Device ===
Initializing firmware...
✓ LEDs initialized
✓ Microphone initialized
✓ Speaker initialized
✓ Touch pads initialized
✓ WiFi connected
IP: 192.168.1.XXX
✓ WebSocket initialized
✓ Tasks created on dual cores
=== Initialization Complete ===
```

## Operation

1. **Tap PAD1** (GPIO 2) — LEDs turn red, recording starts
2. **Speak** — silence is detected automatically after ~1.5s (VAD); no button press needed to stop
3. **LEDs turn purple** — audio is processing
4. **Response plays** through speaker with blue-green VU animation
5. **Cyan LEDs pulse** — 10-second follow-up window opens; speak again without pressing anything, or wait for it to close
6. **PAD1 during playback** — interrupts response and starts a new recording immediately
7. **PAD2 short press** — cycles display modes: Idle → Ambient VU → Sea Gooseberry → Ambient Sounds → Radio → Pomodoro → Meditation → Lamp → Eyes → Idle
8. **Long-press either pad (2s)** — exits any active mode and returns to Idle

## LED States

| State | Colour / Pattern | Description |
|---|---|---|
| BOOT | Orange pulse | Startup sequence |
| IDLE | Slow blue wave | Ready — waiting for input |
| RECORDING | Green→yellow→red VU | Capturing voice input |
| PROCESSING | All off (dark) | Audio sent, awaiting Gemini |
| AUDIO_REACTIVE | Blue→magenta VU bars | Gemini response playing |
| CONVERSATION_WINDOW | Blue countdown bar | Follow-up window open (10s) |
| CONNECTED | Solid green | WebSocket established |
| RECONNECTING | Magenta pulse | Reconnecting to server |
| ERROR | Red flash | Init or connection failure |
| AMBIENT / AMBIENT_VU | Green→yellow→red VU | Ambient sound playing |
| SEA_GOOSEBERRY | Rainbow downward waves | Bio-accurate comb jelly animation |
| RADIO | Teal VU bars | Internet radio streaming |
| POMODORO | Countdown bar (red/green/blue) | Focus or break timer |
| MEDITATION | Chakra colour, breathing | Guided breathing session |
| LAMP | White/Red/Green/Blue solid | Ambient lighting |
| EYES | Animated eye expressions | Eye visualiser |
| TIDE | Blue level fill | Tide status display |
| MOON | White level fill | Moon phase display |
| TIMER | Orange countdown bar | Countdown timer |
| ALARM | Yellow outward pulse | Alarm triggered |

## Architecture

```
ESP32 Firmware  →  WebSocket (/ws)  →  Deno Edge Server  →  Gemini Live API
```

**Firmware — Core 1 (WebSocket & Audio)**
- WebSocket connection lifecycle and reconnection
- Bidirectional audio streaming (mic PCM → Gemini, TTS PCM → speaker)
- Tool call dispatch (timers, alarms, ambient, radio, Pomodoro, tide, moon, weather)

**Firmware — Core 0 (LEDs & Input)**
- FastLED animation updates (20 FPS)
- Touch pad polling and button state machine
- Ambient sound and radio audio playback

**Edge Server (`server/main.ts`)**
- Accepts ESP32 WebSocket connections, proxies to Gemini Live API
- Manages session renewal (9-min proactive, 7-min soft) and ghost turn recovery
- Handles 15+ tool calls server-side (tide, moon, weather, radio, alarms, timers, volume, memory)
- Deno KV memory persistence: hot-tier facts injected into every session, cold-tier session summaries, raw transcript carry-forward

## Performance Targets

- **Recording**: 1-10 seconds of PCM audio
- **Upload**: ~1-2 seconds (via WebSocket)
- **Processing**: ~3-5 seconds (Gemini inference)
- **TTS Generation**: ~2-3 seconds (audio synthesis)
- **Playback**: Real-time via I2S speaker driver
- **Total Latency**: 8-10 seconds end-to-end

## Troubleshooting

### LEDs not lighting or showing wrong colors
- **CRITICAL**: Verify GND connection between ESP32 and LED power supply
- Check GPIO 1 data line connection to LED strip DIN
- Confirm 5V power supply meets 3A minimum (firmware limits brightness but peak draw can reach 2.5A+)
- Optional: Use 3.3V→5V level shifter on data line for better signal integrity
- Test with FastLED example first

### Microphone not recording
- Verify I2S pins (8, 9, 10) not in conflict
- Check INMP441 power (3.3V with decoupling cap)
- Monitor I2S DMA buffer fills on serial

### WebSocket connection fails
- Verify WiFi credentials in Config.h
- Check API key is valid and unrestricted
- Confirm Generative Language API enabled in Google Cloud

### Speaker distorted
- Reduce MAX98357A input volume
- Check for 3.3V I2S signal quality
- Verify speaker impedance (recommend 8Ω)

## Future Enhancements

- [ ] Wake word detection (always-on trigger without button press)
- [ ] Battery power management and sleep modes
- [ ] PCB design for production enclosure
- [ ] Multi-language support

## License

MIT - Feel free to fork, modify, and build!

---

**Happy building! 🎉**
