# JellyBerry - Conversational AI Device Firmware

**A production-ready Gemini 2.5 Realtime conversational AI device for ESP32-S3 with real-time audio I/O, WebSocket communication, and FastLED visualization.**

## Features

- **Real-Time Conversational AI** - Powered by Google Gemini 2.5 Realtime API
- **Native Audio Streaming** - 16kHz PCM audio via I2S (no Whisper transcription overhead)
- **Dual-Core Processing** - FreeRTOS tasks optimized for ESP32-S3 cores
- **144 WS2812B LED Strip** - Sea-gooseberry visualization with audio-reactive animations
- **Touch-Based Interface** - PAD1 to start recording, PAD2 to stop
- **Low Latency** - 8-10 second end-to-end response time (realistic minimum)
- **Production Ready** - Error handling, WiFi reconnection, graceful degradation

## Hardware Requirements

- **ESP32-S3** microcontroller
- **INMP441** omnidirectional MEMS microphone (I2S)
- **MAX98357A** I2S Class-D amplifier with 8Ω 5W speaker
- **144x WS2812B** addressable RGB LEDs (single data line)
- **2x TTP223** capacitive touch pads
- **5V power supply** (minimum 2A for LEDs + audio)

## GPIO Pin Assignments

```
Microphone (INMP441):
  SCK (Clock)   → GPIO 8
  WS (Word Sel) → GPIO 9
  SD (Data)     → GPIO 10

Speaker (MAX98357A):
  LRC (Sync)    → GPIO 5
  BCLK (Clock)  → GPIO 6
  DIN (Data)    → GPIO 7

LED Strip (WS2812B):
  DATA          → GPIO 1 (via level shifter)

Touch Pads (TTP223):
  PAD1 (Start)  → GPIO 3
  PAD2 (Stop)   → GPIO 4
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

1. **Press PAD1** (GPIO 3) to start recording
2. **Speak your question/command** (hold or release - tap pattern)
3. **Press PAD2** (GPIO 4) to stop recording
4. System processes audio via Gemini 2.5 Realtime
5. **Response plays through speaker** while LEDs animate
6. Green LED pulse indicates AI responding
7. Cyan solid when WebSocket connected
8. Red pulse when recording

## LED Animations

- **BOOT** (Blue pulse) - Initial startup
- **IDLE** (Dim blue breathing) - Ready state
- **RECORDING** (Red pulse) - Recording audio
- **PROCESSING** (Yellow rotate) - Sending to Gemini
- **AUDIO_REACTIVE** (Green pulse) - AI responding
- **CONNECTED** (Cyan solid) - WebSocket active
- **ERROR** (Red flash) - Connection or init failure

## Architecture

**Core 1 (WebSocket & Audio Streaming)**
- WebSocket connection lifecycle
- Audio buffer management
- Bidirectional audio streaming to Gemini

**Core 0 (LEDs & Sensors)**
- FastLED animation updates (20 FPS)
- Touch pad polling
- System health monitoring

## Performance Targets

- **Recording**: 1-10 seconds of PCM audio
- **Upload**: ~1-2 seconds (via WebSocket)
- **Processing**: ~3-5 seconds (Gemini inference)
- **TTS Generation**: ~2-3 seconds (audio synthesis)
- **Playback**: Real-time via I2S speaker driver
- **Total Latency**: 8-10 seconds end-to-end

## Troubleshooting

### LEDs not lighting
- Verify GPIO 1 connected via level shifter (5V→3.3V)
- Check 5V power supply (LEDs draw ~100mA at max brightness)
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

- [ ] Tide level visualization on LED strip
- [ ] Weather color patterns (blue=rain, yellow=sun)
- [ ] Moon phase display
- [ ] Multi-language support
- [ ] Local voice synthesis (TensorFlow Lite)
- [ ] Battery power management
- [ ] PCB design for production

## License

MIT - Feel free to fork, modify, and build!

## Credits

Built as a gift device for a teenage son. Combines conversational AI, real-time audio processing, and visual feedback into a polished, gift-ready experience.

---

**Happy building! 🎉**
