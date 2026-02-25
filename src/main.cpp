#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>
#include <FastLED.h>
#include <driver/i2s.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <time.h>
#include "Config.h"
#include "display_mapping.h"
#include "front_text_marquee.h"
#include "SeaGooseberryVisualizer.h"
#include "EyeAnimationVisualizer.h"

// Debug logging macro - controlled by Config.h DEBUG_LOGS flag
#ifdef DEBUG_LOGS
    #define DEBUG_PRINT(...) Serial.printf(__VA_ARGS__)
    #define DEBUG_PRINTLN(...) Serial.println(__VA_ARGS__)
#else
    #define DEBUG_PRINT(...) ((void)0)
    #define DEBUG_PRINTLN(...) ((void)0)
#endif

// Chakra names array (used across multiple functions)
const char* CHAKRA_NAMES[NUM_CHAKRAS] = {
    "ROOT", "SACRAL", "SOLAR", "HEART", "THROAT", "THIRD_EYE", "CROWN"
};

// ============== GLOBAL STATE ==============
WiFiClientSecure wifiClient;
WebSocketsClient webSocket;
CRGB leds[NUM_LEDS];

// Front text marquee system
FrontTextMarquee frontMarquee;

// Sea Gooseberry visualizer
SeaGooseberryVisualizer seaGooseberry;

// Eye Animation visualizer
EyeAnimationVisualizer eyeAnimation;

bool isWebSocketConnected = false;
// volatile: written by one FreeRTOS task / ISR context, read by another on the dual-core ESP32-S3.
volatile bool recordingActive = false;
volatile bool isPlayingResponse = false;
volatile bool isPlayingAmbient = false;  // Track ambient sound playback separately
bool isPlayingAlarm = false;      // Track alarm sound playback
bool turnComplete = false;  // Track when Gemini has finished its turn
bool responseInterrupted = false;  // Flag to ignore audio after interrupt
bool waitingForGreeting = false;  // Flag to skip timeout when waiting for startup greeting
bool shutdownSoundPlayed = false;  // Flag to prevent repeated shutdown sounds during reconnection
bool firstConnection = true;  // Track if this is the first connection (cold boot)
uint32_t recordingStartTime = 0;
uint32_t lastVoiceActivityTime = 0;
uint32_t lastAudioChunkTime = 0;  // Track when we last received audio
uint32_t processingStartTime = 0;  // Track when PROCESSING mode started
uint32_t lastWebSocketSendTime = 0;  // Track last successful send
uint32_t webSocketSendFailures = 0;  // Count send failures
// NOTE: lastWiFiCheck is declared as a static local inside loop() - no global needed.
int32_t lastRSSI = 0;  // Track signal strength changes
bool firstAudioChunk = true;
volatile float volumeMultiplier = 0.25f;  // Volume control - volatile: read by audioTask, written by main/WS task
volatile int32_t currentAudioLevel = 0;  // Current audio amplitude - volatile: written by audioTask, read by ledTask
volatile float smoothedAudioLevel = 0.0f;  // Smoothed audio level - volatile: written by ledTask
volatile bool conversationMode = false;  // Track if we're in conversation window
uint32_t conversationWindowStart = 0;  // Timestamp when conversation window opened
bool conversationRecording = false;  // Track if current recording was triggered from conversation mode

// ---- I2S_NUM_0 ownership ----
// audioTask is the SOLE caller of i2s_read(I2S_NUM_0). Other tasks must NOT call it directly.
// Results are shared via these two volatile values:
volatile int32_t ambientMicRows = 0;       // Pre-computed VU row count; LED renderer reads this
volatile bool   conversationVADDetected = false;  // audioTask sets this when VAD fires during conv. window

// Audio level delay buffer for LED sync (compensates for I2S buffer latency)
// Size: ~240ms delay for optimal LED/audio sync (8 frames @ 30ms each)
// Tuning: Increase for more latency compensation, decrease for tighter sync (may see LED lag)
#define AUDIO_DELAY_BUFFER_SIZE 8
int audioLevelBuffer[AUDIO_DELAY_BUFFER_SIZE] = {0};
int audioBufferIndex = 0;

TaskHandle_t websocketTaskHandle = NULL;
TaskHandle_t ledTaskHandle = NULL;
TaskHandle_t audioTaskHandle = NULL;

// Audio processing (raw PCM - no codec needed)

// Audio buffers
QueueHandle_t audioOutputQueue;   // Queue for playback audio
// Audio queue size: 30 packets = ~1.2s buffer
// Provides good jitter tolerance with paced delivery
// Tuning: Increase for more buffer (higher latency), decrease for lower latency (more underruns)
#define AUDIO_QUEUE_SIZE 30

#include "types.h"

LEDMode currentLEDMode = LED_IDLE;  // Start directly in idle mode
LEDMode targetLEDMode = LED_IDLE;  // Mode to switch to after marquee finishes
bool ambientVUMode = false;  // Toggle for ambient sound VU meter mode

// Ambient sound type (for cycling within AMBIENT mode)
AmbientSoundType currentAmbientSoundType = SOUND_RAIN;

// Startup sound flag
bool startupSoundPlayed = false;

// Tide visualization state
TideState tideState = {"", 0.0, 0, 0, false};

// Timer visualization state
TimerState timerState = {0, 0, false};

// Moon phase visualization state
MoonState moonState = {"", 0, 0.0, 0, false};

// LED mutex for thread-safe access
SemaphoreHandle_t ledMutex = NULL;

// Ambient sound state
AmbientSound ambientSound = {"", false, 0, 0, 0};

// Pomodoro timer state
PomodoroState pomodoroState = {PomodoroState::FOCUS, 0, 25 * 60, 0, 0, false, false, 25, 5, 15, false, 0, 0};

// Meditation mode state
MeditationState meditationState = {MeditationState::ROOT, MeditationState::INHALE, 0, false, false, 1.0f};

// Clock display state
ClockState clockState = {-1, -1, 0, 0, false};

// Lamp mode state
LampState lampState = {LampState::WHITE, LampState::WHITE, 0, 0, 0, {0}, false, false, false};

// Alarm state
Alarm alarms[MAX_ALARMS];

AlarmState alarmState = {false, 0, 0, 0.0f, false, LED_IDLE, false, false};

// Day/Night brightness control
DayNightData dayNightData = {false, 0, 0, 0, true, LED_BRIGHTNESS_DAY};

// ============== FORWARD DECLARATIONS ==============
void onWebSocketEvent(WStype_t type, uint8_t * payload, size_t length);
void websocketTask(void * parameter);
void ledTask(void * parameter);
void audioTask(void * parameter);
void updateLEDs();
bool initI2SMic();
bool initI2SSpeaker();
#include "ws_handler.h"
bool detectVoiceActivity(int16_t* samples, size_t count);
void sendAudioChunk(uint8_t* data, size_t length);
void playStartupSound();
void playZenBell();
void playShutdownSound();
void playVolumeChime();
void startMarquee(String text, CRGB color, LEDMode nextMode);
void clearAudioAndLEDs();  // Helper to clear audio buffers and LEDs

// ============== MARQUEE FUNCTIONS ==============

// Start a marquee animation before switching modes
void startMarquee(String text, CRGB color, LEDMode nextMode) {
    frontMarquee.setText(text);
    frontMarquee.setColor(color);
    frontMarquee.setSpeed(4);  // 4 columns per second for smooth, readable scrolling
    frontMarquee.start();
    targetLEDMode = nextMode;
    currentLEDMode = LED_MARQUEE;
    DEBUG_PRINT("ðŸ“œ Starting marquee: '%s' -> mode %d\n", text.c_str(), nextMode);
}

// ============== HELPER FUNCTIONS ==============

// Helper function to clear audio buffers and LEDs (called during mode transitions)
void clearAudioAndLEDs() {
    // Triple clear audio buffer for clean slate
    i2s_zero_dma_buffer(I2S_NUM_1);
    delay(30);
    i2s_zero_dma_buffer(I2S_NUM_1);
    delay(30);
    i2s_zero_dma_buffer(I2S_NUM_1);
    
    // Clear LED buffer with mutex protection
    if (xSemaphoreTake(ledMutex, portMAX_DELAY) == pdTRUE) {
        fill_solid(leds, NUM_LEDS, CRGB::Black);
        FastLED.show();
        xSemaphoreGive(ledMutex);
    }
    
    delay(50);  // Let everything settle
}

// ============== DAY/NIGHT BRIGHTNESS CONTROL ==============
void updateDayNightBrightness() {
    if (!dayNightData.valid) {
        return;  // No sunrise/sunset data yet, keep default
    }
    
    // Get current time
    struct tm timeinfo;
    if (!getLocalTime(&timeinfo)) {
        return;  // Can't get time
    }
    time_t now = mktime(&timeinfo);
    
    // Check if we're between sunrise and sunset
    bool shouldBeDaytime = (now >= dayNightData.sunriseTime && now < dayNightData.sunsetTime);
    
    // Only update if state changed
    if (shouldBeDaytime != dayNightData.isDaytime) {
        dayNightData.isDaytime = shouldBeDaytime;
        dayNightData.currentBrightness = shouldBeDaytime ? LED_BRIGHTNESS_DAY : LED_BRIGHTNESS_NIGHT;
        FastLED.setBrightness(dayNightData.currentBrightness);
        
        Serial.printf("ðŸŒ… Brightness changed to %s mode (%d/255 = %.0f%%)\n",
                     shouldBeDaytime ? "DAY" : "NIGHT",
                     dayNightData.currentBrightness,
                     (dayNightData.currentBrightness / 255.0) * 100);
    }
}

void onWiFiEvent(WiFiEvent_t event, WiFiEventInfo_t info);

// ============== WIFI EVENT HANDLER ==============
void onWiFiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
    switch(event) {
        case ARDUINO_EVENT_WIFI_STA_CONNECTED:
            Serial.println("[WiFi] Connected to AP");
            break;
            
        case ARDUINO_EVENT_WIFI_STA_GOT_IP:
            Serial.printf("[WiFi] Got IP: %s\n", WiFi.localIP().toString().c_str());
            break;
            
        case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
            {
                uint8_t reason = info.wifi_sta_disconnected.reason;
                Serial.printf("[WiFi] Disconnected - Reason: %d\n", reason);
                
                // Common disconnect reasons that indicate auth/connection issues:
                // 2 = AUTH_EXPIRE, 15 = 4WAY_HANDSHAKE_TIMEOUT
                // 39 = BEACON_TIMEOUT, 202 = AUTH_FAIL
                if (reason == 2 || reason == 15 || reason == 39 || reason == 202) {
                    Serial.println("[WiFi] Auth/handshake issue detected - forcing reconnect");
                    delay(1000);  // Brief delay before retry
                }
            }
            break;
            
        case ARDUINO_EVENT_WIFI_STA_LOST_IP:
            Serial.println("[WiFi] Lost IP address");
            break;
            
        default:
            break;
    }
}

// ============== SETUP ==============
void setup() {
    Serial.begin(SERIAL_BAUD_RATE);
    delay(500);
    Serial.write("SETUP_START\r\n", 13);
    
    // Create LED mutex for thread-safe access
    ledMutex = xSemaphoreCreateMutex();
    if (ledMutex == NULL) {
        Serial.println("FATAL: Failed to create LED mutex - halting!");
        while (true) { delay(1000); }  // Hard halt - nothing is safe without the mutex
    }
    Serial.println("\n\n========================================");
    Serial.println("=== JELLYBERRY BOOT STARTING ===");
    Serial.println("========================================");
    Serial.flush();

    // Initialize LED strip (144 LEDs on GPIO 1)
    Serial.write("LED_INIT_START\r\n", 16);
    FastLED.addLeds<LED_CHIPSET, LED_DATA_PIN, LED_COLOR_ORDER>(leds, NUM_LEDS);
    FastLED.setBrightness(LED_BRIGHTNESS_DAY);  // Start with day brightness until we know otherwise
    FastLED.setDither(0);  // Disable dithering to prevent flickering
    FastLED.setMaxRefreshRate(400);  // Limit refresh rate for stability (default is 400Hz)
    FastLED.setCorrection(TypicalLEDStrip);  // Color correction for consistent output
    
    FastLED.clear();
    fill_solid(leds, NUM_LEDS, CHSV(160, 255, 100));
    FastLED.show();
    Serial.write("LED_INIT_DONE\r\n", 15);

    // Create audio queue
    Serial.println("Creating audio queue...");
    Serial.flush();
    audioOutputQueue = xQueueCreate(AUDIO_QUEUE_SIZE, sizeof(AudioChunk));
    if (!audioOutputQueue) {
        Serial.println("âœ— Failed to create audio queue");
        currentLEDMode = LED_ERROR;
        return;
    }
    Serial.println("âœ“ Audio queue created");
    Serial.flush();
    
    // Raw PCM streaming - no codec initialization needed
    Serial.println("âœ“ Audio pipeline: Raw PCM (16-bit, 16kHz mic â†’ 24kHz speaker)");

    // Initialize I2S audio
    if (!initI2SMic()) {
        Serial.println("âœ— Microphone init failed");
        currentLEDMode = LED_ERROR;
        return;
    }
    Serial.println("âœ“ Microphone initialized");

    if (!initI2SSpeaker()) {
        Serial.println("âœ— Speaker init failed");
        currentLEDMode = LED_ERROR;
        return;
    }
    Serial.println("âœ“ Speaker initialized");

    // Initialize touch pads
    pinMode(TOUCH_PAD_START_PIN, INPUT_PULLDOWN);
    pinMode(TOUCH_PAD_STOP_PIN, INPUT_PULLDOWN);
    Serial.printf("âœ“ Touch pads initialized (START=%d, STOP=%d)\n", 
                  digitalRead(TOUCH_PAD_START_PIN), 
                  digitalRead(TOUCH_PAD_STOP_PIN));

    // Connect WiFi with safeguards for cold boot issues
    Serial.println();  // Clean line break
    Serial.println("Configuring WiFi...");
    
    // Register WiFi event handler for diagnostics and recovery
    WiFi.onEvent(onWiFiEvent);
    
    // Clear any stale WiFi state from previous sessions
    WiFi.disconnect(true);  // true = erase stored credentials
    delay(500);  // Give time for disconnect to complete
    
    // Configure WiFi settings to prevent cold boot issues
    WiFi.persistent(false);  // Don't save WiFi config to flash (prevents corruption)
    WiFi.setAutoReconnect(true);  // Enable auto-reconnect on disconnect
    WiFi.setSleep(false);  // Disable WiFi power saving (improves stability)
    
    // Configure TCP keepalive to detect connection issues faster
    // This helps keep WebSocket connection alive during audio streaming
    WiFi.setTxPower(WIFI_POWER_19_5dBm);  // Max power for stability
    WiFi.mode(WIFI_STA);
    delay(100);  // Small delay for stability
    
    // Attempt WiFi connection with retry logic
    Serial.printf("Attempting WiFi connection to SSID: %s\n", WIFI_SSID);
    Serial.printf("Password length: %d characters\n", strlen(WIFI_PASSWORD));
    
    int retryCount = 0;
    const int maxRetries = 3;
    bool connected = false;
    
    while (!connected && retryCount < maxRetries) {
        if (retryCount > 0) {
            Serial.printf("\nRetry attempt %d/%d\n", retryCount + 1, maxRetries);
            WiFi.disconnect();
            delay(1000 * retryCount);  // Exponential backoff: 1s, 2s, 3s
        }
        
        WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
        Serial.print("Connecting to WiFi");
        
        int attempts = 0;
        while (WiFi.status() != WL_CONNECTED && attempts < 20) {
            delay(500);
            Serial.print(".");
            attempts++;
        }
        
        if (WiFi.status() == WL_CONNECTED) {
            connected = true;
            Serial.println("\nâœ“ WiFi connected");
            Serial.printf("IP: %s\n", WiFi.localIP().toString().c_str());
            Serial.printf("Signal: %d dBm\n", WiFi.RSSI());
            
            // Configure NTP time sync (GMT timezone)
            configTime(0, 0, "pool.ntp.org", "time.nist.gov");
            Serial.println("â° NTP time sync configured");
        } else {
            retryCount++;
            Serial.printf("\nâœ— Connection attempt failed (status: %d)\n", WiFi.status());
        }
    }
    
    if (!connected) {
        Serial.println("âœ— WiFi connection failed after all retries");
        currentLEDMode = LED_ERROR;
        return;
    }

    // Initialize WebSocket to edge server
    wifiClient.setInsecure(); // Skip certificate validation
    String wsPath = String(EDGE_SERVER_PATH) + "?device_id=" + String(DEVICE_ID);
    
    #if USE_SSL
    webSocket.beginSSL(EDGE_SERVER_HOST, EDGE_SERVER_PORT, wsPath.c_str(), "", "wss");
    Serial.printf("âœ“ WebSocket initialized to wss://%s:%d%s\n", EDGE_SERVER_HOST, EDGE_SERVER_PORT, wsPath.c_str());
    #else
    webSocket.begin(EDGE_SERVER_HOST, EDGE_SERVER_PORT, wsPath.c_str());
    Serial.printf("âœ“ WebSocket initialized to ws://%s:%d%s\n", EDGE_SERVER_HOST, EDGE_SERVER_PORT, wsPath.c_str());
    #endif
    
    webSocket.onEvent(onWebSocketEvent);
    webSocket.setReconnectInterval(WS_RECONNECT_INTERVAL);
    webSocket.enableHeartbeat(60000, 30000, 5);  // Ping every 60s, timeout 30s, 5 retries = ~210s tolerance
    Serial.println("âœ“ WebSocket initialized with relaxed keepalive");
    
    // Note: TCP buffer sizes are controlled by lwIP configuration, not runtime changeable
    Serial.println("âœ“ Using default TCP buffers (configured in sdkconfig)");

    // Start FreeRTOS tasks
    // WebSocket needs high priority (3) and larger stack for heavy audio streaming
    xTaskCreatePinnedToCore(websocketTask, "WebSocket", 16384, NULL, 3, &websocketTaskHandle, CORE_1);  // Increased from 8KB to 16KB
    xTaskCreatePinnedToCore(ledTask, "LEDs", 4096, NULL, 0, &ledTaskHandle, CORE_0);
    xTaskCreatePinnedToCore(audioTask, "Audio", 32768, NULL, 2, &audioTaskHandle, CORE_1);  // 32KB for audio buffers + processing
    Serial.println("âœ“ Tasks created on dual cores");
    
    Serial.printf("=== Initialization Complete ===  [LEDMode: IDLE]\n");
    Serial.println("Touch START pad to begin recording");
}

