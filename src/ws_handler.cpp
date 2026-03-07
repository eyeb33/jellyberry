#include "ws_handler.h"

void handleWebSocketMessage(uint8_t* payload, size_t length) {
 JsonDocument doc;
 DeserializationError error = deserializeJson(doc, payload, length);
 
 if (error) {
 Serial.printf("JSON parse error: %s\n", error.c_str());
 return;
 }
 
 // Handle server ready message
 if (doc["type"].is<const char*>() && doc["type"] == "ready") {
 Serial.printf("Server: %s\n", doc["message"].as<const char*>());
 return;
 }
 
 // Handle setup complete message
 if (doc["type"].is<const char*>() && doc["type"] == "setupComplete") {
 Serial.println("Setup complete - ready for interaction");
 // Prime state machine for the incoming boot greeting: treat it like a pending Gemini response.
 // Without this, convState stays IDLE and the auto-transition block (which guards on WAITING)
 // never opens the conversation window after the greeting finishes.
 convState = ConvState::WAITING;
 waitingEnteredAt = millis();
 return;
 }
 
 // Handle turn complete
 if (doc["type"].is<const char*>() && doc["type"] == "turnComplete") {
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
 // Don't change LED mode here - let the audio finish playing naturally
 // isPlayingResponse will be set to false when audio actually stops
 // Conversation window will open after audio completes
 
 // Clear interrupt flag - old turn is done, ready for new response
 if (responseInterrupted) {
 Serial.println("Old turn complete, cleared interrupt flag");
 responseInterrupted = false;
 }
 return;
 }
 
 // Handle function calls
 if (doc["type"].is<const char*>() && doc["type"] == "functionCall") {
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
 // Play a brief chime at the new volume
 playVolumeChime();
 
 } else if (funcName == "set_volume_percent") {
 int percent = doc["args"]["percent"].as<int>();
 volumeMultiplier = constrain(percent / 100.0f, 0.1f, 2.0f);
 Serial.printf("Volume set: %d%%\n", percent);
 
 // Play a brief chime at the new volume
 playVolumeChime();
 }
 
 return;
 }
 
 // Handle tide data from server
 if (doc["type"].is<const char*>() && doc["type"] == "tideData") {
 Serial.println("Received tide data - storing for display after speech");
 tideState.state = doc["state"].as<String>();
 tideState.waterLevel = doc["waterLevel"].as<float>();
 tideState.nextChangeMinutes = doc["nextChangeMinutes"].as<int>();
 tideState.active = true;
 // Don't switch LED mode yet - let it display after audio finishes
 
 Serial.printf("Tide: %s, water level: %.1f%%, next change in %d minutes\n",
 tideState.state.c_str(), 
 tideState.waterLevel * 100,
 tideState.nextChangeMinutes);
 return;
 }
 
 // Handle sunrise/sunset data from server
 if (doc["type"].is<const char*>() && doc["type"] == "sunData") {
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
 if (doc["type"].is<const char*>() && doc["type"] == "timerSet") {
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
 if (doc["type"].is<const char*>() && doc["type"] == "setAlarm") {
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
 if (doc["type"].is<const char*>() && doc["type"] == "timerCancelled") {
 Serial.println("Timer cancelled");
 timerState.active = false;
 if (currentLEDMode == LED_TIMER) {
 currentLEDMode = LED_IDLE;
 }
 return;
 }
 
 // Handle timer expired
 if (doc["type"].is<const char*>() && doc["type"] == "timerExpired") {
 Serial.println("Timer expired!");
 timerState.active = false;
 // Flash LEDs for completion (with mutex)
 for (int i = 0; i < 3; i++) {
 if (xSemaphoreTake(ledMutex, portMAX_DELAY) == pdTRUE) {
 fill_solid(leds, NUM_LEDS, CRGB::Green);
 FastLED.show();
 xSemaphoreGive(ledMutex);
 }
 delay(200);
 if (xSemaphoreTake(ledMutex, portMAX_DELAY) == pdTRUE) {
 fill_solid(leds, NUM_LEDS, CRGB::Black);
 FastLED.show();
 xSemaphoreGive(ledMutex);
 }
 delay(200);
 }
 // Do NOT pre-set isPlayingResponse here the standard audio prebuffer path in
 // onWebSocketEvent(WStype_BIN) will set it once enough packets have buffered.
 // Forcing it here caused the LED to switch before audio arrived (visual glitch)
 // and the 300ms delay was blocking the WebSocket task.
 // Clear waiting guard so the thinking animation doesn't fire for the timer-expiry response
 convState = ConvState::IDLE;
 waitingEnteredAt = 0;
 Serial.println("Timer expired - waiting for Gemini audio notification...");
 return;
 }
 
 // Handle cancel alarm from server
 if (doc["type"].is<const char*>() && doc["type"] == "cancelAlarm") {
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
 if (doc["type"].is<const char*>() && doc["type"] == "listAlarms") {
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
 webSocket.sendTXT(responseMsg);
 Serial.printf("Sent alarm list: %d alarm(s)\n", alarmArray.size());
 }
 return;
 }
 
 // Handle moon data
 if (doc["type"].is<const char*>() && doc["type"] == "moonData") {
 Serial.println("Received moon data - storing for display after speech");
 moonState.phaseName = doc["phaseName"].as<String>();
 moonState.illumination = doc["illumination"].as<int>();
 moonState.moonAge = doc["moonAge"].as<float>();
 moonState.active = true;
 // Don't switch LED mode yet - let it display after audio finishes
 
 Serial.printf("Moon: %s (%d%% illuminated, %.1f days old)\n", 
 moonState.phaseName.c_str(), moonState.illumination, moonState.moonAge);
 return;
 }
 
 // Handle ambient stream completion
 // Handle ambient stream completion
 if (doc["type"].is<const char*>() && doc["type"] == "ambientComplete") {
 String soundName = doc["sound"].as<String>();
 uint16_t sequence = doc["sequence"].as<uint16_t>();
 Serial.printf("Ambient track complete: %s (seq %d)\n", soundName.c_str(), sequence);
 
 // Validate: Only process if this completion matches what we're currently playing
 if (soundName != ambientSound.name) {
 Serial.printf("Ignoring stale completion: expected '%s', got '%s'\n", 
 ambientSound.name.c_str(), soundName.c_str());
 return;
 }
 
 // For meditation mode: auto-advance to next chakra
 if (meditationState.active && currentLEDMode == LED_MEDITATION) {
 if (meditationState.currentChakra < MeditationState::CROWN) {
 // Auto-advance to next chakra
 meditationState.currentChakra = (MeditationState::Chakra)(meditationState.currentChakra + 1);
 meditationState.phase = MeditationState::HOLD_BOTTOM;
 meditationState.phaseStartTime = millis();
 
 Serial.printf("Auto-advancing to %s chakra\n", CHAKRA_NAMES[meditationState.currentChakra]);
 
 // Request next chakra sound
 JsonDocument reqDoc;
 reqDoc["action"] = "requestAmbient";
 char nextSound[16];
 sprintf(nextSound, "om%03d", meditationState.currentChakra + 1);
 reqDoc["sound"] = nextSound;
 reqDoc["sequence"] = ++ambientSound.sequence;
 String reqMsg;
 serializeJson(reqDoc, reqMsg);
 webSocket.sendTXT(reqMsg);
 
 ambientSound.name = nextSound;
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
 if (doc["type"].is<const char*>() && doc["type"] == "pomodoroStart") {
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
 
 if (doc["type"].is<const char*>() && doc["type"] == "pomodoroPause") {
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
 
 if (doc["type"].is<const char*>() && doc["type"] == "pomodoroResume") {
 Serial.println("Pomodoro resumed via voice command");
 if (pomodoroState.active && pomodoroState.paused) {
 pomodoroState.startTime = millis();
 pomodoroState.paused = false;
 playVolumeChime();
 }
 return;
 }
 
 if (doc["type"].is<const char*>() && doc["type"] == "pomodoroStop") {
 Serial.println("Pomodoro stopped via voice command");
 pomodoroState.active = false;
 pomodoroState.paused = false;
 pomodoroState.sessionCount = 0;
 currentLEDMode = LED_IDLE;
 playShutdownSound();
 return;
 }
 
 if (doc["type"].is<const char*>() && doc["type"] == "pomodoroSkip") {
 Serial.println("Skipping to next Pomodoro session");
 if (pomodoroState.active) {
 // Trigger session transition by setting remaining time to 0
 pomodoroState.startTime = millis() - (pomodoroState.totalSeconds * 1000);
 pomodoroState.paused = false;
 }
 return;
 }
 
 if (doc["type"].is<const char*>() && doc["type"] == "pomodoroStatusRequest") {
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
 webSocket.sendTXT(statusMsg);
 Serial.println("Sent Pomodoro status to server");
 return;
 }

 // Handle ambientStart - voice-commanded ambient sound
 if (doc["type"].is<const char*>() && doc["type"] == "ambientStart") {
 const char* sound = doc["sound"] | "rain";
 Serial.printf("ambientStart: %s\n", sound);

 // Stop any currently playing ambient
 if (isPlayingAmbient) {
 JsonDocument stopDoc;
 stopDoc["action"] = "stopAmbient";
 String stopMsg;
 serializeJson(stopDoc, stopMsg);
 webSocket.sendTXT(stopMsg);
 }

 // Stop meditation if active
 if (meditationState.active) {
 volumeMultiplier = meditationState.savedVolume;
 meditationState.active = false;
 }

 // Map sound name to type
 if (strcmp(sound, "ocean") == 0) {
 currentAmbientSoundType = SOUND_OCEAN;
 ambientSound.name = "ocean";
 } else if (strcmp(sound, "rainforest") == 0) {
 currentAmbientSoundType = SOUND_RAINFOREST;
 ambientSound.name = "rainforest";
 } else if (strcmp(sound, "fire") == 0) {
 currentAmbientSoundType = SOUND_FIRE;
 ambientSound.name = "fire";
 } else {
 currentAmbientSoundType = SOUND_RAIN;
 ambientSound.name = "rain";
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
 i2s_zero_dma_buffer(I2S_NUM_1);
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
 webSocket.sendTXT(ambientMsg);
 }
 currentLEDMode = LED_AMBIENT;
 return;
 }

 // Handle meditationStart - voice-commanded meditation
 if (doc["type"].is<const char*>() && doc["type"] == "meditationStart") {
 Serial.println("meditationStart");

 // Stop ambient if playing
 if (isPlayingAmbient) {
 JsonDocument stopDoc;
 stopDoc["action"] = "stopAmbient";
 String stopMsg;
 serializeJson(stopDoc, stopMsg);
 webSocket.sendTXT(stopMsg);
 isPlayingAmbient = false;
 ambientSound.active = false;
 ambientSound.sequence++;
 }

 // Initialise meditation state
 meditationState.currentChakra = MeditationState::ROOT;
 meditationState.phase = MeditationState::HOLD_BOTTOM;
 meditationState.active = true;
 meditationState.savedVolume = volumeMultiplier;
 volumeMultiplier = 0.50f; // 50% volume for meditation

 if (isPlayingResponse) {
 // Gemini is still speaking its verbal confirmation - defer audio start
 // until Gemini's turn ends (detected in the audio completion handler)
 meditationState.phaseStartTime = 0; // 0 = not yet started
 meditationState.streaming = false;
 Serial.println("Meditation queued - will start after Gemini finishes speaking");
 } else {
 // No Gemini audio playing - start immediately
 meditationState.phaseStartTime = millis();
 meditationState.streaming = false;
 ambientSound.name = "om001";
 ambientSound.active = true;
 isPlayingAmbient = true;
 isPlayingResponse = false;
 firstAudioChunk = true;
 lastAudioChunkTime = millis();
 {
 JsonDocument meditationReqDoc;
 meditationReqDoc["action"] = "requestAmbient";
 meditationReqDoc["sound"] = "om001";
 meditationReqDoc["sequence"] = ++ambientSound.sequence;
 String meditationReqMsg;
 serializeJson(meditationReqDoc, meditationReqMsg);
 Serial.printf("Meditation starting: %s (seq %d)\n", meditationReqMsg.c_str(), ambientSound.sequence);
 webSocket.sendTXT(meditationReqMsg);
 }
 meditationState.streaming = true;
 Serial.println("Meditation breathing and audio started (ROOT chakra)");
 }
 currentLEDMode = LED_MEDITATION;
 return;
 }

 // Handle lampStart - voice-commanded lamp
 if (doc["type"].is<const char*>() && doc["type"] == "lampStart") {
 const char* color = doc["color"] | "white";
 Serial.printf("lampStart: %s\n", color);

 // Stop ambient if playing
 if (isPlayingAmbient) {
 JsonDocument stopDoc;
 stopDoc["action"] = "stopAmbient";
 String stopMsg;
 serializeJson(stopDoc, stopMsg);
 webSocket.sendTXT(stopMsg);
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
 if (doc["type"].is<const char*>() && doc["type"] == "switchToIdle") {
 Serial.println("switchToIdle");

 // Stop ambient if playing
 if (isPlayingAmbient) {
 JsonDocument stopDoc;
 stopDoc["action"] = "stopAmbient";
 String stopMsg;
 serializeJson(stopDoc, stopMsg);
 webSocket.sendTXT(stopMsg);
 }
 isPlayingAmbient = false;
 isPlayingResponse = false;
 ambientSound.active = false;
 ambientSound.name = "";
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

 currentLEDMode = LED_IDLE;
 return;
 }
 
 // Handle text responses
 if (doc["type"].is<const char*>() && doc["type"] == "text") {
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
