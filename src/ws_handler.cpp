#include "ws_handler.h"

// ── Shared audio-drain helper ────────────────────────────────────────────────
// Drain buffered audio, zero I2S DMA, and set a suppression window.
// Prevents the ~1.2s audio tail that occurs when voice commands stop a stream
// without touching the local audio queue or DMA buffer.
void drainAudioAndSilence(uint32_t windowMs) {
    AudioChunk dummy;
    while (xQueueReceive(audioOutputQueue, &dummy, 0) == pdTRUE) {}
    i2sZeroSafe();
    ambientSound.drainUntil = millis() + windowMs;
}

// ── Central ConvState transitions ────────────────────────────────────────────
// Owns entry actions that must fire on EVERY transition to each state.
// Callers set LEDs and other context-specific flags after calling this.
void transitionConvState(ConvState newState) {
    convState = newState;
    switch (newState) {
        case ConvState::RECORDING:
            waitingEnteredAt = 0;
            turnComplete = false;  // prevent stale-flag window open before first mic DMA chunk
            responseInterrupted = false;  // Clear stale interrupt flag from previous turn
            break;
        case ConvState::WAITING:
            waitingEnteredAt = millis();
            break;
        case ConvState::IDLE:
            waitingEnteredAt = 0;  // prevent stale value causing spurious timeout guards
            break;
        default:
            break;  // PLAYING, WINDOW: no universally-required entry actions
    }
}

// ── Safe WebSocket send with error logging ───────────────────────────────────
bool wsSendMessage(const String& msg) {
    // Serialise all sends: WebSocketsClient::sendTXT is not thread-safe and is called
    // from websocketTask (WS loop / ping), audioTask (mic PCM), and loop() (control msgs).
    // Concurrent calls corrupt the internal frame buffer and cause server disconnects.
    if (wsSendMutex && xSemaphoreTake(wsSendMutex, pdMS_TO_TICKS(200)) != pdTRUE) {
        Serial.printf("[WS] send mutex timeout — dropped %u bytes\n", msg.length());
        return false;
    }
    bool ok = webSocket.sendTXT(msg.c_str(), msg.length());
    if (wsSendMutex) xSemaphoreGive(wsSendMutex);
    if (!ok) Serial.printf("[WS] sendTXT failed (%u bytes)\n", msg.length());
    return ok;
}