// ============== MAIN LOOP ==============
void loop() {
    static uint32_t lastPrint = 0;
    static uint32_t lastWiFiCheck = 0;
    static uint32_t lastAlarmCheck = 0;
    static uint32_t lastBrightnessCheck = 0;
    
    if (millis() - lastPrint > 5000) {
        // Serial.write("LOOP_TICK\r\n", 11);  // Disabled for clean overnight logging
        lastPrint = millis();
    }
    
    // Update day/night brightness every 60 seconds
    if (millis() - lastBrightnessCheck > 60000) {
        updateDayNightBrightness();
        lastBrightnessCheck = millis();
    }
    
    // Monitor WiFi signal strength every 30 seconds
    if (millis() - lastWiFiCheck > 30000) {
        int32_t rssi = WiFi.RSSI();
        if (rssi < -80) {
            Serial.printf("[WiFi] WEAK SIGNAL: %d dBm (may cause disconnects)\n", rssi);
        }
        lastWiFiCheck = millis();
    }
    
    // Check alarms every 10 seconds
    if (alarmState.active && !alarmState.ringing && millis() - lastAlarmCheck > 10000) {
        struct tm timeinfo;
        if (getLocalTime(&timeinfo)) {
            time_t now = mktime(&timeinfo);
            
            // Check each alarm
            for (int i = 0; i < MAX_ALARMS; i++) {
                if (alarms[i].enabled && !alarms[i].triggered) {
                    // Check if snoozed
                    if (alarms[i].snoozed) {
                        if (now >= alarms[i].snoozeUntil) {
                            // Snooze period ended - ring again
                            alarms[i].snoozed = false;
                            
                            // Save current state before switching to alarm
                            alarmState.previousMode = currentLEDMode;
                            alarmState.wasRecording = recordingActive;
                            alarmState.wasPlayingResponse = isPlayingResponse;
                            
                            alarms[i].triggered = true;  // Prevent re-triggering every scan cycle
                            alarmState.ringing = true;
                            alarmState.ringStartTime = millis();
                            alarmState.pulseStartTime = millis();
                            alarmState.pulseRadius = 0.0f;
                            currentLEDMode = LED_ALARM;
                            DEBUG_PRINT("â° Alarm %u ringing after snooze! (interrupted mode: %d)\n", alarms[i].alarmID, alarmState.previousMode);
                            
                            // Play alarm sound
                            isPlayingAlarm = true;
                            isPlayingResponse = true;
                            firstAudioChunk = true;
                            lastAudioChunkTime = millis();
                            JsonDocument alarmDoc;
                            alarmDoc["action"] = "requestAlarm";
                            String alarmMsg;
                            serializeJson(alarmDoc, alarmMsg);
                            DEBUG_PRINTLN("ðŸ”” Requesting alarm sound from server");
                            webSocket.sendTXT(alarmMsg);
                            
                            break;
                        }
                    } else if (now >= alarms[i].triggerTime) {
                        // Alarm time reached!
                        
                        // Save current state before switching to alarm
                        alarmState.previousMode = currentLEDMode;
                        alarmState.wasRecording = recordingActive;
                        alarmState.wasPlayingResponse = isPlayingResponse;
                        
                        alarms[i].triggered = true;  // Prevent re-triggering every 10-second scan cycle
                        alarmState.ringing = true;
                        alarmState.ringStartTime = millis();
                        alarmState.pulseStartTime = millis();
                        alarmState.pulseRadius = 0.0f;
                        currentLEDMode = LED_ALARM;
                        DEBUG_PRINT("â° Alarm %u triggered at %s (interrupted mode: %d)\n", alarms[i].alarmID, asctime(&timeinfo), alarmState.previousMode);
                        
                        // Play alarm sound
                        isPlayingAlarm = true;
                        isPlayingResponse = true;
                        firstAudioChunk = true;
                        lastAudioChunkTime = millis();
                        JsonDocument alarmDoc;
                        alarmDoc["action"] = "requestAlarm";
                        String alarmMsg;
                        serializeJson(alarmDoc, alarmMsg);
                        DEBUG_PRINTLN("ðŸ”” Requesting alarm sound from server");
                        webSocket.sendTXT(alarmMsg);
                        
                        break;
                    }
                }
            }
        }
        lastAlarmCheck = millis();
    }
    
    // Ignore touch pads for first 5 seconds after boot to avoid false triggers
    static const uint32_t bootIgnoreTime = 5000;
    
    // Poll touch pads with debouncing
    static bool startPressed = false;
    static bool stopPressed = false;
    static uint32_t lastDebounceTime = 0;
    const uint32_t debounceDelay = 10;  // 10ms - TTP223 has hardware debounce
    
    // Button 2 long-press detection
    static uint32_t button2PressStart = 0;
    const uint32_t BUTTON2_LONG_PRESS = 2000;  // 2 seconds
    
    if (millis() > bootIgnoreTime && (millis() - lastDebounceTime) > debounceDelay) {
        bool startTouch = digitalRead(TOUCH_PAD_START_PIN) == HIGH;
        bool stopTouch = digitalRead(TOUCH_PAD_STOP_PIN) == HIGH;
        
        // Detect button edges BEFORE processing handlers
        bool startRisingEdge = (startTouch && !startPressed);
        bool startFallingEdge = (!startTouch && startPressed);
        bool stopRisingEdge = (stopTouch && !stopPressed);
        bool stopFallingEdge = (!stopTouch && stopPressed);
        
        // Update button states IMMEDIATELY (before handlers can block)
        // This prevents double-triggering and state corruption
        startPressed = startTouch;
        stopPressed = stopTouch;
        
        // Detect button 2 press start
        if (stopRisingEdge) {
            button2PressStart = millis();
            Serial.println("ðŸ”˜ Button 2 pressed (start)");
        }
        
        // Button 2 long-press: Return to IDLE and start Gemini recording (from any mode)
        if (stopFallingEdge && !recordingActive && 
            (millis() - button2PressStart) >= BUTTON2_LONG_PRESS) {
            Serial.printf("ðŸ  Button 2 long-press (%lu ms): Returning to IDLE + starting recording\n",
                         millis() - button2PressStart);
            
            // Stop any active mode
            if (isPlayingAmbient) {
                JsonDocument stopDoc;
                stopDoc["action"] = "stopAmbient";
                String stopMsg;
                serializeJson(stopDoc, stopMsg);
                webSocket.sendTXT(stopMsg);
                isPlayingResponse = false;
                isPlayingAmbient = false;
                ambientSound.active = false;
                ambientSound.name = "";
                ambientSound.sequence++;
                i2s_zero_dma_buffer(I2S_NUM_1);
            }
            
            // Clear states
            moonState.active = false;
            tideState.active = false;
            timerState.active = false;
            ambientVUMode = false;
            
            // Clear Pomodoro
            if (pomodoroState.active) {
                pomodoroState.active = false;
                pomodoroState.paused = false;
                pomodoroState.startTime = 0;
                pomodoroState.pausedTime = 0;
            }
            
            // Clear Meditation
            if (meditationState.active) {
                Serial.println("ðŸ›‘ CLEARING meditation state (button 2 long press)");
                meditationState.active = false;
                meditationState.phaseStartTime = 0;
                meditationState.streaming = false;
                volumeMultiplier = meditationState.savedVolume;  // Restore volume
                Serial.printf("ðŸ”Š Volume restored to %.0f%%\n", volumeMultiplier * 100);
            }
            
            // Clear Clock
            if (clockState.active) {
                clockState.active = false;
                clockState.lastHour = -1;
                clockState.lastMinute = -1;
                clockState.scrollPosition = 0;
            }
            
            // Clear Lamp
            if (lampState.active) {
                lampState.active = false;
                lampState.fullyLit = false;
            }
            
            // Return to IDLE and start recording immediately
            currentLEDMode = LED_IDLE;
            targetLEDMode = LED_IDLE;
            
            // Start Gemini recording
            responseInterrupted = false;
            conversationRecording = false;
            recordingActive = true;
            recordingStartTime = millis();
            lastVoiceActivityTime = millis();
            currentLEDMode = LED_RECORDING;
            DEBUG_PRINTLN("ðŸŽ¤ Recording started via long-press");
            
            stopPressed = stopTouch;
            lastDebounceTime = millis();
            return;  // Skip normal button handling
        }
        
        // STOP button short press: Cycle through modes
        // IDLE â†’ AMBIENT_VU â†’ AMBIENT â†’ POMODORO â†’ MEDITATION â†’ IDLE
        // Allow during ambient modes, block only during Gemini responses (non-ambient)
        if (stopRisingEdge && !recordingActive && 
            !(isPlayingResponse && !isPlayingAmbient)) {
            // Stop any current ambient playback
            if (isPlayingAmbient) {
                isPlayingResponse = false;  // Clear playback flag
                // Clear I2S buffer to stop audio immediately
                i2s_zero_dma_buffer(I2S_NUM_1);
            }
            
            // Clear any active display states (moon, tide, timer)
            moonState.active = false;
            tideState.active = false;
            timerState.active = false;
            
            // Use targetLEDMode if marquee is active, otherwise use currentLEDMode
            LEDMode modeToCheck = (currentLEDMode == LED_MARQUEE) ? targetLEDMode : currentLEDMode;
            
            // Cycle to next mode
            if (modeToCheck == LED_IDLE || modeToCheck == LED_MOON || 
                modeToCheck == LED_TIDE || modeToCheck == LED_TIMER) {
                // Show marquee before switching
                ambientVUMode = true;
                ambientSound.sequence++;  // Increment for mode change
                startMarquee("VU MODE", CRGB::Green, LED_AMBIENT_VU);
                DEBUG_PRINTLN("ðŸŽµ Ambient VU meter mode enabled");
            } else if (modeToCheck == LED_AMBIENT_VU) {
                ambientVUMode = false;
                
                // Switch to Sea Gooseberry jellyfish mode (no audio needed)
                DEBUG_PRINTLN("ðŸ”„ VU â†’ Sea Gooseberry mode");
                seaGooseberry.begin();  // Initialize visualizer
                startMarquee("SEA JELLY", CRGB(100, 200, 255), LED_SEA_GOOSEBERRY);
            } else if (modeToCheck == LED_SEA_GOOSEBERRY) {
                // Switch to Rain ambient sound
                // CRITICAL: Flush audio queue to prevent stale VU audio from playing
                AudioChunk dummy;
                while (xQueueReceive(audioOutputQueue, &dummy, 0) == pdTRUE) {
                    // Drain all queued audio packets
                }
                
                // Clear I2S hardware buffer
                i2s_zero_dma_buffer(I2S_NUM_1);
                
                // Set drain period to discard any stale packets still in flight
                ambientSound.drainUntil = millis() + 500;  // 500ms drain window
                Serial.println("ðŸ—‘ï¸  Flushed audio queue for clean Jelly->Rain transition");
                
                currentAmbientSoundType = SOUND_RAIN;  // Start with rain
                ambientSound.name = "rain";
                ambientSound.active = true;
                ambientSound.sequence++;
                isPlayingAmbient = true;
                isPlayingResponse = false;
                firstAudioChunk = true;
                lastAudioChunkTime = millis();  // Initialize timing
                Serial.printf("ðŸŒ§ï¸  MODE: Rain (seq %d)\n", ambientSound.sequence);
                // Show marquee first
                startMarquee("RAIN", CRGB(0, 100, 255), LED_AMBIENT);  // Blue for rain
                // Audio request will be sent after marquee completes (see LED_MARQUEE case)
            } else if (modeToCheck == LED_AMBIENT) {
                // Stop ambient sound and switch to Pomodoro mode
                DEBUG_PRINTLN("ðŸ”„ Mode transition: AMBIENT â†’ POMODORO (cleaning up...)");
                
                // Clear audio buffer to prevent bleed
                i2s_zero_dma_buffer(I2S_NUM_1);
                delay(50);
                i2s_zero_dma_buffer(I2S_NUM_1);  // Double clear for safety
                
                // Clear LED buffer to remove rain/ocean effects (with mutex)
                if (xSemaphoreTake(ledMutex, portMAX_DELAY) == pdTRUE) {
                    if (currentLEDMode == LED_MEDITATION) {
                        DEBUG_PRINT("âš ï¸ INTERRUPTING meditation: mode transition clear\n");
                    }
                    fill_solid(leds, NUM_LEDS, CRGB::Black);
                    FastLED.show();
                    xSemaphoreGive(ledMutex);
                }
                delay(50);  // Let clear propagate
                
                // Always clear ambient state (not just when isPlayingAmbient)
                if (isPlayingAmbient) {
                    // Send stop request to server
                    JsonDocument stopDoc;
                    stopDoc["action"] = "stopAmbient";
                    String stopMsg;
                    serializeJson(stopDoc, stopMsg);
                    webSocket.sendTXT(stopMsg);
                }
                
                // Clear ambient state regardless (prevents reconnection from resuming)
                isPlayingResponse = false;
                isPlayingAmbient = false;
                ambientSound.active = false;
                ambientSound.name = "";
                ambientSound.sequence++;
                
                // Zero buffer again after state clear
                i2s_zero_dma_buffer(I2S_NUM_1);
                
                // Initialize Pomodoro state if not already active
                if (!pomodoroState.active) {
                    pomodoroState.currentSession = PomodoroState::FOCUS;
                    pomodoroState.sessionCount = 0;
                    pomodoroState.totalSeconds = pomodoroState.focusDuration * 60;
                    pomodoroState.startTime = 0;  // Will start after marquee
                    pomodoroState.pausedTime = 0;
                    pomodoroState.active = true;
                    pomodoroState.paused = true;  // Will auto-start after marquee
                }
                
                DEBUG_PRINTLN("ðŸ… Pomodoro mode activated (will auto-start after marquee)");
                startMarquee("POMODORO", CRGB(255, 100, 0), LED_POMODORO);  // Orange for pomodoro
            } else if (modeToCheck == LED_POMODORO) {
                // Exit Pomodoro and go to Meditation
                DEBUG_PRINTLN("â¹ï¸  Pomodoro mode stopped");
                
                // Clear Pomodoro state
                pomodoroState.active = false;
                pomodoroState.paused = false;
                pomodoroState.startTime = 0;
                pomodoroState.pausedTime = 0;
                
                // Stop any audio
                JsonDocument stopDoc;
                stopDoc["action"] = "stopAmbient";
                String stopMsg;
                serializeJson(stopDoc, stopMsg);
                webSocket.sendTXT(stopMsg);
                
                // Set 2-second drain period
                ambientSound.drainUntil = millis() + 2000;
                
                // Zero audio buffer (triple clear for clean slate)
                i2s_zero_dma_buffer(I2S_NUM_1);
                delay(30);
                i2s_zero_dma_buffer(I2S_NUM_1);
                delay(30);
                i2s_zero_dma_buffer(I2S_NUM_1);
                
                // Clear LED buffer to remove Pomodoro timer visualization (with mutex)
                if (xSemaphoreTake(ledMutex, portMAX_DELAY) == pdTRUE) {
                    if (currentLEDMode == LED_MEDITATION) {
                        DEBUG_PRINT("âš ï¸ INTERRUPTING meditation: Pomodoro mode transition (2x clear)\n");
                    }
                    fill_solid(leds, NUM_LEDS, CRGB::Black);
                    FastLED.show();
                    delay(30);
                    fill_solid(leds, NUM_LEDS, CRGB::Black);
                    FastLED.show();
                    xSemaphoreGive(ledMutex);
                }
                delay(50);  // Let everything settle
                
                // Initialize meditation state
                meditationState.currentChakra = MeditationState::ROOT;
                meditationState.phase = MeditationState::HOLD_BOTTOM;  // Start at 20% brightness
                meditationState.phaseStartTime = 0;  // Don't start breathing yet - wait for marquee
                meditationState.active = true;
                meditationState.streaming = false;
                
                // Lower volume for meditation (prevents vibration/distortion)
                meditationState.savedVolume = volumeMultiplier;
                volumeMultiplier = 0.10f;  // 10% volume for meditation
                DEBUG_PRINT("ðŸ”Š Volume: %.0f%% â†’ 10%% for meditation\n", meditationState.savedVolume * 100);
                
                Serial.println("ðŸ§˜ Meditation mode - waiting for marquee to complete");
                startMarquee("MEDITATION", CRGB(255, 0, 255), LED_MEDITATION);  // Magenta
            } else if (modeToCheck == LED_MEDITATION) {
                // Exit Meditation and go to Clock
                DEBUG_PRINTLN("â¹ï¸  Meditation mode stopped");
                
                // Clear meditation state
                meditationState.active = false;
                meditationState.phaseStartTime = 0;
                meditationState.streaming = false;
                volumeMultiplier = meditationState.savedVolume;  // Restore volume
                Serial.printf("ðŸ”Š Volume restored to %.0f%%\n", volumeMultiplier * 100);
                
                // Stop any audio
                JsonDocument stopDoc;
                stopDoc["action"] = "stopAmbient";
                String stopMsg;
                serializeJson(stopDoc, stopMsg);
                webSocket.sendTXT(stopMsg);
                
                // Clear audio and LEDs
                clearAudioAndLEDs();
                
                // Clear ambient state
                isPlayingResponse = false;
                isPlayingAmbient = false;
                ambientSound.active = false;
                ambientSound.name = "";
                ambientSound.sequence++;
                
                // Initialize clock state
                clockState.active = true;
                clockState.lastHour = -1;  // Force initial display
                clockState.lastMinute = -1;
                clockState.scrollPosition = 0;
                clockState.lastScrollUpdate = millis();
                
                DEBUG_PRINTLN("ðŸ• Clock mode activated");
                startMarquee("CLOCK", CRGB::White, LED_CLOCK);
            } else if (modeToCheck == LED_CLOCK) {
                // Exit Clock and go to Lamp
                DEBUG_PRINTLN("â¹ï¸  Clock mode stopped");
                
                // Clear clock state
                clockState.active = false;
                clockState.lastHour = -1;
                clockState.lastMinute = -1;
                clockState.scrollPosition = 0;
                
                // Initialize lamp state
                lampState.active = true;
                lampState.currentColor = LampState::WHITE;
                lampState.previousColor = LampState::WHITE;
                lampState.currentRow = 0;
                lampState.currentCol = 0;
                lampState.lastUpdate = millis();
                lampState.fullyLit = false;
                lampState.transitioning = false;
                for (int i = 0; i < NUM_LEDS; i++) {
                    lampState.ledStartTimes[i] = 0;
                }
                
                DEBUG_PRINTLN("ðŸ’¡ Lamp mode activated");
                startMarquee("LAMP", CRGB::White, LED_LAMP);
            } else if (modeToCheck == LED_LAMP) {
                // Exit Lamp and go to Sea Gooseberry mode
                DEBUG_PRINTLN("â¹ï¸  Lamp mode stopped");
                
                // Clear lamp state
                lampState.active = false;
                lampState.fullyLit = false;
                
                // Initialize Sea Gooseberry visualizer
                seaGooseberry.begin();
                
                DEBUG_PRINTLN("ðŸŒŠ Sea Gooseberry mode activated");
                startMarquee("SEA JELLY", CRGB(100, 200, 255), LED_SEA_GOOSEBERRY);
            } else if (modeToCheck == LED_SEA_GOOSEBERRY) {
                // Exit Sea Gooseberry and go to Eye Animation mode
                DEBUG_PRINTLN("â¹ï¸  Sea Gooseberry mode stopped");
                
                // Initialize Eye Animation visualizer
                eyeAnimation.begin();
                
                DEBUG_PRINTLN("ðŸ‘ï¸  Eye Animation mode activated");
                startMarquee("EYES", CRGB::White, LED_EYES);
            } else if (modeToCheck == LED_EYES) {
                // Exit Eye Animation and return to IDLE
                DEBUG_PRINTLN("â¹ï¸  Eye Animation mode stopped");
                
                DEBUG_PRINTLN("ðŸ’¤ Returning to IDLE mode");
                currentLEDMode = LED_IDLE;
                targetLEDMode = LED_IDLE;
                meditationState.phaseStartTime = 0;
                meditationState.streaming = false;
                
                // Restore original volume
                volumeMultiplier = meditationState.savedVolume;
                DEBUG_PRINT("ðŸ”Š Volume restored to %.0f%%\n", volumeMultiplier * 100);
                
                // Clear LED buffer to remove meditation breathing visualization (with mutex)
                if (xSemaphoreTake(ledMutex, portMAX_DELAY) == pdTRUE) {
                    DEBUG_PRINT("ðŸ§˜ Exiting meditation: clearing LEDs\n");
                    fill_solid(leds, NUM_LEDS, CRGB::Black);
                    FastLED.show();
                    xSemaphoreGive(ledMutex);
                }
                
                // Stop audio
                JsonDocument stopDoc;
                stopDoc["action"] = "stopAmbient";
                String stopMsg;
                serializeJson(stopDoc, stopMsg);
                webSocket.sendTXT(stopMsg);
                
                // Set 2-second drain period
                ambientSound.drainUntil = millis() + 2000;
                
                // Show "IDLE MODE" marquee
                startMarquee("IDLE MODE", CRGB(100, 100, 255), LED_IDLE);
            }
        }
        
        // VU meter mode: Button 1 disabled (button 2 advances to next mode)
        // Skip START button handling in VU mode
        if (currentLEDMode == LED_AMBIENT_VU && startRisingEdge) {
            DEBUG_PRINTLN("âš ï¸  Button 1 disabled in VU mode - use button 2 to advance");
            // Do nothing - button 1 is disabled in VU mode
        }
        // Ambient mode & Sea Gooseberry: Button 1 cycles to ambient sounds
        else {
            static uint32_t lastAmbientCycle = 0;
            if ((currentLEDMode == LED_AMBIENT || currentLEDMode == LED_SEA_GOOSEBERRY) && startRisingEdge && 
                (millis() - lastAmbientCycle) > 500) {  // 500ms debounce
            
            lastAmbientCycle = millis();
            
            // Stop current audio (if any)
            JsonDocument stopDoc;
            stopDoc["action"] = "stopAmbient";
            String stopMsg;
            serializeJson(stopDoc, stopMsg);
            webSocket.sendTXT(stopMsg);
            i2s_zero_dma_buffer(I2S_NUM_1);
            
            // If coming from Sea Gooseberry, go to Rain
            if (currentLEDMode == LED_SEA_GOOSEBERRY) {
                currentAmbientSoundType = SOUND_RAIN;
                ambientSound.name = "rain";
                Serial.printf("ðŸŒ§ï¸  MODE: Rain (seq %d)\n", ambientSound.sequence + 1);
                startMarquee("RAIN", CRGB(0, 100, 255), LED_AMBIENT);  // Blue
            }
            // Cycle to next sound in Ambient mode
            else if (currentAmbientSoundType == SOUND_RAIN) {
                currentAmbientSoundType = SOUND_OCEAN;
                ambientSound.name = "ocean";
                Serial.printf("ðŸŒŠ MODE: Ocean (seq %d)\n", ambientSound.sequence + 1);
                startMarquee("OCEAN", CRGB(0, 150, 200), LED_AMBIENT);  // Cyan
            } else if (currentAmbientSoundType == SOUND_OCEAN) {
                currentAmbientSoundType = SOUND_RAINFOREST;
                ambientSound.name = "rainforest";
                Serial.printf("ðŸŒ¿ MODE: Rainforest (seq %d)\n", ambientSound.sequence + 1);
                startMarquee("FOREST", CRGB(50, 255, 50), LED_AMBIENT);  // Green
            } else if (currentAmbientSoundType == SOUND_RAINFOREST) {
                currentAmbientSoundType = SOUND_FIRE;
                ambientSound.name = "fire";
                Serial.printf("ðŸ”¥ MODE: Fire (seq %d)\n", ambientSound.sequence + 1);
                startMarquee("FIRE", CRGB(255, 100, 0), LED_AMBIENT);  // Orange
            } else {  // SOUND_FIRE
                currentAmbientSoundType = SOUND_RAIN;
                ambientSound.name = "rain";
                Serial.printf("ðŸŒ§ï¸  MODE: Rain (seq %d)\n", ambientSound.sequence + 1);
                startMarquee("RAIN", CRGB(0, 100, 255), LED_AMBIENT);  // Blue
            }
            
            // Update state
            ambientSound.sequence++;
            isPlayingAmbient = true;
            isPlayingResponse = false;
            firstAudioChunk = true;
            lastAudioChunkTime = millis();
            
            // Request new sound from server (will be sent after marquee completes)
            }
        }
        
        // Pomodoro mode: Button 1 long press = pause/resume, short press = Gemini
        // Button 2 cycles modes as usual
        static uint32_t button1PressStart = 0;
        static uint32_t lastPomodoroAction = 0;
        const uint32_t LONG_PRESS_DURATION = 2000;  // 2 seconds
        const uint32_t ACTION_DEBOUNCE = 500;  // 500ms between actions
        
        if (currentLEDMode == LED_POMODORO && pomodoroState.active) {
            // Detect button 1 press start
            if (startRisingEdge) {
                button1PressStart = millis();
            }
            
            // On button 1 release, check duration
            if (startFallingEdge && (millis() - lastPomodoroAction) > ACTION_DEBOUNCE) {
                uint32_t pressDuration = millis() - button1PressStart;
                
                if (pressDuration >= LONG_PRESS_DURATION) {
                    // Long press: Toggle pause/resume
                    if (pomodoroState.paused) {
                        // Resume from paused state
                        // Use the totalSeconds already stored when the session started.
                        // Do NOT re-derive from focusDuration etc. â€” those may have been changed
                        // by voice command between when the session started and when it was paused.
                        int timeAlreadyElapsed = pomodoroState.totalSeconds - pomodoroState.pausedTime;
                        pomodoroState.startTime = millis() - (timeAlreadyElapsed * 1000);
                        pomodoroState.pausedTime = 0;
                        pomodoroState.paused = false;
                        
                        DEBUG_PRINT("â–¶ï¸  Pomodoro resumed from %d seconds remaining (long press)\n", pomodoroState.totalSeconds - timeAlreadyElapsed);
                        // No sound on resume - user will source alternative
                    } else {
                        // Pause and save current position
                        uint32_t elapsed = (millis() - pomodoroState.startTime) / 1000;
                        pomodoroState.pausedTime = max(0, pomodoroState.totalSeconds - (int)elapsed);
                        pomodoroState.startTime = 0;
                        pomodoroState.paused = true;
                        DEBUG_PRINT("â¸ï¸  Pomodoro paused at %d seconds remaining (long press)\n", pomodoroState.pausedTime);
                        // No sound on pause - user will source alternative
                    }
                    lastPomodoroAction = millis();
                }
                // Short press falls through to recording trigger below
            }
        }
        
        // MEDITATION MODE: Button 1 advances to next chakra (must come BEFORE interrupt/recording handlers)
        // Same pattern as ambient mode sound cycling
        bool meditationHandled = false;  // Flag to skip other handlers
        if (currentLEDMode == LED_MEDITATION && meditationState.active && startRisingEdge) {
            meditationHandled = true;  // Mark as handled to skip other button handlers
            
            Serial.printf("ðŸ§˜ Button 1: Advancing from chakra %d (%s) | edge detected\n", 
                         meditationState.currentChakra, CHAKRA_NAMES[meditationState.currentChakra]);
            
            // CRITICAL: Stop audio playback completely to prevent glitches
            isPlayingAmbient = false;  // Stop audio task from playing queued packets
            isPlayingResponse = false;
            
            // Stop current audio immediately
            JsonDocument stopDoc;
            stopDoc["action"] = "stopAmbient";
            String stopMsg;
            serializeJson(stopDoc, stopMsg);
            webSocket.sendTXT(stopMsg);
            
            // Clear I2S hardware buffer
            i2s_zero_dma_buffer(I2S_NUM_1);
            
            // Flush the software audio queue (removes buffered packets)
            AudioChunk dummy;
            while (xQueueReceive(audioOutputQueue, &dummy, 0) == pdTRUE) {
                // Drain all queued audio packets
            }
            Serial.printf("ðŸ—‘ï¸  Flushed audio queue for clean transition\n");
            
            // Set drain period to discard any stale packets still in flight
            ambientSound.drainUntil = millis() + 500;  // 500ms drain window
            
            // Check if we can advance
            if (meditationState.currentChakra < MeditationState::CROWN) {
                // Advance to next chakra (breathing continues smoothly)
                meditationState.currentChakra = (MeditationState::Chakra)(meditationState.currentChakra + 1);
                
                Serial.printf("ðŸ§˜ Advanced to chakra %d (%s) - breathing continues\n", 
                             meditationState.currentChakra, CHAKRA_NAMES[meditationState.currentChakra]);
                
                // Request new chakra sound (om001, om002, ..., om007)
                char soundName[16];
                sprintf(soundName, "om%03d", meditationState.currentChakra + 1);
                
                JsonDocument reqDoc;
                reqDoc["action"] = "requestAmbient";
                reqDoc["sound"] = soundName;
                reqDoc["sequence"] = ++ambientSound.sequence;
                String reqMsg;
                serializeJson(reqDoc, reqMsg);
                webSocket.sendTXT(reqMsg);
                
                ambientSound.name = soundName;
                ambientSound.active = true;
                isPlayingAmbient = true;  // Re-enable playback for new sound
                isPlayingResponse = false;
                firstAudioChunk = true;
                lastAudioChunkTime = millis();
                
                Serial.printf("âœ… Chakra advance complete: %s ready to stream\n", soundName);
            } else {
                // At final chakra - complete meditation
                Serial.println("ðŸ§˜ At CROWN chakra - meditation complete");
                
                // Stop audio playback completely
                isPlayingAmbient = false;
                isPlayingResponse = false;
                
                // Clear I2S hardware buffer
                i2s_zero_dma_buffer(I2S_NUM_1);
                
                // Flush software audio queue
                AudioChunk dummy;
                while (xQueueReceive(audioOutputQueue, &dummy, 0) == pdTRUE) {
                    // Drain all queued packets
                }
                
                // Clear meditation state completely
                meditationState.active = false;
                meditationState.phaseStartTime = 0;
                meditationState.streaming = false;
                meditationState.currentChakra = MeditationState::ROOT;  // Reset to start
                meditationState.phase = MeditationState::HOLD_BOTTOM;
                
                // Restore volume
                volumeMultiplier = meditationState.savedVolume;
                Serial.printf("ðŸ”Š Volume restored to %.0f%%\n", volumeMultiplier * 100);
                
                // Clear ambient audio state
                ambientSound.active = false;
                ambientSound.name = "";
                ambientSound.sequence++;
                ambientSound.drainUntil = millis() + 1000;  // Drain for 1s
                
                // Clear LEDs (with mutex)
                if (xSemaphoreTake(ledMutex, portMAX_DELAY) == pdTRUE) {
                    fill_solid(leds, NUM_LEDS, CRGB::Black);
                    FastLED.show();
                    xSemaphoreGive(ledMutex);
                }
                
                Serial.println("âœ… Meditation state fully cleared - returning to idle");
                
                // Return to idle with marquee (no sound)
                startMarquee("COMPLETE", CRGB(255, 255, 255), LED_IDLE);
            }
        }
        // Interrupt feature: START button during active playback stops audio and starts recording
        // Only interrupt if we've received audio recently (within 500ms) and turn is not complete
        // Note: Pomodoro now allowed - button 1 can interrupt responses during Pomodoro
        else if (!meditationHandled && currentLEDMode != LED_MEDITATION && startRisingEdge && isPlayingResponse && !turnComplete && 
            (millis() - lastAudioChunkTime) < 500) {
            DEBUG_PRINTLN("â¸ï¸  Interrupted response - starting new recording");
            responseInterrupted = true;  // Flag to ignore remaining audio chunks
            isPlayingResponse = false;
            i2s_zero_dma_buffer(I2S_NUM_1);  // Stop audio immediately
            
            recordingActive = true;
            recordingStartTime = millis();
            lastVoiceActivityTime = millis();
            currentLEDMode = LED_RECORDING;
            DEBUG_PRINT("ðŸŽ¤ Recording started... (START=%d, STOP=%d)\n", startTouch, stopTouch);
        }
        // ALARM MODE: Button 1 or 2 dismisses alarm
        else if (currentLEDMode == LED_ALARM && alarmState.ringing && (startRisingEdge || stopRisingEdge)) {
            DEBUG_PRINTLN("â° Alarm dismissed");
            
            // Clear alarm from memory
            for (int i = 0; i < MAX_ALARMS; i++) {
                if (alarms[i].enabled && !alarms[i].triggered) {
                    DEBUG_PRINT("âœ“ Alarm %u dismissed and cleared from slot %d\n", alarms[i].alarmID, i);
                    
                    // Zero out the alarm slot
                    alarms[i].alarmID = 0;
                    alarms[i].triggerTime = 0;
                    alarms[i].enabled = false;
                    alarms[i].triggered = false;
                    alarms[i].snoozed = false;
                    alarms[i].snoozeUntil = 0;
                    
                    break;
                }
            }
            
            // Stop ringing
            alarmState.ringing = false;
            
            // Stop alarm sound streaming from server
            isPlayingAlarm = false;
            isPlayingResponse = false;  // Stop audio playback
            JsonDocument stopDoc;
            stopDoc["action"] = "stopAlarm";
            String stopMsg;
            serializeJson(stopDoc, stopMsg);
            webSocket.sendTXT(stopMsg);
            DEBUG_PRINTLN("ðŸ”• Sent stop alarm request to server");
            
            // Stop I2S output - let buffered audio drain naturally
            i2s_zero_dma_buffer(I2S_NUM_1);
            
            // Don't clear the queue - let audio task handle cleanup naturally
            // (clearing queue while audio task is active causes crashes)
            
            // Restore previous mode
            DEBUG_PRINT("â†©ï¸  Restoring previous mode: %d (recording=%d, playing=%d)\n", 
                         alarmState.previousMode, alarmState.wasRecording, alarmState.wasPlayingResponse);
            
            currentLEDMode = alarmState.previousMode;
            
            // Restore recording state if it was active
            if (alarmState.wasRecording) {
                recordingActive = true;
                DEBUG_PRINTLN("â†©ï¸  Resuming recording");
            }
            
            // Restore playback state if it was active
            if (alarmState.wasPlayingResponse) {
                isPlayingResponse = true;
                lastAudioChunkTime = millis();  // Reset timeout
                DEBUG_PRINTLN("â†©ï¸  Resuming audio playback");
            }
            
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
            
            playVolumeChime();  // Confirmation beep
        }
        // LAMP MODE: Button 1 cycles colors (WHITE â†’ RED â†’ GREEN â†’ BLUE)
        else if (!meditationHandled && currentLEDMode == LED_LAMP && lampState.active && startRisingEdge) {
            // Cycle to next color
            lampState.previousColor = lampState.currentColor;
            lampState.currentColor = (LampState::Color)((lampState.currentColor + 1) % 4);
            
            // Reset spiral for transition effect
            lampState.currentRow = 0;
            lampState.currentCol = 0;
            lampState.lastUpdate = millis();
            lampState.fullyLit = false;
            lampState.transitioning = true;
            
            // Reset LED start times for new spiral
            for (int i = 0; i < NUM_LEDS; i++) {
                lampState.ledStartTimes[i] = 0;
            }
            
            DEBUG_PRINT("ðŸŽ¨ Lamp color: %s â†’ %s\n", 
                         (const char*[]){"WHITE", "RED", "GREEN", "BLUE"}[lampState.previousColor], 
                         (const char*[]){"WHITE", "RED", "GREEN", "BLUE"}[lampState.currentColor]);
        }
        // Start recording on rising edge (normal case - not interrupting)
        // Block if: recording active, playing response, OR in ambient sound mode, OR conversation window is open, OR in Meditation/Ambient/Lamp/VU/SeaGooseberry mode
        // Special handling for Pomodoro: only trigger on button release after short press
        else if (!meditationHandled && currentLEDMode != LED_MEDITATION && currentLEDMode != LED_AMBIENT && currentLEDMode != LED_AMBIENT_VU && currentLEDMode != LED_SEA_GOOSEBERRY && currentLEDMode != LED_LAMP && !recordingActive && !isPlayingResponse && !isPlayingAmbient && !conversationMode) {
            // Pomodoro mode: only start recording on button release after SHORT press
            bool shouldStartRecording = false;
            if (currentLEDMode == LED_POMODORO) {
                // Wait for button release
                if (startFallingEdge && (millis() - lastPomodoroAction) > ACTION_DEBOUNCE) {
                    uint32_t pressDuration = millis() - button1PressStart;
                    // Only start recording if SHORT press (long press handled above for pause/resume)
                    if (pressDuration < LONG_PRESS_DURATION) {
                        shouldStartRecording = true;
                        DEBUG_PRINTLN("ðŸŽ¤ Short press detected in Pomodoro - starting Gemini");
                    }
                }
            } else {
                // Other modes: start recording on button press as usual
                shouldStartRecording = startRisingEdge;
            }
            
            if (shouldStartRecording) {
            // Additional safety: don't start recording if alarm is ringing
            if (alarmState.ringing) {
                DEBUG_PRINTLN("âš ï¸  Cannot start recording - alarm is ringing");
                return;  // Button state already updated above
            }
            
            // Clear all previous state
            responseInterrupted = false;
            conversationRecording = false;  // Button press = normal recording timeout
            tideState.active = false;
            moonState.active = false;
            // DON'T clear timer or Pomodoro - let them run in background and return after interaction
            // timerState.active = false;
            // pomodoroState.paused = true;
            
            // CRITICAL: Cancel drain timer so Gemini responses can play
            if (ambientSound.drainUntil > 0) {
                DEBUG_PRINTLN("âœ“ Cancelled drain timer - ready for new audio");
                ambientSound.drainUntil = 0;
            }
            
            // Exit ambient VU mode
            if (ambientVUMode) {
                ambientVUMode = false;
                DEBUG_PRINTLN("ðŸŽµ Ambient VU meter mode disabled");
            }
            
            recordingActive = true;
            recordingStartTime = millis();
            lastVoiceActivityTime = millis();
            currentLEDMode = LED_RECORDING;
            DEBUG_PRINT("ðŸŽ¤ Recording started... (START=%d, STOP=%d)\n", startTouch, stopTouch);
            }  // End of shouldStartRecording block
        }
        
        // Stop recording only on timeout (manual stop removed - rely on VAD)
        if (recordingActive && (millis() - recordingStartTime) > MAX_RECORDING_DURATION_MS) {
            recordingActive = false;
            // Don't change LED mode if ambient sound is already active
            if (!ambientSound.active) {
                currentLEDMode = LED_PROCESSING;
                processingStartTime = millis();
            }
            DEBUG_PRINT("â¹ï¸  Recording stopped - Duration: %dms (max duration reached)\n", millis() - recordingStartTime);
        }
        
        lastDebounceTime = millis();
    }
    
    // Auto-stop on silence (VAD) - use longer timeout for conversation mode
    uint32_t silenceTimeout = conversationRecording ? VAD_CONVERSATION_SILENCE_MS : VAD_SILENCE_MS;
    if (recordingActive && (millis() - lastVoiceActivityTime) > silenceTimeout) {
        recordingActive = false;
        conversationRecording = false;  // Reset flag for next recording
        // Don't change LED mode if ambient sound is already active
        if (!ambientSound.active) {
            // Don't show thinking animation yet - wait to see if response is fast
            processingStartTime = millis();
            // Stay in recording mode for now
        }
        DEBUG_PRINTLN("â¹ï¸  Recording stopped - Silence detected");
    }
    
    // Show thinking animation if response is taking too long (after delay)
    if (currentLEDMode == LED_RECORDING && processingStartTime > 0 && 
        (millis() - processingStartTime) > THINKING_ANIMATION_DELAY_MS && 
        (millis() - processingStartTime) < 10000) {
        currentLEDMode = LED_PROCESSING;
        DEBUG_PRINTLN("â³ Response delayed - showing thinking animation");
    }
    
    // Timeout PROCESSING mode if no response after 10 seconds
    if ((currentLEDMode == LED_PROCESSING || currentLEDMode == LED_RECORDING) && 
        processingStartTime > 0 && (millis() - processingStartTime) > 10000) {
        DEBUG_PRINT("âš ï¸  Processing timeout after 10s - no response received (mode was %d)\n", currentLEDMode);
        processingStartTime = 0;
        
        // Return to visualizations if active, otherwise IDLE
        if (pomodoroState.active) {
            currentLEDMode = LED_POMODORO;
            DEBUG_PRINTLN("â†©ï¸  Timeout - returning to POMODORO display");
        } else if (timerState.active) {
            currentLEDMode = LED_TIMER;
            DEBUG_PRINTLN("â†©ï¸  Timeout - returning to TIMER display");
        } else if (moonState.active) {
            currentLEDMode = LED_MOON;
            moonState.displayStartTime = millis();
            DEBUG_PRINTLN("â†©ï¸  Timeout - returning to MOON display");
        } else if (tideState.active) {
            currentLEDMode = LED_TIDE;
            tideState.displayStartTime = millis();
            DEBUG_PRINTLN("â†©ï¸  Timeout - returning to TIDE display");
        } else {
            currentLEDMode = LED_IDLE;
            DEBUG_PRINTLN("â†©ï¸  Timeout - returning to IDLE");
        }
    }
    
    // Check for audio playback completion
    // Timeout only if BOTH: (1) no new packets for 2s AND (2) queue nearly drained
    // This prevents cutting off audio that's still queued but TCP-delayed
    if (isPlayingResponse && !isPlayingAmbient) {
        uint32_t queueDepth = uxQueueMessagesWaiting(audioOutputQueue);
        bool noNewPackets = (millis() - lastAudioChunkTime) > 2000;
        bool queueDrained = queueDepth < 3;  // < 120ms of audio remaining
        
        if (noNewPackets && queueDrained) {
            isPlayingResponse = false;
            DEBUG_PRINT("â¹ï¸  Audio playback complete (timeout + queue drained to %u), turnComplete=%d\n", queueDepth, turnComplete);
        
        // Check if turn is complete - if so, decide what to show
        // Skip conversation mode for startup greeting
        if (turnComplete && !waitingForGreeting) {
            // Always open conversation window first (10 second listening period)
            // Visualizations will show after conversation window closes
            conversationMode = true;
            conversationWindowStart = millis();
            currentLEDMode = LED_CONVERSATION_WINDOW;
            Serial.println("ðŸ’¬ Conversation window opened - speak anytime in next 10 seconds");
        } else {
            // Turn not complete - show visualizations or return to idle/ambient
            // Priority: Pomodoro > Timer > Moon > Tide > Ambient VU > Idle
            if (pomodoroState.active) {
                currentLEDMode = LED_POMODORO;
                DEBUG_PRINTLN("âœ“ Audio playback complete - switching to POMODORO display");
            } else if (timerState.active) {
                currentLEDMode = LED_TIMER;
                DEBUG_PRINTLN("âœ“ Audio playback complete - switching to TIMER display");
            } else if (moonState.active) {
                currentLEDMode = LED_MOON;
                moonState.displayStartTime = millis();
                DEBUG_PRINTLN("âœ“ Audio playback complete - switching to MOON display");
            } else if (tideState.active) {
                currentLEDMode = LED_TIDE;
                tideState.displayStartTime = millis();
                DEBUG_PRINT("âœ“ Audio playback complete - switching to TIDE display (state=%s, level=%.2f)\n", 
                             tideState.state.c_str(), tideState.waterLevel);
            } else if (ambientVUMode) {
                currentLEDMode = LED_AMBIENT_VU;
                DEBUG_PRINTLN("âœ“ Audio playback complete - returning to AMBIENT VU mode");
            } else {
                currentLEDMode = LED_IDLE;
                DEBUG_PRINTLN("âœ“ Audio playback complete - switching to IDLE");
            }
        }
        }
    }
    
    // Handle non-blocking Pomodoro flash animation
    if (pomodoroState.flashing && (millis() - pomodoroState.flashStartTime) >= 200) {
        pomodoroState.flashCount++;
        pomodoroState.flashStartTime = millis();
        
        if (pomodoroState.flashCount >= 6) {
            // Animation complete (3 on/off cycles = 6 state changes)
            pomodoroState.flashing = false;
        } else {
            // Toggle LED state
            if (xSemaphoreTake(ledMutex, portMAX_DELAY) == pdTRUE) {
                if (pomodoroState.flashCount % 2 == 0) {
                    fill_solid(leds, NUM_LEDS, CRGB::White);
                } else {
                    fill_solid(leds, NUM_LEDS, CRGB::Black);
                }
                FastLED.show();
                xSemaphoreGive(ledMutex);
            }
        }
    }
    
    // Update Sea Gooseberry animation (non-blocking)
    if (currentLEDMode == LED_SEA_GOOSEBERRY) {
        seaGooseberry.update(millis());
    }
    
    // Update Eye Animation (non-blocking)
    if (currentLEDMode == LED_EYES) {
        eyeAnimation.update(millis());
    }
    
    // Check for Pomodoro session completion and auto-advance
    if (currentLEDMode == LED_POMODORO && pomodoroState.active && !pomodoroState.paused && pomodoroState.startTime > 0) {
        uint32_t elapsed = (millis() - pomodoroState.startTime) / 1000;
        int secondsRemaining = pomodoroState.totalSeconds - (int)elapsed;
        
        if (secondsRemaining <= 0) {
            DEBUG_PRINTLN("â° Pomodoro session complete!");
            
            // Start non-blocking flash animation
            pomodoroState.flashing = true;
            pomodoroState.flashCount = 0;
            pomodoroState.flashStartTime = millis();
            
            // Play completion chime
            playZenBell();
            
            // Advance to next session
            if (pomodoroState.currentSession == PomodoroState::FOCUS) {
                pomodoroState.sessionCount++;
                
                if (pomodoroState.sessionCount >= 4) {
                    // After 4 focus sessions, take a long break
                    DEBUG_PRINT("ðŸ… â†’ ðŸŸ¦ Focus complete! Starting long break (%d min)\n", pomodoroState.longBreakDuration);
                    pomodoroState.currentSession = PomodoroState::LONG_BREAK;
                    pomodoroState.totalSeconds = pomodoroState.longBreakDuration * 60;
                    startMarquee("LONG BREAK", CRGB(0, 100, 255), LED_POMODORO);
                    
                    // Start immediately (don't pause)
                    pomodoroState.startTime = millis();
                    pomodoroState.pausedTime = 0;
                    pomodoroState.paused = false;
                } else {
                    // Normal short break after focus
                    DEBUG_PRINT("ðŸ… â†’ ðŸŸ© Focus complete! Starting short break (%d min) [%d/4]\n", pomodoroState.shortBreakDuration, pomodoroState.sessionCount);
                    pomodoroState.currentSession = PomodoroState::SHORT_BREAK;
                    pomodoroState.totalSeconds = pomodoroState.shortBreakDuration * 60;
                    startMarquee("SHORT BREAK", CRGB(0, 255, 0), LED_POMODORO);
                    
                    // Start immediately (don't pause)
                    pomodoroState.startTime = millis();
                    pomodoroState.pausedTime = 0;
                    pomodoroState.paused = false;
                }
            } else if (pomodoroState.currentSession == PomodoroState::LONG_BREAK) {
                // Long break complete - END OF CYCLE, return to IDLE
                DEBUG_PRINTLN("ðŸŸ¦ â†’ ðŸ›‘ Long break complete! Pomodoro cycle finished - returning to IDLE");
                pomodoroState.active = false;
                pomodoroState.currentSession = PomodoroState::FOCUS;
                pomodoroState.totalSeconds = pomodoroState.focusDuration * 60;
                pomodoroState.sessionCount = 0;  // Reset counter for next cycle
                pomodoroState.startTime = 0;
                pomodoroState.pausedTime = 0;
                pomodoroState.paused = false;
                startMarquee("COMPLETE", CRGB(255, 255, 0), LED_IDLE);  // Return to IDLE with yellow completion
            } else {
                // Short break complete, return to focus
                DEBUG_PRINT("ðŸŸ© â†’ ðŸ… Break complete! Starting focus session (%d min)\n", pomodoroState.focusDuration);
                pomodoroState.currentSession = PomodoroState::FOCUS;
                pomodoroState.totalSeconds = pomodoroState.focusDuration * 60;
                startMarquee("FOCUS TIME", CRGB(255, 0, 0), LED_POMODORO);
                
                // Start immediately (don't pause)
                pomodoroState.startTime = millis();
                pomodoroState.pausedTime = 0;
                pomodoroState.paused = false;
            }
        }
    }
    
    // Check for ambient sound streaming completion
    // When stream ends, return to IDLE mode (no looping)
    // Skip this check for meditation mode (has its own completion handler)
    if (isPlayingAmbient && ambientSound.active && !firstAudioChunk && 
        (millis() - lastAudioChunkTime) > 7000 && currentLEDMode != LED_MEDITATION) {
        Serial.printf("âœ“ Ambient sound completed: %s - returning to IDLE\n", ambientSound.name.c_str());
        
        // Return to IDLE mode
        currentLEDMode = LED_IDLE;
        isPlayingAmbient = false;
        isPlayingResponse = false;
        ambientSound.active = false;
        ambientSound.name = "";
    }
    
    // Auto-transition from visualizations to conversation window
    // After 10 seconds of showing tide/moon/timer, open conversation window if turn is complete
    if (turnComplete && !conversationMode && !isPlayingResponse && !recordingActive) {
        bool shouldOpenConversation = false;
        
        if (currentLEDMode == LED_TIDE && tideState.active) {
            if (millis() - tideState.displayStartTime > 10000) {
                Serial.println("ðŸŒŠ Tide display complete - opening conversation window");
                tideState.active = false;
                shouldOpenConversation = true;
            }
        } else if (currentLEDMode == LED_MOON && moonState.active) {
            if (millis() - moonState.displayStartTime > 10000) {
                Serial.println("ðŸŒ™ Moon display complete - opening conversation window");
                moonState.active = false;
                shouldOpenConversation = true;
            }
        }
        // Note: Timer has its own expiry logic and shouldn't auto-transition
        
        if (shouldOpenConversation) {
            Serial.printf("ðŸ”„ Transition to conversation: LED=%d, recording=%d, playing=%d, alarm=%d\n",
                         currentLEDMode, recordingActive, isPlayingResponse, alarmState.ringing);
            conversationMode = true;
            conversationWindowStart = millis();
            currentLEDMode = LED_CONVERSATION_WINDOW;
            Serial.println("ðŸ’¬ Conversation window opened - speak anytime in next 10 seconds");
        }
    }
    
    // Conversation window monitoring
    if (conversationMode && !isPlayingResponse && !recordingActive && !alarmState.ringing) {
        uint32_t elapsed = millis() - conversationWindowStart;
        
        // Debug every 2 seconds - show current state
        static uint32_t lastDebugPrint = 0;
        if (millis() - lastDebugPrint > 2000) {
            Serial.printf("ðŸ’¬ [CONV] active, window=%ums/%u, LED=%d, turnComplete=%d\n", 
                         elapsed, CONVERSATION_WINDOW_MS, currentLEDMode, turnComplete);
            lastDebugPrint = millis();
        }
        
        if (elapsed < CONVERSATION_WINDOW_MS) {
            // Voice activity is detected by audioTask (the sole I2S_NUM_0 reader).
            // It sets conversationVADDetected = true when amplitude exceeds threshold.
            // We just need to check that flag here.
            
            // Safety: don't start recording if alarm is active
            if (isPlayingAlarm) {
                return;
            }
            
            if (conversationVADDetected) {
                conversationVADDetected = false;  // Consume the flag
                
                // Voice detected - log and start recording
                Serial.printf("ðŸŽ¤ Voice detected in conversation window - avgAmp=%d, starting recording\n", (int)currentAudioLevel);
                
                // Exit conversation mode and start recording
                conversationMode = false;
                conversationRecording = true;
                recordingActive = true;
                recordingStartTime = millis();
                lastVoiceActivityTime = millis();
                processingStartTime = 0;  // CRITICAL: Reset processing timer to prevent immediate timeout
                currentLEDMode = LED_RECORDING;
                lastDebounceTime = millis();
                
                Serial.printf("âœ… Recording mode activated: LED=%d, audioLevel=%d\n", currentLEDMode, (int)currentAudioLevel);
            }
        } else {
            // Window expired with no voice - return to visualizations or idle
            Serial.println("ðŸ’¬ Conversation window expired");
            conversationMode = false;
            
            // Priority: Pomodoro > Timer > Moon > Tide > Idle
            if (pomodoroState.active) {
                currentLEDMode = LED_POMODORO;
                Serial.println("â†©ï¸  Returning to POMODORO display");
            } else if (timerState.active) {
                currentLEDMode = LED_TIMER;
                Serial.println("â†©ï¸  Returning to TIMER display");
            } else if (moonState.active) {
                currentLEDMode = LED_MOON;
                moonState.displayStartTime = millis();
                Serial.println("â†©ï¸  Returning to MOON display");
            } else if (tideState.active) {
                currentLEDMode = LED_TIDE;
                tideState.displayStartTime = millis();
                Serial.println("â†©ï¸  Returning to TIDE display");
            } else {
                currentLEDMode = LED_IDLE;
                Serial.println("â†©ï¸  Returning to IDLE");
            }
        }
    }
    
    delay(10);
}// ============== I2S INITIALIZATION ==============
bool initI2SMic() {
    i2s_config_t i2s_config = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
        .sample_rate = AUDIO_SAMPLE_RATE,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 8,
        .dma_buf_len = 512,
        .use_apll = false,
        .tx_desc_auto_clear = false,
        .fixed_mclk = 0
    };

    if (i2s_driver_install(I2S_NUM_0, &i2s_config, 0, NULL) != ESP_OK) return false;

    i2s_pin_config_t pin_config = {
        .bck_io_num = I2S_MIC_SCK_PIN,
        .ws_io_num = I2S_MIC_WS_PIN,
        .data_out_num = I2S_PIN_NO_CHANGE,
        .data_in_num = I2S_MIC_SD_PIN
    };

    return i2s_set_pin(I2S_NUM_0, &pin_config) == ESP_OK;
}

