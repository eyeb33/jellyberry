# Jellyberry Audio Streaming Diagnostics

## Current Issue Summary
- **Audio bunching**: Packets may still be arriving in bursts despite server 30ms pacing
- **Conversation mode timing**: Mode transitions not happening when expected

## Diagnostic Logging Added

### 1. Packet Timing & Burst Detection
**Location**: `WStype_BIN` handler in `onWebSocketEvent()`

**What it shows**:
```
📊 [STREAM] 150 packets, 33.3ms avg interval, 15 fast (<20ms), 7 KB/s, queue=12
```

**Interpretation**:
- **packets**: Count received in last 5 seconds
- **avg interval**: Average time between packets (should be ~30-35ms with server pacing)
- **fast (<20ms)**: Packets arriving faster than expected (indicates bursting)
- **KB/s**: Bandwidth utilization
- **queue**: Current queue depth (healthy: 5-20, problem: >25 or dropping to 0)

**What to look for**:
- ✅ **Good**: avg interval ~30-35ms, fast packets <10%, queue stable 8-15
- ⚠️ **Bursting**: avg interval <25ms, fast packets >30%, queue bouncing 0→30
- ❌ **Severe burst**: avg interval <15ms, fast packets >50%, packet drops

### 2. Queue State Tracking
**Location**: Same section, after `xQueueSend()`

**What it shows**:
```
📈 Queue started filling: 0 → 1
⚠️  Queue near full: 28/30
⚠️  Dropped 47 packets (queue full at 30, total=123)
```

**Interpretation**:
- **Queue 0→1**: Fresh audio starting (normal at beginning of response)
- **Near full**: Queue filling faster than draining (bursting problem)
- **Dropped packets**: Lost audio data (will cause speed-up artifacts)

**What to look for**:
- ✅ **Good**: Queue grows slowly 0→5→10, stays 8-15, no drops
- ⚠️ **Mild issue**: Occasional near-full warnings, <10 drops per response
- ❌ **Major issue**: Rapid 0→28, frequent drops >50

### 3. Conversation Mode Lifecycle
**Location**: Main loop, conversation window monitoring section

**What it shows**:
```
💬 [CONV] active, window=3500ms/10000, LED=12, turnComplete=1
```

**Interpretation**:
- **active**: Conversation mode is ON
- **window**: Time elapsed / total window (10s)
- **LED**: Current LED mode (should be LED_CONVERSATION_WINDOW=12)
- **turnComplete**: Whether Gemini finished responding (0=still talking, 1=done)

**What to look for**:
- ✅ **Good**: After audio ends, CONV mode starts, window counts up, LED=12
- ⚠️ **Timing issue**: Audio ends but CONV mode doesn't start for several seconds
- ❌ **Never starts**: Audio ends, turnComplete=1, but conversationMode=0

### 4. Audio Playback Lifecycle
**Location**: Main loop, audio completion detection

**What it shows**:
```
⏹️  Audio playback complete (2s timeout, queue=0, turn=1, interrupted=0)
```

**Interpretation**:
- **2s timeout**: No new audio for 2 seconds (expected at end)
- **queue=0**: Queue empty (good - all audio played)
- **turn=1**: Gemini finished turn (should trigger CONV mode)
- **interrupted=0**: Response not interrupted by user (0=complete, 1=cut off)

**What to look for**:
- ✅ **Good**: timeout→queue=0→turn=1→CONV mode starts
- ⚠️ **Premature**: timeout→queue>0 (audio cut off early)
- ❌ **Stuck**: timeout→turn=0 (turnComplete message never received)

## Testing Procedure

### Step 1: Check Server Pacing
1. Restart Deno server: `deno run --allow-all server/main.ts`
2. Upload ESP32 firmware
3. Ask simple question: "What time is it?"
4. **Look for**:
   - `📊 [STREAM]` with avg interval ~30-35ms
   - fast packets <20%
   - queue staying 5-15

### Step 2: Check Queue Behavior
1. Ask longer question: "Tell me about Brighton"
2. **Look for**:
   - `📈 Queue started filling: 0 → 1` at beginning
   - NO "⚠️ Queue near full" warnings
   - NO "⚠️ Dropped packets" messages
   - Queue staying stable in `📊 [STREAM]` logs

### Step 3: Check Conversation Mode
1. Ask simple question: "What's the weather?"
2. Wait for audio to finish
3. **Look for sequence**:
   ```
   ⏹️  Audio playback complete (queue=0, turn=1, interrupted=0)
   💬 [CONV] active, window=0ms/10000, LED=12, turnComplete=1
   🕐 Showing conversation window countdown
   ```
4. **Should see**: LED mode change to countdown, 10 second window
5. **Try**: Ask follow-up question immediately (should start recording)

### Step 4: Long Response Stress Test
1. Ask: "Tell me a story about a jellyfish"
2. Monitor throughout 30+ second response
3. **Look for**:
   - Consistent `📊 [STREAM]` metrics (no degradation)
   - No packet drops
   - Queue stable throughout
   - Clean completion at end

## Known Issues to Watch For

### Issue: Packets Still Bursting
**Symptoms**:
- `📊 [STREAM]` shows avg interval <20ms
- fast packets >30%
- Queue rapidly fills to 28-30
- Packet drops occur

**Possible causes**:
1. Server code not deployed (check Deno restart)
2. Network buffering between server and ESP32
3. WebSocket library buffering despite server pacing

**Solutions**:
- Verify server has 30ms `setTimeout()` in Opus loop
- Try increasing server delay to 40ms or 50ms
- Check network path (direct connection vs router)

### Issue: Conversation Mode Not Starting
**Symptoms**:
- `⏹️ Audio playback complete` shows turn=1
- But no `💬 [CONV] active` message appears
- LED stays in IDLE mode instead of countdown

**Possible causes**:
1. `conversationMode` flag not being set
2. `isPlayingResponse` not clearing properly
3. Logic condition failing

**Solutions**:
- Check server sends `turnComplete` message
- Verify `responseInterrupted` is false
- Add breakpoint in conversation mode condition

## Next Steps

1. **Upload firmware with diagnostics**
2. **Run all 4 test procedures**
3. **Capture serial output** for analysis
4. **Share diagnostic logs** - paste the `📊 [STREAM]`, `💬 [CONV]`, and any warning messages

This will give us concrete data about:
- Whether server pacing is working
- Where conversation mode is failing

