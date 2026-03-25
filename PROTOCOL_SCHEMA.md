# Jellyberry WebSocket Protocol Schema

This document defines the JSON message schemas exchanged between the ESP32-S3 firmware and the Deno edge server.

## Overview

- **Transport**: WebSocket (WSS for production, WS for local dev)
- **Audio Format**: Raw PCM, 16-bit signed, mono 16kHz (mic) / stereo 24kHz (speaker)
- **Message Format**: JSON text (UTF-8) for control messages, binary for audio
- **Device ID**: Sent as query parameter: `wss://server.example.com/api/gemini?device_id=JELLY001`

---

## Firmware → Server Messages

### 1. `setup`
**Purpose**: Sent once on WebSocket connection to identify device and request initial state.  
**Direction**: Firmware → Server  
**Timing**: Immediately after `WStype_CONNECTED` event

```json
{
  "type": "setup",
  "deviceId": "JELLY001"
}
```

**Fields**:
- `deviceId` (string): Unique device identifier (defined in Config.h)

**Expected Response**: Server sends `setupComplete` with volume and connection status (see below).

---

### 2. `recordingStart`
**Purpose**: Sent on the first audio chunk of each recording to provide device state context to Gemini.  
**Direction**: Firmware → Server → Gemini (reformatted as SYSTEM message)  
**Timing**: First mic chunk after user starts recording (button press or conversation window VAD)

```json
{
  "type": "recordingStart",
  "pomodoro": {
    "active": true,
    "session": "Focus" | "Short Break" | "Long Break",
    "paused": false,
    "secondsRemaining": 1200
  },
  "meditation": {
    "active": false,
    "chakra": "Root" | "Sacral" | "Solar Plexus" | "Heart" | "Throat" | "Third Eye" | "Crown"
  },
  "ambient": {
    "active": false,
    "sound": "rain" | "ocean" | "rainforest" | "fire"
  },
  "timer": {
    "active": false,
    "secondsRemaining": 300
  },
  "lamp": {
    "active": false,
    "color": "white" | "red" | "green" | "blue"
  },
  "radio": {
    "active": false,
    "streaming": false,
    "station": "BBC Radio 1"
  },
  "alarmCount": 2
}
```

**Fields**:
- `pomodoro.active` (boolean): Whether Pomodoro timer is running
- `pomodoro.session` (string): Current session type (only present if active)
- `pomodoro.paused` (boolean): Whether timer is paused (only present if active)
- `pomodoro.secondsRemaining` (number): Time left in current session (only present if active)
- `meditation.active` (boolean): Whether chakra breathing meditation is active
- `meditation.chakra` (string): Current chakra name (only present if active)
- `ambient.active` (boolean): Whether ambient sound (rain/ocean/etc.) is playing
- `ambient.sound` (string): Sound type (only present if active)
- `timer.active` (boolean): Whether countdown timer is running
- `timer.secondsRemaining` (number): Time left on timer (only present if active)
- `lamp.active` (boolean): Whether lamp mode is active
- `lamp.color` (string): Lamp color (only present if active)
- `radio.active` (boolean): Whether radio mode is active (includes discovery mode)
- `radio.streaming` (boolean): Whether station stream is currently playing
- `radio.station` (string): Station name (only present if active and station selected)
- `alarmCount` (number): Count of enabled alarms (0-3)

**Server Behavior**:
- Server stores in `conn.deviceState` for future reference
- Formats as human-readable text and sends to Gemini as SYSTEM message
- Example: `"Pomodoro active Focus session, 20m 0s remaining (running). Meditation: inactive. Ambient sound: inactive. Timer: inactive. 2 alarms set."`

---

### 3. `recordingStop`
**Purpose**: Signals end of user speech (VAD silence detected or max duration reached).  
**Direction**: Firmware → Server → Gemini (`activityEnd`)  
**Timing**: 3 seconds after last voice detected, or 30 seconds max duration

```json
{
  "type": "recordingStop"
}
```