bool initI2SSpeaker() {
    i2s_config_t i2s_config = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
        .sample_rate = SPEAKER_SAMPLE_RATE,  // 24kHz for Gemini output
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 16,  // More buffers for smoother streaming
        .dma_buf_len = 1024,  // Larger buffer to match our 1KB chunks
        .use_apll = true,  // Use APLL for more accurate sample rate
        .tx_desc_auto_clear = true,
        .fixed_mclk = 0
    };

    if (i2s_driver_install(I2S_NUM_1, &i2s_config, 0, NULL) != ESP_OK) return false;

    i2s_pin_config_t pin_config = {
        .bck_io_num = I2S_SPEAKER_BCLK_PIN,
        .ws_io_num = I2S_SPEAKER_LRC_PIN,
        .data_out_num = I2S_SPEAKER_DIN_PIN,
        .data_in_num = I2S_PIN_NO_CHANGE
    };

    return i2s_set_pin(I2S_NUM_1, &pin_config) == ESP_OK;
}

// ============== VOICE ACTIVITY DETECTION ==============
bool detectVoiceActivity(int16_t* samples, size_t count) {
    int32_t sum = 0;
    for (size_t i = 0; i < count; i++) {
        sum += abs(samples[i]);
    }
    int32_t avgAmplitude = sum / count;
    
    if (avgAmplitude > VAD_THRESHOLD) {
        lastVoiceActivityTime = millis();
        return true;
    }
    return false;
}

