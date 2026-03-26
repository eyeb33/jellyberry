# Jellyberry Edge Server

WebSocket proxy server for Jellyberry device to communicate with Gemini Live API.

**Model:** `gemini-2.5-flash-native-audio-preview-12-2025` (fast, low-latency audio generation)

## Local Development

1. Install Deno: https://deno.land/

2. Set environment variable:
```bash
export GEMINI_API_KEY="your-api-key-here"
```

3. Run locally:
```bash
cd server
deno task dev
```

Server will run on http://localhost:8000

## Deploy to Deno Deploy (Free)

1. Install Deno Deploy CLI:
```bash
deno install -Arf jsr:@deno/deployctl
```

2. Create project on Deno Deploy:
- Go to https://dash.deno.com/
- Create new project (e.g., "jellyberry-server")

3. Set environment variable in Deno Deploy dashboard:
- Go to your project settings
- Add `GEMINI_API_KEY` environment variable

4. Deploy:
```bash
deployctl deploy --project=jellyberry-server main.ts
```

Your server will be available at: `https://jellyberry-server.deno.dev`

## Update ESP32 Config

In your ESP32 `Config.h`, update:
```cpp
#define WS_SERVER_HOST "jellyberry-server.deno.dev"  // Your Deno Deploy URL
#define WS_SERVER_PORT 443
#define WS_SERVER_PATH "/ws"
```

## Testing

Test the server health:
```bash
curl http://localhost:8000/health
```

## Architecture

```
ESP32 Device  →  WebSocket (/ws)  →  Deno Edge Server  →  Gemini Live API
```

The server:
1. Accepts WebSocket connections from multiple ESP32 devices simultaneously (identified by `deviceId`)
2. Establishes and manages a Gemini Live API session per device (9-min proactive renewal, 7-min soft renewal between turns)
3. Forwards audio bidirectionally — ESP32 mic PCM to Gemini, Gemini TTS PCM back to ESP32
4. Handles ghost turn recovery (0-audio responses that would otherwise loop)
5. Dispatches 15+ tool calls on behalf of Gemini:

| Tool | Description |
|---|---|
| `get_tide_status` | Brighton tide state, level, and next change time |
| `get_moon_phase` | Current lunar phase and illumination |
| `get_weather_forecast` | Current conditions and multi-day forecast |
| `get_current_time` | Current time in Europe/London |
| `get_device_state` | Full firmware state snapshot |
| `set_volume_level` | Hardware volume (1–10 scale) |
| `start_ambient_sound` | Play rain/ocean/rainforest/fire loops |
| `stop_ambient_sound` | Stop ambient playback |
| `search_radio_stations` | Find internet radio stations by query |
| `play_radio_station` | Stream a station to the device |
| `stop_radio` | Stop radio stream |
| `start_pomodoro` | Start a Pomodoro session with custom durations |
| `stop_pomodoro` | End the current Pomodoro |
| `set_alarm` | Schedule a persistent alarm (survives reboot) |
| `start_meditation` | Begin guided chakra meditation |
| `store_memory` | Persist a user fact/preference to long-term memory |
| `recall_sessions` | Retrieve past session summaries (up to 7 days) |

6. Deno KV memory persistence:
   - **Hot tier**: curated facts injected into every new session's system prompt
   - **Cold tier**: 7-day rolling session summaries, retrieved on demand via `recall_sessions`
   - **Session carry-forward**: raw transcript from the last turn, always included in the next session