**Server Behavior**:
- Forwards `{ realtimeInput: { activityEnd: {} } }` to Gemini
- Sets `conn.userSpokeThisTurn = true` and `conn.geminiTurnActive = true`
- Resets `conn.turnCompleteFired = false` to prepare for next turn

---

### 4. Audio Chunks (Binary)
**Purpose**: Raw microphone PCM data during recording.  
**Direction**: Firmware → Server → Gemini (Base64-encoded)  
**Timing**: Continuous stream during `recordingActive == true`

**Format**: Raw binary, 320 bytes per chunk (20ms at 16kHz mono, 16-bit signed PCM)

**Server Behavior**:
- Base64-encodes the raw bytes
- Wraps in `{ realtimeInput: { audio: { data: "<base64>", encoding: "pcm_s16le", sampleRate: 16000 } } }`
- Forwards to Gemini Live API

---

### 5. `deviceStateRequest`
**Purpose**: Requests full device state from firmware (triggered by Gemini tool call).  
**Direction**: Server → Firmware (but initiated by Gemini, so documented here for completeness)

```json
{
  "type": "deviceStateRequest"
}
```

**Expected Response**: Firmware sends `deviceStateResponse` (see below).

---

### 6. `deviceStateResponse`
**Purpose**: Full device state snapshot (alarms, volume, connected status).  
**Direction**: Firmware → Server → Gemini (returned as tool response)  
**Timing**: In response to `deviceStateRequest`

```json
{
  "type": "deviceStateResponse",
  "volumeLevel": 5,
  "connected": true,
  "alarms": [
    {
      "id": 1,
      "time": "07:30",
      "enabled": true,
      "daysOfWeek": "Mon,Tue,Wed,Thu,Fri"
    }
  ]
}
```

**Fields**:
- `volumeLevel` (number, 1-10): Current volume (mapped from `volumeMultiplier * 10`)
- `connected` (boolean): WebSocket connection status
- `alarms` (array): List of configured alarms
  - `id` (number, 1-15): Alarm ID
  - `time` (string, "HH:MM"): Trigger time in 24-hour format
  - `enabled` (boolean): Whether alarm is active
  - `daysOfWeek` (string): Comma-separated day abbreviations ("Mon,Tue,Wed,Thu,Fri,Sat,Sun")

---

### 7. `radioModeActivated`
**Purpose**: Notifies server that radio discovery mode was entered (button 1 in AMBIENT mode).  
**Direction**: Firmware → Server  
**Timing**: When user cycles from LED_AMBIENT to LED_RADIO

```json
{
  "type": "radioModeActivated"
}
```