// ============== AUDIO PROCESSING TASK ==============
void audioTask(void * parameter) {
    int16_t inputBuffer[MIC_FRAME_SIZE];  // For microphone recording (320 samples = 20ms @ 16kHz)
    static int16_t stereoBuffer[1920];  // For playback stereo conversion (960 samples * 2 channels)
    size_t bytes_read = 0;
    static uint32_t lastDebug = 0;
    AudioChunk playbackChunk;
    
    while(1) {
        bool processedAudio = false;
        
        // PRIORITY 1: Process playback queue (don't wait if empty during playback)
        while (xQueueReceive(audioOutputQueue, &playbackChunk, 0) == pdTRUE) {
            processedAudio = true;
            
            // Raw PCM from server (24kHz, 16-bit mono, little-endian)
            // Convert bytes to 16-bit samples
            int numSamples = playbackChunk.length / 2;  // 2 bytes per sample
            int16_t* pcmSamples = (int16_t*)playbackChunk.data;
            
            if (numSamples > 0 && numSamples <= 2880) {  // Max 2880 samples (stereo buffer size / 2)
                // Calculate audio level
                int32_t sum = 0;
                for (int i = 0; i < numSamples; i++) {
                    sum += abs(pcmSamples[i]);
                }
                int instantLevel = sum / numSamples;
                
                // Update LED sync buffer
                audioLevelBuffer[audioBufferIndex] = instantLevel;
                audioBufferIndex = (audioBufferIndex + 1) % AUDIO_DELAY_BUFFER_SIZE;
                currentAudioLevel = audioLevelBuffer[audioBufferIndex];
                
                // Convert mono â†’ stereo with volume
                for (int i = 0; i < numSamples; i++) {
                    int32_t sample = (int32_t)(pcmSamples[i] * volumeMultiplier);
                    if (sample > 32767) sample = 32767;
                    if (sample < -32768) sample = -32768;
                    stereoBuffer[i * 2] = (int16_t)sample;
                    stereoBuffer[i * 2 + 1] = (int16_t)sample;
                }
                
                // Write to I2S (long timeout but not infinite - 500ms should be plenty)
                size_t bytes_written;
                esp_err_t result = i2s_write(I2S_NUM_1, stereoBuffer, numSamples * 4, &bytes_written, pdMS_TO_TICKS(500));
                
                if (result != ESP_OK || bytes_written < numSamples * 4) {
                    Serial.printf("âš ï¸  I2S write failed: result=%d, wrote=%u/%u\n", result, bytes_written, numSamples*4);
                    // Continue anyway - don't get stuck
                }
                
                // Update last audio chunk time to prevent timeout while queue has data
                lastAudioChunkTime = millis();
                
                // Debug periodically
                static uint32_t lastPlaybackDebug = 0;
                if (millis() - lastPlaybackDebug > 1000) {
                    Serial.printf("[PLAYBACK] Raw PCM: %d bytes â†’ %d samples, level=%d, queue=%d\n", 
                                 playbackChunk.length, numSamples, currentAudioLevel, uxQueueMessagesWaiting(audioOutputQueue));
                    lastPlaybackDebug = millis();
                }
            } else {
                Serial.printf("âŒ Invalid PCM chunk: %d bytes (%d samples)\n", playbackChunk.length, numSamples);
            }
        }
        
        // If we processed audio, yield and check queue again immediately
        if (processedAudio) {
            taskYIELD();
            continue;
        }
        
        // PRIORITY 2: Recording (only when not playing)
        if (recordingActive && !isPlayingResponse) {
            if (i2s_read(I2S_NUM_0, inputBuffer, MIC_FRAME_SIZE * sizeof(int16_t), &bytes_read, 100) == ESP_OK) {
                if (bytes_read == MIC_FRAME_SIZE * sizeof(int16_t)) {
                    // Apply software gain
                    const int16_t GAIN = 16;
                    for (size_t i = 0; i < MIC_FRAME_SIZE; i++) {
                        int32_t amplified = (int32_t)inputBuffer[i] * GAIN;
                        if (amplified > 32767) amplified = 32767;
                        if (amplified < -32768) amplified = -32768;
                        inputBuffer[i] = (int16_t)amplified;
                    }
                    
                    // Calculate amplitude for VAD
                    int32_t sum = 0;
                    for (size_t i = 0; i < MIC_FRAME_SIZE; i++) {
                        sum += abs(inputBuffer[i]);
                    }
                    int32_t avgAmplitude = sum / MIC_FRAME_SIZE;
                    currentAudioLevel = avgAmplitude;
                    
                    // VAD check
                    bool hasVoice = detectVoiceActivity(inputBuffer, MIC_FRAME_SIZE);
                    
                    // Debug every 2 seconds
                    if (millis() - lastDebug > 2000) {
                        Serial.printf("[AUDIO] Recording: bytes_read=%d, hasVoice=%d, avgAmp=%d, threshold=%d\n", 
                                      bytes_read, hasVoice, avgAmplitude, VAD_THRESHOLD);
                        lastDebug = millis();
                    }
                    
                    // Send raw PCM
                    sendAudioChunk((uint8_t*)inputBuffer, bytes_read);
                    
                    if (hasVoice) {
                        lastVoiceActivityTime = millis();
                    }
                }
            }
        } else if (conversationMode || ambientVUMode) {
            // audioTask is the sole reader of I2S_NUM_0. When the main loop opens a
            // conversation window, or the LED task needs ambient VU levels, this branch
            // reads the mic and posts results via the two volatile shared values above.
            // Neither loop() nor ledTask call i2s_read(I2S_NUM_0) directly.
            static int16_t micBuf[MIC_FRAME_SIZE];  // 320 samples = 20ms at 16kHz
            
            // Auto-gain state for VU meter (persists across calls)
            static float vuAutoGain   = 25.0f;
            static float vuPeakRMS    = 100.0f;
            static float vuSmoothedRMS = 0.0f;
            static uint32_t lastGainAdjust = 0;
            
            size_t mic_bytes = 0;
            if (i2s_read(I2S_NUM_0, micBuf, sizeof(micBuf), &mic_bytes, 0) == ESP_OK && mic_bytes > 0) {
                size_t samples = mic_bytes / sizeof(int16_t);
                
                // Apply gain and accumulate stats in one pass
                const int16_t GAIN = 16;
                int64_t sumSq  = 0;
                int32_t sumAbs = 0;
                for (size_t i = 0; i < samples; i++) {
                    int32_t amp = (int32_t)micBuf[i] * GAIN;
                    if (amp >  32767) amp =  32767;
                    if (amp < -32768) amp = -32768;
                    micBuf[i] = (int16_t)amp;
                    sumSq  += (int64_t)amp * amp;
                    sumAbs += abs(amp);
                }
                
                // --- Conversation window VAD ---
                if (conversationMode && samples > 0) {
                    int32_t avgAmp = sumAbs / samples;
                    if (avgAmp > VAD_CONVERSATION_THRESHOLD) {
                        currentAudioLevel = avgAmp;
                        conversationVADDetected = true;  // loop() checks this flag
                    }
                }
                
                // --- Ambient VU meter ---
                if (ambientVUMode && samples > 0) {
                    float rms = sqrtf((float)sumSq / samples);
                    
                    // Peak tracking + auto-gain
                    vuPeakRMS = vuPeakRMS * 0.995f + rms * 0.005f;
                    if (rms > vuPeakRMS) vuPeakRMS = rms;
                    
                    uint32_t nowMs = millis();
                    if (nowMs - lastGainAdjust > 50) {
                        if (vuPeakRMS < 150  && vuAutoGain < 50.0f) vuAutoGain *= 1.15f;
                        else if (vuPeakRMS > 2500 && vuAutoGain >  3.0f) vuAutoGain *= 0.85f;
                        lastGainAdjust = nowMs;
                    }
                    
                    float gained = rms * vuAutoGain;
                    if (gained > 1500) gained = 1500 + (gained - 1500) * 0.3f;
                    vuSmoothedRMS = vuSmoothedRMS * 0.80f + gained * 0.20f;
                    ambientMicRows = map(constrain((int)vuSmoothedRMS, 150, 1600), 150, 1600, 0, LEDS_PER_COLUMN);
                }
            }
            vTaskDelay(pdMS_TO_TICKS(5));  // 5 ms = ~200 Hz mic poll rate (plenty for VAD)
        } else {
            // Nothing to do - sleep briefly
            vTaskDelay(pdMS_TO_TICKS(1));
        }
    }
}

// ============== AUDIO FEEDBACK ==============
void playStartupSound() {
    // Request startup sound from server
    if (!isWebSocketConnected) {
        Serial.println("âš ï¸  Cannot play startup sound - WebSocket not connected");
        return;
    }
    
    JsonDocument startupDoc;
    startupDoc["action"] = "requestStartup";
    String startupMsg;
    serializeJson(startupDoc, startupMsg);
    webSocket.sendTXT(startupMsg);
    Serial.println("ðŸ”Š Requesting startup sound from server");
}

