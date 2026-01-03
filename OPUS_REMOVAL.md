# Opus Codec Removal - Raw PCM Streaming

**Date:** November 30, 2025  
**Reason:** Simplify audio pipeline, eliminate codec-related bugs, and improve debuggability

## Problem Statement

The audio streaming pipeline was experiencing:
- Garbled and glitchy audio playback
- Queue bursting (packets arriving in bursts despite server-side pacing)
- Server disconnects with error code 10054
- Added complexity from Opus encoding/decoding on both ends

## Analysis

**Throughput Requirements:**
- Input: 16kHz × 16-bit × mono = 256 kbit/s
- Output: 24kHz × 16-bit × mono = 384 kbit/s
- **ESP32-S3 over LAN WiFi can easily handle these rates**

**Root Cause:**
Opus was unnecessary for performance and likely introducing:
- Sample rate conversion issues
- Framing/decoding errors causing garbling
- Additional CPU overhead
- Complexity masking the real issue (network buffering)

## Solution: Raw PCM Streaming

Removed Opus entirely and stream raw PCM directly between components:

```
ESP32 Mic (16kHz PCM) → Server → Gemini API
ESP32 Speaker (24kHz PCM) ← Server ← Gemini API
```

## Changes Made

### ESP32 Firmware (src/main.cpp)

**Removed:**
- `#include "opus.h"` and extern "C" wrapper
- `OpusEncoder *encoder` and `OpusDecoder *decoder` globals
- Opus encoder/decoder initialization in `setup()`
- `opus_decode()` call in `audioTask()`

**Modified:**
- `audioTask()`: Now processes raw PCM bytes directly
  - Convert bytes to 16-bit samples: `int16_t* pcmSamples = (int16_t*)playbackChunk.data;`
  - Calculate numSamples: `playbackChunk.length / 2`
  - Skip decoding step entirely
- Playback debug log now shows: "Raw PCM: X bytes → Y samples"

### Deno Server (server/main.ts)

**Removed:**
- `import OpusScript from "npm:opusscript@0.1.1"`
- `opusEncoder` field from `ClientConnection` interface
- OpusScript encoder initialization
- Opus encoding loop with 45ms setTimeout delays
- Frame-based encoding logic (FRAME_SIZE, FRAME_BYTES)

**Modified:**
- Audio streaming: Now sends raw PCM from Gemini directly to ESP32
  - Decode base64 to PCM bytes (already 24kHz mono)
  - Split into 1920-byte chunks (960 samples = 40ms)
  - Send chunks via WebSocket: `connection.socket.send(chunk)`
- Removed artificial pacing delays (unnecessary for raw PCM)
- Updated log: "ESP32 Raw PCM: X bytes (Y samples, Zms)"

### Configuration (src/Config.h)

**Removed:**
- `#define OPUS_BITRATE 16000`
- `#define OPUS_COMPLEXITY 5`

**Updated:**
- Added clear documentation:
  ```cpp
  // PCM Streaming Configuration (no codec - raw PCM)
  // Mic: 16-bit PCM @ 16kHz mono → server
  // Speaker: 16-bit PCM @ 24kHz mono ← server
  #define PCM_CHUNK_SIZE 1920  // 960 samples * 2 bytes = 40ms at 24kHz
  #define OPUS_FRAME_SIZE 320  // 320 samples @ 16kHz = 20ms chunks for mic recording
  ```

### Build Configuration (platformio.ini)

**Removed:**
```ini
https://github.com/pschatzmann/arduino-audio-tools.git#v1.0.1
https://github.com/pschatzmann/arduino-libopus.git
```

## Benefits

1. **Simpler Pipeline**: No codec overhead, just raw sample streaming
2. **Better Debuggability**: Issues are now clearly in network layer, not codec
3. **Lower CPU Usage**: No encoding/decoding overhead
4. **Fewer Dependencies**: Removed 2 Arduino libraries, 1 npm package
5. **Clearer Errors**: If audio is garbled, it's a network/buffering issue, not codec

## Testing Plan

### Expected Results

✅ **Audio quality:** Should sound clean and clear (no garbling)  
✅ **Queue behavior:** Should fill gradually, not burst (0→1→2→3... not 0→25→30)  
✅ **Disconnects:** If 10054 still occurs, confirms server-side error handling issue  
✅ **Latency:** May be slightly higher (no compression) but still acceptable on LAN  

### Diagnostic Commands

**ESP32 Serial Monitor:**
```
[PLAYBACK] Raw PCM: 1920 bytes → 960 samples, level=XXX, queue=Y
📊 [STREAM] X packets, Y.Zms avg interval, N fast (<20ms), M KB/s, queue=Q
```

**Server Console:**
```
📤 ESP32 Raw PCM: 1920 bytes (960 samples, 40ms)
```

### What to Monitor

- Queue depth during playback (should stay under 10 packets)
- Packet interval (should be ~20-30ms, matching Gemini's delivery rate)
- Audio clarity (no stuttering, no robotic artifacts)
- Server errors (check for unhandled exceptions causing 10054)

## Rollback Plan

If raw PCM causes issues (unlikely):

1. Revert commits to restore Opus
2. Investigate alternative Opus libraries (e.g., esp-adf native Opus)
3. Consider pull-based flow control (ESP32 requests chunks when ready)

## Next Steps

1. ✅ Remove Opus from both ESP32 and server
2. ✅ Update configuration and documentation
3. ⏳ **Test the pipeline** - verify audio quality and queue behavior
4. 🔍 If disconnects persist, focus on server error handling (not codec)
5. 🔍 If queue still bursts, investigate WebSocket library buffering

## Conclusion

Opus was solving a problem we didn't have (bandwidth) while introducing complexity. Raw PCM is the right choice for LAN deployment and will make the real issues (network buffering, server errors) much easier to diagnose and fix.

---

**Reference:** User insight from conversation - "The ESP32 can comfortably handle a mono PCM stream at the bitrates you need for Gemini."