void handleWebSocketMessage(uint8_t* payload, size_t length) {
 JsonDocument doc;
 DeserializationError error = deserializeJson(doc, payload, length);
 
 if (error) {
 Serial.printf("JSON parse error: %s\n", error.c_str());
 return;
 }
 const char* msgType = doc["type"] | "";

 // Handle server ready message
 if (strcmp(msgType, "ready") == 0) {
 Serial.printf("Server: %s\n", doc["message"].as<const char*>());
 return;
 }
 
 // Handle lazy-reconnect in-progress signal: Gemini was idle and is reconnecting.
 // Show LED_RECONNECTING with distinct animation so the user knows the device is
 // reconnecting, not processing their request. The button press that triggered
 // this was dropped; the user just needs to press again once reconnectComplete arrives.
 if (strcmp(msgType, "reconnecting") == 0) {
 Serial.println("Gemini reconnecting after idle — showing reconnecting animation");
 currentLEDMode = LED_RECONNECTING;
 transitionConvState(ConvState::IDLE);
 turnComplete = false;  // Clear stale flag from previous session
 return;
 }

 // Handle lazy-reconnect complete: Gemini is ready again.
 // Restore the mode that was active before reconnection.
 if (strcmp(msgType, "reconnectComplete") == 0) {
 Serial.println("Gemini reconnect complete — ready for interaction");
 transitionConvState(ConvState::IDLE);
 // Restore active mode rather than unconditionally going to LED_IDLE
 if (radioState.active) {
 currentLEDMode = LED_RADIO;
 } else if (meditationState.active) {
 currentLEDMode = LED_MEDITATION;
 } else if (pomodoroState.active) {
 currentLEDMode = LED_POMODORO;
 } else if (ambientSound.active) {
 currentLEDMode = LED_AMBIENT;
 } else {
 currentLEDMode = LED_IDLE;
 }
 return;
 }

 // Handle setup complete message
 if (strcmp(msgType, "setupComplete") == 0) {
 Serial.println("Setup complete - ready for interaction");
 // Prime state machine for the incoming boot greeting: treat it like a pending Gemini response.
 // Without this, convState stays IDLE and the auto-transition block (which guards on WAITING)
 // never opens the conversation window after the greeting finishes.
 transitionConvState(ConvState::WAITING);
 return;
 }
 
 // Handle turn complete
 if (strcmp(msgType, "turnComplete") == 0) {
 // Always store turnComplete, even during recording.
 // Gemini sends exactly ONE turnComplete per turn. If we discard it because
 // recordingActive=true, no second one will ever arrive and the device hangs.
 // The auto-transition block already guards on !recordingActive, so the
 // conversation window cannot open while the user is still speaking.
 if (recordingActive) {
 Serial.println("Turn complete (stored - recording still active)");
 } else {
 Serial.println("Turn complete");
 }
 turnComplete = true; // Mark turn as finished
 // Don't clear responseInterrupted here — it must persist until the user
 // explicitly starts a new recording. Otherwise late-arriving audio chunks
 // after the interrupt can leak into the new session.
 // Don't change LED mode here - let the audio finish playing naturally
 // isPlayingResponse will be set to false when audio actually stops
 // Conversation window will open after audio completes
 return;
 }
 
 // Handle function calls
 if (strcmp(msgType, "functionCall") == 0) {
 String funcName = doc["name"].as<String>();
 Serial.printf("Function call: %s\n", funcName.c_str());
 
 if (funcName == "set_volume") {
 String direction = doc["args"]["direction"].as<String>();
 if (direction == "up") {
 volumeMultiplier = min(2.0f, volumeMultiplier + 0.2f);
 Serial.printf("Volume up: %.0f%%\n", volumeMultiplier * 100);
 } else if (direction == "down") {
 volumeMultiplier = max(0.1f, volumeMultiplier - 0.2f);
 Serial.printf("Volume down: %.0f%%\n", volumeMultiplier * 100);
 }

 } else if (funcName == "set_volume_percent") {
 int percent = doc["args"]["percent"].as<int>();
 volumeMultiplier = constrain(percent / 100.0f, 0.1f, 2.0f);
 Serial.printf("Volume set: %d%%\n", percent);

 } else if (funcName == "set_volume_level") {
 // 1-10 scale from Gemini, maps to 10%-100%
 int level = doc["args"]["level"].as<int>();
 level = constrain(level, 1, 10);
 float newVolume = level * 10 / 100.0f; // 1→0.10, 10→1.00
 Serial.printf("Volume: level %d → %.0f%% (was %.0f%%)\n", level, newVolume * 100, volumeMultiplier * 100);
 volumeMultiplier = newVolume;
 // Always update savedVolume so radio duck/restore doesn't revert the change
 radioState.savedVolume = volumeMultiplier;
 }
 
 return;
 }
 
 // Handle tide data from server
 if (strcmp(msgType, "tideData") == 0) {
 Serial.println("Received tide data - storing for display after speech");
 strlcpy(tideState.state, doc["state"] | "", sizeof(tideState.state));
 tideState.waterLevel = doc["waterLevel"].as<float>();
 tideState.nextChangeMinutes = doc["nextChangeMinutes"].as<int>();
 tideState.active = true;
 // Don't switch LED mode yet - let it display after audio finishes
 
 Serial.printf("Tide: %s, water level: %.1f%%, next change in %d minutes\n",
 tideState.state,
 tideState.waterLevel * 100,
 tideState.nextChangeMinutes);
 return;
 }
 
 // Handle sunrise/sunset data from server
 if (strcmp(msgType, "sunData") == 0) {
 dayNightData.sunriseTime = doc["sunrise"].as<long long>() / 1000; // Convert ms to seconds
 dayNightData.sunsetTime = doc["sunset"].as<long long>() / 1000;
 dayNightData.valid = true;
 dayNightData.lastUpdate = millis();
 
 // Immediately update brightness
 updateDayNightBrightness();
 
 // Log the times
 struct tm sunriseInfo, sunsetInfo;
 localtime_r((time_t*)&dayNightData.sunriseTime, &sunriseInfo);
 localtime_r((time_t*)&dayNightData.sunsetTime, &sunsetInfo);
 char sunriseStr[10], sunsetStr[10];
 strftime(sunriseStr, sizeof(sunriseStr), "%H:%M", &sunriseInfo);
 strftime(sunsetStr, sizeof(sunsetStr), "%H:%M", &sunsetInfo);
 
 Serial.printf("Sunrise/sunset received: %s / %s (brightness: %s mode)\n",
 sunriseStr, sunsetStr,
 dayNightData.isDaytime ? "DAY": "NIGHT");
 return;
 }
 
 // Handle timer set from server
 if (strcmp(msgType, "timerSet") == 0) {
 Serial.println("Timer set - storing for display after speech");
 timerState.totalSeconds = doc["durationSeconds"].as<int>();
 timerState.startTime = millis();
 timerState.active = true;
 // Don't switch LED mode yet - let it display after audio finishes
 
 Serial.printf("Timer: %d seconds (%d minutes)\n",
 timerState.totalSeconds,
 timerState.totalSeconds / 60);
 return;
 }
 
 // Handle alarm set from server
 if (strcmp(msgType, "setAlarm") == 0) {
 uint32_t alarmID = doc["alarmID"].as<uint32_t>();
 time_t triggerTime = doc["triggerTime"].as<long long>() / 1000; // Convert ms to seconds
 
 // Find empty slot
 int slot = -1;
 for (int i = 0; i < MAX_ALARMS; i++) {
 if (!alarms[i].enabled) {
 slot = i;
 break;
 }
 }
 
 if (slot >= 0) {
 alarms[slot].alarmID = alarmID;
 alarms[slot].triggerTime = triggerTime;
 alarms[slot].enabled = true;
 alarms[slot].triggered = false;
 alarms[slot].snoozed = false;
 alarms[slot].snoozeUntil = 0;
 
 // Format time for logging
 struct tm timeinfo;
 localtime_r(&triggerTime, &timeinfo);
 char timeStr[20];
 strftime(timeStr, sizeof(timeStr), "%Y-%m-%d %H:%M:%S", &timeinfo);
 
 Serial.printf("Alarm set: ID=%u, time=%s (slot %d)\n", alarmID, timeStr, slot);
 alarmState.active = true;
 } else {
 Serial.println("No alarm slots available!");
 }
 return;
 }
 
 // Handle timer cancelled
 if (strcmp(msgType, "timerCancelled") == 0) {
 Serial.println("Timer cancelled");
 timerState.active = false;
 if (currentLEDMode == LED_TIMER) {
 currentLEDMode = LED_IDLE;
 }
 return;
 }
 
 // Handle timer expired — trigger the same alert as a real alarm
 if (strcmp(msgType, "timerExpired") == 0) {
 Serial.println("Timer expired!");
 timerState.active = false;

 // Save current state so the dismiss handler can restore it (same pattern as checkAlarms())
 alarmState.previousMode = currentLEDMode;
 alarmState.wasRecording = recordingActive;
 alarmState.wasPlayingResponse = isPlayingResponse;

 // Start alarm visual
 alarmState.ringing = true;
 alarmState.ringStartTime = millis();
 alarmState.pulseStartTime = millis();
 alarmState.pulseRadius = 0.0f;
 currentLEDMode = LED_ALARM;

 // Start alarm sound (server loops alarm audio until it receives "stopAlarm")
 isPlayingAlarm = true;
 isPlayingResponse = true;
 firstAudioChunk = true;
 lastAudioChunkTime = millis();
 JsonDocument alarmDoc;
 alarmDoc["action"] = "requestAlarm";
 String alarmMsg;
 serializeJson(alarmDoc, alarmMsg);
 wsSendMessage(alarmMsg);

 // Suppress thinking animation — timer-expiry Gemini notification should not show waiting state
 transitionConvState(ConvState::IDLE);
 Serial.println("Timer expired - alarm alert active, press button to dismiss");
 return;
 }
 
 // Handle cancel alarm from server
 if (strcmp(msgType, "cancelAlarm") == 0) {
 String which = doc["which"].as<String>();
 Serial.printf("Cancel alarm request: %s\n", which.c_str());
 
 if (which == "all") {
 // Cancel all alarms
 int cancelledCount = 0;
 for (int i = 0; i < MAX_ALARMS; i++) {
 if (alarms[i].enabled) {
 alarms[i].alarmID = 0;
 alarms[i].triggerTime = 0;
 alarms[i].enabled = false;
 alarms[i].triggered = false;
 alarms[i].snoozed = false;
 alarms[i].snoozeUntil = 0;
 cancelledCount++;
 }
 }
 Serial.printf("Cancelled %d alarm(s)\n", cancelledCount);
 alarmState.active = false;
 } else {
 // Cancel next alarm (earliest one)
 time_t earliestTime = 0;
 int earliestSlot = -1;
 
 struct tm timeinfo;
 if (getLocalTime(&timeinfo)) {
 time_t now = mktime(&timeinfo);
 
 for (int i = 0; i < MAX_ALARMS; i++) {
 if (alarms[i].enabled && alarms[i].triggerTime > now) {
 if (earliestSlot == -1 || alarms[i].triggerTime < earliestTime) {
 earliestTime = alarms[i].triggerTime;
 earliestSlot = i;
 }
 }
 }
 
 if (earliestSlot >= 0) {
 uint32_t cancelledID = alarms[earliestSlot].alarmID;
 
 // Clear the slot
 alarms[earliestSlot].alarmID = 0;
 alarms[earliestSlot].triggerTime = 0;
 alarms[earliestSlot].enabled = false;
 alarms[earliestSlot].triggered = false;
 alarms[earliestSlot].snoozed = false;
 alarms[earliestSlot].snoozeUntil = 0;
 
 Serial.printf("Cancelled next alarm ID=%u from slot %d\n", cancelledID, earliestSlot);
 
 // Check if any more alarms are active
 bool hasActiveAlarms = false;
 for (int i = 0; i < MAX_ALARMS; i++) {
 if (alarms[i].enabled) {
 hasActiveAlarms = true;
 break;
 }
 }
 if (!hasActiveAlarms) {
 alarmState.active = false;
 }
 } else {
 Serial.println("No active alarms to cancel");
 }
 }
 }
 return;
 }
 
 // Handle list alarms request
 if (strcmp(msgType, "listAlarms") == 0) {
 Serial.println("List alarms request");
 
 struct tm timeinfo;
 if (getLocalTime(&timeinfo)) {
 time_t now = mktime(&timeinfo);
 
 // Build alarm list
 JsonDocument responseDoc;
 responseDoc["type"] = "alarmList";
 JsonArray alarmArray = responseDoc["alarms"].to<JsonArray>();
 
 for (int i = 0; i < MAX_ALARMS; i++) {
 // List all enabled alarms (don't filter by time - let Gemini handle past alarms)
 if (alarms[i].enabled) {
 JsonObject alarmObj = alarmArray.add<JsonObject>();
 alarmObj["alarmID"] = alarms[i].alarmID;
 alarmObj["triggerTime"] = (long long)alarms[i].triggerTime * 1000; // Convert to ms
 
 // Format time string for logging
 struct tm alarmTimeinfo;
 localtime_r(&alarms[i].triggerTime, &alarmTimeinfo);
 char timeStr[32];
 strftime(timeStr, sizeof(timeStr), "%Y-%m-%d %H:%M", &alarmTimeinfo);
 alarmObj["formattedTime"] = timeStr;
 
 // Add flag for whether alarm is in the past
 alarmObj["isPast"] = (alarms[i].triggerTime <= now);
 
 Serial.printf("Alarm %u: %s (isPast=%d)\n", alarms[i].alarmID, timeStr, (alarms[i].triggerTime <= now));
 }
 }
 
 String responseMsg;
 serializeJson(responseDoc, responseMsg);
 wsSendMessage(responseMsg);
 Serial.printf("Sent alarm list: %d alarm(s)\n", alarmArray.size());
 }
 return;
 }
 
 // Handle moon data
 if (strcmp(msgType, "moonData") == 0) {
 Serial.println("Received moon data - storing for display after speech");
 strlcpy(moonState.phaseName, doc["phaseName"] | "", sizeof(moonState.phaseName));
 moonState.illumination = doc["illumination"].as<int>();
 moonState.moonAge = doc["moonAge"].as<float>();
 moonState.active = true;
 // Don't switch LED mode yet - let it display after audio finishes
 
 Serial.printf("Moon: %s (%d%% illuminated, %.1f days old)\n", 
 moonState.phaseName, moonState.illumination, moonState.moonAge);
 return;
 }
 
 // Handle ambient stream completion
 if (strcmp(msgType, "ambientComplete") == 0) {
 String soundName = doc["sound"].as<String>();
 uint16_t sequence = doc["sequence"].as<uint16_t>();
 Serial.printf("Ambient track complete: %s (seq %d)\n", soundName.c_str(), sequence);
 
 // Validate: Only process if this completion matches what we're currently playing
 if (strcmp(ambientSound.name, soundName.c_str()) != 0) {
 Serial.printf("Ignoring stale completion: expected '%s', got '%s'\n", 
 ambientSound.name, soundName.c_str());
 return;
 }
 
 // For meditation mode: auto-advance to next chakra
 if (meditationState.active && currentLEDMode == LED_MEDITATION) {
 if (meditationState.currentChakra < MeditationState::CROWN) {
 // Auto-advance to next chakra
 meditationState.currentChakra = (MeditationState::Chakra)(meditationState.currentChakra + 1);
 meditationState.phase = MeditationState::HOLD_BOTTOM;
        meditationState.phaseStartTime = 0; // Will sync to first arriving audio chunk
 // Request next chakra sound
 JsonDocument reqDoc;
 reqDoc["action"] = "requestAmbient";
 char nextSound[16];
 sprintf(nextSound, "bell%03d", meditationState.currentChakra + 1);
 reqDoc["sound"] = nextSound;
 reqDoc["loops"] = 8;
 reqDoc["sequence"] = ++ambientSound.sequence;
 String reqMsg;
 serializeJson(reqDoc, reqMsg);
 wsSendMessage(reqMsg);
 
 strlcpy(ambientSound.name, nextSound, sizeof(ambientSound.name));
 firstAudioChunk = true;
 lastAudioChunkTime = millis();
 } else {
 // Completed all 7 chakras - return to IDLE
 Serial.println("Meditation sequence complete - returning to IDLE");
 meditationState.active = false;
 isPlayingAmbient = false;
 isPlayingResponse = false;
 volumeMultiplier = meditationState.savedVolume;
 Serial.printf("Volume restored to %.0f%%\n", volumeMultiplier * 100);
 currentLEDMode = LED_IDLE;
 }
 }
 return;
 }
 
 // Handle Pomodoro commands
 if (strcmp(msgType, "pomodoroStart") == 0) {
 // Get custom durations if provided, otherwise use current settings
 if (doc["focusMinutes"].is<int>()) {
 pomodoroState.focusDuration = doc["focusMinutes"].as<int>();
 Serial.printf("Custom focus duration: %d minutes\n", pomodoroState.focusDuration);
 }
 if (doc["shortBreakMinutes"].is<int>()) {
 pomodoroState.shortBreakDuration = doc["shortBreakMinutes"].as<int>();
 Serial.printf("Custom short break: %d minutes\n", pomodoroState.shortBreakDuration);
 }
 if (doc["longBreakMinutes"].is<int>()) {
 pomodoroState.longBreakDuration = doc["longBreakMinutes"].as<int>();
 Serial.printf("Custom long break: %d minutes\n", pomodoroState.longBreakDuration);
 }
 
 Serial.println("Pomodoro started via voice command");
 currentLEDMode = LED_POMODORO;
 pomodoroState.active = true;
 pomodoroState.currentSession = PomodoroState::FOCUS;
 pomodoroState.sessionCount = 0;
 pomodoroState.totalSeconds = pomodoroState.focusDuration * 60;
 pomodoroState.paused = false;
 pomodoroState.startTime = millis();
 playVolumeChime();
 return;
 }
 
 if (strcmp(msgType, "pomodoroPause") == 0) {
 Serial.println("Pomodoro paused via voice command");
 if (pomodoroState.active && !pomodoroState.paused) {
 uint32_t elapsed = (millis() - pomodoroState.startTime) / 1000;
 pomodoroState.pausedTime = pomodoroState.totalSeconds - elapsed;
 pomodoroState.paused = true;
 pomodoroState.startTime = 0;
 playVolumeChime();
 }
 return;
 }
 
 if (strcmp(msgType, "pomodoroResume") == 0) {
 Serial.println("Pomodoro resumed via voice command");
 if (pomodoroState.active && pomodoroState.paused) {
 pomodoroState.startTime = millis();
 pomodoroState.paused = false;
 playVolumeChime();
 }
 return;
 }
 
 if (strcmp(msgType, "pomodoroStop") == 0) {
 Serial.println("Pomodoro stopped via voice command");
 pomodoroState.active = false;
 pomodoroState.paused = false;
 pomodoroState.sessionCount = 0;
 currentLEDMode = LED_IDLE;
 playShutdownSound();
 return;
 }
 
 if (strcmp(msgType, "pomodoroSkip") == 0) {
 Serial.println("Skipping to next Pomodoro session");
 if (pomodoroState.active) {
 // Trigger session transition by setting remaining time to 0
 pomodoroState.startTime = millis() - (pomodoroState.totalSeconds * 1000);
 pomodoroState.paused = false;
 }
 return;
 }
 
 if (strcmp(msgType, "pomodoroStatusRequest") == 0) {
 Serial.println("Pomodoro status requested");
 
 JsonDocument statusDoc;
 statusDoc["type"] = "pomodoroStatusResponse";
 
 if (pomodoroState.active) {
 uint32_t secondsRemaining;
 if (pomodoroState.paused) {
 secondsRemaining = pomodoroState.pausedTime;
 } else {
 uint32_t elapsed = (millis() - pomodoroState.startTime) / 1000;
 secondsRemaining = pomodoroState.totalSeconds > elapsed ? pomodoroState.totalSeconds - elapsed : 0;
 }
 int minutes = secondsRemaining / 60;
 int seconds = secondsRemaining % 60;
 const char* sessionName = (pomodoroState.currentSession == PomodoroState::FOCUS) ? "Focus":
 (pomodoroState.currentSession == PomodoroState::SHORT_BREAK) ? "Short Break": "Long Break";
 
 statusDoc["active"] = true;
 statusDoc["session"] = sessionName;
 statusDoc["minutesRemaining"] = minutes;
 statusDoc["secondsRemaining"] = seconds;
 statusDoc["paused"] = pomodoroState.paused;
 statusDoc["cycleNumber"] = pomodoroState.sessionCount + 1;
 
 Serial.printf("Status: %s session, %d:%02d remaining, %s, cycle %d/4\n",
 sessionName, minutes, seconds, pomodoroState.paused ? "paused": "running", pomodoroState.sessionCount + 1);
 } else {
 statusDoc["active"] = false;
 Serial.println("Pomodoro not active");
 }
 
 String statusMsg;
 serializeJson(statusDoc, statusMsg);
 wsSendMessage(statusMsg);
 Serial.println("Sent Pomodoro status to server");
 return;
 }

 // Handle device state request — returns full device state snapshot to the server
 // Used by the get_device_state Gemini tool to give the model accurate self-awareness.
 if (strcmp(msgType, "deviceStateRequest") == 0) {
 Serial.println("Device state requested");

 JsonDocument stateDoc;
 stateDoc["type"] = "deviceStateResponse";

 // Pomodoro state
 JsonObject pomDoc = stateDoc["pomodoro"].to<JsonObject>();
 pomDoc["active"] = pomodoroState.active;
 if (pomodoroState.active) {
 const char* sessionName = (pomodoroState.currentSession == PomodoroState::FOCUS) ? "Focus" :
 (pomodoroState.currentSession == PomodoroState::SHORT_BREAK) ? "Short Break" : "Long Break";
 pomDoc["session"] = sessionName;
 pomDoc["paused"] = pomodoroState.paused;
 uint32_t secsLeft;
 if (pomodoroState.paused) {
 secsLeft = pomodoroState.pausedTime;
 } else if (pomodoroState.startTime > 0) {
 uint32_t elapsed = (millis() - pomodoroState.startTime) / 1000;
 secsLeft = pomodoroState.totalSeconds > elapsed ? pomodoroState.totalSeconds - elapsed : 0;
 } else {
 secsLeft = pomodoroState.totalSeconds;
 }
 pomDoc["secondsRemaining"] = secsLeft;
 pomDoc["cycleNumber"] = pomodoroState.sessionCount + 1;
 }

 // Meditation state
 JsonObject medDoc = stateDoc["meditation"].to<JsonObject>();
 medDoc["active"] = meditationState.active;
 if (meditationState.active) {
 medDoc["chakra"] = CHAKRA_NAMES[meditationState.currentChakra];
 }

 // Ambient sound state
 JsonObject ambDoc = stateDoc["ambient"].to<JsonObject>();
 ambDoc["active"] = ambientSound.active;
 if (ambientSound.active) {
 ambDoc["sound"] = ambientSound.name;
 }

 // Lamp state
 JsonObject lampDoc = stateDoc["lamp"].to<JsonObject>();
 lampDoc["active"] = lampState.active;
 if (lampState.active) {
 const char* colorName = "white";
 if (lampState.currentColor == LampState::RED) colorName = "red";
 else if (lampState.currentColor == LampState::GREEN) colorName = "green";
 else if (lampState.currentColor == LampState::BLUE) colorName = "blue";
 lampDoc["color"] = colorName;
 }

 // Volume (0-100%)
 stateDoc["volume"] = (int)roundf(volumeMultiplier * 100.0f);

 // Radio state
 JsonObject radioDoc = stateDoc["radio"].to<JsonObject>();
 radioDoc["active"] = radioState.active;
 radioDoc["streaming"] = radioState.streaming;
 if (radioState.active && radioState.streaming) {
 radioDoc["station"] = radioState.stationName;
 radioDoc["isHLS"] = radioState.isHLS;
 radioDoc["visualsActive"] = radioState.visualsActive;
 }

 // Alarm list
 JsonArray alarmArray = stateDoc["alarms"].to<JsonArray>();
 struct tm timeinfo;
 if (getLocalTime(&timeinfo)) {
 for (int i = 0; i < MAX_ALARMS; i++) {
 if (alarms[i].enabled) {
 JsonObject alarmObj = alarmArray.add<JsonObject>();
 alarmObj["alarmID"] = alarms[i].alarmID;
 alarmObj["triggerTime"] = (long long)alarms[i].triggerTime * 1000; // ms
 struct tm alarmTimeinfo;
 localtime_r(&alarms[i].triggerTime, &alarmTimeinfo);
 char timeStr[16];
 strftime(timeStr, sizeof(timeStr), "%H:%M", &alarmTimeinfo);
 alarmObj["formattedTime"] = timeStr;
 }
 }
 }

 String stateMsg;
 serializeJson(stateDoc, stateMsg);
 wsSendMessage(stateMsg);
 Serial.printf("Sent deviceStateResponse (%d chars)\n", stateMsg.length());
 return;
 }
 if (strcmp(msgType, "ambientStart") == 0) {
 const char* sound = doc["sound"] | "rain";
 Serial.printf("ambientStart: %s\n", sound);

 // Stop any currently playing ambient
 if (isPlayingAmbient) {
 JsonDocument stopDoc;
 stopDoc["action"] = "stopAmbient";
 String stopMsg;
 serializeJson(stopDoc, stopMsg);
 wsSendMessage(stopMsg);
 }

 // Stop meditation if active
 if (meditationState.active) {
 volumeMultiplier = meditationState.savedVolume;
 meditationState.active = false;
 }

 // Map sound name to type
 if (strcmp(sound, "ocean") == 0) {
 currentAmbientSoundType = SOUND_OCEAN;
 strlcpy(ambientSound.name, "ocean", sizeof(ambientSound.name));
 } else if (strcmp(sound, "rainforest") == 0) {
 currentAmbientSoundType = SOUND_RAINFOREST;
 strlcpy(ambientSound.name, "rainforest", sizeof(ambientSound.name));
 } else if (strcmp(sound, "fire") == 0) {
 currentAmbientSoundType = SOUND_FIRE;
 strlcpy(ambientSound.name, "fire", sizeof(ambientSound.name));
 } else {
 currentAmbientSoundType = SOUND_RAIN;
 strlcpy(ambientSound.name, "rain", sizeof(ambientSound.name));
 }

 ambientSound.active = true;
 ambientSound.sequence++;
 isPlayingAmbient = true;
 isPlayingResponse = false;
 firstAudioChunk = true;
 lastAudioChunkTime = millis();

 // Flush any stale Gemini audio still queued to prevent overlap with ambient
 {
 AudioChunk dummy;
 while (xQueueReceive(audioOutputQueue, &dummy, 0) == pdTRUE) {}
 i2sZeroSafe();
 }

 // Request ambient audio from server immediately
 {
 JsonDocument ambientDoc;
 ambientDoc["action"] = "requestAmbient";
 ambientDoc["sound"] = ambientSound.name;
 ambientDoc["sequence"] = ambientSound.sequence;
 String ambientMsg;
 serializeJson(ambientDoc, ambientMsg);
 Serial.printf("Ambient audio request: %s (seq %d)\n", ambientMsg.c_str(), ambientSound.sequence);
 wsSendMessage(ambientMsg);
 }
 currentLEDMode = LED_AMBIENT;
 return;
 }

 // Handle meditationStart - voice-commanded meditation
 if (strcmp(msgType, "meditationStart") == 0) {
 Serial.println("meditationStart");

 // Stop ambient if playing
 if (isPlayingAmbient) {
 JsonDocument stopDoc;
 stopDoc["action"] = "stopAmbient";
 String stopMsg;
 serializeJson(stopDoc, stopMsg);
 wsSendMessage(stopMsg);
 isPlayingAmbient = false;
 ambientSound.active = false;
 ambientSound.sequence++;
 }

 // Initialise meditation state
 meditationState.currentChakra = MeditationState::ROOT;
 meditationState.phase = MeditationState::HOLD_BOTTOM;
 meditationState.active = true;
 meditationState.savedVolume = volumeMultiplier;
 volumeMultiplier = 0.10f; // 10% volume for meditation

 if (isPlayingResponse) {
 // Gemini is still speaking its verbal confirmation - defer audio start
 // until Gemini's turn ends (detected in the audio completion handler)
 meditationState.phaseStartTime = 0; // 0 = not yet started
 meditationState.streaming = false;
 Serial.println("Meditation queued - will start after Gemini finishes speaking");
 } else {
 // No Gemini audio playing - start immediately (phaseStartTime syncs to first chunk)
 meditationState.phaseStartTime = 0;
 meditationState.streaming = false;
 strlcpy(ambientSound.name, "bell001", sizeof(ambientSound.name));
 ambientSound.active = true;
 isPlayingAmbient = true;
 isPlayingResponse = false;
 firstAudioChunk = true;
 lastAudioChunkTime = millis();
 {
 JsonDocument meditationReqDoc;
 meditationReqDoc["action"] = "requestAmbient";
 meditationReqDoc["sound"] = "bell001";
 meditationReqDoc["loops"] = 8;
 meditationReqDoc["sequence"] = ++ambientSound.sequence;
 String meditationReqMsg;
 serializeJson(meditationReqDoc, meditationReqMsg);
 Serial.printf("Meditation starting: %s (seq %d)\n", meditationReqMsg.c_str(), ambientSound.sequence);
 wsSendMessage(meditationReqMsg);
 }
 meditationState.streaming = true;
 Serial.println("Meditation breathing and audio started (ROOT chakra)");
 }
 currentLEDMode = LED_MEDITATION;
 return;
 }

 // Handle radioStart - start internet radio stream
 if (strcmp(msgType, "radioStart") == 0) {
 const char* stationName = doc["stationName"] | "Radio";
 const char* streamUrl = doc["streamUrl"] | "";
 bool isHLS = doc["isHLS"] | false;
 Serial.printf("radioStart: %s (HLS: %s)\n", stationName, isHLS ? "yes" : "no");

 // Stop any ambient/meditation
 if (isPlayingAmbient) {
 JsonDocument stopDoc;
 stopDoc["action"] = "stopAmbient";
 String stopMsg;
 serializeJson(stopDoc, stopMsg);
 wsSendMessage(stopMsg);
 isPlayingAmbient = false;
 ambientSound.active = false;
 ambientSound.sequence++;
 }
 if (meditationState.active) {
 volumeMultiplier = meditationState.savedVolume;
 meditationState.active = false;
 }

 // Flush stale audio
 {
 AudioChunk dummy;
 while (xQueueReceive(audioOutputQueue, &dummy, 0) == pdTRUE) {}
 i2sZeroSafe();
 }

 // Set radio state
 radioState.active = true;
 radioState.streaming = false; // will become true after first chunk arrives
 radioState.paused = false;    // clear any pause from a previous conversation
 radioState.visualsActive = true;
 radioState.isHLS = isHLS;
 strlcpy(radioState.stationName, stationName, sizeof(radioState.stationName));
 strlcpy(radioState.streamUrl, streamUrl, sizeof(radioState.streamUrl));
 volumeMultiplier = RADIO_DEFAULT_VOLUME;     // Start at comfortable level (volume 3)
 radioState.savedVolume = RADIO_DEFAULT_VOLUME;  // Duck/restore anchor

 // Reuse ambient pipeline for radio PCM chunks
 ambientSound.active = true;
 strlcpy(ambientSound.name, stationName, sizeof(ambientSound.name));
 ambientSound.sequence++;
 isPlayingAmbient = true;
 isPlayingResponse = false;
 firstAudioChunk = true;
 lastAudioChunkTime = millis();

 // Tell server to start streaming
 {
 JsonDocument radioReqDoc;
 radioReqDoc["action"] = "requestRadio";
 radioReqDoc["streamUrl"] = radioState.streamUrl;
 radioReqDoc["stationName"] = radioState.stationName;
 radioReqDoc["sequence"] = ambientSound.sequence;
 radioReqDoc["isHLS"] = isHLS;
 String radioReqMsg;
 serializeJson(radioReqDoc, radioReqMsg);
 Serial.printf("Radio request sent (seq %d)\n", ambientSound.sequence);
 wsSendMessage(radioReqMsg);
 }
 currentLEDMode = LED_RADIO;
 return;
 }

 // Handle radioEnded - station went offline or stream stopped
 if (strcmp(msgType, "radioEnded") == 0) {
 const char* stationName = doc["stationName"] | radioState.stationName;
 bool isError = doc["error"] | false;
 Serial.printf("radioEnded: %s (error: %s)\n", stationName, isError ? "yes" : "no");

 // Restore volume if it was ducked
 if (radioState.savedVolume > 0.05f) {
 volumeMultiplier = radioState.savedVolume;
 }

 // Clear radio + ambient state
 radioState.active = false;
 radioState.streaming = false;
 radioState.paused = false;
 radioState.stationName[0] = '\0';
 radioState.streamUrl[0] = '\0';
 isPlayingAmbient = false;
 ambientSound.active = false;
 ambientSound.name[0] = '\0';
 ambientSound.sequence++;

 drainAudioAndSilence(2000); // radio stream may have queued chunks; 2s window to flush tail
 currentLEDMode = LED_IDLE;
 return;
 }

 // Handle lampStart - voice-commanded lamp
 if (strcmp(msgType, "lampStart") == 0) {
 const char* color = doc["color"] | "white";
 Serial.printf("lampStart: %s\n", color);

 // Stop ambient if playing
 if (isPlayingAmbient) {
 JsonDocument stopDoc;
 stopDoc["action"] = "stopAmbient";
 String stopMsg;
 serializeJson(stopDoc, stopMsg);
 wsSendMessage(stopMsg);
 isPlayingAmbient = false;
 ambientSound.active = false;
 ambientSound.sequence++;
 }

 // Stop meditation if active
 if (meditationState.active) {
 volumeMultiplier = meditationState.savedVolume;
 meditationState.active = false;
 }

 // Map colour string
 LampState::Color lampColor = LampState::WHITE;
 if (strcmp(color, "red") == 0) lampColor = LampState::RED;
 else if (strcmp(color, "green") == 0) lampColor = LampState::GREEN;
 else if (strcmp(color, "blue") == 0) lampColor = LampState::BLUE;

 lampState.active = true;
 lampState.currentColor = lampColor;
 lampState.previousColor = lampColor;
 lampState.currentRow = 0;
 lampState.currentCol = 0;
 lampState.lastUpdate = millis();
 lampState.fullyLit = false;
 lampState.transitioning = false;
 for (int i = 0; i < NUM_LEDS; i++) lampState.ledStartTimes[i] = 0;

 // Only switch LED mode immediately if Gemini audio has finished.
 // If audio is still draining, lampState.active=true is enough - the
 // isPersistentMode recovery block will transition to LED_LAMP cleanly.
 if (!isPlayingResponse) {
 currentLEDMode = LED_LAMP;
 } else {
 Serial.println("lampStart: audio still playing, will switch to LED_LAMP after drain");
 }
 return;
 }

 // Handle switchToIdle - voice-commanded return to idle
 if (strcmp(msgType, "switchToIdle") == 0) {
 Serial.println("switchToIdle");

 // Stop ambient if playing
 if (isPlayingAmbient) {
 JsonDocument stopDoc;
 stopDoc["action"] = "stopAmbient";
 String stopMsg;
 serializeJson(stopDoc, stopMsg);
 wsSendMessage(stopMsg);
 }
 isPlayingAmbient = false;
 isPlayingResponse = false;
 ambientSound.active = false;
 ambientSound.name[0] = '\0';
 ambientSound.sequence++;

 // Restore meditation volume if needed
 if (meditationState.active) {
 volumeMultiplier = meditationState.savedVolume;
 }
 meditationState.active = false;

 // Clear Pomodoro if active
 pomodoroState.active = false;
 pomodoroState.paused = false;
 pomodoroState.startTime = 0;

 // Clear lamp
 lampState.active = false;
 lampState.fullyLit = false;

 // Clear radio state
 if (radioState.active) {
 if (radioState.savedVolume > 0.05f) {
 volumeMultiplier = radioState.savedVolume;
 }
 radioState.active = false;
 radioState.streaming = false;
 radioState.stationName[0] = '\0';
 radioState.streamUrl[0] = '\0';
 }

 drainAudioAndSilence(500);
 currentLEDMode = LED_IDLE;
 return;
 }
 
 // Handle text responses
 if (strcmp(msgType, "text") == 0) {
 Serial.printf("Text: %s\n", doc["text"].as<const char*>());
 return;
 }
 
 // Handle errors
 if (doc["error"].is<const char*>()) {
 Serial.printf("Error: %s\n", doc["error"].as<const char*>());
 currentLEDMode = LED_ERROR;
 return;
 }
}