void playZenBell() {
    // Request zen bell sound from server
    if (!isWebSocketConnected) {
        Serial.println("âš ï¸  Cannot play zen bell - WebSocket not connected");
        return;
    }
    
    JsonDocument bellDoc;
    bellDoc["action"] = "requestZenBell";
    String bellMsg;
    serializeJson(bellDoc, bellMsg);
    webSocket.sendTXT(bellMsg);
    Serial.println("ðŸ”” Requesting zen bell from server");
}

void playShutdownSound() {
    // Play a descending melody on disconnect (reverse of startup)
    const int sampleRate = 24000;
    const int noteDuration = 120;  // 120ms per note
    const int numSamples = (sampleRate * noteDuration) / 1000;
    
    // Musical notes: C6, G5, E5, C5 (descending - reverse of startup)
    const float frequencies[] = {1046.50f, 783.99f, 659.25f, 523.25f};
    const int numNotes = 4;
    
    static int16_t toneBuffer[5760];  // 120ms at 24kHz stereo (2880 samples * 2 channels)
    
    for (int note = 0; note < numNotes; note++) {
        for (int i = 0; i < numSamples; i++) {
            // Generate sine wave with fade-in/fade-out envelope
            float t = (float)i / sampleRate;
            float envelope = 1.0f;
            if (i < numSamples / 10) {
                envelope = (float)i / (numSamples / 10);  // Fade in
            } else if (i > numSamples * 9 / 10) {
                envelope = (float)(numSamples - i) / (numSamples / 10);  // Fade out
            }
            
            int16_t sample = (int16_t)(sin(2.0f * PI * frequencies[note] * t) * 6000 * envelope * volumeMultiplier);
            
            // Stereo output
            toneBuffer[i * 2] = sample;
            toneBuffer[i * 2 + 1] = sample;
        }
        
        size_t bytes_written;
        i2s_write(I2S_NUM_1, toneBuffer, numSamples * 4, &bytes_written, pdMS_TO_TICKS(100));
    }
}

void playVolumeChime() {
    // Play a brief 1kHz tone at the current volume level
    const int sampleRate = 24000;
    const int durationMs = 50;  // Very short 50ms beep (reduced from 100ms)
    const int numSamples = (sampleRate * durationMs) / 1000;
    const float frequency = 1200.0f;  // 1.2kHz tone (slightly higher pitch)
    
    static int16_t toneBuffer[2400];  // 50ms at 24kHz stereo (1200 samples * 2 channels)
    
    for (int i = 0; i < numSamples; i++) {
        // Generate sine wave
        float t = (float)i / sampleRate;
        int16_t sample = (int16_t)(sin(2.0f * PI * frequency * t) * 8000 * volumeMultiplier);
        
        // Stereo output
        toneBuffer[i * 2] = sample;
        toneBuffer[i * 2 + 1] = sample;
    }
    
    size_t bytes_written;
    i2s_write(I2S_NUM_1, toneBuffer, numSamples * 4, &bytes_written, pdMS_TO_TICKS(100));
}

// ============== WEBSOCKET HANDLERS ==============
void sendAudioChunk(uint8_t* data, size_t length) {
    static uint32_t chunkCount = 0;
    static uint32_t lastDebug = 0;
    
    if (!isWebSocketConnected) {
        if (millis() - lastDebug > 5000) {
            Serial.println("[WS] Not connected, skipping audio send");
            lastDebug = millis();
        }
        return;
    }
    
    // Check if we're sending too fast (> 100 chunks/sec = connection issues)
    static uint32_t sendRateCheck = 0;
    static uint32_t sendsSinceCheck = 0;
    sendsSinceCheck++;
    if (millis() - sendRateCheck >= 1000) {
        if (sendsSinceCheck > 100) {
            Serial.printf("[WS WARNING] High send rate: %u chunks/sec\n", sendsSinceCheck);
        }
        sendsSinceCheck = 0;
        sendRateCheck = millis();
    }
    
    chunkCount++;
    if (chunkCount % 50 == 0) {
        Serial.printf("[WS] Sent %d audio chunks\n", chunkCount);
    }
    
    // Create Live API realtimeInput message with Base64 encoded audio
    JsonDocument doc;
    
    // Base64 encode into a pre-allocated static buffer to avoid repeated heap allocations
    // (the old String += approach allocates/reallocates every character, causing heap fragmentation)
    // Max mic frame = MIC_FRAME_SIZE * 2 = 640 bytes â†’ Base64 = ceil(640/3)*4 = 856 chars + NUL
    static char encodedBuf[1024];
    const char* base64_chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t outPos = 0;
    int val = 0, valb = -6;
    for (size_t i = 0; i < length && outPos < sizeof(encodedBuf) - 5; i++) {
        val = (val << 8) + data[i];
        valb += 8;
        while (valb >= 0) {
            encodedBuf[outPos++] = base64_chars[(val >> valb) & 0x3F];
            valb -= 6;
        }
    }
    if (valb > -6) encodedBuf[outPos++] = base64_chars[((val << 8) >> (valb + 8)) & 0x3F];
    while (outPos % 4) encodedBuf[outPos++] = '=';
    encodedBuf[outPos] = '\0';
    
    // Live API format: realtimeInput.audio as Blob
    JsonObject audio = doc["realtimeInput"]["audio"].to<JsonObject>();
    audio["data"] = encodedBuf;
    audio["mimeType"] = "audio/pcm;rate=16000";
    
    String output;
    serializeJson(doc, output);
    
    // Send to WebSocket
    bool sendResult = webSocket.sendTXT(output);
    
    if (sendResult) {
        lastWebSocketSendTime = millis();
    } else {
        webSocketSendFailures++;
        Serial.printf("[WS ERROR] Send failed! Total failures: %u\n", webSocketSendFailures);
    }
}// Track WebSocket stats
static uint32_t disconnectCount = 0;
static uint32_t lastDisconnectTime = 0;

void onWebSocketEvent(WStype_t type, uint8_t * payload, size_t length) {
    switch(type) {
        case WStype_CONNECTED:
            {
                Serial.printf("âœ“ WebSocket Connected to Edge Server! (disconnect count: %d)\n", disconnectCount);
                isWebSocketConnected = true;
                shutdownSoundPlayed = false;  // Reset flag on successful connection
                currentLEDMode = LED_CONNECTED;
                
                // Edge server handles Gemini setup automatically
                Serial.println("âœ“ Waiting for 'ready' message from server");
                
                // Show connection on LEDs (with mutex)
                if (xSemaphoreTake(ledMutex, portMAX_DELAY) == pdTRUE) {
                    fill_solid(leds, NUM_LEDS, CRGB::Green);
                    FastLED.show();
                    xSemaphoreGive(ledMutex);
                }
                delay(500);
                
                // Resume ambient mode if it was active before disconnect
                if (ambientSound.active && !ambientSound.name.isEmpty()) {
                    Serial.printf("â–¶ï¸  Resuming ambient sound: %s (seq %d)\n", 
                                 ambientSound.name.c_str(), ambientSound.sequence);
                    
                    // Restore LED mode based on ambient sound
                    currentLEDMode = LED_AMBIENT;
                    
                    // Set the current ambient sound
                    if (ambientSound.name == "rain") {
                        currentAmbientSoundType = SOUND_RAIN;
                    } else if (ambientSound.name == "ocean") {
                        currentAmbientSoundType = SOUND_OCEAN;
                    } else if (ambientSound.name == "rainforest") {
                        currentAmbientSoundType = SOUND_RAINFOREST;
                    } else if (ambientSound.name == "fire") {
                        currentAmbientSoundType = SOUND_FIRE;
                    }
                    
                    // Request the ambient sound again
                    JsonDocument ambientDoc;
                    ambientDoc["action"] = "requestAmbient";
                    ambientDoc["sound"] = ambientSound.name;
                    ambientDoc["sequence"] = ambientSound.sequence;
                    String ambientMsg;
                    serializeJson(ambientDoc, ambientMsg);
                    webSocket.sendTXT(ambientMsg);
                    
                    isPlayingAmbient = true;
                    firstAudioChunk = true;
                    lastAudioChunkTime = millis();
                } else if (ambientVUMode) {
                    // Resume VU meter mode
                    currentLEDMode = LED_AMBIENT_VU;
                    Serial.println("â–¶ï¸  Resuming VU meter mode");
                } else {
                    currentLEDMode = LED_IDLE;
                }
            }
            break;
            
        case WStype_TEXT:
            Serial.printf("ðŸ“¥ Received TEXT: %d bytes: %.*s\n", length, (int)min(length, (size_t)200), (char*)payload);
            handleWebSocketMessage(payload, length);
            break;
            
        case WStype_BIN:
            {
                // ðŸ” DIAGNOSTIC: Track packet timing to detect bursting
                static uint32_t packetCount = 0;
                static uint32_t lastPacketTime = 0;
                static uint32_t fastPackets = 0;  // Packets received < 20ms apart
                static uint32_t binaryBytesReceived = 0;
                static uint32_t lastBinaryRateLog = 0;
                
                packetCount++;
                uint32_t now = millis();
                uint32_t timeSinceLastPacket = now - lastPacketTime;
                
                // Detect burst: packets arriving faster than expected (~33ms with server pacing)
                if (lastPacketTime > 0 && timeSinceLastPacket < 20) {
                    fastPackets++;
                }
                lastPacketTime = now;
                
                binaryBytesReceived += length;
                
                if (now - lastBinaryRateLog > 5000) {
                    uint32_t bytesPerSec = binaryBytesReceived / 5;
                    float avgInterval = packetCount > 1 ? 5000.0f / packetCount : 0;
                    uint32_t queueDepth = uxQueueMessagesWaiting(audioOutputQueue);
                    Serial.printf("ðŸ“Š [STREAM] %u packets, %.1fms avg interval, %u fast (<20ms), %u KB/s, queue=%u\n", 
                                 packetCount, avgInterval, fastPackets, bytesPerSec/1024, queueDepth);
                    packetCount = 0;
                    fastPackets = 0;
                    binaryBytesReceived = 0;
                    lastBinaryRateLog = now;
                }
                
                // Check for ambient magic header + sequence number FIRST
                // Magic bytes 0xA5 0x5A are very unlikely to appear in PCM audio
                bool isAmbientPacket = (length >= 4 && payload != nullptr && payload[0] == 0xA5 && payload[1] == 0x5A);
                uint16_t chunkSequence = 0;
                
                if (isAmbientPacket) {
                    // Extract sequence number
                    chunkSequence = payload[2] | (payload[3] << 8);
                    
                    // Check if this is a stale packet (old sequence number)
                    if (chunkSequence != ambientSound.sequence || !ambientSound.active) {
                        // Stale ambient chunk - discard silently during drain period
                        if (ambientSound.drainUntil > 0 && millis() < ambientSound.drainUntil) {
                            // Silent discard during drain window
                            static uint32_t drainCount = 0;
                            drainCount++;
                            break;
                        }
                        
                        // After drain period: log and discard
                        // Rate-limit logging to prevent spam (max 1 every 10 seconds)
                        static uint32_t lastDiscardLog = 0;
                        static uint32_t discardsSinceLog = 0;
                        discardsSinceLog++;
                        
                        if (millis() - lastDiscardLog > 10000) {
                            if (discardsSinceLog > 0) {
                                Serial.printf("ðŸš« Discarded %u stale ambient chunks in last 10s (seq %d, active=%d, expected=%d)\n", 
                                             discardsSinceLog, chunkSequence, ambientSound.active, ambientSound.sequence);
                            }
                            discardsSinceLog = 0;
                            lastDiscardLog = millis();
                        }
                        break;  // Discard this chunk
                    }
                    
                    // Valid ambient chunk - strip magic header + sequence
                    payload += 4;
                    length -= 4;
                    
                    // Clear drain timer when we receive first packet of new sequence
                    if (ambientSound.drainUntil > 0) {
                        Serial.printf("âœ“ New sequence %d arrived - drain complete\n", chunkSequence);
                        ambientSound.drainUntil = 0;
                    }
                }
                // else: Gemini audio (no magic header) - continue to play
                
                // Ignore audio if response was interrupted (but not for ambient/alarm sounds)
                if (responseInterrupted && !isPlayingAmbient && !isPlayingAlarm) {
                    Serial.println("ðŸš« Discarding audio chunk (response was interrupted)");
                    break;
                }
                
                // Raw PCM audio data from server (16-bit mono samples)
                // This handles BOTH Gemini responses and ambient sounds
                if (!isPlayingResponse) {
                    // CRITICAL: Stop recording immediately when response arrives (even during prebuffer)
                    // This prevents connection overload from bidirectional audio traffic
                    if (recordingActive) {
                        Serial.println("â¹ï¸  Stopping recording - response arriving");
                        recordingActive = false;
                    }
                    
                    // Wait for prebuffer before starting playback to eliminate initial stutter
                    uint32_t queueDepth = uxQueueMessagesWaiting(audioOutputQueue);
                    const uint32_t MIN_PREBUFFER = 8;  // ~320ms buffer (8 packets Ã— 40ms)
                    
                    if (queueDepth >= MIN_PREBUFFER) {
                        isPlayingResponse = true;
                        
                        // Only reset turn state for non-ambient audio
                        if (!isPlayingAmbient && !isPlayingAlarm) {
                            turnComplete = false;  // New Gemini turn starting
                        }
                        
                        recordingActive = false;  // Ensure recording is stopped
                        
                        // Show VU meter during Gemini playback, keep current mode for ambient/alarm
                        if (!ambientSound.active && !isPlayingAlarm) {
                            currentLEDMode = LED_AUDIO_REACTIVE;
                        }
                        
                        firstAudioChunk = true;
                        // NOTE: Don't clear responseInterrupted here! Only clear it on turnComplete
                        // to prevent buffered chunks from interrupted response playing through
                        // Clear all LEDs immediately when starting playback (with mutex)
                        if (xSemaphoreTake(ledMutex, portMAX_DELAY) == pdTRUE) {
                            fill_solid(leds, NUM_LEDS, CRGB::Black);
                            FastLED.show();
                            xSemaphoreGive(ledMutex);
                        }
                        // Clear delay buffer for clean LED sync
                        for (int i = 0; i < AUDIO_DELAY_BUFFER_SIZE; i++) {
                            audioLevelBuffer[i] = 0;
                        }
                        audioBufferIndex = 0;
                        
                        if (isPlayingAmbient) {
                            Serial.printf("ðŸ”Š Starting ambient audio stream: %s (prebuffered %u packets)\n", 
                                         ambientSound.name.c_str(), queueDepth);
                        } else if (isPlayingAlarm) {
                            Serial.printf("ðŸ”” Starting alarm audio playback (prebuffered %u packets)\n", queueDepth);
                        } else {
                            Serial.printf("ðŸ”Š Starting audio playback with %u packets prebuffered\n", queueDepth);
                        }
                    } else {
                        // Silent prebuffering phase - log once per stream
                        static uint32_t lastPrebufferLog = 0;
                        if (millis() - lastPrebufferLog > 1000) {
                            Serial.printf("â³ Prebuffering... (%u/%u packets)\n", queueDepth, MIN_PREBUFFER);
                            lastPrebufferLog = millis();
                        }
                    }
                }
                
                // Update last audio chunk time
                lastAudioChunkTime = millis();
                
                // Debug first chunk
                if (firstAudioChunk) {
                    Serial.print("First bytes (hex): ");
                    for (int i = 0; i < min(8, (int)length); i++) {
                        Serial.printf("%02X ", payload[i]);
                    }
                    Serial.println();
                    firstAudioChunk = false;
                }
                
                // Queue raw PCM chunk for audio task
                // Use blocking send with timeout to apply backpressure instead of dropping
                if (length <= sizeof(AudioChunk::data)) {
                    AudioChunk chunk;
                    memcpy(chunk.data, payload, length);
                    chunk.length = length;
                    
                    // ðŸ” DIAGNOSTIC: Track queue depth before send
                    uint32_t queueBefore = uxQueueMessagesWaiting(audioOutputQueue);
                    
                    // Block up to 100ms if queue is full (applies backpressure to TCP)
                    // This prevents bursting by slowing down the receive rate
                    if (xQueueSend(audioOutputQueue, &chunk, pdMS_TO_TICKS(100)) != pdTRUE) {
                        // Only drop if truly stuck (audio system frozen)
                        static uint32_t lastDropWarning = 0;
                        static uint32_t dropsince = 0;
                        dropsince++;
                        if (millis() - lastDropWarning > 2000) {
                            if (dropsince > 0) {
                                Serial.printf("âš ï¸  Blocked on queue for 100ms+ (%u times, queue=%u/%u) - audio system may be frozen\n", 
                                             dropsince, queueBefore, AUDIO_QUEUE_SIZE);
                                dropsince = 0;
                            }
                            lastDropWarning = millis();
                        }
                    } else {
                        // Queue growth logging disabled (too noisy during meditation)
                        // if (queueBefore == 0 || queueBefore >= AUDIO_QUEUE_SIZE - 5) {
                        //     Serial.printf("ðŸ“ˆ Queue: %u â†’ %u/%u\n", queueBefore, queueBefore + 1, AUDIO_QUEUE_SIZE);
                        // }
                    }
                } else {
                    Serial.printf("âŒ PCM chunk too large: %d bytes\n", length);
                }
            }
            break;
            
        case WStype_DISCONNECTED:
            disconnectCount++;
            lastDisconnectTime = millis();
            Serial.printf("âœ— WebSocket Disconnected (#%d) - isPlaying=%d, recording=%d, uptime=%lus\n",
                         disconnectCount, isPlayingResponse, recordingActive, millis()/1000);
            isWebSocketConnected = false;
            
            // Pause ambient playback but keep mode state for resume on reconnect
            if (isPlayingAmbient || ambientSound.active) {
                Serial.printf("â¸ï¸  Pausing ambient sound due to disconnect: %s (will resume)\n", ambientSound.name.c_str());
                isPlayingAmbient = false;
                isPlayingResponse = false;
                // Keep ambientSound.active and name to resume on reconnect
            }
            
            // Play shutdown sound only once per disconnect session
            if (!shutdownSoundPlayed) {
                playShutdownSound();
                shutdownSoundPlayed = true;
            }
            // Don't set error mode - this is expected during reconnection attempts
            if (currentLEDMode == LED_CONNECTED) {
                currentLEDMode = LED_IDLE;
            }
            break;
            
        case WStype_ERROR:
            Serial.println("âœ— WebSocket Error");
            currentLEDMode = LED_ERROR;
            break;
            
        default:
            break;
    }
}