**Server Behavior**:
- Logs event
- No immediate action (waits for Gemini's `get_radio_stations` tool call)

---

## Server → Firmware Messages

### 1. `setupComplete`
**Purpose**: Initial connection handshake response.  
**Direction**: Server → Firmware  
**Timing**: Immediately after receiving `setup` from firmware

```json
{
  "type": "setupComplete",
  "volumeLevel": 5,
  "connected": true
}
```

**Fields**:
- `volumeLevel` (number, 1-10): Restored volume from previous session
- `connected` (boolean): Always `true` (indicates WebSocket active)

**Firmware Behavior**:
- Sets `volumeMultiplier = volumeLevel / 10.0`
- Transitions `currentLEDMode` from `LED_BOOT` to `LED_IDLE`

---

### 2. `turnComplete`
**Purpose**: Signals Gemini has finished responding.  
**Direction**: Server → Firmware  
**Timing**: When Gemini sends `generationComplete` or `turnComplete: true`

```json
{
  "type": "turnComplete"
}
```

**Firmware Behavior**:
- Sets `turnComplete = true`
- If not in persistent mode (Meditation, Radio, Lamp, Pomodoro), opens conversation window after 10s tide/moon display
- If in persistent mode, stays in that mode

---

### 3. `reconnecting`
**Purpose**: Notifies firmware that server is reconnecting to Gemini (lazy reconnect).  
**Direction**: Server → Firmware  
**Timing**: When `recordingStart` arrives but Gemini socket is not connected

```json
{
  "type": "reconnecting"
}
```

**Firmware Behavior**:
- Sets `currentLEDMode = LED_RECONNECTING` (magenta pulse animation)
- Logs event

---

### 4. Audio Packets (Binary)
**Purpose**: PCM audio from Gemini's voice response or ambient/radio streams.  
**Direction**: Server → Firmware  
**Timing**: Streamed during Gemini response or ambient playback

**Format**: 
- **Gemini audio**: Raw PCM, 512-1024 bytes per packet (24kHz stereo, 16-bit signed)
- **Ambient/Radio audio**: Header `0xA5 0x5A <seq_low> <seq_high>` + PCM payload (1024 bytes)

**Firmware Behavior**:
- Enqueues to `audioOutputQueue` (FreeRTOS queue)
- `audioTask` dequeues and writes to `I2S_NUM_1` (speaker)
- Updates `lastAudioChunkTime` to prevent drain timeout
- Prebuffer check: sets `isPlayingResponse = true` after 3 packets queued

---

### 5. `ambientComplete`
**Purpose**: Signals end of ambient sound stream.  
**Direction**: Server → Firmware  
**Timing**: When ffmpeg finishes encoding ambient audio file (end-of-file)

```json
{
  "type": "ambientComplete",
  "name": "rain",
  "sequence": 42
}
```

**Fields**:
- `name` (string): Sound name ("rain", "ocean", "rainforest", "fire", or chakra bell names)
- `sequence` (number): Sequence ID (matches `ambientSound.sequence` to discard stale streams)

**Firmware Behavior**:
- If `sequence` matches current `ambientSound.sequence`, clears `isPlayingAmbient = false`
- Logs completion

---

### 6. `radioEnded`
**Purpose**: Signals end of radio stream (natural EOF or cancellation).  
**Direction**: Server → Firmware  
**Timing**: When ffmpeg process exits or stream is cancelled

```json
{
  "type": "radioEnded",
  "stationName": "BBC Radio 1",
  "error": false
}
```

**Fields**:
- `stationName` (string): Station name
- `error` (boolean, optional): `true` if stream failed (0 chunks received)

**Firmware Behavior**:
- Clears `radioState.active = false`, `radioState.streaming = false`
- If `error == true`, shows LED_ERROR briefly then returns to idle
- Logs event

---

### 7. `set_volume_level`
**Purpose**: Tool call response — updates device volume.  
**Direction**: Server → Firmware  
**Timing**: After Gemini calls `set_volume_level` tool

```json
{
  "type": "set_volume_level",
  "volumeLevel": 7
}
```

**Fields**:
- `volumeLevel` (number, 1-10): New volume level

**Firmware Behavior**:
- Sets `volumeMultiplier = volumeLevel / 10.0`
- Plays volume chime (brief 1.2kHz tone)
- Logs change

---

### 8. `setAlarm`
**Purpose**: Tool call response — schedules a new alarm.  
**Direction**: Server → Firmware  
**Timing**: After Gemini calls `set_alarm` tool

```json
{
  "type": "setAlarm",
  "alarmID": 1,
  "triggerTime": "07:30",
  "daysOfWeek": "Mon,Tue,Wed,Thu,Fri"
}
```

**Fields**:
- `alarmID` (number, 1-15): Alarm slot ID
- `triggerTime` (string, "HH:MM"): Time in 24-hour format
- `daysOfWeek` (string): Comma-separated day abbreviations (or empty string for one-time alarm)

**Firmware Behavior**:
- Parses time and stores in `alarms[alarmID - 1]`
- Sets `alarms[alarmID - 1].enabled = true`
- Logs confirmation

---

### 9. `cancelAlarm`
**Purpose**: Tool call response — disables an alarm.  
**Direction**: Server → Firmware  
**Timing**: After Gemini calls `cancel_alarm` tool

```json
{
  "type": "cancelAlarm",
  "alarmID": 1
}
```

**Fields**:
- `alarmID` (number, 1-15): Alarm slot ID to disable

**Firmware Behavior**:
- Sets `alarms[alarmID - 1].enabled = false`
- Logs confirmation

---

### 10. `startTimer`
**Purpose**: Tool call response — starts countdown timer.  
**Direction**: Server → Firmware  
**Timing**: After Gemini calls `start_timer` tool

```json
{
  "type": "startTimer",
  "seconds": 300
}
```

**Fields**:
- `seconds` (number): Timer duration in seconds

**Firmware Behavior**:
- Sets `timerState.active = true`, `timerState.totalSeconds = seconds`, `timerState.startTime = millis()`
- Transitions to `LED_TIMER` mode (timer visualization)
- Logs start

---

### 11. `cancelTimer`
**Purpose**: Tool call response — stops countdown timer.  
**Direction**: Server → Firmware  
**Timing**: After Gemini calls `cancel_timer` tool

```json
{
  "type": "cancelTimer"
}
```

**Firmware Behavior**:
- Sets `timerState.active = false`
- Returns to `LED_IDLE` if no other persistent mode
- Logs cancellation

---

### 12. `startPomodoro`
**Purpose**: Tool call response — starts Pomodoro timer.  
**Direction**: Server → Firmware  
**Timing**: After Gemini calls `start_pomodoro` tool

```json
{
  "type": "startPomodoro",
  "focusMinutes": 25,
  "shortBreakMinutes": 5,
  "longBreakMinutes": 15
}
```

**Fields**:
- `focusMinutes` (number): Focus session duration
- `shortBreakMinutes` (number): Short break duration
- `longBreakMinutes` (number): Long break duration

**Firmware Behavior**:
- Initializes `pomodoroState` with custom durations
- Starts first Focus session
- Transitions to `LED_POMODORO` mode
- Plays zen bell to mark start

---

### 13. `pausePomodoro` / `resumePomodoro` / `cancelPomodoro`
**Purpose**: Tool call responses for Pomodoro control.  
**Direction**: Server → Firmware

```json
{ "type": "pausePomodoro" }
{ "type": "resumePomodoro" }
{ "type": "cancelPomodoro" }
```

**Firmware Behavior**: Documented in types.h `PomodoroState` struct.

---

### 14. Tool Call Responses (Tide, Moon, Lamp, Meditation)
**Purpose**: Trigger LED visualizations or feature modes.  
**Direction**: Server → Firmware

```json
{ "type": "showTide", "location": "Brighton, UK", "tideType": "high" | "low", "minutes": 120 }
{ "type": "showMoon", "location": "Brighton, UK", "phase": "Waxing Gibbous", "illumination": 0.87 }
{ "type": "lampOn", "color": "white" | "red" | "green" | "blue" }
{ "type": "lampOff" }
{ "type": "startMeditation" }
{ "type": "stopMeditation" }
```

**Schema**: Documented inline in server/main.ts tool handlers (lines 1200-1470).

---

## Firmware → Server Actions (Non-Type Messages)

### `action: "requestAmbient"`
**Purpose**: Request ambient sound stream from server.  
**Direction**: Firmware → Server  
**Timing**: Button 1 in AMBIENT mode, or Gemini tool call response

```json
{
  "action": "requestAmbient",
  "sound": "rain",
  "loops": 1,
  "sequence": 42
}
```

**Fields**:
- `sound` (string): Sound name ("rain", "ocean", "rainforest", "fire", or "bell001"-"bell007")
- `loops` (number, optional): Number of times to loop (-1 for infinite, default 1)
- `sequence` (number): Unique sequence ID to discard stale streams

**Server Behavior**:
- Spawns ffmpeg to encode audio file at 24kHz mono PCM
- Streams chunks with `0xA5 0x5A <seq>` header
- Sends `ambientComplete` when done

---

### `action: "stopAmbient"`
**Purpose**: Cancel current ambient/radio stream.  
**Direction**: Firmware → Server  
**Timing**: User voice command to stop, or mode transition

```json
{
  "action": "stopAmbient"
}
```

**Server Behavior**:
- Kills active ffmpeg process
- Cancels stream via `conn.ambientStreamCancel()` or `conn.radioStreamCancel()`
- Sends `radioEnded` if radio was playing (as of protocol fix)

---

### `action: "requestRadio"`
**Purpose**: Request radio station stream.  
**Direction**: Firmware → Server  
**Timing**: After Gemini tool call selects station

```json
{
  "action": "requestRadio",
  "streamUrl": "https://stream.example.com/radio.m3u8",
  "stationName": "BBC Radio 1",
  "isHLS": true,
  "sequence": 42
}
```

**Fields**:
- `streamUrl` (string): Stream URL (HTTP, HTTPS, or HLS manifest)
- `stationName` (string): Human-readable station name
- `isHLS` (boolean): Whether stream is HLS (triggers buffering animation)
- `sequence` (number): Unique sequence ID

**Server Behavior**:
- Spawns ffmpeg with reconnection flags for reliability
- Streams 1024-byte PCM chunks with `0xA5 0x5A <seq>` header
- Sends `radioEnded` when stream ends or is cancelled

---

### `action: "requestAlarm"`
**Purpose**: Request alarm sound playback.  
**Direction**: Firmware → Server  
**Timing**: When alarm triggers (time matches current time)

```json
{
  "action": "requestAlarm",
  "alarmID": 1,
  "sequence": 42
}
```

**Fields**:
- `alarmID` (number, 1-15): Alarm that triggered
- `sequence` (number): Audio sequence ID

**Server Behavior**:
- Plays alarm sound (bell or chime depending on config)
- Streams PCM with ambient header
- Sends `ambientComplete` when done

---

### `action: "requestZenBell"`
**Purpose**: Request zen bell sound (used by Pomodoro session transitions).  
**Direction**: Firmware → Server  
**Timing**: Pomodoro session completion

```json
{
  "action": "requestZenBell",
  "sequence": 42
}
```

**Server Behavior**: Same as `requestAlarm`

---

### `action: "stopAlarm"`
**Purpose**: Stop ringing alarm.  
**Direction**: Firmware → Server  
**Timing**: User dismisses alarm (button press)

```json
{
  "action": "stopAlarm",
  "alarmID": 1
}
```

**Server Behavior**: Cancels active alarm stream

---

## Protocol Invariants

1. **Audio Format**:
   - Microphone: 16kHz mono, 16-bit signed PCM, 320 bytes/chunk (20ms)
   - Speaker: 24kHz stereo, 16-bit signed PCM, variable chunk size
   - Firmware resamples Gemini audio from mono → stereo and applies volume

2. **Sequence Numbers**:
   - Ambient/Radio streams tagged with sequence ID in 4-byte header
   - Firmware discards packets with stale sequence ID (prevents audio bleed during mode transitions)
   - Incremented on every new stream request

3. **Turn State Machine**:
   - Firmware: `IDLE → RECORDING → WAITING → PLAYING → (WINDOW or IDLE)`
   - Server: Tracks `conn.geminiTurnActive`, `conn.userSpokeThisTurn`, `conn.turnCompleteFired`
   - Must stay synchronized (server resets flags on `recordingStop`, firmware resets on `recordingStart`)

4. **Reconnection**:
   - Firmware: Lazy reconnect (waits until user speaks)
   - Server: Clears Gemini session, re-initializes on next `recordingStart`
   - Device state (`recordingStart` payload) sent on first chunk after reconnect

---

## Known Issues & Deviations

- ⚠️ **Alarm persistence**: Alarms not persisted to NVS, lost on reboot (planned fix)
- ⚠️ **Voice-triggered lamp color**: Gemini can set color via tool, but firmware doesn't expose current color in `deviceStateResponse` (minor)
- ⚠️ **Radio station favorites**: No bookmark/recall mechanism (feature request)
- ⚠️ **Conversation window during radio**: VAD guard extended to 3s when radio active (protocol fix applied)

---

## Version History

- **v1.0** (March 2026): Initial schema documentation post-architecture audit
