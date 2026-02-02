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
ESP32 Device
    ↓ WebSocket (/ws endpoint)
Deno Edge Server (this server)
    ↓ WebSocket (Gemini Live API)
Google Gemini API
```

The server:
1. Accepts WebSocket connections from ESP32 devices
2. Establishes connection to Gemini Live API
3. Forwards audio/messages bidirectionally
4. Handles setup, errors, and reconnection