// ============== LED CONTROLLER ==============
void updateLEDs() {
    static uint8_t hue = 0;
    static uint8_t brightness = 100;
    
    // Smooth the audio level with exponential moving average
    const float smoothing = 0.18f;  // Balanced response time
    smoothedAudioLevel = smoothedAudioLevel * (1.0f - smoothing) + currentAudioLevel * smoothing;
    
    // Faster decay when no audio to prevent LEDs lingering after speech ends
    if (currentAudioLevel == 0) {
        smoothedAudioLevel *= 0.60f;  // Very fast decay
        // Force to zero when low to prevent lingering
        if (smoothedAudioLevel < 20) {
            smoothedAudioLevel = 0;
        }
    }
    
    // When transitioning away from AUDIO_REACTIVE, quickly fade out any residual levels
    if (currentLEDMode != LED_AUDIO_REACTIVE && currentLEDMode != LED_RECORDING && 
        currentLEDMode != LED_AMBIENT_VU && smoothedAudioLevel > 0) {
        smoothedAudioLevel *= 0.4f;  // Very rapid fade
        if (smoothedAudioLevel < 5) {
            smoothedAudioLevel = 0;
            currentAudioLevel = 0;
        }
    }

    switch(currentLEDMode) {
        case LED_BOOT:
            // Orange pulsing during boot to distinguish from idle blue
            {
                static uint32_t lastBootDebug = 0;
                if (millis() - lastBootDebug > 1000) {
                    Serial.println("ðŸ”¶ LED_BOOT: Orange pulsing (connecting...)");
                    lastBootDebug = millis();
                }
                brightness = constrain(100 + (int)(50 * sin(millis() / 500.0)), 0, 255);
                fill_solid(leds, NUM_LEDS, CHSV(25, 255, brightness));  // Orange instead of cyan
            }
            break;
            
        case LED_IDLE:
            // Gentle blue pulse - all strips in sync, bottom to top to bottom
            // Smooth bouncing wave with even fade at both ends
            {
                // 5.9 second cycle (10% slower than 5.3s), bounces up and down
                // ORIGINAL (to revert): 5333ms for 5.3s cycle
                float t = (millis() % 5866) / 5866.0;
                
                // Create bouncing wave: centered for symmetry at both ends
                // Range -2.5â†’13.5â†’-2.5 (centered at row 5.5, perfectly symmetric)
                // ORIGINAL (to revert): Range 0â†’14â†’0, use "wavePos = (t * 2.0) * 14.0" and "wavePos = ((1.0 - t) * 2.0) * 14.0"
                float wavePos;
                if (t < 0.5) {
                    // First half: bottom to top (-2.5â†’13.5)
                    wavePos = (t * 2.0) * 16.0 - 2.5;
                } else {
                    // Second half: top to bottom (13.5â†’-2.5)
                    wavePos = ((1.0 - t) * 2.0) * 16.0 - 2.5;
                }
                
                // Debug every 2 seconds
                static uint32_t lastIdleDebug = 0;
                if (millis() - lastIdleDebug > 2000) {
                    Serial.printf("ðŸ’™ IDLE: t=%.2f, wavePos=%.2f, hue=160 (blue)\n", t, wavePos);
                    lastIdleDebug = millis();
                }
                
                // Process all strips identically (column-based indexing)
                for (int col = 0; col < LED_COLUMNS; col++) {
                    for (int row = 0; row < LEDS_PER_COLUMN; row++) {
                        int ledIndex = col * LEDS_PER_COLUMN + row;
                        
                        // Calculate distance from wave center (based on row position)
                        float distance = abs(wavePos - row);
                        
                        // Soft wave with gradient trail (6 LED spread for smoother effect)
                        if (distance < IDLE_WAVE_SPREAD) {
                            float waveBrightness = (1.0 - (distance / IDLE_WAVE_SPREAD));
                            waveBrightness = waveBrightness * waveBrightness;  // Squared for softer falloff
                            uint8_t brightness = (uint8_t)(waveBrightness * (IDLE_WAVE_BRIGHTNESS_MAX - IDLE_WAVE_BRIGHTNESS_MIN) + IDLE_WAVE_BRIGHTNESS_MIN);
                            leds[ledIndex] = CHSV(160, 200, brightness);  // Blue hue locked at 160
                        } else {
                            // Base ambient glow when wave is far away
                            leds[ledIndex] = CHSV(160, 200, IDLE_WAVE_BRIGHTNESS_MIN);  // Dim blue
                        }
                    }
                }
            }
            break;
            
        case LED_RECORDING:
            // VU meter during recording - vertical bars on all strips with fade trail
            // Traditional VU meter colors: green â†’ yellow â†’ red
            {
                // Fade all LEDs for trail effect
                for (int i = 0; i < NUM_LEDS; i++) {
                    leds[i].fadeToBlackBy(80);  // Gentle fade creates smooth trail
                }
                
                // Map audio level to row count (0-12 rows)
                // Typical range: 0-5000 (after 16x gain), map to 0-12 rows
                int numRows = map(constrain((int)smoothedAudioLevel, 0, 5000), 0, 5000, 0, LEDS_PER_COLUMN);
                
                // Light all strips identically (column-based)
                for (int col = 0; col < LED_COLUMNS; col++) {
                    for (int row = 0; row < LEDS_PER_COLUMN; row++) {
                        int ledIndex = col * LEDS_PER_COLUMN + row;
                        
                        // Bounds check to prevent array overflow
                        if (ledIndex >= NUM_LEDS) {
                            Serial.printf("âš ï¸ LED overflow: col=%d, row=%d, idx=%d\n", col, row, ledIndex);
                            continue;
                        }
                        
                        if (row < numRows) {
                            // Traditional VU meter gradient based on row height
                            // Green at bottom, yellow in middle, red at top
                            float progress = (float)row / (float)LEDS_PER_COLUMN;
                            
                            if (progress < 0.5) {
                                leds[ledIndex] = CRGB(0, 255, 0);  // Green (bottom 50% - 6 LEDs)
                            } else if (progress < 0.83) {
                                leds[ledIndex] = CRGB(255, 255, 0);  // Yellow (50-83% - 4 LEDs)
                            } else {
                                leds[ledIndex] = CRGB(255, 0, 0);  // Red (top 83-100% - 2 LEDs)
                            }
                        }
                        // LEDs above numRows keep their faded value
                    }
                }
            }
            break;
            
        case LED_PROCESSING:
            hue = (millis() / 10) % 256;
            for (int i = 0; i < NUM_LEDS; i++) {
                leds[i] = CHSV(hue + (i * 28), 255, 255);  // 28 = 256/9 for even spread
            }
            break;
            
        case LED_AMBIENT_VU:
            // Ambient sound VU meter - vertical bars on all strips with fade trail.
            // Row count is computed by audioTask (the sole I2S_NUM_0 reader) and
            // stored in the volatile ambientMicRows global - no i2s_read here.
            {
                // Fade all LEDs slightly for trail effect (instead of clearing)
                for (int i = 0; i < NUM_LEDS; i++) {
                    leds[i].fadeToBlackBy(80);  // Gentle fade creates smooth trail
                }
                
                // Row count is produced by audioTask via the full auto-gain + compression
                // + smoothing pipeline.  Just read it atomically.
                int numRows = ambientMicRows;
                
                // VU meter gradient - vertical on all strips (column-based)
                for (int col = 0; col < LED_COLUMNS; col++) {
                    for (int row = 0; row < LEDS_PER_COLUMN; row++) {
                        int ledIndex = col * LEDS_PER_COLUMN + row;
                        
                        if (row < numRows) {
                            // Gradient based on row height
                            if (row < LEDS_PER_COLUMN * 0.5) {
                                leds[ledIndex] = CRGB(0, 255, 0);  // Green (bottom 50% - 6 LEDs)
                            } else if (row < LEDS_PER_COLUMN * 0.83) {
                                leds[ledIndex] = CRGB(255, 255, 0);  // Yellow (50-83% - 4 LEDs)
                            } else {
                                leds[ledIndex] = CRGB(255, 0, 0);  // Red (top 83-100% - 2 LEDs)
                            }
                        }
                        // LEDs above numRows keep their faded value from fadeToBlackBy
                    }
                }
            }
            break;
            
        case LED_AUDIO_REACTIVE:
            // VU meter during playback - vertical bars on all strips with fade trail
            // Blue/teal/indigo gradient for Gemini speaking (AI, calm)
            {
                // Fade all LEDs for trail effect
                for (int i = 0; i < NUM_LEDS; i++) {
                    leds[i].fadeToBlackBy(80);  // Gentle fade creates smooth trail
                }
                
                // Map audio level to row count (0-12 rows)
                // Playback audio range: 0-3000, map to 0-12 rows
                int numRows = map(constrain((int)smoothedAudioLevel, 0, 3000), 0, 3000, 0, LEDS_PER_COLUMN);
                
                // Light all strips identically (column-based)
                for (int col = 0; col < LED_COLUMNS; col++) {
                    for (int row = 0; row < LEDS_PER_COLUMN; row++) {
                        int ledIndex = col * LEDS_PER_COLUMN + row;
                        
                        // Bounds check to prevent array overflow
                        if (ledIndex >= NUM_LEDS) {
                            Serial.printf("âš ï¸ LED overflow: col=%d, row=%d, idx=%d\n", col, row, ledIndex);
                            continue;
                        }
                        
                        if (row < numRows) {
                            // Blue â†’ cyan â†’ magenta gradient with distinct color zones
                            // More distinct colors for better visibility
                            float progress = (float)row / (float)LEDS_PER_COLUMN;
                            
                            if (progress < 0.5) {
                                leds[ledIndex] = CRGB(0, 100, 200);  // Deeper blue (bottom 50% - 6 LEDs)
                            } else if (progress < 0.83) {
                                leds[ledIndex] = CRGB(0, 255, 150);  // Bright cyan (50-83% - 4 LEDs)
                            } else {
                                leds[ledIndex] = CRGB(200, 0, 255);  // Bright magenta (top 83-100% - 2 LEDs)
                            }
                        }
                        // LEDs above numRows keep their faded value
                    }
                }
            }
            break;
            
        case LED_TIDE:
            // Tide visualization: water level shown as vertical bars on all strips
            // Blue = flooding (incoming), Orange = ebbing (outgoing)
            // Wave effect ripples around the circular array
            {
                // Debug: log mode switch
                static uint32_t lastDebugLog = 0;
                if (millis() - lastDebugLog > 5000) {
                    Serial.printf("ðŸŒŠ LED_TIDE active: state=%s, level=%.2f, mode=%d\n", 
                                 tideState.state.c_str(), tideState.waterLevel, currentLEDMode);
                    lastDebugLog = millis();
                }
                
                // Calculate base water level in rows (0-12)
                int baseRows = max(1, (int)(tideState.waterLevel * LEDS_PER_COLUMN));
                
                // Choose color based on tide state
                CRGB tideColor = tideState.state == "flooding" ? CRGB(0, 100, 255) : CRGB(255, 100, 0);
                
                // Create wave effect that travels around the circle
                // Each column gets a phase offset based on its position
                float time = millis() / 1000.0;
                
                for (int col = 0; col < LED_COLUMNS; col++) {
                    // Phase offset for this column (creates traveling wave around circle)
                    float phaseOffset = (float)col / (float)LED_COLUMNS * TWO_PI;
                    
                    // Wave oscillation: -2 to +2 rows
                    float wave = sin(time * 1.5 + phaseOffset) * 2.0;
                    
                    // Calculate water level for this column with wave effect
                    int waterRows = constrain(baseRows + (int)wave, 0, LEDS_PER_COLUMN);
                    
                    // Light the column from bottom up to water level
                    for (int row = 0; row < LEDS_PER_COLUMN; row++) {
                        int ledIndex = col * LEDS_PER_COLUMN + row;
                        
                        if (ledIndex >= NUM_LEDS) continue;
                        
                        if (row < waterRows) {
                            // Add subtle brightness variation for water shimmer
                            float shimmer = 0.7 + 0.3 * sin(time * 3.0 + phaseOffset * 2.0);
                            uint8_t r = (uint8_t)(tideColor.r * shimmer);
                            uint8_t g = (uint8_t)(tideColor.g * shimmer);
                            uint8_t b = (uint8_t)(tideColor.b * shimmer);
                            leds[ledIndex] = CRGB(r, g, b);
                        } else {
                            leds[ledIndex] = CRGB(0, 0, 0);
                        }
                    }
                }
                
                // No timeout - stays until next interaction
            }
            break;
            
        case LED_TIMER:
            // Timer countdown visualization
            // Show progress as LEDs fade away
            {
                if (timerState.active) {
                    uint32_t elapsed = (millis() - timerState.startTime) / 1000;
                    int remaining = timerState.totalSeconds - elapsed;
                    
                    if (remaining <= 0) {
                        // Timer finished - clear LEDs
                        fill_solid(leds, NUM_LEDS, CRGB::Black);
                    } else {
                        // Calculate how many LEDs to show based on remaining time
                        float progress = (float)remaining / (float)timerState.totalSeconds;
                        float exactLEDs = progress * NUM_LEDS;
                        int numLEDs = (int)exactLEDs;  // Full brightness LEDs
                        float fractionalPart = exactLEDs - numLEDs;  // For fade-out LED
                        
                        // Color transitions: Green -> Yellow -> Orange -> Red as time runs out
                        uint8_t hue;
                        if (progress > 0.66) {
                            hue = 96;  // Green
                        } else if (progress > 0.33) {
                            hue = 64;  // Yellow
                        } else if (progress > 0.15) {
                            hue = 32;  // Orange
                        } else {
                            hue = 0;   // Red (urgent)
                        }
                        
                        // Pulse effect when timer is low
                        uint8_t baseBrightness = 255;
                        if (progress < 0.15) {
                            baseBrightness = 128 + (uint8_t)(127 * sin(millis() / 200.0));
                        }
                        
                        for (int i = 0; i < NUM_LEDS; i++) {
                            if (i < numLEDs) {
                                // Full brightness LEDs
                                leds[i] = CHSV(hue, 255, baseBrightness);
                            } else if (i == numLEDs && fractionalPart > 0) {
                                // Fading LED - gradually dims as time runs out
                                uint8_t fadeBrightness = (uint8_t)(baseBrightness * fractionalPart);
                                leds[i] = CHSV(hue, 255, fadeBrightness);
                            } else {
                                leds[i] = CRGB::Black;
                            }
                        }
                    }
                } else {
                    // Timer not active
                    fill_solid(leds, NUM_LEDS, CRGB::Black);
                }
            }
            break;
            
        case LED_MOON:
            // Moon phase visualization - grows from center outward
            // New moon = center only, Full moon = all columns, then shrinks back
            {
                if (moonState.active) {
                    // Soft blue-white color (low saturation for pale moon glow)
                    uint8_t moonHue = 160;  // Blue-cyan
                    uint8_t moonSat = 80;   // Low saturation for pale white-blue
                    
                    // Gentle pulse effect for moon glow
                    float pulse = 0.85 + 0.15 * sin(millis() / 1500.0);
                    uint8_t baseBrightness = (uint8_t)(220 * pulse);
                    
                    // Calculate how many columns to light from center outward
                    // illumination: 0% = 1 column (center), 100% = all 12 columns
                    int numColumns = max(1, (int)((moonState.illumination / 100.0) * LED_COLUMNS));
                    
                    // Center column is column 5 or 6 (middle of 0-11)
                    int centerCol = LED_COLUMNS / 2;  // 6 for 12 columns
                    
                    // Expand outward from center
                    // e.g., 1 col = [6], 2 cols = [5,6], 3 cols = [5,6,7], etc.
                    int leftMost = centerCol - (numColumns / 2);
                    int rightMost = leftMost + numColumns - 1;
                    
                    // Clear all LEDs first
                    fill_solid(leds, NUM_LEDS, CRGB::Black);
                    
                    // Light the columns from center outward
                    for (int col = 0; col < LED_COLUMNS; col++) {
                        if (col >= leftMost && col <= rightMost) {
                            // This column should be lit - calculate brightness based on distance from center
                            int distanceFromCenter = abs(col - centerCol);
                            float brightnessFactor = 1.0 - (distanceFromCenter / (float)LED_COLUMNS * 0.3);  // Slight fade at edges
                            uint8_t colBrightness = (uint8_t)(baseBrightness * brightnessFactor);
                            
                            // Light all rows in this column
                            for (int row = 0; row < LEDS_PER_COLUMN; row++) {
                                int ledIndex = col * LEDS_PER_COLUMN + row;
                                if (ledIndex < NUM_LEDS) {
                                    leds[ledIndex] = CHSV(moonHue, moonSat, colBrightness);
                                }
                            }
                        }
                    }
                    
                    // Note: Auto-return removed - handled by auto-transition to conversation window
                } else {
                    fill_solid(leds, NUM_LEDS, CRGB::Black);
                }
            }
            break;
            
        case LED_AMBIENT:
            // Single ambient mode with different visualizations based on current sound
            {
                if (currentAmbientSoundType == SOUND_RAIN) {
                    // Falling rain effect - drops hit and run down like on a window
                    static uint32_t lastDrop = 0;
                    static float dropPosition[144] = {-1.0f};  // Vertical position of each drop (0=top, 12=bottom, -1=inactive)
                    static float dropSpeed[144] = {0.0f};  // Speed of each drop (randomized)
                    static bool rainInitialized = false;
                    
                    // Reset on first entry to rain mode
                    if (!rainInitialized) {
                        for (int i = 0; i < 144; i++) {
                            dropPosition[i] = -1.0f;
                            dropSpeed[i] = 0.0f;
                        }
                        lastDrop = millis();
                        rainInitialized = true;
                    }
                    
                    // Clear all LEDs
                    fill_solid(leds, NUM_LEDS, CRGB::Black);
                    
                    // Add new drops randomly at random strips
                    if (millis() - lastDrop > RAIN_DROP_SPAWN_INTERVAL_MS) {
                        if (random(100) < RAIN_DROP_SPAWN_CHANCE) {  // 30% chance each 100ms
                            int strip = random(LED_COLUMNS);  // Random strip
                            // Only spawn if this strip doesn't already have an active drop
                            if (dropPosition[strip] < 0.0f) {
                                dropPosition[strip] = 0.0f;  // Start at top of strip
                                dropSpeed[strip] = 0.08f + (random(0, 100) / 1000.0f);  // Random speed 0.08-0.18
                            }
                        }
                        lastDrop = millis();
                    }
                    
                    // Update and render drops running down
                    for (int strip = 0; strip < LED_COLUMNS; strip++) {
                        if (dropPosition[strip] >= 0.0f) {
                            // Drop is active - move it down at its own speed
                            dropPosition[strip] += dropSpeed[strip];
                            
                            // If reached bottom, deactivate
                            if (dropPosition[strip] >= (float)LEDS_PER_COLUMN) {
                                dropPosition[strip] = -1.0f;
                                dropSpeed[strip] = 0.0f;
                                continue;
                            }
                            
                            // Render the drop with a 2-3 LED trail
                            int currentLED = (int)dropPosition[strip];
                            
                            // Initial impact flash (first LED, very bright white-blue)
                            if (dropPosition[strip] < 0.5f && currentLED == 0) {
                                int ledIdx = strip * LEDS_PER_COLUMN + currentLED;
                                leds[ledIdx] = CRGB(200, 220, 255);  // Bright white-blue flash
                            }
                            // Main drop (brightest blue)
                            else if (currentLED < LEDS_PER_COLUMN) {
                                int ledIdx = strip * LEDS_PER_COLUMN + currentLED;
                                leds[ledIdx] = CHSV(160, 255, 255);  // Bright blue
                            }
                            
                            // Trail (fading)
                            if (currentLED > 0 && currentLED - 1 < LEDS_PER_COLUMN) {
                                int ledIdx = strip * LEDS_PER_COLUMN + (currentLED - 1);
                                leds[ledIdx] = CHSV(160, 255, 150);  // Dimmer
                            }
                            if (currentLED > 1 && currentLED - 2 < LEDS_PER_COLUMN) {
                                int ledIdx = strip * LEDS_PER_COLUMN + (currentLED - 2);
                                leds[ledIdx] = CHSV(160, 255, 80);  // Even dimmer
                            }
                        }
                    }
                } else if (currentAmbientSoundType == SOUND_OCEAN) {
                    // Swelling ocean waves - synced with actual ocean sound amplitude
                    // Wave height follows audio amplitude from playback stream
                    static float smoothedWave = 0.0f;
                    static uint32_t lastDebugLog = 0;
                    static bool oceanInitialized = false;
                    
                    // Reset on first entry to ocean mode
                    if (!oceanInitialized) {
                        smoothedWave = 0.0f;
                        oceanInitialized = true;
                    }
                    
                    // Use the actual playback audio level (calculated in audio task)
                    // currentAudioLevel is the average amplitude from PCM playback
                    float instantLevel = (float)currentAudioLevel;
                    
                    // Smooth the wave height for gentler, more fluid motion
                    smoothedWave = smoothedWave * 0.80f + instantLevel * 0.20f;  // Balanced smoothing
                    
                    // Debug log every 2 seconds
                    if (millis() - lastDebugLog > 2000) {
                        Serial.printf("ðŸŒŠ Ocean: Level=%d, Smoothed=%.0f, Rows=%d/%d\n", 
                                     currentAudioLevel, smoothedWave, 
                                     (int)(constrain(smoothedWave / 500.0f, 0.15f, 0.75f) * LEDS_PER_COLUMN), 
                                     LEDS_PER_COLUMN);
                        lastDebugLog = millis();
                    }
                    
                    // Map audio level to wave height with gentler range
                    // Cap at 75% height so waves don't constantly fill the entire display
                    float normalizedWave = constrain(smoothedWave / 500.0f, 0.15f, 0.75f);  // Gentler range (was /400 and 0.95)
                    
                    // Convert to number of rows (vertical wave on all columns)
                    int waveRows = (int)(normalizedWave * LEDS_PER_COLUMN);
                    
                    // Add MORE traveling wave phase for dramatic effect
                    float time = millis() / 3000.0;
                    
                    for (int col = 0; col < LED_COLUMNS; col++) {
                        // Phase offset for traveling wave effect
                        float phaseOffset = (float)col / (float)LED_COLUMNS * TWO_PI;
                        float phaseWave = sin(time + phaseOffset) * 3.0;  // Â±3 rows variation (was Â±1.5)
                        
                        int colWaveRows = constrain(waveRows + (int)phaseWave, 1, LEDS_PER_COLUMN);
                        
                        // Light column from bottom to wave height
                        for (int row = 0; row < LEDS_PER_COLUMN; row++) {
                            int ledIndex = col * LEDS_PER_COLUMN + row;
                            
                            if (ledIndex >= NUM_LEDS) continue;
                            
                            if (row < colWaveRows) {
                                // Gradient from deep teal (bottom) to bright aquamarine (top)
                                // Aquamarine colors: deep water (hue 170) to bright cyan-green (hue 140)
                                float progress = (float)row / (float)colWaveRows;  // 0 at bottom, 1 at wave top
                                
                                // Hue: 170 (deep cyan-blue) -> 140 (bright aquamarine/turquoise)
                                uint8_t hue = 170 - (uint8_t)(progress * 30);  // 170 -> 140
                                
                                // Saturation: fuller at bottom, slightly less at top for foam effect
                                uint8_t saturation = 255 - (uint8_t)(progress * 40);  // 255 -> 215
                                
                                // Brightness: darker at bottom (deep water), brighter at top (surface/foam)
                                uint8_t brightness = 80 + (uint8_t)(progress * 175);  // 80 -> 255
                                
                                leds[ledIndex] = CHSV(hue, saturation, brightness);
                            } else {
                                leds[ledIndex] = CRGB::Black;
                            }
                        }
                    }
                } else if (currentAmbientSoundType == SOUND_RAINFOREST) {
                    // Tropical rainforest at dusk/night with fireflies and animal eyes
                    static float fireflyPositions[6][3];  // 6 fireflies: [strip, row, brightness]
                    static uint32_t fireflyTimers[6];
                    static float eyePair[3];  // 1 pair: [strip, row, timer]
                    static bool rainforestInitialized = false;
                    
                    if (!rainforestInitialized) {
                        for (int i = 0; i < 6; i++) {
                            fireflyPositions[i][0] = -1.0f;  // inactive
                            fireflyTimers[i] = 0;
                        }
                        eyePair[0] = -1.0f;  // inactive
                        rainforestInitialized = true;
                    }
                    
                    uint32_t now = millis();
                    
                    // Update fireflies
                    for (int i = 0; i < 6; i++) {
                        if (fireflyPositions[i][0] < 0.0f) {
                            // Spawn new firefly (3% chance per frame)
                            if (random(100) < 3) {
                                fireflyPositions[i][0] = random(0, 12);  // strip
                                fireflyPositions[i][1] = random(3, 10);  // row (mid-height)
                                fireflyPositions[i][2] = 1.0f;  // full brightness
                                fireflyTimers[i] = now + 2000 + random(0, 1000);  // 2-3s lifespan
                            }
                        } else {
                            // Fade and drift
                            fireflyPositions[i][2] -= 0.008f;  // fade slowly
                            fireflyPositions[i][1] += random(-1, 2) * 0.05f;  // subtle drift
                            
                            if (now > fireflyTimers[i] || fireflyPositions[i][2] <= 0.0f) {
                                fireflyPositions[i][0] = -1.0f;  // deactivate
                            }
                        }
                    }
                    
                    // Update single animal eye pair
                    if (eyePair[0] < 0.0f) {
                        // Spawn new pair (0.5% chance per frame - rare)
                        if (random(1000) < 5) {
                            int strip = random(0, 9);  // leave room for pair
                            eyePair[0] = strip;  // first eye strip
                            eyePair[1] = random(5, 8);  // row (mid-height)
                            eyePair[2] = now + 3000 + random(0, 2000);  // 3-5s duration (longer)
                        }
                    } else {
                        if (now > (uint32_t)eyePair[2]) {
                            eyePair[0] = -1.0f;  // deactivate
                        }
                    }
                    
                    // Render base canopy (dark emerald green gradient)
                    for (int strip = 0; strip < 12; strip++) {
                        // Each strip breathes at slightly different rate
                        float stripPhase = (strip * 0.2f);
                        float pulse = 0.7 + 0.3 * sin((now / 5000.0) + stripPhase);  // 5s cycle
                        
                        for (int row = 0; row < 12; row++) {
                            int ledIdx = strip * 12 + row;
                            
                            // Vertical gradient: dark forest floor to lighter canopy
                            float verticalProgress = (float)row / 11.0f;
                            uint8_t hue = 85 + (uint8_t)(verticalProgress * 15.0f);  // 85-100 (emerald to green)
                            uint8_t sat = 255 - (uint8_t)(verticalProgress * 40.0f);  // 255-215
                            uint8_t brightness = 60 + (uint8_t)(verticalProgress * 80.0f * pulse);  // 60-140 (dark)
                            
                            leds[ledIdx] = CHSV(hue, sat, brightness);
                        }
                    }
                    
                    // Render fireflies
                    for (int i = 0; i < 6; i++) {
                        if (fireflyPositions[i][0] >= 0.0f) {
                            int strip = (int)fireflyPositions[i][0];
                            int row = (int)fireflyPositions[i][1];
                            if (row >= 0 && row < 12 && strip >= 0 && strip < 12) {
                                int ledIdx = strip * 12 + row;
                                uint8_t brightness = (uint8_t)(255 * fireflyPositions[i][2]);
                                leds[ledIdx] = CHSV(70, 200, brightness);  // Yellow-green
                            }
                        }
                    }
                    
                    // Render animal eye pair (2 LEDs each for visibility)
                    if (eyePair[0] >= 0.0f) {
                        int strip1 = (int)eyePair[0];
                        int strip2 = strip1 + 3;  // 3 strips apart for clear separation
                        int row = (int)eyePair[1];
                        
                        if (strip1 < 12 && strip2 < 12 && row >= 0 && row < 11) {
                            // Blink effect
                            uint32_t duration = (uint32_t)eyePair[2] - now;
                            uint32_t totalDuration = 4000;  // ~4s average
                            uint32_t age = totalDuration - duration;
                            
                            uint8_t brightness = 255;
                            // Blink twice during display
                            if ((age > 1000 && age < 1150) || (age > 2500 && age < 2650)) {
                                brightness = 30;  // blink
                            }
                            
                            // Each eye is 2 LEDs vertically
                            leds[strip1 * 12 + row] = CHSV(30, 220, brightness);  // Amber
                            leds[strip1 * 12 + row + 1] = CHSV(30, 220, brightness);  // Amber
                            leds[strip2 * 12 + row] = CHSV(30, 220, brightness);  // Amber
                            leds[strip2 * 12 + row + 1] = CHSV(30, 220, brightness);  // Amber
                        }
                    }
                }
            }
            
            // Fire mode visualization (after rainforest check)
            if (currentAmbientSoundType == SOUND_FIRE) {
                // Rising/falling flames with sparks
                static float flameHeights[12];  // Per-strip flame height (0.0-1.0)
                static float flamePhases[12];   // Per-strip phase offset
                static float sparkPositions[12]; // Per-strip spark position (-1 = none)
                static float sparkBrightness[12]; // Per-strip spark brightness
                static bool fireInitialized = false;
                
                // Initialize on first run
                if (!fireInitialized) {
                    for (int s = 0; s < 12; s++) {
                        flameHeights[s] = 0.3f + (random(0, 300) / 1000.0f);  // 0.3-0.6
                        flamePhases[s] = random(0, 1000) / 1000.0f;  // 0.0-1.0
                        sparkPositions[s] = -1.0f;  // No spark
                        sparkBrightness[s] = 0.0f;
                    }
                    fireInitialized = true;
                }
                
                // Update flame heights (organic sine wave motion)
                float globalTime = millis() / 1000.0f;
                for (int s = 0; s < 12; s++) {
                    // Each strip has its own frequency and phase (slower for relaxation)
                    float frequency = 0.3f + (s * 0.03f);  // 0.3-0.63 Hz variation (was 0.5-1.1)
                    float targetHeight = 0.35f + 0.12f * sin((globalTime * frequency) + flamePhases[s]);  // 0.23-0.47 (reduced amplitude)
                    
                    // Smooth approach to target (slower for gentler motion)
                    flameHeights[s] += (targetHeight - flameHeights[s]) * 0.05f;
                    
                    // Spawn new sparks occasionally (2% chance per frame from flame tip, reduced from 5%)
                    if (sparkPositions[s] < 0.0f && random(100) < 2 && flameHeights[s] > 0.3f) {
                        sparkPositions[s] = flameHeights[s] * 12.0f;  // Start at flame tip
                        sparkBrightness[s] = 1.0f;
                    }
                    
                    // Update existing sparks (float upward and fade)
                    if (sparkPositions[s] >= 0.0f) {
                        sparkPositions[s] += 0.18f + (random(0, 70) / 1000.0f);  // 0.18-0.25 LEDs/frame
                        sparkBrightness[s] -= 0.12f;  // Fade over ~8 frames
                        
                        // Remove if reached top or fully faded
                        if (sparkPositions[s] >= 12.0f || sparkBrightness[s] <= 0.0f) {
                            sparkPositions[s] = -1.0f;
                            sparkBrightness[s] = 0.0f;
                        }
                    }
                }
                
                // Clear all LEDs first
                fill_solid(leds, NUM_LEDS, CRGB::Black);
                
                // Render flames for each strip
                for (int strip = 0; strip < 12; strip++) {
                    int maxFlameRow = (int)(flameHeights[strip] * 12.0f);
                    maxFlameRow = constrain(maxFlameRow, 0, 6);  // Cap at row 6 (max flame height)
                    
                    for (int row = 0; row < 12; row++) {
                        // LED index: strip * 12 + row (all wired bottom-up)
                        int ledIdx = strip * 12 + row;
                        if (ledIdx < 0 || ledIdx >= NUM_LEDS) continue;
                        
                        // Check if spark is at this position
                        bool isSpark = false;
                        if (sparkPositions[strip] >= 0.0f) {
                            int sparkRow = (int)sparkPositions[strip];
                            if (row == sparkRow && sparkBrightness[strip] > 0.0f) {
                                isSpark = true;
                                // Bright yellow-orange spark
                                uint8_t sparkHue = 25 + random(0, 10);  // 25-35 (yellow-orange)
                                uint8_t sparkBr = (uint8_t)(255 * sparkBrightness[strip]);
                                leds[ledIdx] = CHSV(sparkHue, 220, sparkBr);
                            }
                        }
                        
                        // Render flame if below max height and not spark
                        if (!isSpark && row <= maxFlameRow) {
                            // Progress from bottom (0.0) to tip (1.0)
                            float progress = (float)row / (float)maxFlameRow;
                            
                            // Color gradient: deep red â†’ orange â†’ yellow-orange
                            uint8_t hue;
                            if (progress < 0.4f) {
                                // Bottom 40%: Deep red (0-5)
                                hue = 0 + (uint8_t)(progress * 2.5f * 5.0f);
                            } else if (progress < 0.7f) {
                                // Mid 30%: Red to orange (5-15)
                                hue = 5 + (uint8_t)((progress - 0.4f) * 3.33f * 10.0f);
                            } else {
                                // Top 30%: Orange to yellow-orange (15-25)
                                hue = 15 + (uint8_t)((progress - 0.7f) * 3.33f * 10.0f);
                            }
                            
                            // Subtle hue variation (Â±1 for gentle organic feel)
                            hue += random(-1, 2);
                            
                            // Brightness gradient: dimmer at base, brighter at tips
                            uint8_t brightness;
                            if (progress < 0.5f) {
                                brightness = 150 + (uint8_t)(progress * 2.0f * 50.0f);  // 150-200
                            } else {
                                brightness = 200 + (uint8_t)((progress - 0.5f) * 2.0f * 55.0f);  // 200-255
                            }
                            
                            // Gentle brightness variation (Â±5 for subtle glow)
                            brightness += random(-5, 6);
                            brightness = constrain(brightness, 100, 255);
                            
                            leds[ledIdx] = CHSV(hue, 255, brightness);
                        }
                    }
                }
            }  // End of fire mode
            
            break;  // End of LED_AMBIENT case
            
        case LED_POMODORO:
            // Pomodoro timer visualization with hybrid countdown/countup
            // Focus (red): countdown from full
            // Breaks (green/blue): count-up to full
            {
                if (pomodoroState.active) {
                    // Calculate time remaining or elapsed
                    int secondsRemaining;
                    if (pomodoroState.paused) {
                        // When paused, use saved time (or full duration if just started)
                        secondsRemaining = pomodoroState.pausedTime > 0 ? pomodoroState.pausedTime : pomodoroState.totalSeconds;
                    } else if (pomodoroState.startTime > 0) {
                        uint32_t elapsed = (millis() - pomodoroState.startTime) / 1000;
                        secondsRemaining = max(0, pomodoroState.totalSeconds - (int)elapsed);
                    } else {
                        secondsRemaining = pomodoroState.totalSeconds;
                    }
                    
                    // Calculate progress (0.0 to 1.0)
                    float progress = 1.0 - ((float)secondsRemaining / (float)pomodoroState.totalSeconds);
                    
                    // Determine if this is a break session
                    bool isBreakSession = (pomodoroState.currentSession == PomodoroState::SHORT_BREAK || 
                                          pomodoroState.currentSession == PomodoroState::LONG_BREAK);
                    
                    // Choose color based on session type
                    CRGB sessionColor;
                    if (pomodoroState.currentSession == PomodoroState::FOCUS) {
                        sessionColor = CRGB(255, 0, 0);  // Red for focus
                    } else if (pomodoroState.currentSession == PomodoroState::SHORT_BREAK) {
                        sessionColor = CRGB(0, 255, 0);  // Green for short break
                    } else {
                        sessionColor = CRGB(0, 100, 255);  // Blue for long break
                    }
                    
                    // Calculate number of rows to light
                    int litRows;
                    if (isBreakSession) {
                        // Breaks: count UP (filling = recharging energy)
                        litRows = (int)ceil(progress * LEDS_PER_COLUMN);
                    } else {
                        // Focus: count DOWN (draining = time running out)
                        litRows = (int)ceil((1.0 - progress) * LEDS_PER_COLUMN);
                    }
                    litRows = constrain(litRows, 0, LEDS_PER_COLUMN);
                    
                    // Pulse effect for active LED (slow breathing) - smooth sine wave
                    float activePulse = 1.0;
                    if (pomodoroState.paused) {
                        // When paused, ALL LEDs breathe together
                        float breathe = sin(millis() / 3000.0 * PI);
                        activePulse = 0.30 + 0.70 * ((breathe + 1.0) / 2.0);
                    } else {
                        // When running, only active LED breathes (brighter for diffuser)
                        float breathe = sin(millis() / 2000.0 * PI);  // Faster 4-second cycle
                        activePulse = 0.70 + 0.30 * ((breathe + 1.0) / 2.0);  // 70-100% range
                    }
                    
                    // Calculate which LED is currently "active" (the moving indicator)
                    // Use floor for discrete LED positions
                    int activeLED;
                    
                    if (isBreakSession) {
                        // Countup: active LED moves from bottom (0) to top (11)
                        activeLED = constrain((int)(progress * LEDS_PER_COLUMN), 0, LEDS_PER_COLUMN - 1);
                    } else {
                        // Countdown: active LED moves from top (11) to bottom (0)
                        // As progress goes 0â†’1, active LED goes 11â†’0
                        activeLED = constrain(LEDS_PER_COLUMN - 1 - (int)(progress * LEDS_PER_COLUMN), 0, LEDS_PER_COLUMN - 1);
                    }
                    
                    // Debug logging (every 5 seconds)
                    static uint32_t lastPomodoroDebug = 0;
                    if (millis() - lastPomodoroDebug > 5000) {
                        Serial.printf("ðŸ… Progress: %.1f%%, Active LED row: %d, Pulse: %.2f, Paused: %d, Remaining: %ds\n",
                                     progress * 100, activeLED, activePulse, pomodoroState.paused, secondsRemaining);
                        lastPomodoroDebug = millis();
                    }
                    
                    // Light all columns identically (vertical bars)
                    for (int col = 0; col < LED_COLUMNS; col++) {
                        for (int row = 0; row < LEDS_PER_COLUMN; row++) {
                            int ledIndex = col * LEDS_PER_COLUMN + row;
                            if (ledIndex >= NUM_LEDS) continue;
                            
                            // Determine if this LED should be lit
                            // LEDs stay lit from bottom (0) up to and including the active LED
                            bool shouldBeLit = (row <= activeLED);
                            
                            if (!shouldBeLit) {
                                leds[ledIndex] = CRGB::Black;
                                continue;
                            }
                            
                            // All lit LEDs get brightness treatment
                            float ledBrightness;
                            
                            if (pomodoroState.paused) {
                                // When paused: all LEDs breathe together at same brightness
                                ledBrightness = activePulse;
                            } else {
                                // When running: active LED breathes, others stay at 10%
                                if (row == activeLED) {
                                    ledBrightness = activePulse;  // 70-100% breathing
                                } else {
                                    ledBrightness = 0.10;  // 10% for inactive LEDs (visible background)
                                }
                            }
                            
                            // Apply brightness to session color
                            uint8_t r = (uint8_t)(sessionColor.r * ledBrightness);
                            uint8_t g = (uint8_t)(sessionColor.g * ledBrightness);
                            uint8_t b = (uint8_t)(sessionColor.b * ledBrightness);
                            leds[ledIndex] = CRGB(r, g, b);
                        }
                    }
                } else {
                    fill_solid(leds, NUM_LEDS, CRGB::Black);
                }
            }
            break;
            
        case LED_MEDITATION:
            // Meditation breathing visualization with box breathing (4-4-4-4)
            // Smooth bouncing indicator shows breath position
            // NOTE: Uses raw FastLED, not NeoMatrix, to avoid interference
            {
                if (meditationState.active) {
                    // Define chakra colors (RGB)
                    const CRGB chakraColors[7] = {
                        CRGB(255, 0, 0),      // ROOT - Red
                        CRGB(255, 100, 0),    // SACRAL - Orange
                        CRGB(255, 200, 0),    // SOLAR - Yellow
                        CRGB(0, 255, 0),      // HEART - Green
                        CRGB(0, 100, 255),    // THROAT - Blue (more blue, less cyan)
                        CRGB(75, 0, 130),     // THIRD_EYE - Indigo (darker purple)
                        CRGB(180, 0, 255)     // CROWN - Violet (brighter purple)
                    };
                    
                    CRGB currentColor = chakraColors[meditationState.currentChakra];
                    
                    // Smooth color transition between chakras
                    static int lastChakra = -1;  // -1 = uninitialized
                    static CRGB lastColor = CRGB::Black;
                    static uint32_t colorTransitionStart = 0;
                    
                    // Initialize on first use
                    if (lastChakra == -1) {
                        lastChakra = meditationState.currentChakra;
                        lastColor = currentColor;
                        colorTransitionStart = millis() - COLOR_TRANSITION_MS;  // Already complete
                    }
                    
                    // Detect chakra change and start color transition
                    if (meditationState.currentChakra != lastChakra) {
                        lastChakra = meditationState.currentChakra;
                        colorTransitionStart = millis();
                        
                        Serial.printf("ðŸŽ¨ Chakra changed to %s: RGB(%d,%d,%d) - starting 3s color fade\n", 
                                     CHAKRA_NAMES[meditationState.currentChakra],
                                     currentColor.r, currentColor.g, currentColor.b);
                    }
                    
                    // Calculate color blend during transition
                    CRGB displayColor;
                    if (millis() - colorTransitionStart < COLOR_TRANSITION_MS) {
                        // Mid-transition: blend from lastColor to currentColor
                        float blendProgress = (float)(millis() - colorTransitionStart) / COLOR_TRANSITION_MS;
                        displayColor = CRGB(
                            lastColor.r + (currentColor.r - lastColor.r) * blendProgress,
                            lastColor.g + (currentColor.g - lastColor.g) * blendProgress,
                            lastColor.b + (currentColor.b - lastColor.b) * blendProgress
                        );
                    } else {
                        // Transition complete: use current color
                        displayColor = currentColor;
                        lastColor = currentColor;  // Update for next transition
                    }
                    
                    if (meditationState.phaseStartTime > 0) {
                        // Calculate phase progress
                        const uint32_t PHASE_DURATION = 4000;  // 4 seconds per phase
                        uint32_t phaseElapsed = millis() - meditationState.phaseStartTime;
                        
                        // Check if phase is complete and advance
                        if (phaseElapsed >= PHASE_DURATION) {
                            meditationState.phase = (MeditationState::BreathPhase)((meditationState.phase + 1) % 4);
                            meditationState.phaseStartTime = millis();
                            phaseElapsed = 0;
                            
                            const char* phaseNames[] = {"INHALE", "HOLD_TOP", "EXHALE", "HOLD_BOTTOM"};
                            Serial.printf("ðŸ§˜ Breath phase: %s\n", phaseNames[meditationState.phase]);
                        }
                        
                        // CRITICAL: Capture phase state at start of frame to prevent mid-frame changes
                        MeditationState::BreathPhase currentPhase = meditationState.phase;
                        float phaseProgress = (float)phaseElapsed / PHASE_DURATION;
                        
                        // Calculate brightness based on breath phase (all LEDs breathe together)
                        // 20% to 100% breathing effect for entire sphere
                        float breathBrightness = MEDITATION_BREATH_MIN;  // Default to minimum
                        
                        switch (currentPhase) {
                            case MeditationState::INHALE:
                                // Smooth fade from 20% to 100% over 4 seconds
                                breathBrightness = MEDITATION_BREATH_MIN + ((MEDITATION_BREATH_MAX - MEDITATION_BREATH_MIN) * phaseProgress);
                                break;
                            case MeditationState::HOLD_TOP:
                                // Hold at 100% for 4 seconds
                                breathBrightness = MEDITATION_BREATH_MAX;
                                break;
                            case MeditationState::EXHALE:
                                // Smooth fade from 100% to 20% over 4 seconds
                                breathBrightness = MEDITATION_BREATH_MAX - ((MEDITATION_BREATH_MAX - MEDITATION_BREATH_MIN) * phaseProgress);
                                break;
                            case MeditationState::HOLD_BOTTOM:
                                // Hold at 20% for 4 seconds
                                breathBrightness = MEDITATION_BREATH_MIN;
                                break;
                        }
                        
                        // Apply smooth easing for more organic breathing feel
                        float easedBrightness = (1.0 - cos(breathBrightness * PI)) / 2.0;
                        
                        // Convert to 0-255 range
                        uint8_t brightness = (uint8_t)(easedBrightness * 255);
                        
                        // Set all LEDs to the same chakra color at calculated brightness
                        CRGB breathColor = CRGB(
                            (displayColor.r * brightness) / 255,
                            (displayColor.g * brightness) / 255,
                            (displayColor.b * brightness) / 255
                        );
                        
                        fill_solid(leds, NUM_LEDS, breathColor);
                    } else {
                        // Not started yet (phaseStartTime = 0): Show static chakra color at 30%
                        for (int i = 0; i < NUM_LEDS; i++) {
                            uint8_t r = (uint8_t)((displayColor.r * 77) / 255);  // 30% = 77/255
                            uint8_t g = (uint8_t)((displayColor.g * 77) / 255);
                            uint8_t b = (uint8_t)((displayColor.b * 77) / 255);
                            leds[i] = CRGB(r, g, b);
                        }
                    }
                } else {
                    fill_solid(leds, NUM_LEDS, CRGB::Black);
                }
            }
            break;
            
        case LED_CLOCK:
            // Display current time (hours and minutes) revolving around the sphere
            {
                if (clockState.active) {
                    // Get current time from RTC
                    struct tm timeinfo;
                    if (getLocalTime(&timeinfo)) {
                        int currentHour = timeinfo.tm_hour;
                        int currentMinute = timeinfo.tm_min;
                        
                        // Update time string if time has changed
                        if (currentHour != clockState.lastHour || currentMinute != clockState.lastMinute) {
                            clockState.lastHour = currentHour;
                            clockState.lastMinute = currentMinute;
                            
                            // Format time string (24-hour format: "HH:MM")
                            char timeStr[6];
                            snprintf(timeStr, sizeof(timeStr), "%02d:%02d", currentHour, currentMinute);
                            
                            // Update the front marquee with new time (but don't auto-complete)
                            frontMarquee.setText(timeStr);
                            frontMarquee.setColor(CRGB::White);
                            frontMarquee.setSpeed(3);  // Slow scroll for clock
                            if (!frontMarquee.isActive()) {
                                frontMarquee.start();
                            }
                            
                            Serial.printf("ðŸ• Clock updated: %02d:%02d\n", currentHour, currentMinute);
                        }
                        
                        // Update and render the scrolling time
                        frontMarquee.update();
                        frontMarquee.render(leds);
                        
                        // Reset scroll position when it completes to loop seamlessly
                        if (frontMarquee.isComplete()) {
                            frontMarquee.start();  // Restart scroll
                        }
                    } else {
                        // If time not available, show error pattern
                        static uint32_t lastBlink = 0;
                        static bool blinkState = false;
                        
                        if (millis() - lastBlink > 500) {
                            lastBlink = millis();
                            blinkState = !blinkState;
                            
                            if (blinkState) {
                                fill_solid(leds, NUM_LEDS, CRGB(50, 0, 0));  // Dim red
                            } else {
                                fill_solid(leds, NUM_LEDS, CRGB::Black);
                            }
                        }
                    }
                } else {
                    fill_solid(leds, NUM_LEDS, CRGB::Black);
                }
            }
            break;
            
        case LED_LAMP:
            // White lamp with spiral swoosh lighting effect
            // Button 1: cycle colors (WHITE â†’ RED â†’ GREEN â†’ BLUE)
            {
                if (lampState.active) {
                    const uint32_t FADE_DURATION_MS = 150;  // Quick fade-up per LED
                    const uint32_t LED_INTERVAL_MS = 40;    // 40ms between lighting each LED (slowed down for better visual)
                    
                    uint32_t now = millis();
                    
                    // Get RGB values for current and previous colors at 50% brightness
                    auto getColorRGB = [](LampState::Color color) -> CRGB {
                        switch (color) {
                            case LampState::RED:   return CRGB(128, 0, 0);    // 50% red
                            case LampState::GREEN: return CRGB(0, 128, 0);    // 50% green
                            case LampState::BLUE:  return CRGB(0, 0, 128);    // 50% blue
                            default:               return CRGB(128, 128, 128);  // 50% white
                        }
                    };
                    
                    CRGB targetColor = getColorRGB(lampState.currentColor);
                    CRGB previousColor = getColorRGB(lampState.previousColor);
                    
                    // Light next LED if it's time
                    if (!lampState.fullyLit && (now - lampState.lastUpdate) >= LED_INTERVAL_MS) {
                        // Calculate LED index: spiral pattern (row by row, rotating around strips)
                        int ledIndex = lampState.currentCol * LEDS_PER_COLUMN + lampState.currentRow;
                        
                        if (ledIndex < NUM_LEDS) {
                            lampState.ledStartTimes[ledIndex] = now;
                            lampState.lastUpdate = now;
                            
                            // Move to next LED in spiral
                            lampState.currentCol++;
                            if (lampState.currentCol >= LED_COLUMNS) {
                                lampState.currentCol = 0;
                                lampState.currentRow++;
                                
                                if (lampState.currentRow >= LEDS_PER_COLUMN) {
                                    lampState.fullyLit = true;
                                    lampState.transitioning = false;
                                    Serial.println("ðŸ’¡ Lamp fully lit");
                                }
                            }
                        }
                    }
                    
                    // Render all LEDs with fade effect
                    for (int i = 0; i < NUM_LEDS; i++) {
                        if (lampState.transitioning && lampState.ledStartTimes[i] == 0) {
                            // Not reached by spiral yet - show previous color
                            leds[i] = previousColor;
                        } else if (lampState.ledStartTimes[i] > 0) {
                            uint32_t elapsed = now - lampState.ledStartTimes[i];
                            
                            if (elapsed < FADE_DURATION_MS) {
                                // Fading up to new color
                                float progress = (float)elapsed / FADE_DURATION_MS;
                                progress = progress * progress;  // Ease-in
                                
                                if (lampState.transitioning) {
                                    // Blend from previous to target color
                                    leds[i] = CRGB(
                                        previousColor.r + (targetColor.r - previousColor.r) * progress,
                                        previousColor.g + (targetColor.g - previousColor.g) * progress,
                                        previousColor.b + (targetColor.b - previousColor.b) * progress
                                    );
                                } else {
                                    // Fading from black to target color (initial lighting)
                                    leds[i] = CRGB(
                                        (targetColor.r * progress),
                                        (targetColor.g * progress),
                                        (targetColor.b * progress)
                                    );
                                }
                            } else {
                                // Fully lit at target color
                                leds[i] = targetColor;
                            }
                        } else {
                            // Not lit yet (initial lighting)
                            leds[i] = CRGB::Black;
                        }
                    }
                } else {
                    fill_solid(leds, NUM_LEDS, CRGB::Black);
                }
            }
            break;
            
        case LED_SEA_GOOSEBERRY:
            // Sea Gooseberry Mode - organic downward-traveling iridescent waves
            // Mimics real comb jelly bioluminescence with phase-shifted rainbow colors
            {
                seaGooseberry.render(leds, NUM_LEDS);
            }
            break;
            
        case LED_EYES:
            // Eye Animation Mode - expressive robot eyes
            // Uses strips 2-3 for left eye, 11-12 for right eye
            {
                eyeAnimation.render(leds);
            }
            break;
            
        case LED_ALARM:
            // Alarm ringing - pulsing orange/red from center outward
            {
                if (alarmState.ringing) {
                    const uint32_t PULSE_DURATION_MS = 1500;  // 1.5 seconds per pulse cycle
                    const float MAX_RADIUS = 8.0f;  // Maximum pulse radius (extends beyond sphere)
                    
                    uint32_t now = millis();
                    uint32_t elapsed = now - alarmState.pulseStartTime;
                    
                    // Reset pulse if cycle complete
                    if (elapsed >= PULSE_DURATION_MS) {
                        alarmState.pulseStartTime = now;
                        elapsed = 0;
                    }
                    
                    // Calculate current pulse radius (0 to MAX_RADIUS)
                    float progress = (float)elapsed / PULSE_DURATION_MS;
                    alarmState.pulseRadius = progress * MAX_RADIUS;
                    
                    // Sphere center point (in LED grid space)
                    const float centerX = LED_COLUMNS / 2.0f;
                    const float centerY = LEDS_PER_COLUMN / 2.0f;
                    
                    // Render pulse effect
                    for (int col = 0; col < LED_COLUMNS; col++) {
                        for (int row = 0; row < LEDS_PER_COLUMN; row++) {
                            int ledIndex = col * LEDS_PER_COLUMN + row;
                            
                            // Calculate distance from center
                            float dx = col - centerX;
                            float dy = row - centerY;
                            float distance = sqrt(dx * dx + dy * dy);
                            
                            // Calculate brightness based on distance from pulse wavefront
                            float wavefront = alarmState.pulseRadius;
                            float distanceFromWave = abs(distance - wavefront);
                            
                            // Make wave thicker and brighter at the front
                            const float WAVE_THICKNESS = 2.5f;
                            float intensity = 0.0f;
                            
                            if (distanceFromWave < WAVE_THICKNESS) {
                                // Inside the wave - bright
                                intensity = 1.0f - (distanceFromWave / WAVE_THICKNESS);
                                intensity = intensity * intensity;  // Ease curve
                            } else if (distance < wavefront) {
                                // Behind the wave - dim glow
                                intensity = 0.2f;
                            }
                            
                            // Color: orange to red gradient based on intensity
                            uint8_t red = 255;
                            uint8_t green = (uint8_t)(120 * (1.0f - intensity * 0.5f));  // More orange at edges, more red at center
                            uint8_t blue = 0;
                            
                            leds[ledIndex] = CRGB(
                                (uint8_t)(red * intensity),
                                (uint8_t)(green * intensity),
                                blue
                            );
                        }
                    }
                } else {
                    fill_solid(leds, NUM_LEDS, CRGB::Black);
                }
            }
            break;
            
        case LED_CONVERSATION_WINDOW:
            // Progress bar countdown - vertical bars showing remaining conversation window time
            // All strips sync together
            {
                if (conversationMode) {
                    uint32_t elapsed = millis() - conversationWindowStart;
                    uint32_t remaining = CONVERSATION_WINDOW_MS - elapsed;
                    
                    if (remaining > 0) {
                        // Calculate number of rows to light based on remaining time
                        float progress = (float)remaining / (float)CONVERSATION_WINDOW_MS;
                        int numRows = (int)(progress * LEDS_PER_COLUMN);
                        
                        // Pulse effect in final 3 seconds for urgency
                        uint8_t brightness = 255;
                        if (remaining < 3000) {
                            float pulse = 0.5 + 0.5 * sin(millis() / 150.0);
                            brightness = (uint8_t)(255 * pulse);
                        }
                        
                        // Cyan color for conversation listening mode - all strips sync
                        for (int col = 0; col < LED_COLUMNS; col++) {
                            for (int row = 0; row < LEDS_PER_COLUMN; row++) {
                                int ledIndex = col * LEDS_PER_COLUMN + row;
                                
                                if (row < numRows) {
                                    leds[ledIndex] = CHSV(160, 200, brightness);  // Cyan
                                } else {
                                    leds[ledIndex] = CRGB::Black;
                                }
                            }
                        }
                    } else {
                        // Window expired - clear LEDs
                        fill_solid(leds, NUM_LEDS, CRGB::Black);
                    }
                } else {
                    fill_solid(leds, NUM_LEDS, CRGB::Black);
                }
            }
            break;
            
        case LED_MARQUEE:
            // Front text marquee using new chunky glyph system
            {
                frontMarquee.update();  // Advance scroll position
                frontMarquee.render(leds);  // Draw to display
                
                // Check if complete
                if (frontMarquee.isComplete()) {
                    frontMarquee.stop();
                    currentLEDMode = targetLEDMode;
                    Serial.printf("ðŸ“œ Marquee complete, switching to mode %d\n", targetLEDMode);
                    
                    // If switching to ambient mode, request the audio now
                    if (targetLEDMode == LED_AMBIENT) {
                        // Request the current ambient sound from server
                        JsonDocument ambientDoc;
                        ambientDoc["action"] = "requestAmbient";
                        ambientDoc["sound"] = ambientSound.name;  // rain/ocean/rainforest
                        ambientDoc["sequence"] = ambientSound.sequence;
                        String ambientMsg;
                        serializeJson(ambientDoc, ambientMsg);
                        Serial.printf("ðŸ“¤ Ambient audio request: %s (seq %d)\n", ambientMsg.c_str(), ambientSound.sequence);
                        webSocket.sendTXT(ambientMsg);
                    }
                    
                    // If switching to Pomodoro mode, auto-start the timer
                    if (targetLEDMode == LED_POMODORO && pomodoroState.active && pomodoroState.paused) {
                        pomodoroState.startTime = millis();
                        pomodoroState.paused = false;
                        Serial.println("â–¶ï¸  Pomodoro timer auto-started");
                        playZenBell();  // Play zen bell on start
                    }
                    
                    // If switching to meditation mode, start the breathing and audio now
                    if (targetLEDMode == LED_MEDITATION && meditationState.active && meditationState.phaseStartTime == 0) {
                        // Start breathing animation
                        meditationState.phaseStartTime = millis();
                        
                        // Set ambient audio flags
                        ambientSound.name = "om001";
                        ambientSound.active = true;
                        isPlayingAmbient = true;
                        isPlayingResponse = false;
                        firstAudioChunk = true;
                        lastAudioChunkTime = millis();
                        
                        // Request ROOT chakra audio (om001.pcm)
                        JsonDocument reqDoc;
                        reqDoc["action"] = "requestAmbient";
                        reqDoc["sound"] = "om001";
                        reqDoc["sequence"] = ++ambientSound.sequence;
                        String reqMsg;
                        serializeJson(reqDoc, reqMsg);
                        Serial.printf("ðŸ“¤ Meditation starting: %s (seq %d)\n", reqMsg.c_str(), ambientSound.sequence);
                        webSocket.sendTXT(reqMsg);
                        meditationState.streaming = true;
                        
                        Serial.println("ðŸ§˜ Meditation breathing and audio started (ROOT chakra)");
                    }
                }
            }
            break;
            
        case LED_CONNECTED:
            {
                static uint32_t lastConnDebug = 0;
                if (millis() - lastConnDebug > 500) {
                    Serial.println("âœ… LED_CONNECTED: Solid green");
                    lastConnDebug = millis();
                }
                fill_solid(leds, NUM_LEDS, CRGB(0, 255, 0));  // Pure green
            }
            break;
            
        case LED_ERROR:
            brightness = (millis() / 200) % 2 ? 255 : 50;
            fill_solid(leds, NUM_LEDS, CHSV(0, 255, brightness));
            break;
    }
}

// ============== FREERTOS TASKS ==============
void websocketTask(void * parameter) {
    static uint32_t lastConnCheck = 0;
    static uint32_t lastHealthLog = 0;
    static uint32_t startupHeap = 0;
    static uint32_t lowestHeap = UINT32_MAX;
    static uint32_t startTime = millis();
    
    while(1) {
        webSocket.loop();
        
        // Health monitoring every 5s (more frequent for weak signal detection)
        if (millis() - lastHealthLog > 5000) {
            int32_t rssi = WiFi.RSSI();
            
            // Get memory statistics
            uint32_t freeHeap = ESP.getFreeHeap();
            uint32_t freePsram = ESP.getFreePsram();
            
            // Memory leak detection tracking (quiet mode - only show baseline and hourly reports)
            if (startupHeap == 0) {
                startupHeap = freeHeap;
                lowestHeap = freeHeap;
                Serial.printf("ðŸ“Š Memory baseline: Heap=%u KB, PSRAM=%u KB\n", freeHeap/1024, freePsram/1024);
            }
            if (freeHeap < lowestHeap) {
                lowestHeap = freeHeap;
                Serial.printf("ðŸ“‰ New low heap: %u KB (lost %u KB since startup)\n", 
                             freeHeap/1024, (startupHeap - freeHeap)/1024);
            }
            
            // Hourly summary for leak detection
            uint32_t uptime = (millis() - startTime) / 1000;
            if (uptime > 0 && uptime % 3600 == 0) {
                Serial.printf("\nâ•”â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•—\n");
                Serial.printf("â•‘  HOURLY MEMORY REPORT - %u hours runtime  â•‘\n", uptime/3600);
                Serial.printf("â• â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•£\n");
                Serial.printf("â•‘  Current Heap:  %6u KB                â•‘\n", freeHeap/1024);
                Serial.printf("â•‘  Startup Heap:  %6u KB                â•‘\n", startupHeap/1024);
                Serial.printf("â•‘  Lowest Heap:   %6u KB                â•‘\n", lowestHeap/1024);
                Serial.printf("â•‘  Heap Lost:     %6u KB                â•‘\n", (startupHeap - freeHeap)/1024);
                Serial.printf("â•‘  PSRAM Free:    %6u KB                â•‘\n", freePsram/1024);
                Serial.printf("â•‘  Mode: %-30s    â•‘\n", 
                    currentLEDMode == LED_IDLE ? "IDLE" :
                    currentLEDMode == LED_AMBIENT ? "AMBIENT" :
                    currentLEDMode == LED_AMBIENT_VU ? "VU METER" :
                    currentLEDMode == LED_SEA_GOOSEBERRY ? "SEA GOOSEBERRY" :
                    currentLEDMode == LED_POMODORO ? "POMODORO" :
                    currentLEDMode == LED_MEDITATION ? "MEDITATION" : "OTHER");
                Serial.printf("â•šâ•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•\n\n");
            }
            
            lastRSSI = rssi;
            lastHealthLog = millis();
            
            // Warn if heap is getting low
            if (freeHeap < 50000) {  // Less than 50KB free
                Serial.printf("âš ï¸  LOW HEAP WARNING: Only %u KB free!\n", freeHeap/1024);
            }
            
            // Attempt to reconnect WiFi if signal is very poor but still connected
            if (rssi < -80 && WiFi.status() == WL_CONNECTED) {
                Serial.println("âš ï¸  Very weak signal detected - WiFi may drop soon");
            }
        }
        
        // Monitor WiFi connection and attempt reconnection if needed
        if (millis() - lastConnCheck > 5000) {  // Check every 5s (more frequent)
            if (WiFi.status() != WL_CONNECTED) {
                Serial.printf("[WebSocket Task] WiFi disconnected! Status: %d - Attempting reconnect...\n", WiFi.status());
                WiFi.reconnect();
            } else if (!isWebSocketConnected) {
                int32_t rssi = WiFi.RSSI();
                Serial.printf("[WebSocket Task] WebSocket not connected. WiFi RSSI: %d dBm\n", rssi);
            }
            lastConnCheck = millis();
        }
        
        // Reduced delay from 10ms to 5ms for faster WebSocket processing
        // This ensures keepalive pings are sent promptly even during heavy audio streaming
        vTaskDelay(5 / portTICK_PERIOD_MS);
    }
}

void ledTask(void * parameter) {
    static uint32_t lastLedUpdate = 0;
    static uint32_t ledTaskStalls = 0;
    
    while(1) {
        // Watchdog: detect if LED updates are stalling
        uint32_t now = millis();
        if (lastLedUpdate > 0 && (now - lastLedUpdate) > 200) {
            ledTaskStalls++;
            Serial.printf("âš ï¸ LED task stalled #%d: %dms since last update\n", 
                         ledTaskStalls, now - lastLedUpdate);
        }
        
        // Mutex-protected LED rendering
        if (xSemaphoreTake(ledMutex, portMAX_DELAY) == pdTRUE) {
            // Track updateLEDs calls
            static uint32_t updateCount = 0;
            static uint32_t lastUpdateLog = 0;
            updateCount++;
            
            if (millis() - lastUpdateLog > 30000) {
                // Serial.printf("ðŸ”„ LED task: %u updates in 30s (expect ~990)\n", updateCount);  // Disabled for clean logging
                updateCount = 0;
                lastUpdateLog = millis();
            }
            
            updateLEDs();
            FastLED.show();
            xSemaphoreGive(ledMutex);
        }
        lastLedUpdate = millis();
        
        vTaskDelay(30 / portTICK_PERIOD_MS);  // 33Hz update rate - matches audio chunk timing better
    }
}
