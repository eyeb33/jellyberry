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
#include "types.h"
#include "SeaGooseberryVisualizer.h"
#include "EyeAnimationVisualizer.h"
#include "ws_handler.h"
#include "LedModes.h"

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

// Sea Gooseberry visualizer
SeaGooseberryVisualizer seaGooseberry;

// Eye Animation visualizer
EyeAnimationVisualizer eyeAnimation;

bool isWebSocketConnected = false;
// volatile: written by one FreeRTOS task / ISR context, read by another on the dual-core ESP32-S3.
volatile bool recordingActive = false;
volatile bool isPlayingResponse = false;
volatile bool isPlayingAmbient = false; // Track ambient sound playback separately
bool isPlayingAlarm = false; // Track alarm sound playback
volatile bool turnComplete = false; // Track when Gemini has finished its turn
bool responseInterrupted = false; // Flag to ignore audio after interrupt
bool shutdownSoundPlayed = false; // Flag to prevent repeated shutdown sounds during reconnection
uint32_t recordingStartTime = 0;
uint32_t lastVoiceActivityTime = 0;
volatile uint32_t lastAudioChunkTime = 0;      // Track when we last received ANY audio chunk
volatile uint32_t lastGeminiAudioTime = 0;     // Track when we last received a Gemini (non-ambient) chunk — used for drain detection during radio overlap
ConvState convState = ConvState::IDLE;   // Main-loop UX state machine (not cross-task)
uint32_t waitingEnteredAt = 0;           // When we entered WAITING; drives thinking-animation and 60s timeout
uint32_t lastWebSocketSendTime = 0; // Track last successful send
uint32_t webSocketSendFailures = 0; // Count send failures
// NOTE: lastWiFiCheck is declared as a static local inside loop() - no global needed.
int32_t lastRSSI = 0; // Track signal strength changes
bool firstAudioChunk = true;
volatile float volumeMultiplier = 0.25f;  // Volume control - volatile: read by audioTask, written by main/WS task
volatile int32_t currentAudioLevel = 0;  // Current audio amplitude - volatile: written by audioTask, read by ledTask
volatile float smoothedAudioLevel = 0.0f;  // Smoothed audio level - volatile: written by ledTask
volatile bool conversationMode = false;  // Track if we're in conversation window
uint32_t conversationWindowStart = 0;  // Timestamp when conversation window opened
bool conversationRecording = false;  // Track if current recording was triggered from conversation mode
bool recordingStartSent = false;    // Track if recordingStart state message has been sent for current recording

// ---- I2S_NUM_0 ownership ----
// audioTask is the SOLE caller of i2s_read(I2S_NUM_0). Other tasks must NOT call it directly.
// Results are shared via these two volatile values:
volatile int32_t ambientMicRows = 0; // Pre-computed VU row count; LED renderer reads this
volatile bool conversationVADDetected = false; // audioTask sets this when VAD fires during conv. window

// Audio level delay buffer for LED sync.
// Size 1: reads the value just written — no artificial delay.
// The I2S DMA latency (~42ms) is small enough that the 30ms LED frame period
// adequately masks any visible lead. Increase to 2 if LEDs visibly lead audio.
#define AUDIO_DELAY_BUFFER_SIZE 1
int audioLevelBuffer[AUDIO_DELAY_BUFFER_SIZE] = {0};
int audioBufferIndex = 0;

TaskHandle_t websocketTaskHandle = NULL;
TaskHandle_t ledTaskHandle = NULL;
TaskHandle_t audioTaskHandle = NULL;

// Audio processing (raw PCM - no codec needed)

// Audio buffers
QueueHandle_t audioOutputQueue; // Queue for playback audio
// Audio queue size: 30 packets = ~1.2s buffer
// Provides good jitter tolerance with paced delivery
// Tuning: Increase for more buffer (higher latency), decrease for lower latency (more underruns)
// AUDIO_QUEUE_SIZE is defined in Config.h

LEDMode currentLEDMode = LED_IDLE;  // Start directly in idle mode
bool isAmbientVUMode = false;  // Toggle for ambient sound VU meter mode

// Ambient sound type (for cycling within AMBIENT mode)
AmbientSoundType currentAmbientSoundType = SOUND_RAIN;

// Tide visualization state
TideState tideState = {"", 0.0, 0, 0, false};

// Timer visualization state
TimerState timerState = {0, 0, false, false, 0, 0};

// Moon phase visualization state
MoonState moonState = {"", 0, 0.0, 0, false};

// LED mutex for thread-safe access
SemaphoreHandle_t ledMutex = NULL;

// I2S speaker mutex - prevents audioTask and tone functions writing I2S_NUM_1 simultaneously
SemaphoreHandle_t i2sSpeakerMutex = NULL;

// WebSocket send mutex - serialises all sendTXT calls across websocketTask, audioTask, and loop()
// Without this, concurrent sends from different tasks corrupt the TCP frame buffer and cause disconnects.
SemaphoreHandle_t wsSendMutex = NULL;

// Ambient sound state
AmbientSound ambientSound = {"", false, 0, 0, 0};

// Pomodoro timer state
PomodoroState pomodoroState = {PomodoroState::FOCUS, 0, 25 * 60, 0, 0, false, false, 25, 5, 15, false, 0, 0};

// Meditation mode state
MeditationState meditationState = {MeditationState::ROOT, MeditationState::INHALE, 0, false, false, 1.0f};

// Lamp mode state
LampState lampState = {LampState::WHITE, LampState::WHITE, 0, 0, 0, {0}, false, false, false};

// Radio mode state
RadioState radioState = {};

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
bool detectVoiceActivity(int16_t* samples, size_t count);
void sendAudioChunk(uint8_t* data, size_t length);
void playZenBell();
void playShutdownSound();
void playVolumeChime();
void clearAudioAndLEDs();  // Helper to clear audio buffers and LEDs
// Loop sub-tasks (defined just before loop())
static void checkAlarms();
static void handleFlashAnimations();
static void handlePomodoroTick();
static void handleAmbientCompletion();
static void resumeRadioStream();

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
 
 delay(50); // Let everything settle
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
        
        Serial.printf("Brightness changed to %s mode (%d/255 = %.0f%%)\n",
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
 delay(1000); // Brief delay before retry
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
 while (true) { delay(1000); } // Hard halt - nothing is safe without the mutex
 }

 // Create I2S speaker mutex to prevent audioTask and tone functions writing I2S_NUM_1 simultaneously
 i2sSpeakerMutex = xSemaphoreCreateMutex();
 if (i2sSpeakerMutex == NULL) {
 Serial.println("FATAL: Failed to create I2S speaker mutex - halting!");
 while (true) { delay(1000); }
 }

 // Create WebSocket send mutex - serialises sendTXT across all tasks to prevent frame corruption
 wsSendMutex = xSemaphoreCreateMutex();
 if (wsSendMutex == NULL) {
 Serial.println("FATAL: Failed to create WS send mutex - halting!");
 while (true) { delay(1000); }
 }

 Serial.println("\n\n========================================");
 Serial.println("=== JELLYBERRY BOOT STARTING ===");
 Serial.println("========================================");
 Serial.flush();

 // Initialize LED strip (144 LEDs on GPIO 1)
 Serial.write("LED_INIT_START\r\n", 16);
 FastLED.addLeds<LED_CHIPSET, LED_DATA_PIN, LED_COLOR_ORDER>(leds, NUM_LEDS);
 FastLED.setBrightness(LED_BRIGHTNESS_DAY); // Start with day brightness until we know otherwise
 FastLED.setDither(0); // Disable dithering to prevent flickering
 FastLED.setMaxRefreshRate(400); // Limit refresh rate for stability (default is 400Hz)
 FastLED.setCorrection(TypicalLEDStrip); // Color correction for consistent output
 
 FastLED.clear();
 fill_solid(leds, NUM_LEDS, CHSV(160, 255, 100));
 FastLED.show();
 Serial.write("LED_INIT_DONE\r\n", 15);

    // Create audio queue
    Serial.println("Creating audio queue...");
    Serial.flush();
    audioOutputQueue = xQueueCreate(AUDIO_QUEUE_SIZE, sizeof(AudioChunk));
    if (!audioOutputQueue) {
        Serial.println("Failed to create audio queue");
        currentLEDMode = LED_ERROR;
        return;
    }
    Serial.println("Audio queue created");
    Serial.flush();
    
    // Raw PCM streaming - no codec initialization needed
    Serial.println("Audio pipeline: Raw PCM (16-bit, 16kHz mic  24kHz speaker)");

    // Initialize I2S audio
    if (!initI2SMic()) {
        Serial.println("Microphone init failed");
        currentLEDMode = LED_ERROR;
        return;
    }
    Serial.println("Microphone initialized");

    if (!initI2SSpeaker()) {
        Serial.println("Speaker init failed");
        currentLEDMode = LED_ERROR;
        return;
    }
    Serial.println("Speaker initialized");

    // Initialize touch pads
    pinMode(TOUCH_PAD_START_PIN, INPUT_PULLDOWN);
    pinMode(TOUCH_PAD_STOP_PIN, INPUT_PULLDOWN);
    Serial.printf("Touch pads initialized (START=%d, STOP=%d)\n", 
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
            delay(1000 * retryCount);  // Linear backoff: 0s, 1s, 2s before each retry
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
            Serial.println("\n WiFi connected");
            Serial.printf("IP: %s\n", WiFi.localIP().toString().c_str());
            Serial.printf("Signal: %d dBm\n", WiFi.RSSI());
            
            // Configure NTP time sync (GMT timezone)
            configTime(0, 0, "pool.ntp.org", "time.nist.gov");
            Serial.println("NTP time sync configured");
        } else {
            retryCount++;
            Serial.printf("\n Connection attempt failed (status: %d)\n", WiFi.status());
        }
    }
    4
    if (!connected) {
        Serial.println("WiFi connection failed after all retries");
        currentLEDMode = LED_ERROR;
        return;
    }

    // Initialize WebSocket to edge server
    wifiClient.setInsecure(); // Skip certificate validation
    String wsPath = String(EDGE_SERVER_PATH) + "?device_id=" + String(DEVICE_ID);
    
    #if USE_SSL
    webSocket.beginSSL(EDGE_SERVER_HOST, EDGE_SERVER_PORT, wsPath.c_str(), "", "wss");
    Serial.printf("WebSocket initialized to wss://%s:%d%s\n", EDGE_SERVER_HOST, EDGE_SERVER_PORT, wsPath.c_str());
    #else
    webSocket.begin(EDGE_SERVER_HOST, EDGE_SERVER_PORT, wsPath.c_str());
    Serial.printf("WebSocket initialized to ws://%s:%d%s\n", EDGE_SERVER_HOST, EDGE_SERVER_PORT, wsPath.c_str());
    #endif
    
    webSocket.onEvent(onWebSocketEvent);
    webSocket.setReconnectInterval(WS_RECONNECT_INTERVAL);
    webSocket.enableHeartbeat(WS_HEARTBEAT_PING_MS, WS_HEARTBEAT_TIMEOUT_MS, WS_HEARTBEAT_RETRIES);
    Serial.println("WebSocket initialized with relaxed keepalive");
    
    // Note: TCP buffer sizes are controlled by lwIP configuration, not runtime changeable
    Serial.println("Using default TCP buffers (configured in sdkconfig)");

    // Start FreeRTOS tasks
    // WebSocket needs high priority (3) and larger stack for heavy audio streaming
    xTaskCreatePinnedToCore(websocketTask, "WebSocket", TASK_STACK_WEBSOCKET, NULL, 3, &websocketTaskHandle, CORE_1);
    xTaskCreatePinnedToCore(ledTask,         "LEDs",      TASK_STACK_LED,       NULL, 0, &ledTaskHandle,       CORE_0);
    xTaskCreatePinnedToCore(audioTask,       "Audio",     TASK_STACK_AUDIO,     NULL, 2, &audioTaskHandle,     CORE_1);
    Serial.println("Tasks created on dual cores");
    
    Serial.printf("=== Initialization Complete ===  [LEDMode: IDLE]\n");
    Serial.println("Touch START pad to begin recording");
}

// ============================================================
// Loop helpers — extracted from loop() for readability
// ============================================================

static void checkAlarms() {
    static uint32_t lastAlarmCheck = 0;
    // Check alarms every 10 seconds
    if (alarmState.active && !alarmState.ringing && millis() - lastAlarmCheck > ALARM_CHECK_INTERVAL_MS) {
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
                            DEBUG_PRINT(" Alarm %u ringing after snooze! (interrupted mode: %d)\n", alarms[i].alarmID, alarmState.previousMode);
                            
                            // Play alarm sound
                            isPlayingAlarm = true;
                            isPlayingResponse = true;
                            firstAudioChunk = true;
                            lastAudioChunkTime = millis();
                            JsonDocument alarmDoc;
                            alarmDoc["action"] = "requestAlarm";
                            String alarmMsg;
                            serializeJson(alarmDoc, alarmMsg);
                            DEBUG_PRINTLN(" Requesting alarm sound from server");
                            wsSendMessage(alarmMsg);
                            
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
                        DEBUG_PRINT(" Alarm %u triggered at %s (interrupted mode: %d)\n", alarms[i].alarmID, asctime(&timeinfo), alarmState.previousMode);
                        
                        // Play alarm sound
                        isPlayingAlarm = true;
                        isPlayingResponse = true;
                        firstAudioChunk = true;
                        lastAudioChunkTime = millis();
                        JsonDocument alarmDoc;
                        alarmDoc["action"] = "requestAlarm";
                        String alarmMsg;
                        serializeJson(alarmDoc, alarmMsg);
                        DEBUG_PRINTLN(" Requesting alarm sound from server");
                        wsSendMessage(alarmMsg);
                        
                        break;
                    }
                }
            }
        }
        lastAlarmCheck = millis();
    }
}

static void handleFlashAnimations() {
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
    
    // Handle non-blocking timer flash animation (triggered by timerExpired WebSocket message).
    // Runs at 200ms intervals in loop() — WebSocket task is never blocked.
    if (timerState.flashing && (millis() - timerState.flashStartTime) >= 200) {
        timerState.flashCount++;
        timerState.flashStartTime = millis();
    
        if (timerState.flashCount >= 6) {
            // Animation complete (3 on/off cycles = 6 state changes)
            timerState.flashing = false;
        } else {
            // Toggle LEDs green/black
            if (xSemaphoreTake(ledMutex, portMAX_DELAY) == pdTRUE) {
                if (timerState.flashCount % 2 == 0) {
                    fill_solid(leds, NUM_LEDS, CRGB::Green);
                } else {
                    fill_solid(leds, NUM_LEDS, CRGB::Black);
                }
                FastLED.show();
                xSemaphoreGive(ledMutex);
            }
        }
    }
}

static void handlePomodoroTick() {
    // Check for Pomodoro session completion and auto-advance
    if (currentLEDMode == LED_POMODORO && pomodoroState.active && !pomodoroState.paused && pomodoroState.startTime > 0) {
        uint32_t elapsed = (millis() - pomodoroState.startTime) / 1000;
        int secondsRemaining = pomodoroState.totalSeconds - (int)elapsed;
        
        if (secondsRemaining <= 0) {
            DEBUG_PRINTLN(" Pomodoro session complete!");
            
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
                    DEBUG_PRINT("   Focus complete! Starting long break (%d min)\n", pomodoroState.longBreakDuration);
                    pomodoroState.currentSession = PomodoroState::LONG_BREAK;
                    pomodoroState.totalSeconds = pomodoroState.longBreakDuration * 60;
                    
                    // Start immediately (don't pause)
                    pomodoroState.startTime = millis();
                    pomodoroState.pausedTime = 0;
                    pomodoroState.paused = false;
                } else {
                    // Normal short break after focus
                    DEBUG_PRINT("   Focus complete! Starting short break (%d min) [%d/4]\n", pomodoroState.shortBreakDuration, pomodoroState.sessionCount);
                    pomodoroState.currentSession = PomodoroState::SHORT_BREAK;
                    pomodoroState.totalSeconds = pomodoroState.shortBreakDuration * 60;
                    
                    // Start immediately (don't pause)
                    pomodoroState.startTime = millis();
                    pomodoroState.pausedTime = 0;
                    pomodoroState.paused = false;
                }
            } else if (pomodoroState.currentSession == PomodoroState::LONG_BREAK) {
                // Long break complete - END OF CYCLE, return to IDLE
                DEBUG_PRINTLN("   Long break complete! Pomodoro cycle finished - returning to IDLE");
                pomodoroState.active = false;
                pomodoroState.currentSession = PomodoroState::FOCUS;
                pomodoroState.totalSeconds = pomodoroState.focusDuration * 60;
                pomodoroState.sessionCount = 0;  // Reset counter for next cycle
                pomodoroState.startTime = 0;
                pomodoroState.pausedTime = 0;
                pomodoroState.paused = false;
                currentLEDMode = LED_IDLE;  // Return to IDLE - cycle complete
            } else {
                // Short break complete, return to focus
                DEBUG_PRINT("   Break complete! Starting focus session (%d min)\n", pomodoroState.focusDuration);
                pomodoroState.currentSession = PomodoroState::FOCUS;
                pomodoroState.totalSeconds = pomodoroState.focusDuration * 60;
                
                // Start immediately (don't pause)
                pomodoroState.startTime = millis();
                pomodoroState.pausedTime = 0;
                pomodoroState.paused = false;
            }
        }
    }
}

static void handleAmbientCompletion() {
    // Check for ambient sound streaming completion
    // When stream ends, return to IDLE mode (no looping)
    // Guards:
    //   LED_MEDITATION  — has its own completion handler
    //   radioState.active — live stream; gaps/stalls must never be treated as end-of-stream
    //   convState != IDLE — covers RECORDING, WAITING (processing), PLAYING, and WINDOW;
    //                       lastAudioChunkTime is not updated during voice commands so the
    //                       timeout would false-fire against ambient audio that is just paused
    if (isPlayingAmbient && ambientSound.active && !firstAudioChunk && 
        (millis() - lastAudioChunkTime) > AMBIENT_COMPLETION_TIMEOUT_MS &&
        currentLEDMode != LED_MEDITATION && !radioState.active &&
        convState == ConvState::IDLE) {
        Serial.printf("Ambient sound completed: %s - returning to IDLE\n", ambientSound.name);
        
        // Return to IDLE mode
        currentLEDMode = LED_IDLE;
        isPlayingAmbient = false;
        isPlayingResponse = false;
        ambientSound.active = false;
        ambientSound.name[0] = '\0';
    }
}

// Resume the radio stream after a Gemini conversation pause.
// Sends requestRadio to the server and re-arms the ambient pipeline.
static void resumeRadioStream() {
    Serial.printf("Radio: resuming stream '%s' (seq %d)\n", radioState.stationName, ambientSound.sequence + 1);
    ambientSound.sequence++;
    strlcpy(ambientSound.name, radioState.stationName, sizeof(ambientSound.name));
    ambientSound.active = true;
    ambientSound.drainUntil = 0;
    isPlayingAmbient = true;
    isPlayingResponse = false;
    firstAudioChunk = true;
    lastAudioChunkTime = millis();
    radioState.paused = false;
    radioState.streaming = false;  // will become true on first incoming chunk
    JsonDocument radioReqDoc;
    radioReqDoc["action"] = "requestRadio";
    radioReqDoc["streamUrl"] = radioState.streamUrl;
    radioReqDoc["stationName"] = radioState.stationName;
    radioReqDoc["sequence"] = ambientSound.sequence;
    radioReqDoc["isHLS"] = radioState.isHLS;
    String radioReqMsg;
    serializeJson(radioReqDoc, radioReqMsg);
    wsSendMessage(radioReqMsg);
}

// ============== MAIN LOOP ==============
void loop() {
    static uint32_t lastPrint = 0;
    static uint32_t lastWiFiCheck = 0;
    static uint32_t lastBrightnessCheck = 0;

    // Log mode changes
    {
        static LEDMode lastLoggedMode = (LEDMode)-1;
        if (currentLEDMode != lastLoggedMode) {
            const char* modeNames[] = {
                "BOOT", "IDLE", "RECORDING", "PROCESSING", "AUDIO_REACTIVE",
                "CONNECTED", "ERROR", "TIDE", "TIMER", "MOON",
                "AMBIENT_VU", "AMBIENT", "RADIO", "POMODORO", "MEDITATION",
                "LAMP", "SEA_GOOSEBERRY", "EYES", "ALARM", "CONVERSATION"
            };
            int idx = (int)currentLEDMode;
            const char* name = (idx >= 0 && idx < (int)(sizeof(modeNames)/sizeof(modeNames[0])))
                               ? modeNames[idx] : "UNKNOWN";
            Serial.printf("[MODE] %s\n", name);
            lastLoggedMode = currentLEDMode;
        }
    }

    if (millis() - lastPrint > 5000) {
        // Serial.write("LOOP_TICK\r\n", 11);  // Disabled for clean overnight logging
        lastPrint = millis();
    }
    
    // Update day/night brightness periodically
    if (millis() - lastBrightnessCheck > BRIGHTNESS_CHECK_INTERVAL_MS) {
        updateDayNightBrightness();
        lastBrightnessCheck = millis();
    }

    // Monitor WiFi signal strength periodically
    if (millis() - lastWiFiCheck > WIFI_RSSI_CHECK_INTERVAL_MS) {
        int32_t rssi = WiFi.RSSI();
        if (rssi < WIFI_WEAK_RSSI_DBM) {
            Serial.printf("[WiFi] WEAK SIGNAL: %d dBm (may cause disconnects)\n", rssi);
        }
        lastWiFiCheck = millis();
    }
    
    checkAlarms();
    
    // Ignore touch pads for first 5 seconds after boot to avoid false triggers
    static const uint32_t bootIgnoreTime = 5000;
    
    // Poll touch pads with debouncing
    static bool startPressed = false;
    static bool stopPressed = false;
    static uint32_t lastDebounceTime = 0;
const uint32_t debounceDelay = DEBOUNCE_DELAY_MS;  // TTP223 has hardware debounce
    
    // Button 2 long-press detection
    static uint32_t button2PressStart = 0;
const uint32_t BUTTON2_LONG_PRESS = LONG_PRESS_MS;
    
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
            Serial.println("Button 2 pressed (start)");
        }
        
        // Button 2 long-press: Return to IDLE and start Gemini recording (from any mode)
        if (stopFallingEdge && !recordingActive && 
            (millis() - button2PressStart) >= BUTTON2_LONG_PRESS) {
            Serial.printf("Button 2 long-press (%lu ms): Returning to IDLE + starting recording\n",
                         millis() - button2PressStart);
            
            // Stop any active mode
            if (isPlayingAmbient) {
                JsonDocument stopDoc;
                stopDoc["action"] = "stopAmbient";
                String stopMsg;
                serializeJson(stopDoc, stopMsg);
                wsSendMessage(stopMsg);
                isPlayingResponse = false;
                isPlayingAmbient = false;
                ambientSound.active = false;
                ambientSound.name[0] = '\0';
                ambientSound.sequence++;
                i2s_zero_dma_buffer(I2S_NUM_1);
            }
            
            // Clear states
            moonState.active = false;
            tideState.active = false;
            timerState.active = false;
            isAmbientVUMode = false;
            
            // Clear Pomodoro
            if (pomodoroState.active) {
                pomodoroState.active = false;
                pomodoroState.paused = false;
                pomodoroState.startTime = 0;
                pomodoroState.pausedTime = 0;
            }
            
            // Clear Meditation
            if (meditationState.active) {
                Serial.println("CLEARING meditation state (button 2 long press)");
                meditationState.active = false;
                meditationState.phaseStartTime = 0;
                meditationState.streaming = false;
                volumeMultiplier = meditationState.savedVolume;  // Restore volume
                Serial.printf("Volume restored to %.0f%%\n", volumeMultiplier * 100);
            }
            
            // Clear Lamp
            if (lampState.active) {
                lampState.active = false;
                lampState.fullyLit = false;
            }
            
            // Clear Radio
            if (radioState.active) {
                if (radioState.savedVolume > 0.05f) {
                    volumeMultiplier = radioState.savedVolume;
                }
                radioState.active = false;
                radioState.streaming = false;
                radioState.stationName[0] = '\0';
                radioState.streamUrl[0] = '\0';
            }
            
            // Return to IDLE and start recording immediately
            currentLEDMode = LED_IDLE;
            
            // Start Gemini recording
            responseInterrupted = false;
            conversationRecording = false;
            tideState.active = false;  // New question - clear viz state
            moonState.active = false;
            recordingActive = true;
            recordingStartTime = millis();
            lastVoiceActivityTime = millis();
            currentLEDMode = LED_RECORDING;
            DEBUG_PRINTLN(" Recording started via long-press");
            
            stopPressed = stopTouch;
            lastDebounceTime = millis();
            return;  // Skip normal button handling
        }
        
        // STOP button short press: Cycle through modes
        // IDLE -> VU Mode -> Sea Jelly -> Ambient -> Radio -> Pomodoro -> Meditation -> Lamp -> Eyes -> IDLE
        // Allow during ambient modes, block only during Gemini responses (non-ambient)

        // Radio streaming: short press toggles VU visualizer, doesn't cycle mode
        bool radioVisualToggled = false;
        if (stopRisingEdge && !recordingActive && currentLEDMode == LED_RADIO && radioState.active && radioState.streaming) {
            radioState.visualsActive = !radioState.visualsActive;
            Serial.printf("Radio VU visuals: %s\n", radioState.visualsActive ? "ON" : "OFF");
            radioVisualToggled = true;
        }

        if (!radioVisualToggled && stopRisingEdge && !recordingActive &&
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
            
            LEDMode modeToCheck = currentLEDMode;
            
            // Cycle to next mode
            if (modeToCheck == LED_IDLE || modeToCheck == LED_MOON || 
                modeToCheck == LED_TIDE || modeToCheck == LED_TIMER) {
                // Show marquee before switching
                isAmbientVUMode = true;
                ambientSound.sequence++;  // Increment for mode change
                currentLEDMode = LED_AMBIENT_VU;
                DEBUG_PRINTLN(" Ambient VU meter mode enabled");
            } else if (modeToCheck == LED_AMBIENT_VU) {
                isAmbientVUMode = false;
                
                // Stop any in-flight ambient stream before entering a no-audio mode.
                // Defensive: AMBIENT_VU normally has no audio, but a stale stream could
                // still be pumping if VU was entered without explicitly stopping it.
                if (isPlayingAmbient) {
                    JsonDocument stopDoc;
                    stopDoc["action"] = "stopAmbient";
                    String stopMsg;
                    serializeJson(stopDoc, stopMsg);
                    wsSendMessage(stopMsg);
                    isPlayingAmbient = false;
                    ambientSound.active = false;
                }
                
                // Switch to Sea Gooseberry jellyfish mode (no audio needed)
                DEBUG_PRINTLN(" VU  Sea Gooseberry mode");
                seaGooseberry.begin();  // Initialize visualizer
                currentLEDMode = LED_SEA_GOOSEBERRY;
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
                Serial.println("Flushed audio queue for clean Jelly->Rain transition");
                
                currentAmbientSoundType = SOUND_RAIN;  // Start with rain
                strlcpy(ambientSound.name, "rain", sizeof(ambientSound.name));
                ambientSound.active = true;
                ambientSound.sequence++;
                isPlayingAmbient = true;
                isPlayingResponse = false;
                firstAudioChunk = true;
                lastAudioChunkTime = millis();  // Initialize timing
                Serial.printf("MODE: Rain (seq %d)\n", ambientSound.sequence);
                // Show marquee first
                currentLEDMode = LED_AMBIENT;
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
            } else if (modeToCheck == LED_AMBIENT) {
                // Stop ambient sound and enter Radio discovery mode
                DEBUG_PRINTLN(" Mode transition: AMBIENT  RADIO (cleaning up...)");
                
                // Clear audio buffer to prevent bleed
                i2s_zero_dma_buffer(I2S_NUM_1);
                delay(50);
                i2s_zero_dma_buffer(I2S_NUM_1);
                
                // Clear LED buffer (with mutex)
                if (xSemaphoreTake(ledMutex, portMAX_DELAY) == pdTRUE) {
                    fill_solid(leds, NUM_LEDS, CRGB::Black);
                    FastLED.show();
                    xSemaphoreGive(ledMutex);
                }
                delay(50);
                
                // Stop ambient audio
                if (isPlayingAmbient) {
                    JsonDocument stopDoc;
                    stopDoc["action"] = "stopAmbient";
                    String stopMsg;
                    serializeJson(stopDoc, stopMsg);
                    wsSendMessage(stopMsg);
                }
                isPlayingResponse = false;
                isPlayingAmbient = false;
                ambientSound.active = false;
                ambientSound.name[0] = '\0';
                ambientSound.sequence++;
                i2s_zero_dma_buffer(I2S_NUM_1);
                
                // Enter radio discovery mode
                radioState.active = true;
                radioState.streaming = false;
                radioState.visualsActive = true;
                radioState.stationName[0] = '\0';
                radioState.streamUrl[0] = '\0';
                volumeMultiplier = RADIO_DEFAULT_VOLUME;
                radioState.savedVolume = RADIO_DEFAULT_VOLUME;

                // Notify server: source=button triggers Gemini greeting
                {
                    JsonDocument radioNotifyDoc;
                    radioNotifyDoc["type"] = "radioModeActivated";
                    radioNotifyDoc["source"] = "button";
                    String radioNotifyMsg;
                    serializeJson(radioNotifyDoc, radioNotifyMsg);
                    wsSendMessage(radioNotifyMsg);
                }

                DEBUG_PRINTLN(" Radio discovery mode activated");
                currentLEDMode = LED_RADIO;

            } else if (modeToCheck == LED_RADIO) {
                // Exit Radio mode and enter Pomodoro mode
                DEBUG_PRINTLN(" Mode transition: RADIO  POMODORO (cleaning up...)");

                // Stop any radio stream
                if (isPlayingAmbient || radioState.streaming) {
                    JsonDocument stopDoc;
                    stopDoc["action"] = "stopAmbient";
                    String stopMsg;
                    serializeJson(stopDoc, stopMsg);
                    wsSendMessage(stopMsg);
                }

                // Restore volume if ducked
                if (radioState.savedVolume > 0.05f) {
                    volumeMultiplier = radioState.savedVolume;
                }

                // Clear radio state
                radioState.active = false;
                radioState.streaming = false;
                radioState.stationName[0] = '\0';
                radioState.streamUrl[0] = '\0';

                // Clear ambient/audio state
                isPlayingResponse = false;
                isPlayingAmbient = false;
                ambientSound.active = false;
                ambientSound.name[0] = '\0';
                ambientSound.sequence++;
                ambientSound.drainUntil = millis() + 2000;

                // Clear audio buffers
                {
                    AudioChunk dummy;
                    while (xQueueReceive(audioOutputQueue, &dummy, 0) == pdTRUE) {}
                }
                i2s_zero_dma_buffer(I2S_NUM_1);
                delay(50);
                i2s_zero_dma_buffer(I2S_NUM_1);

                // Clear LED buffer
                if (xSemaphoreTake(ledMutex, portMAX_DELAY) == pdTRUE) {
                    fill_solid(leds, NUM_LEDS, CRGB::Black);
                    FastLED.show();
                    xSemaphoreGive(ledMutex);
                }
                delay(50);

                // Initialize Pomodoro state if not already active
                if (!pomodoroState.active) {
                    pomodoroState.currentSession = PomodoroState::FOCUS;
                    pomodoroState.sessionCount = 0;
                    pomodoroState.totalSeconds = pomodoroState.focusDuration * 60;
                    pomodoroState.startTime = millis();
                    pomodoroState.pausedTime = 0;
                    pomodoroState.active = true;
                    pomodoroState.paused = false;
                }

                DEBUG_PRINTLN(" Pomodoro mode activated");
                playZenBell();
                currentLEDMode = LED_POMODORO;
            } else if (modeToCheck == LED_POMODORO) {
                // Exit Pomodoro and go to Meditation
                DEBUG_PRINTLN("  Pomodoro mode stopped");
                
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
                wsSendMessage(stopMsg);
                
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
                        DEBUG_PRINT(" INTERRUPTING meditation: Pomodoro mode transition (2x clear)\n");
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
                meditationState.phaseStartTime = 0; // Will sync to first arriving audio chunk
                meditationState.active = true;
                meditationState.streaming = false;
                
                // Lower volume for meditation ambiance
                meditationState.savedVolume = volumeMultiplier;
                volumeMultiplier = 0.10f;  // 10% volume for meditation
                DEBUG_PRINT(" Volume: %.0f%%  10%% for meditation\n", meditationState.savedVolume * 100);

                // Flush any in-flight audio (e.g. zen bell still draining from Pomodoro)
                {
                    AudioChunk dummy;
                    while (xQueueReceive(audioOutputQueue, &dummy, 0) == pdTRUE) {}
                }
                i2s_zero_dma_buffer(I2S_NUM_1);

                // Request ROOT chakra audio immediately
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
                currentLEDMode = LED_MEDITATION;
            } else if (modeToCheck == LED_MEDITATION) {
                // Exit Meditation and go to Lamp
                DEBUG_PRINTLN("  Meditation mode stopped");
                
                // Clear meditation state
                meditationState.active = false;
                meditationState.phaseStartTime = 0;
                meditationState.streaming = false;
                volumeMultiplier = meditationState.savedVolume;  // Restore volume
                Serial.printf("Volume restored to %.0f%%\n", volumeMultiplier * 100);
                
                // Stop any audio
                JsonDocument stopDoc;
                stopDoc["action"] = "stopAmbient";
                String stopMsg;
                serializeJson(stopDoc, stopMsg);
                wsSendMessage(stopMsg);
                
                // Clear audio and LEDs
                clearAudioAndLEDs();
                
                // Clear ambient state
                isPlayingResponse = false;
                isPlayingAmbient = false;
                ambientSound.active = false;
                ambientSound.name[0] = '\0';
                ambientSound.sequence++;
                
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
                
                DEBUG_PRINTLN(" Lamp mode activated");
                currentLEDMode = LED_LAMP;
            } else if (modeToCheck == LED_LAMP) {
                // Exit Lamp and go directly to Eye Animation mode
                lampState.active = false;
                lampState.fullyLit = false;
                
                // Clear lamp LEDs immediately to prevent a one-frame colour flash
                if (xSemaphoreTake(ledMutex, portMAX_DELAY) == pdTRUE) {
                    fill_solid(leds, NUM_LEDS, CRGB::Black);
                    FastLED.show();
                    xSemaphoreGive(ledMutex);
                }
                
                // Initialize Eye Animation visualizer
                eyeAnimation.begin();
                
                currentLEDMode = LED_EYES;
            } else if (modeToCheck == LED_EYES) {
                // Exit Eye Animation and return to IDLE
                // Clear LED buffer (with mutex)
                if (xSemaphoreTake(ledMutex, portMAX_DELAY) == pdTRUE) {
                    fill_solid(leds, NUM_LEDS, CRGB::Black);
                    FastLED.show();
                    xSemaphoreGive(ledMutex);
                }
                
                // Stop audio
                JsonDocument stopDoc;
                stopDoc["action"] = "stopAmbient";
                String stopMsg;
                serializeJson(stopDoc, stopMsg);
                wsSendMessage(stopMsg);
                
                // Set 2-second drain period
                ambientSound.drainUntil = millis() + 2000;
                
                // Return to IDLE
                currentLEDMode = LED_IDLE;
            }
        }
        
        // Display-only modes: Button 1 disabled (button 2 advances to next mode)
        if ((currentLEDMode == LED_AMBIENT_VU || currentLEDMode == LED_SEA_GOOSEBERRY || currentLEDMode == LED_EYES) && startRisingEdge) {
            DEBUG_PRINTLN("Button 1 disabled in this mode - use button 2 to advance");
            // Do nothing - button 1 is disabled in display-only modes
        }
        // Ambient mode: Button 1 cycles to ambient sounds
        else {
            static uint32_t lastAmbientCycle = 0;
            if (currentLEDMode == LED_AMBIENT && startRisingEdge &&
                (millis() - lastAmbientCycle) > 500) {  // 500ms debounce
            lastAmbientCycle = millis();
            
            // Stop current audio (if any)
            JsonDocument stopDoc;
            stopDoc["action"] = "stopAmbient";
            String stopMsg;
            serializeJson(stopDoc, stopMsg);
            wsSendMessage(stopMsg);
            i2s_zero_dma_buffer(I2S_NUM_1);
            
            // If coming from Sea Gooseberry, go to Rain
            if (currentLEDMode == LED_SEA_GOOSEBERRY) {
                currentAmbientSoundType = SOUND_RAIN;
                strlcpy(ambientSound.name, "rain", sizeof(ambientSound.name));
                Serial.printf("MODE: Rain (seq %d)\n", ambientSound.sequence + 1);
            }
            // Cycle to next sound in Ambient mode
            else if (currentAmbientSoundType == SOUND_RAIN) {
                currentAmbientSoundType = SOUND_OCEAN;
                strlcpy(ambientSound.name, "ocean", sizeof(ambientSound.name));
                Serial.printf("MODE: Ocean (seq %d)\n", ambientSound.sequence + 1);
            } else if (currentAmbientSoundType == SOUND_OCEAN) {
                currentAmbientSoundType = SOUND_RAINFOREST;
                strlcpy(ambientSound.name, "rainforest", sizeof(ambientSound.name));
                Serial.printf("MODE: Rainforest (seq %d)\n", ambientSound.sequence + 1);
            } else if (currentAmbientSoundType == SOUND_RAINFOREST) {
                currentAmbientSoundType = SOUND_FIRE;
                strlcpy(ambientSound.name, "fire", sizeof(ambientSound.name));
                Serial.printf("MODE: Fire (seq %d)\n", ambientSound.sequence + 1);
            } else {  // SOUND_FIRE
                currentAmbientSoundType = SOUND_RAIN;
                strlcpy(ambientSound.name, "rain", sizeof(ambientSound.name));
                Serial.printf("MODE: Rain (seq %d)\n", ambientSound.sequence + 1);
            }
            
            // Update state
            ambientSound.sequence++;
            isPlayingAmbient = true;
            isPlayingResponse = false;
            firstAudioChunk = true;
            lastAudioChunkTime = millis();
            currentLEDMode = LED_AMBIENT;
            
            // Request new sound from server immediately
            {
                JsonDocument ambientCycleDoc;
                ambientCycleDoc["action"] = "requestAmbient";
                ambientCycleDoc["sound"] = ambientSound.name;
                ambientCycleDoc["sequence"] = ambientSound.sequence;
                String ambientCycleMsg;
                serializeJson(ambientCycleDoc, ambientCycleMsg);
                Serial.printf("Ambient audio request: %s (seq %d)\n", ambientCycleMsg.c_str(), ambientSound.sequence);
                wsSendMessage(ambientCycleMsg);
            }
            }
        }
        
        // Pomodoro mode: Button 1 long press = pause/resume, short press = Gemini
        // Button 2 cycles modes as usual
        static uint32_t button1PressStart = 0;
        static uint32_t lastPomodoroAction = 0;
        const uint32_t LONG_PRESS_DURATION = LONG_PRESS_MS;  // 2 seconds
        const uint32_t ACTION_DEBOUNCE = 500;  // 500ms between actions
        
        if (currentLEDMode == LED_POMODORO && pomodoroState.active) {
            // Detect button 1 press start -> immediately start recording so VU meter shows while talking
            if (startRisingEdge && !recordingActive && !isPlayingResponse && !isPlayingAmbient && !conversationMode && !alarmState.ringing) {
                button1PressStart = millis();
                // Start recording immediately for visual VU feedback; long press will cancel it below
                responseInterrupted = false;
                conversationRecording = false;
                tideState.active = false;
                moonState.active = false;
                if (ambientSound.drainUntil > 0) { ambientSound.drainUntil = 0; }
                recordingActive = true;
                recordingStartTime = millis();
                lastVoiceActivityTime = millis();
                currentLEDMode = LED_RECORDING;
                DEBUG_PRINTLN("Pomodoro button pressed - recording + VU active");
            } else if (startRisingEdge) {
                // Track press start even when recording couldn't start (e.g. response playing)
                button1PressStart = millis();
            }
            
            // On button 1 release, check duration
            if (startFallingEdge && (millis() - lastPomodoroAction) > ACTION_DEBOUNCE) {
                uint32_t pressDuration = millis() - button1PressStart;
                
                if (pressDuration >= LONG_PRESS_DURATION) {
                    // Long press: cancel any recording started on press, then toggle pause/resume
                    if (recordingActive) {
                        recordingActive = false;
                        currentLEDMode = LED_POMODORO;
                        DEBUG_PRINTLN("Long press - cancelled recording, toggling pause/resume");
                    }
                    if (pomodoroState.paused) {
                        // Resume from paused state
                        // Use the totalSeconds already stored when the session started.
                        // Do NOT re-derive from focusDuration etc.  those may have been changed
                        // by voice command between when the session started and when it was paused.
                        int timeAlreadyElapsed = pomodoroState.totalSeconds - pomodoroState.pausedTime;
                        pomodoroState.startTime = millis() - (timeAlreadyElapsed * 1000);
                        pomodoroState.pausedTime = 0;
                        pomodoroState.paused = false;
                        
                        DEBUG_PRINT("  Pomodoro resumed from %d seconds remaining (long press)\n", pomodoroState.totalSeconds - timeAlreadyElapsed);
                        // No sound on resume - user will source alternative
                    } else {
                        // Pause and save current position
                        uint32_t elapsed = (millis() - pomodoroState.startTime) / 1000;
                        pomodoroState.pausedTime = max(0, pomodoroState.totalSeconds - (int)elapsed);
                        pomodoroState.startTime = 0;
                        pomodoroState.paused = true;
                        DEBUG_PRINT("  Pomodoro paused at %d seconds remaining (long press)\n", pomodoroState.pausedTime);
                        // No sound on pause - user will source alternative
                    }
                    lastPomodoroAction = millis();
                }
                // Short press: recording already started on press, continues naturally
            }
        }
        
        // MEDITATION MODE: Button 1 advances to next chakra (must come BEFORE interrupt/recording handlers)
        // Same pattern as ambient mode sound cycling
        bool meditationHandled = false;  // Flag to skip other handlers
        if (currentLEDMode == LED_MEDITATION && meditationState.active && startRisingEdge) {
            meditationHandled = true;  // Mark as handled to skip other button handlers
            
            Serial.printf("Button 1: Advancing from chakra %d (%s) | edge detected\n", 
                         meditationState.currentChakra, CHAKRA_NAMES[meditationState.currentChakra]);
            
            // CRITICAL: Stop audio playback completely to prevent glitches
            isPlayingAmbient = false;  // Stop audio task from playing queued packets
            isPlayingResponse = false;
            
            // Stop current audio immediately
            JsonDocument stopDoc;
            stopDoc["action"] = "stopAmbient";
            String stopMsg;
            serializeJson(stopDoc, stopMsg);
            wsSendMessage(stopMsg);
            
            // Clear I2S hardware buffer
            i2s_zero_dma_buffer(I2S_NUM_1);
            
            // Flush the software audio queue (removes buffered packets)
            AudioChunk dummy;
            while (xQueueReceive(audioOutputQueue, &dummy, 0) == pdTRUE) {
                // Drain all queued audio packets
            }
            Serial.printf("Flushed audio queue for clean transition\n");
            
            // Set drain period to discard any stale packets still in flight
            ambientSound.drainUntil = millis() + 500;  // 500ms drain window
            
            // Check if we can advance
            if (meditationState.currentChakra < MeditationState::CROWN) {
                // Advance to next chakra (breathing continues smoothly)
                meditationState.currentChakra = (MeditationState::Chakra)(meditationState.currentChakra + 1);
                
                Serial.printf("Advanced to chakra %d (%s) - breathing continues\n", 
                             meditationState.currentChakra, CHAKRA_NAMES[meditationState.currentChakra]);
                
                // Request new chakra sound (bell001, bell002, ..., bell007)
                char soundName[16];
                sprintf(soundName, "bell%03d", meditationState.currentChakra + 1);
                
                JsonDocument reqDoc;
                reqDoc["action"] = "requestAmbient";
                reqDoc["sound"] = soundName;
                reqDoc["loops"] = 8;
                reqDoc["sequence"] = ++ambientSound.sequence;
                String reqMsg;
                serializeJson(reqDoc, reqMsg);
                wsSendMessage(reqMsg);
                
                strlcpy(ambientSound.name, soundName, sizeof(ambientSound.name));
                ambientSound.active = true;
                isPlayingAmbient = true;  // Re-enable playback for new sound
                isPlayingResponse = false;
                firstAudioChunk = true;
                lastAudioChunkTime = millis();
                
                Serial.printf("Chakra advance complete: %s ready to stream\n", soundName);
            } else {
                // At final chakra - complete meditation
                Serial.println("At CROWN chakra - meditation complete");
                
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
                Serial.printf("Volume restored to %.0f%%\n", volumeMultiplier * 100);
                
                // Clear ambient audio state
                ambientSound.active = false;
                ambientSound.name[0] = '\0';
                ambientSound.sequence++;
                ambientSound.drainUntil = millis() + 1000;  // Drain for 1s
                
                // Clear LEDs (with mutex)
                if (xSemaphoreTake(ledMutex, portMAX_DELAY) == pdTRUE) {
                    fill_solid(leds, NUM_LEDS, CRGB::Black);
                    FastLED.show();
                    xSemaphoreGive(ledMutex);
                }
                
                Serial.println("Meditation state fully cleared - returning to idle");
                
                // Return to idle
                currentLEDMode = LED_IDLE;
            }
        }
        // Interrupt feature: START button during active playback stops audio and starts recording
        // Only interrupt if we've received audio recently (within 500ms) and turn is not complete
        // Note: Pomodoro now allowed - button 1 can interrupt responses during Pomodoro
        else if (!meditationHandled && currentLEDMode != LED_MEDITATION && startRisingEdge && isPlayingResponse && !turnComplete && 
            (millis() - lastAudioChunkTime) < 500) {
            DEBUG_PRINTLN("  Interrupted response - starting new recording");
            responseInterrupted = true;  // Flag to ignore remaining audio chunks
            isPlayingResponse = false;
            i2s_zero_dma_buffer(I2S_NUM_1);  // Stop audio immediately
            tideState.active = false;
            moonState.active = false;
            recordingActive = true;
            recordingStartTime = millis();
            lastVoiceActivityTime = millis();
            transitionConvState(ConvState::RECORDING);
            currentLEDMode = LED_RECORDING;
            DEBUG_PRINT(" Recording started... (START=%d, STOP=%d)\n", startTouch, stopTouch);
        }
        // ALARM MODE: Button 1 or 2 dismisses alarm
        else if (currentLEDMode == LED_ALARM && alarmState.ringing && (startRisingEdge || stopRisingEdge)) {
            DEBUG_PRINTLN(" Alarm dismissed");
            
            // Clear alarm from memory
            for (int i = 0; i < MAX_ALARMS; i++) {
                if (alarms[i].enabled && alarms[i].triggered) {
                    DEBUG_PRINT(" Alarm %u dismissed and cleared from slot %d\n", alarms[i].alarmID, i);
                    
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
            wsSendMessage(stopMsg);
            DEBUG_PRINTLN(" Sent stop alarm request to server");
            
            // Stop I2S output - let buffered audio drain naturally
            i2s_zero_dma_buffer(I2S_NUM_1);
            
            // Don't clear the queue - let audio task handle cleanup naturally
            // (clearing queue while audio task is active causes crashes)
            
            // Restore previous mode
            DEBUG_PRINT("  Restoring previous mode: %d (recording=%d, playing=%d)\n", 
                         alarmState.previousMode, alarmState.wasRecording, alarmState.wasPlayingResponse);
            
            currentLEDMode = alarmState.previousMode;
            
            // Restore recording state if it was active
            if (alarmState.wasRecording) {
                recordingActive = true;
                DEBUG_PRINTLN("  Resuming recording");
            }
            
            // Restore playback state if it was active
            if (alarmState.wasPlayingResponse) {
                isPlayingResponse = true;
                lastAudioChunkTime = millis();  // Reset timeout
                DEBUG_PRINTLN("  Resuming audio playback");
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
        // LAMP MODE: Button 1 cycles colors (WHITE  RED  GREEN  BLUE)
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
            
            DEBUG_PRINT(" Lamp color: %s  %s\n", 
                         (const char*[]){"WHITE", "RED", "GREEN", "BLUE"}[lampState.previousColor], 
                         (const char*[]){"WHITE", "RED", "GREEN", "BLUE"}[lampState.currentColor]);
        }
        // Radio: Button 1 pauses the stream then starts a Gemini recording.
        // Pausing (not just ducking) avoids mixed-mode drain complexity and lets the
        // user ask Gemini to change station freely. Stream resumes after turn completes.
        // Guard convState==IDLE: don't re-record while already waiting for Gemini's response.
        else if (!meditationHandled && currentLEDMode == LED_RADIO && radioState.active &&
                 startRisingEdge && !recordingActive && !conversationMode && convState == ConvState::IDLE) {
            // Stop the server-side stream
            {
                JsonDocument stopDoc;
                stopDoc["action"] = "stopAmbient";
                String stopMsg;
                serializeJson(stopDoc, stopMsg);
                wsSendMessage(stopMsg);
            }
            // Drain local audio queue and silence I2S
            { AudioChunk dummy; while (xQueueReceive(audioOutputQueue, &dummy, 0) == pdTRUE) {} }
            i2s_zero_dma_buffer(I2S_NUM_1);
            isPlayingAmbient = false;
            isPlayingResponse = false;
            ambientSound.active = false;
            ambientSound.drainUntil = millis() + 500;
            radioState.paused = true;      // remember to resume after the turn
            radioState.streaming = false;   // clear stale flag — prevents volume restore logic
                                            // from reverting a set_volume_level change at resume
            Serial.println("Radio: stream paused for voice command");
            // Start recording
            responseInterrupted = false;
            conversationRecording = false;
            tideState.active = false;
            moonState.active = false;
            recordingActive = true;
            recordingStartTime = millis();
            lastVoiceActivityTime = millis();
            transitionConvState(ConvState::RECORDING);
            currentLEDMode = LED_RECORDING;
            DEBUG_PRINT(" Radio recording started...\n");
        }
        // Start recording on rising edge (normal case - not interrupting)
        // Block if: recording active, playing response, OR in ambient sound mode, OR conversation window is open,
        // OR in any display-only/non-Gemini mode (Meditation, Ambient, Lamp, VU, SeaJelly, Pomodoro, Eyes)
        // Note: Pomodoro recording is handled above (starts on press for instant VU feedback)
        else if (!meditationHandled && currentLEDMode != LED_MEDITATION && currentLEDMode != LED_AMBIENT && currentLEDMode != LED_AMBIENT_VU && currentLEDMode != LED_SEA_GOOSEBERRY && currentLEDMode != LED_LAMP && currentLEDMode != LED_POMODORO && currentLEDMode != LED_EYES && !recordingActive && !isPlayingResponse && !isPlayingAmbient && !conversationMode) {
            bool shouldStartRecording = startRisingEdge;
            
            if (shouldStartRecording) {
            // Additional safety: don't start recording if alarm is ringing
            if (alarmState.ringing) {
                DEBUG_PRINTLN("  Cannot start recording - alarm is ringing");
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
                DEBUG_PRINTLN(" Cancelled drain timer - ready for new audio");
                ambientSound.drainUntil = 0;
            }
            
            // Exit ambient VU mode
            if (isAmbientVUMode) {
                isAmbientVUMode = false;
                DEBUG_PRINTLN(" Ambient VU meter mode disabled");
            }

            tideState.active = false;  // New question - clear viz state so it doesn't re-display
            moonState.active = false;
            recordingActive = true;
            recordingStartTime = millis();
            lastVoiceActivityTime = millis();
            transitionConvState(ConvState::RECORDING);
            currentLEDMode = LED_RECORDING;
            DEBUG_PRINT(" Recording started... (START=%d, STOP=%d)\n", startTouch, stopTouch);
            }  // End of shouldStartRecording block
        }
        
        // Stop recording only on timeout (manual stop removed - rely on VAD)
        if (recordingActive && (millis() - recordingStartTime) > MAX_RECORDING_DURATION_MS) {
            recordingActive = false;
            wsSendMessage("{\"type\":\"recordingStop\"}");
            Serial.println("[WS] recordingStop sent");
            if (radioState.active) {
                // Radio: return to radio display while waiting for Gemini
                currentLEDMode = LED_RADIO;
                transitionConvState(ConvState::WAITING);
            } else if (!ambientSound.active) {
                currentLEDMode = LED_PROCESSING;
                transitionConvState(ConvState::WAITING);
            }
            DEBUG_PRINT("  Recording stopped - Duration: %dms (max duration reached)\n", millis() - recordingStartTime);
        }
        
        lastDebounceTime = millis();
    }
    
    // Auto-stop on silence (VAD)
    if (recordingActive && (millis() - lastVoiceActivityTime) > VAD_SILENCE_MS) {
        recordingActive = false;
        wsSendMessage("{\"type\":\"recordingStop\"}");
        Serial.println("[WS] recordingStop sent");
        conversationRecording = false;  // Reset flag for next recording
        if (radioState.active) {
            // Radio: return to radio display while waiting for Gemini
            currentLEDMode = LED_RADIO;
            transitionConvState(ConvState::WAITING);
        } else if (!ambientSound.active) {
            // Don't change LED mode if ambient sound is already active
            currentLEDMode = LED_PROCESSING;
            transitionConvState(ConvState::WAITING);
        }
        DEBUG_PRINTLN("  Recording stopped - Silence detected");
    }
    
    // Show thinking animation once we've been WAITING long enough.
    // Guard on !isPlayingResponse: once audio starts arriving the VU meter must win.
    if (convState == ConvState::WAITING &&
        !isPlayingResponse &&
        (millis() - waitingEnteredAt) > THINKING_ANIMATION_DELAY_MS) {
        currentLEDMode = LED_PROCESSING;
        DEBUG_PRINTLN(" Response delayed - showing thinking animation");
    }

    // Hard timeout: Gemini never responded (accommodates 40+ second tool calls).
    // Checked against convState==WAITING, not against currentLEDMode, so visualizations
    // (tide/moon/timer) cannot accidentally reset or suppress the guard.
    if (convState == ConvState::WAITING &&
        (millis() - waitingEnteredAt) > 60000) {
        Serial.printf("  Gemini response timeout after 60s (LED=%d)\n", currentLEDMode);
        transitionConvState(ConvState::IDLE);
        
        // Return to visualizations if active, otherwise IDLE
        if (pomodoroState.active) {
            currentLEDMode = LED_POMODORO;
            DEBUG_PRINTLN("  Timeout - returning to POMODORO display");
        } else if (timerState.active) {
            currentLEDMode = LED_TIMER;
            DEBUG_PRINTLN("  Timeout - returning to TIMER display");
        } else if (moonState.active) {
            currentLEDMode = LED_MOON;
            moonState.displayStartTime = millis();
            DEBUG_PRINTLN("  Timeout - returning to MOON display");
        } else if (tideState.active) {
            currentLEDMode = LED_TIDE;
            tideState.displayStartTime = millis();
            DEBUG_PRINTLN("  Timeout - returning to TIDE display");
        } else if (radioState.active) {
            volumeMultiplier = radioState.savedVolume;  // Restore any ducked volume
            if (radioState.paused) {
                if (radioState.streamUrl[0] != '\0') {
                    resumeRadioStream();
                } else {
                    radioState.paused = false;  // Discovery mode — no stream to resume
                    Serial.println("Radio: discovery mode, no stream URL to resume after timeout");
                }
            }
            currentLEDMode = LED_RADIO;
            DEBUG_PRINTLN("  Timeout - returning to RADIO");
        } else {
            currentLEDMode = LED_IDLE;
            DEBUG_PRINTLN("  Timeout - returning to IDLE");
        }
    }

    // Hard timeout: audio drained into PLAYING state but turnComplete never arrived.
    // Prevents the device being silently stuck with no LEDs and no response.
    if (convState == ConvState::PLAYING &&
        !isPlayingResponse &&
        waitingEnteredAt > 0 &&
        (millis() - waitingEnteredAt) > 60000) {
        Serial.printf("  PLAYING timeout - turnComplete never arrived after 60s\n");
        transitionConvState(ConvState::IDLE);
        if (pomodoroState.active) {
            currentLEDMode = LED_POMODORO;
        } else if (timerState.active) {
            currentLEDMode = LED_TIMER;
        } else if (radioState.active) {
            volumeMultiplier = radioState.savedVolume;  // Restore any ducked volume
            if (radioState.paused) {
                if (radioState.streamUrl[0] != '\0') {
                    resumeRadioStream();
                } else {
                    radioState.paused = false;  // Discovery mode — no stream to resume
                    Serial.println("Radio: discovery mode, no stream URL to resume after PLAYING timeout");
                }
            }
            currentLEDMode = LED_RADIO;
        } else {
            currentLEDMode = LED_IDLE;
        }
    }
    
    // Stale-RECORDING repair: if recordingActive was cleared internally (e.g. PREBUF stopping
    // an overlapping recording) but convState was never transitioned away from RECORDING,
    // the drain guard below (which only skips for IDLE) will fire immediately on every radio
    // packet because lastGeminiAudioTime is stale during pure radio. Detect and repair here.
    if (convState == ConvState::RECORDING && !recordingActive && isPlayingAmbient) {
        Serial.println("[STATE] Stale RECORDING+ambient detected — repairing to IDLE");
        transitionConvState(ConvState::IDLE);
        if (radioState.active) currentLEDMode = LED_RADIO;
    }

    // Check for audio playback completion
    // Timeout only if BOTH: (1) no new packets for 2s AND (2) queue nearly drained
    // This prevents cutting off audio that's still queued but TCP-delayed
    // Skip drain check for pure radio/ambient whenever no Gemini turn is in progress.
    // The WAITING/PLAYING states represent an active Gemini turn; all other states mean
    // the audio pipeline is carrying pure ambient/radio data that ends via radioEnded or
    // ambientComplete messages — not via this drain timer. Using the Gemini-only timer
    // (lastGeminiAudioTime) in those states causes an immediate drain cycle because
    // lastGeminiAudioTime is never updated by ambient/radio (0xA5 0x5A) packets.
    if (isPlayingResponse && !(isPlayingAmbient &&
            convState != ConvState::WAITING && convState != ConvState::PLAYING)) {
        // Transition to PLAYING as soon as audio is confirmed active (cancels thinking animation)
        if (convState == ConvState::WAITING) {
            transitionConvState(ConvState::PLAYING);
        }
        uint32_t queueDepth = uxQueueMessagesWaiting(audioOutputQueue);
        // Mixed mode (radio + Gemini): radio keeps lastAudioChunkTime and queue populated,
        // so use Gemini-specific timer and skip queue-depth check.
        bool noNewPackets = isPlayingAmbient
            ? (millis() - lastGeminiAudioTime) > 3000   // mixed: Gemini-only timer
            : (millis() - lastAudioChunkTime)  > 300;   // solo: 300ms matches reduced DMA pipeline (~128ms)
        bool queueDrained = !isPlayingAmbient && queueDepth < 3;  // skip in mixed mode
        
        if (noNewPackets && (queueDrained || isPlayingAmbient)) {
            isPlayingResponse = false;
            Serial.printf("[DRAIN] Audio done: queueDepth=%u, turnComplete=%d, convState=%d, waitAge=%dms\n",
                         queueDepth, turnComplete, (int)convState,
                         waitingEnteredAt > 0 ? (int)(millis() - waitingEnteredAt) : -1);

        // Check if turn is complete - if so, decide what to show
        if (turnComplete) {
            LEDMode effectiveMode = currentLEDMode;
            bool isPersistentMode = (effectiveMode == LED_MEDITATION ||
                                     effectiveMode == LED_AMBIENT   ||
                                     effectiveMode == LED_RADIO     ||
                                     effectiveMode == LED_LAMP      ||
                                     effectiveMode == LED_POMODORO  ||
                                     radioState.active              ||  // Radio discovery mode (LED may be PROCESSING)
                                     meditationState.active         ||  // Voice-triggered meditation may be in AUDIO_REACTIVE during verbal response
                                     lampState.active);                 // Voice-triggered lamp may be in AUDIO_REACTIVE during verbal response
            if (!isPersistentMode) {
                // Tide/Moon: show the visualization for 10s first, then auto-transition opens window.
                // The auto-transition block (convState==WAITING + tideState.active + 10s timer)
                // handles the tide→window transition correctly.
                if (tideState.active) {
                    transitionConvState(ConvState::WAITING);
                    currentLEDMode = LED_TIDE;
                    tideState.displayStartTime = millis();
                    Serial.println("Tide active - displaying 10s then opening conversation window");
                } else if (moonState.active) {
                    transitionConvState(ConvState::WAITING);
                    currentLEDMode = LED_MOON;
                    moonState.displayStartTime = millis();
                    Serial.println("Moon active - displaying 10s then opening conversation window");
                } else {
                // Open 10-second conversation window
                transitionConvState(ConvState::WINDOW);
                conversationMode = true;
                conversationWindowStart = millis();
                conversationVADDetected = false;  // Flush any residual VAD from speaker resonance
                currentLEDMode = LED_CONVERSATION_WINDOW;
                Serial.println("Conversation window opened - speak anytime in next 10 seconds");
                }
            } else {
                Serial.printf("Skipping conversation window - entering persistent mode %d\n", effectiveMode);
                transitionConvState(ConvState::IDLE);  // Persistent mode: no follow-up window

                // Radio: restore volume and resume stream after Gemini's verbal response finishes
                if (radioState.active) {
                    if (radioState.streaming && volumeMultiplier < radioState.savedVolume) {
                        volumeMultiplier = radioState.savedVolume;
                        Serial.printf("Radio: volume restored to %.0f%% after Gemini response\n", volumeMultiplier * 100);
                    }
                    if (radioState.paused) {
                        if (radioState.streamUrl[0] != '\0') {
                            resumeRadioStream();
                        } else {
                            radioState.paused = false;  // Discovery mode — no stream to resume
                            Serial.println("Radio: discovery mode, no stream URL to resume after response");
                        }
                    }
                }

                // Deferred meditation start: if meditationStart arrived while Gemini was still
                // speaking, phaseStartTime was set to 0 (waiting). Now that audio is done, start it.
                if (meditationState.active && meditationState.phaseStartTime == 0 && !meditationState.streaming) {
                    meditationState.phaseStartTime = 0; // Stays 0 until first audio chunk arrives
                    // Flush any residual Gemini audio still in queue
                    { AudioChunk dummy; while (xQueueReceive(audioOutputQueue, &dummy, 0) == pdTRUE) {} }
                    i2s_zero_dma_buffer(I2S_NUM_1);
                    strlcpy(ambientSound.name, "bell001", sizeof(ambientSound.name));
                    ambientSound.active = true;
                    isPlayingAmbient = true;
                    isPlayingResponse = false;
                    firstAudioChunk = true;
                    lastAudioChunkTime = millis();
                    JsonDocument meditationReqDoc;
                    meditationReqDoc["action"] = "requestAmbient";
                    meditationReqDoc["sound"] = "bell001";
                    meditationReqDoc["loops"] = 8;
                    meditationReqDoc["sequence"] = ++ambientSound.sequence;
                    String meditationReqMsg;
                    serializeJson(meditationReqDoc, meditationReqMsg);
                    Serial.printf("Meditation deferred start: %s (seq %d)\n", meditationReqMsg.c_str(), ambientSound.sequence);
                    wsSendMessage(meditationReqMsg);
                    meditationState.streaming = true;
                    Serial.println("Meditation breathing and audio started (ROOT chakra, deferred)");
                }

                // If Gemini's verbal response set currentLEDMode to a transient state (AUDIO_REACTIVE
                // etc.), we need to explicitly return to the active persistent mode now that audio is done.
                if (currentLEDMode == LED_AUDIO_REACTIVE || currentLEDMode == LED_RECORDING || currentLEDMode == LED_PROCESSING) {
                    if (meditationState.active) {
                        currentLEDMode = LED_MEDITATION;
                        Serial.println("Returning to MEDITATION after Gemini response");
                    } else if (radioState.active) {
                        currentLEDMode = LED_RADIO;
                        Serial.println("Returning to RADIO after Gemini response");
                    } else if (pomodoroState.active) {
                        currentLEDMode = LED_POMODORO;
                        Serial.println("Returning to POMODORO after Gemini response");
                    } else if (ambientSound.active) {
                        currentLEDMode = LED_AMBIENT;
                        Serial.println("Returning to AMBIENT after Gemini response");
                    } else if (lampState.active) {
                        currentLEDMode = LED_LAMP;
                        Serial.println("Returning to LAMP after Gemini response");
                    }
                }
            }
        } else {
            // Audio finished but turnComplete not yet received.
            // Stay in PLAYING — do not re-enter WAITING. Re-entering WAITING restarts the
            // 60s timeout and re-triggers the thinking animation (dark-screen glitch).
            // The auto-transition block below handles turnComplete arriving while PLAYING.
            // Show PROCESSING so user knows we're still waiting - NOT idle
            // Pomodoro/Meditation/Timer/viz take priority if active
            if (pomodoroState.active) {
                currentLEDMode = LED_POMODORO;
                DEBUG_PRINTLN(" Audio playback complete - switching to POMODORO display");
            } else if (meditationState.active) {
                currentLEDMode = LED_MEDITATION;
                DEBUG_PRINTLN(" Audio playback complete - returning to MEDITATION");
            } else if (timerState.active) {
                currentLEDMode = LED_TIMER;
                DEBUG_PRINTLN(" Audio playback complete - switching to TIMER display");
            } else if (moonState.active) {
                currentLEDMode = LED_MOON;
                moonState.displayStartTime = millis();
                DEBUG_PRINTLN(" Audio playback complete - switching to MOON display");
            } else if (tideState.active) {
                currentLEDMode = LED_TIDE;
                tideState.displayStartTime = millis();
                DEBUG_PRINT(" Audio playback complete - switching to TIDE display (state=%s, level=%.2f)\n", 
                             tideState.state, tideState.waterLevel);
            } else if (radioState.active) {
                currentLEDMode = LED_RADIO;
                DEBUG_PRINTLN(" Audio playback complete - returning to RADIO (waiting for turnComplete)");
            } else if (isAmbientVUMode) {
                currentLEDMode = LED_AMBIENT_VU;
                DEBUG_PRINTLN(" Audio playback complete - returning to AMBIENT VU mode");
            } else {
                currentLEDMode = LED_PROCESSING;
                DEBUG_PRINTLN(" Audio playback complete - waiting for turnComplete (LED_PROCESSING)");
            }
        }
        }
    }
    
    handleFlashAnimations();
    
    // Update Sea Gooseberry animation (non-blocking)
    if (currentLEDMode == LED_SEA_GOOSEBERRY) {
        seaGooseberry.update(millis());
    }
    
    // Update Eye Animation (non-blocking)
    if (currentLEDMode == LED_EYES) {
        eyeAnimation.update(millis());
    }
    
    handlePomodoroTick();
    
    handleAmbientCompletion();
    
    // Periodic state dump while WAITING for Gemini response (shows thinking animation, timeout countdown)
    if (convState == ConvState::WAITING) {
        static uint32_t lastWaitingDump = 0;
        if (millis() - lastWaitingDump > 3000) {
            Serial.printf("[WAIT] LED=%d turnComplete=%d isPlaying=%d recording=%d waitAge=%dms\n",
                         currentLEDMode, turnComplete, isPlayingResponse, recordingActive,
                         waitingEnteredAt > 0 ? (int)(millis() - waitingEnteredAt) : -1);
            lastWaitingDump = millis();
        }
    }

    // Auto-transition to conversation window when Gemini's turn is complete.
    // In WAITING state, convState already tells us we're expecting turnComplete.
    // Tide/Moon visualizations are respected with their own display timers.
    if (turnComplete && !conversationMode && !isPlayingResponse && !recordingActive) {
        bool shouldOpenConversation = false;

        if (convState == ConvState::WAITING) {
            // General case: covers zero-audio turns, short-audio turns where audio drained
            // before turnComplete, and viz turns where audio finished + waiting for complete.
            // Tide / Moon get extra display time before the window opens.
            if (currentLEDMode == LED_TIDE && tideState.active) {
                if (millis() - tideState.displayStartTime > 10000) {
                    Serial.println("Tide display complete - opening conversation window");
                    tideState.active = false;
                    shouldOpenConversation = true;
                }
            } else if (currentLEDMode == LED_MOON && moonState.active) {
                if (millis() - moonState.displayStartTime > 10000) {
                    Serial.println("Moon display complete - opening conversation window");
                    moonState.active = false;
                    shouldOpenConversation = true;
                }
            } else {
                // Same persistent-mode guard as the PLAYING path — prevents spurious
                // conversation windows popping up over radio/meditation/lamp.
                bool isPersistentMode = (currentLEDMode == LED_MEDITATION ||
                                         currentLEDMode == LED_AMBIENT   ||
                                         currentLEDMode == LED_RADIO     ||
                                         currentLEDMode == LED_LAMP      ||
                                         currentLEDMode == LED_POMODORO  ||
                                         radioState.active              ||
                                         meditationState.active         ||
                                         lampState.active);
                if (isPersistentMode) {
                    Serial.printf("turnComplete in WAITING state - persistent mode %d, skipping window\n", currentLEDMode);
                    transitionConvState(ConvState::IDLE);
                    if (radioState.active && radioState.streaming && volumeMultiplier < radioState.savedVolume) {
                        volumeMultiplier = radioState.savedVolume;
                        Serial.printf("Radio: volume restored to %.0f%% after Gemini response\n", volumeMultiplier * 100);
                    }
                    if (currentLEDMode == LED_AUDIO_REACTIVE || currentLEDMode == LED_RECORDING || currentLEDMode == LED_PROCESSING) {
                        if (radioState.active)           { currentLEDMode = LED_RADIO; }
                        else if (meditationState.active) { currentLEDMode = LED_MEDITATION; }
                        else if (pomodoroState.active)   { currentLEDMode = LED_POMODORO; }
                        else if (ambientSound.active)    { currentLEDMode = LED_AMBIENT; }
                        else if (lampState.active)       { currentLEDMode = LED_LAMP; }
                    }
                } else {
                    Serial.printf("turnComplete in WAITING state (LED=%d, waitAge=%dms) - opening conversation window\n",
                                 currentLEDMode,
                                 waitingEnteredAt > 0 ? (int)(millis() - waitingEnteredAt) : -1);
                    shouldOpenConversation = true;
                }
            }
        } else if (convState == ConvState::PLAYING) {
            // turnComplete arrived after audio already drained (device stayed in PLAYING).
            // Decide whether to open a conversation window or return to a persistent mode.
            LEDMode effectiveMode = currentLEDMode;
            bool isPersistentMode = (effectiveMode == LED_MEDITATION ||
                                     effectiveMode == LED_AMBIENT   ||
                                     effectiveMode == LED_RADIO     ||
                                     effectiveMode == LED_LAMP      ||
                                     effectiveMode == LED_POMODORO  ||
                                     radioState.active              ||  // Radio discovery mode (LED may be PROCESSING)
                                     meditationState.active         ||
                                     lampState.active);
            if (!isPersistentMode) {
                Serial.printf("turnComplete in PLAYING state (LED=%d) - opening conversation window\n", effectiveMode);
                shouldOpenConversation = true;
            } else {
                Serial.printf("turnComplete in PLAYING state - persistent mode %d, skipping window\n", effectiveMode);
                transitionConvState(ConvState::IDLE);
                // Radio: restore volume and resume stream after Gemini verbal response finishes
                if (radioState.active) {
                    if (radioState.streaming && volumeMultiplier < radioState.savedVolume) {
                        volumeMultiplier = radioState.savedVolume;
                        Serial.printf("Radio: volume restored to %.0f%% after Gemini response\n", volumeMultiplier * 100);
                    }
                    if (radioState.paused) {
                        if (radioState.streamUrl[0] != '\0') {
                            resumeRadioStream();
                        } else {
                            radioState.paused = false;  // Discovery mode — no stream to resume
                            Serial.println("Radio: discovery mode, no stream URL to resume (PLAYING path)");
                        }
                    }
                }
                if (currentLEDMode == LED_AUDIO_REACTIVE || currentLEDMode == LED_RECORDING || currentLEDMode == LED_PROCESSING) {
                    if (radioState.active)         { currentLEDMode = LED_RADIO; }
                    else if (meditationState.active) { currentLEDMode = LED_MEDITATION; }
                    else if (pomodoroState.active)  { currentLEDMode = LED_POMODORO; }
                    else if (ambientSound.active)   { currentLEDMode = LED_AMBIENT; }
                    else if (lampState.active)      { currentLEDMode = LED_LAMP; }
                }
            }
        }
        // Note: Timer has its own expiry logic and doesn't use this block.
        
        if (shouldOpenConversation) {
            Serial.printf("Opening conversation window: LED=%d, waitAge=%dms\n",
                         currentLEDMode,
                         waitingEnteredAt > 0 ? (int)(millis() - waitingEnteredAt) : -1);
            transitionConvState(ConvState::WINDOW);
            conversationMode = true;
            conversationWindowStart = millis();
            conversationVADDetected = false;  // Flush any residual VAD from speaker resonance
            currentLEDMode = LED_CONVERSATION_WINDOW;
            Serial.println("Conversation window opened - speak anytime in next 10 seconds");
        }
    }
    
    // Conversation window monitoring
    if (conversationMode && !isPlayingResponse && !recordingActive && !alarmState.ringing) {
        uint32_t elapsed = millis() - conversationWindowStart;
        
        // Debug every 2 seconds - show current state
        static uint32_t lastDebugPrint = 0;
        if (millis() - lastDebugPrint > 2000) {
            Serial.printf("[CONV] active, window=%ums/%u, LED=%d, turnComplete=%d\n", 
                         elapsed, CONVERSATION_WINDOW_MS, currentLEDMode, turnComplete);
            lastDebugPrint = millis();
        }
        
        if (elapsed < CONVERSATION_WINDOW_MS) {
            // Voice activity is detected by audioTask (the sole I2S_NUM_0 reader).
            // It sets conversationVADDetected = true when amplitude exceeds threshold.
            // We just need to check that flag here.
            
            // Guard: ignore VAD for first 800ms after window opens to let speaker resonance die down.
            // Without this, the tail of Gemini's audio playback can false-trigger a new recording.
            const uint32_t VAD_GUARD_MS = 800;
            
            // Safety: don't start recording if alarm is active
            if (isPlayingAlarm) {
                return;
            }
            
            if (elapsed >= VAD_GUARD_MS && conversationVADDetected) {
                conversationVADDetected = false;  // Consume the flag
                
                // Voice detected - log and start recording
                Serial.printf("Voice detected in conversation window - avgAmp=%d, starting recording\n", (int)currentAudioLevel);
                
                // Exit conversation mode and start recording
                conversationMode = false;
                conversationRecording = true;
                tideState.active = false;   // New question - clear viz state so it doesn't re-display
                moonState.active = false;
                recordingActive = true;
                recordingStartTime = millis();
                lastVoiceActivityTime = millis();
                transitionConvState(ConvState::RECORDING);
                currentLEDMode = LED_RECORDING;
                lastDebounceTime = millis();
                
                Serial.printf("Recording mode activated: LED=%d, audioLevel=%d\n", currentLEDMode, (int)currentAudioLevel);
            }
        } else {
            // Window expired with no voice - return to visualizations or idle
            Serial.println("Conversation window expired");
            conversationMode = false;
            transitionConvState(ConvState::IDLE);
            
            // Priority: Pomodoro > Meditation > Timer > Moon > Tide > Idle
            if (pomodoroState.active) {
                currentLEDMode = LED_POMODORO;
                Serial.println("Returning to POMODORO display");
            } else if (meditationState.active) {
                currentLEDMode = LED_MEDITATION;
                Serial.println("Returning to MEDITATION");
            } else if (timerState.active) {
                currentLEDMode = LED_TIMER;
                Serial.println("Returning to TIMER display");
            } else if (tideState.active) {
                // Clear tide - user had their chance during the window.
                // Re-entering LED_TIDE with convState=IDLE causes a stuck state
                // (auto-transition requires WAITING). Go straight to idle.
                tideState.active = false;
                currentLEDMode = LED_IDLE;
                Serial.println("Returning to IDLE (tide already shown)");
            } else if (moonState.active) {
                moonState.active = false;
                currentLEDMode = LED_IDLE;
                Serial.println("Returning to IDLE (moon already shown)");
            } else if (radioState.active) {
                currentLEDMode = LED_RADIO;
                Serial.println("Returning to RADIO");
            } else {
                currentLEDMode = LED_IDLE;
                Serial.println("Returning to IDLE");
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
 .sample_rate = SPEAKER_SAMPLE_RATE, // 24kHz for Gemini output
 .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
 .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
 .communication_format = I2S_COMM_FORMAT_STAND_I2S,
 .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
 .dma_buf_count = 6,  // Reduced from 16: 6x512/24000 = ~128ms pipeline (was 682ms)
 .dma_buf_len = 512,  // Reduced from 1024 to sync LEDs with voice
 .use_apll = true, // Use APLL for more accurate sample rate
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

 // Require 3 consecutive frames above threshold before treating as real speech.
 // A single 20ms spike from ambient noise / tap will NOT reset the silence timer.
 // 3 frames * 20ms = 60ms of sustained audio required.
 static int consecutiveFrames = 0;
 if (avgAmplitude > VAD_THRESHOLD) {
 consecutiveFrames++;
 if (consecutiveFrames >= 3) {
  lastVoiceActivityTime = millis();
  return true;
 }
 } else {
 consecutiveFrames = 0;
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
        
        // PRIORITY 1: Process playback queue
        // First call uses a short blocking wait when actively playing to absorb network jitter:
        // at ~20ms/packet, any gap >1 packet duration risks I2S underrun without this.
        // Subsequent iterations drain the rest of the queue immediately (waitTime=0).
        {
            TickType_t waitTime = (isPlayingResponse && !recordingActive) ? pdMS_TO_TICKS(25) : 0;
            while (xQueueReceive(audioOutputQueue, &playbackChunk, waitTime) == pdTRUE) {
                waitTime = 0;  // only block on first receive; drain remainder immediately
                processedAudio = true;
            
            // Raw PCM from server (24kHz, 16-bit mono, little-endian)
            // Convert bytes to 16-bit samples
            int numSamples = playbackChunk.length / 2;  // 2 bytes per sample
            int16_t* pcmSamples = (int16_t*)playbackChunk.data;
            
            if (numSamples > 0 && numSamples <= 960) {  // Max 960 samples (stereo buffer holds 960 stereo pairs)
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
                
                // Convert mono  stereo with volume
                for (int i = 0; i < numSamples; i++) {
                    int32_t sample = (int32_t)(pcmSamples[i] * volumeMultiplier);
                    if (sample > 32767) sample = 32767;
                    if (sample < -32768) sample = -32768;
                    stereoBuffer[i * 2] = (int16_t)sample;
                    stereoBuffer[i * 2 + 1] = (int16_t)sample;
                }
                
                // Write to I2S - guarded by mutex to prevent race with tone functions on main task
                size_t bytes_written;
                if (xSemaphoreTake(i2sSpeakerMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                    esp_err_t result = i2s_write(I2S_NUM_1, stereoBuffer, numSamples * 4, &bytes_written, pdMS_TO_TICKS(500));
                    xSemaphoreGive(i2sSpeakerMutex);
                    if (result != ESP_OK || bytes_written < numSamples * 4) {
                        Serial.printf("I2S write failed: result=%d, wrote=%u/%u\n", result, bytes_written, numSamples*4);
                    }
                } else {
                    // Mutex held by tone function (e.g. volume chime) - skip this frame
                    Serial.println("[audioTask] I2S mutex busy - skipping frame");
                }
                
                // Update last audio chunk time to prevent timeout while queue has data
                lastAudioChunkTime = millis();
                
                // Debug periodically
                static uint32_t lastPlaybackDebug = 0;
                if (millis() - lastPlaybackDebug > 1000) {
                    Serial.printf("[PLAYBACK] Raw PCM: %d bytes  %d samples, level=%d, queue=%d\n", 
                                 playbackChunk.length, numSamples, currentAudioLevel, uxQueueMessagesWaiting(audioOutputQueue));
                    lastPlaybackDebug = millis();
                }
            } else {
                Serial.printf("Invalid PCM chunk: %d bytes (%d samples)\n", playbackChunk.length, numSamples);
            }
        }
        }  // end playback queue scope
        
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
                    
                    // On first chunk of each recording, send device state context to server
                    if (!recordingStartSent) {
                        recordingStartSent = true;
                        turnComplete = false;  // New user turn starting - clear previous turn's flag
                        JsonDocument stateDoc;
                        stateDoc["type"] = "recordingStart";

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

                        // Timer state
                        JsonObject timDoc = stateDoc["timer"].to<JsonObject>();
                        timDoc["active"] = timerState.active;
                        if (timerState.active && timerState.startTime > 0) {
                            uint32_t elapsed = (millis() - timerState.startTime) / 1000;
                            int remaining = timerState.totalSeconds > (int)elapsed ? timerState.totalSeconds - (int)elapsed : 0;
                            timDoc["secondsRemaining"] = remaining;
                        }

                        // Lamp state
                        JsonObject lamDoc = stateDoc["lamp"].to<JsonObject>();
                        lamDoc["active"] = lampState.active;
                        if (lampState.active) {
                            const char* colorName = "white";
                            if (lampState.currentColor == LampState::RED) colorName = "red";
                            else if (lampState.currentColor == LampState::GREEN) colorName = "green";
                            else if (lampState.currentColor == LampState::BLUE) colorName = "blue";
                            lamDoc["color"] = colorName;
                        }

                        // Radio state
                        JsonObject radDoc = stateDoc["radio"].to<JsonObject>();
                        radDoc["active"] = radioState.active;
                        radDoc["streaming"] = radioState.streaming;
                        // Include station name whenever active (not just when streaming — Gemini
                        // needs it when the stream is paused for a voice command too)
                        if (radioState.active && radioState.stationName[0] != '\0') {
                            radDoc["station"] = radioState.stationName;
                        }

                        // Alarm count (quick summary — full list available via deviceStateRequest)
                        int activeAlarmCount = 0;
                        for (int i = 0; i < MAX_ALARMS; i++) {
                            if (alarms[i].enabled) activeAlarmCount++;
                        }
                        stateDoc["alarmCount"] = activeAlarmCount;

                        String stateMsg;
                        serializeJson(stateDoc, stateMsg);
                        wsSendMessage(stateMsg);
                        Serial.printf("[STATE] recordingStart: %s\n", stateMsg.c_str());
                    }

                    // Re-check recordingActive: loop() may have set it to false and sent
                    // recordingStop during the 100ms i2s_read block. If so, skip this frame
                    // to avoid sending audio after activityEnd has already been forwarded.
                    // NOTE: must be `continue` not `break` — `break` exits the outer while(1)
                    // which causes audioTask to return, hitting the ILL trap at line 2490.
                    if (!recordingActive) continue;

                    // Send raw PCM
                    sendAudioChunk((uint8_t*)inputBuffer, bytes_read);
                    
                    if (hasVoice) {
                        lastVoiceActivityTime = millis();
                    }
                }
            }
        } else if (conversationMode || isAmbientVUMode) {
            recordingStartSent = false;  // Not recording - reset flag for next recording
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
                if (isAmbientVUMode && samples > 0) {
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
            recordingStartSent = false;  // Not recording - reset flag for next recording
            vTaskDelay(pdMS_TO_TICKS(1));
        }
    }
}

// ============== AUDIO FEEDBACK ==============
void playZenBell() {
    // Request zen bell sound from server
    if (!isWebSocketConnected) {
        Serial.println("Cannot play zen bell - WebSocket not connected");
        return;
    }
    
    JsonDocument bellDoc;
    bellDoc["action"] = "requestZenBell";
    String bellMsg;
    serializeJson(bellDoc, bellMsg);
    wsSendMessage(bellMsg);
    Serial.println("Requesting zen bell from server");
}

void playShutdownSound() {
 // Play a descending melody on disconnect (reverse of startup)
 const int sampleRate = 24000;
 const int noteDuration = 120; // 120ms per note
 const int numSamples = (sampleRate * noteDuration) / 1000;
 
 // Musical notes: C6, G5, E5, C5 (descending - reverse of startup)
 const float frequencies[] = {1046.50f, 783.99f, 659.25f, 523.25f};
 const int numNotes = 4;
 
 static int16_t toneBuffer[5760]; // 120ms at 24kHz stereo (2880 samples * 2 channels)
 
 for (int note = 0; note < numNotes; note++) {
 for (int i = 0; i < numSamples; i++) {
 // Generate sine wave with fade-in/fade-out envelope
 float t = (float)i / sampleRate;
 float envelope = 1.0f;
 if (i < numSamples / 10) {
 envelope = (float)i / (numSamples / 10); // Fade in
 } else if (i > numSamples * 9 / 10) {
 envelope = (float)(numSamples - i) / (numSamples / 10); // Fade out
 }
 
 int16_t sample = (int16_t)(sin(2.0f * PI * frequencies[note] * t) * 6000 * envelope * volumeMultiplier);
 
 // Stereo output
 toneBuffer[i * 2] = sample;
 toneBuffer[i * 2 + 1] = sample;
 }
 
 size_t bytes_written;
 if (xSemaphoreTake(i2sSpeakerMutex, portMAX_DELAY) == pdTRUE) {
     i2s_write(I2S_NUM_1, toneBuffer, numSamples * 4, &bytes_written, pdMS_TO_TICKS(200));
     xSemaphoreGive(i2sSpeakerMutex);
 }
 }
}

void playVolumeChime() {
 // Play a brief 1kHz tone at the current volume level
 const int sampleRate = 24000;
 const int durationMs = 50; // Very short 50ms beep (reduced from 100ms)
 const int numSamples = (sampleRate * durationMs) / 1000;
 const float frequency = 1200.0f; // 1.2kHz tone (slightly higher pitch)
 
 static int16_t toneBuffer[2400]; // 50ms at 24kHz stereo (1200 samples * 2 channels)
 
 for (int i = 0; i < numSamples; i++) {
 // Generate sine wave
 float t = (float)i / sampleRate;
 int16_t sample = (int16_t)(sin(2.0f * PI * frequency * t) * 8000 * volumeMultiplier);
 
 // Stereo output
 toneBuffer[i * 2] = sample;
 toneBuffer[i * 2 + 1] = sample;
 }
 
 size_t bytes_written;
 if (xSemaphoreTake(i2sSpeakerMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
     i2s_write(I2S_NUM_1, toneBuffer, numSamples * 4, &bytes_written, pdMS_TO_TICKS(100));
     xSemaphoreGive(i2sSpeakerMutex);
 }
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
    // Max mic frame = MIC_FRAME_SIZE * 2 = 640 bytes  Base64 = ceil(640/3)*4 = 856 chars + NUL
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
    if (wsSendMessage(output)) {
        lastWebSocketSendTime = millis();
    } else {
        webSocketSendFailures++;  // wsSendMessage already logged the failure
    }
}// Track WebSocket stats
static uint32_t disconnectCount = 0;
static uint32_t lastDisconnectTime = 0;

void onWebSocketEvent(WStype_t type, uint8_t * payload, size_t length) {
    switch(type) {
        case WStype_CONNECTED:
            {
                Serial.printf("WebSocket Connected to Edge Server! (disconnect count: %d)\n", disconnectCount);
                isWebSocketConnected = true;
                shutdownSoundPlayed = false;  // Reset flag on successful connection
                currentLEDMode = LED_CONNECTED;
                
                // Edge server handles Gemini setup automatically
                Serial.println("Waiting for 'ready' message from server");
                
                // Show connection on LEDs (with mutex)
                if (xSemaphoreTake(ledMutex, portMAX_DELAY) == pdTRUE) {
                    fill_solid(leds, NUM_LEDS, CRGB::Green);
                    FastLED.show();
                    xSemaphoreGive(ledMutex);
                }
                vTaskDelay(pdMS_TO_TICKS(500));  // Yield properly in FreeRTOS task context
                
                // Resume ambient mode if it was active before disconnect
                if (ambientSound.active && ambientSound.name[0] != '\0') {
                    Serial.printf("Resuming ambient sound: %s (seq %d)\n", 
                                 ambientSound.name, ambientSound.sequence);
                    
                    // Restore LED mode based on ambient sound
                    currentLEDMode = LED_AMBIENT;
                    
                    // Set the current ambient sound
                    if (strcmp(ambientSound.name, "rain") == 0) {
                        currentAmbientSoundType = SOUND_RAIN;
                    } else if (strcmp(ambientSound.name, "ocean") == 0) {
                        currentAmbientSoundType = SOUND_OCEAN;
                    } else if (strcmp(ambientSound.name, "rainforest") == 0) {
                        currentAmbientSoundType = SOUND_RAINFOREST;
                    } else if (strcmp(ambientSound.name, "fire") == 0) {
                        currentAmbientSoundType = SOUND_FIRE;
                    }
                    
                    // Request the ambient sound again
                    JsonDocument ambientDoc;
                    ambientDoc["action"] = "requestAmbient";
                    ambientDoc["sound"] = ambientSound.name;
                    ambientDoc["sequence"] = ambientSound.sequence;
                    String ambientMsg;
                    serializeJson(ambientDoc, ambientMsg);
                    wsSendMessage(ambientMsg);
                    
                    isPlayingAmbient = true;
                    firstAudioChunk = true;
                    lastAudioChunkTime = millis();
                } else if (isAmbientVUMode) {
                    // Resume VU meter mode
                    currentLEDMode = LED_AMBIENT_VU;
                    Serial.println("Resuming VU meter mode");
                } else {
                    currentLEDMode = LED_IDLE;
                }
            }
            break;
            
        case WStype_TEXT:
            Serial.printf("Received TEXT: %d bytes: %.*s\n", length, (int)min(length, (size_t)200), (char*)payload);
            handleWebSocketMessage(payload, length);
            break;
            
        case WStype_BIN:
            {
                //  DIAGNOSTIC: Track packet timing to detect bursting
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
                    Serial.printf("[STREAM] %u packets, %.1fms avg interval, %u fast (<20ms), %u KB/s, queue=%u\n", 
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
                                Serial.printf("Discarded %u stale ambient chunks in last 10s (seq %d, active=%d, expected=%d)\n", 
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

                    // Sync meditation breathing animation to first audio chunk
                    if (meditationState.active && meditationState.phaseStartTime == 0) {
                        meditationState.phaseStartTime = millis();
                        meditationState.phase = MeditationState::HOLD_BOTTOM;
                        Serial.println("[MEDITATION] Breathing synced to first audio chunk");
                    }

                    // Clear drain timer when we receive first packet of new sequence
                    if (ambientSound.drainUntil > 0) {
                        Serial.printf("New sequence %d arrived - drain complete\n", chunkSequence);
                        ambientSound.drainUntil = 0;
                    }
                }
                // else: Gemini audio (no magic header) - continue to play
                
                // Ignore audio if response was interrupted (but not for ambient/alarm sounds)
                if (responseInterrupted && !isPlayingAmbient && !isPlayingAlarm) {
                    Serial.println("Discarding audio chunk (response was interrupted)");
                    break;
                }

                // Discard stale non-ambient audio arriving after a mode switch to meditation/ambient
                // (e.g. zen bell chunks still in TCP pipeline from previous Pomodoro mode)
                if (!isAmbientPacket && !isPlayingAlarm && meditationState.active && !isPlayingResponse) {
                    static uint32_t lastStaleLog = 0;
                    if (millis() - lastStaleLog > 2000) {
                        Serial.println("Discarding stale non-ambient chunk during meditation");
                        lastStaleLog = millis();
                    }
                    break;
                }
                
                // Raw PCM audio data from server (16-bit mono samples)
                // This handles BOTH Gemini responses and ambient sounds

                // Update last audio chunk time
                lastAudioChunkTime = millis();
                if (!isAmbientPacket) {
                    lastGeminiAudioTime = millis();  // Gemini-specific timer for drain detection during radio overlap
                }
                
                // Debug first chunk
                if (firstAudioChunk) {
                    Serial.print("First bytes (hex): ");
                    for (int i = 0; i < min(8, (int)length); i++) {
                        Serial.printf("%02X ", payload[i]);
                    }
                    Serial.println();
                    firstAudioChunk = false;
                    // Radio: mark as actively streaming once first chunk arrives
                    if (radioState.active && !radioState.streaming) {
                        radioState.streaming = true;
                        Serial.printf("Radio streaming started: %s\n", radioState.stationName);
                    }
                }
                
                // Queue raw PCM chunk for audio task
                // Use blocking send with timeout to apply backpressure instead of dropping
                if (length == 0) {
                    // Empty chunk after header strip - silently discard
                    break;
                }
                if (length <= sizeof(AudioChunk::data)) {
                    AudioChunk chunk;
                    memcpy(chunk.data, payload, length);
                    chunk.length = length;
                    
                    //  DIAGNOSTIC: Track queue depth before send
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
                                Serial.printf("Blocked on queue for 100ms+ (%u times, queue=%u/%u) - audio system may be frozen\n", 
                                             dropsince, queueBefore, AUDIO_QUEUE_SIZE);
                                dropsince = 0;
                            }
                            lastDropWarning = millis();
                        }
                        break;  // discard this chunk — queue blocked
                    }
                } else {
                    Serial.printf("PCM chunk too large: %d bytes\n", length);
                    break;
                }

                // Prebuffer check: runs AFTER xQueueSend so queueDepth includes the
                // packet we just added. Previously this ran before enqueue, so depth
                // was always 0 and isPlayingResponse was never set for zen bell / ambient
                // streams that arrive near their consumption rate.
                if (!isPlayingResponse) {
                    // CRITICAL: Stop recording immediately when response arrives
                    if (recordingActive) {
                        Serial.println("Stopping recording - response arriving");
                        recordingActive = false;
                    }

                    uint32_t queueDepth = uxQueueMessagesWaiting(audioOutputQueue);
                    // MIN_PREBUFFER=3: ~63ms head start before declaring playback active.
                    // Absorbs network jitter for short-burst sounds (zen bell, alarms)
                    // whose packets arrive near their playback rate with zero queue headroom.
                    const uint32_t MIN_PREBUFFER = 3;

                    if (queueDepth >= MIN_PREBUFFER) {
                        isPlayingResponse = true;
                        Serial.printf("[PREBUF] Playback start: queueDepth=%u, turnComplete=%d, convState=%d, waitAge=%dms\n",
                                     queueDepth, turnComplete, (int)convState,
                                     waitingEnteredAt > 0 ? (int)(millis() - waitingEnteredAt) : -1);

                        // NOTE: turnComplete is NOT reset here to avoid a race condition where
                        // Gemini's turnComplete message arrives before the prebuffer fills (common
                        // for short responses like the boot greeting). Instead, turnComplete is
                        // reset at recordingStart (when the user begins a new turn).

                        recordingActive = false;  // Ensure recording is stopped

                        // Show VU meter during Gemini playback, keep current mode for ambient/alarm
                        if (!ambientSound.active && !isPlayingAlarm) {
                            currentLEDMode = LED_AUDIO_REACTIVE;
                        }

                        firstAudioChunk = true;
                        // NOTE: Don't clear responseInterrupted here! Only clear it on turnComplete
                        // to prevent buffered chunks from interrupted response playing through
                        // Clear all LEDs immediately when starting Gemini playback (not for ambient/radio -
                        // clearing LEDs for radio would cause a visible flash every drain/prebuf cycle)
                        if (!isPlayingAmbient && !isPlayingAlarm) {
                            if (xSemaphoreTake(ledMutex, portMAX_DELAY) == pdTRUE) {
                                fill_solid(leds, NUM_LEDS, CRGB::Black);
                                FastLED.show();
                                xSemaphoreGive(ledMutex);
                            }
                        }
                        // Clear delay buffer for clean LED sync
                        for (int i = 0; i < AUDIO_DELAY_BUFFER_SIZE; i++) {
                            audioLevelBuffer[i] = 0;
                        }
                        audioBufferIndex = 0;

                        if (isPlayingAmbient) {
                            Serial.printf("Starting ambient audio stream: %s (prebuffered %u packets)\n",
                                         ambientSound.name, queueDepth);
                        } else if (isPlayingAlarm) {
                            Serial.printf("Starting alarm audio playback (prebuffered %u packets)\n", queueDepth);
                        } else {
                            Serial.printf("Starting audio playback with %u packets prebuffered\n", queueDepth);
                        }
                    } else {
                        // Silent prebuffering phase - log once per stream
                        static uint32_t lastPrebufferLog = 0;
                        if (millis() - lastPrebufferLog > 1000) {
                            Serial.printf("Prebuffering... (%u/%u packets)\n", queueDepth, MIN_PREBUFFER);
                            lastPrebufferLog = millis();
                        }
                    }
                }
            }
            break;
            
        case WStype_DISCONNECTED:
            disconnectCount++;
            lastDisconnectTime = millis();
            Serial.printf("WebSocket Disconnected (#%d) - isPlaying=%d, recording=%d, uptime=%lus\n",
                         disconnectCount, isPlayingResponse, recordingActive, millis()/1000);
            isWebSocketConnected = false;
            
            // Pause ambient playback but keep mode state for resume on reconnect
            if (isPlayingAmbient || ambientSound.active) {
                Serial.printf("Pausing ambient sound due to disconnect: %s (will resume)\n", ambientSound.name);
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
            Serial.println("WebSocket Error");
            currentLEDMode = LED_ERROR;
            break;
            
        default:
            break;
    }
}

// ============== LED CONTROLLER ==============
void updateLEDs() {
    // Seed smoothedAudioLevel immediately when recording starts, bypassing the EMA
    // ramp-up from zero so the VU meter responds on the very first frame.
    static LEDMode prevLEDMode = LED_IDLE;
    if (currentLEDMode == LED_RECORDING && prevLEDMode != LED_RECORDING) {
        smoothedAudioLevel = (float)currentAudioLevel;
    }
    prevLEDMode = currentLEDMode;

 // Smooth the audio level with exponential moving average
// Fast rise (α=0.5, ~43ms time constant) so peaks track speech closely.
        // Decay is handled separately below (0.60× per frame when silent).
        const float smoothing = 0.50f;
 smoothedAudioLevel = smoothedAudioLevel * (1.0f - smoothing) + currentAudioLevel * smoothing;
 
 // Faster decay when no audio to prevent LEDs lingering after speech ends
 if (currentAudioLevel == 0) {
 smoothedAudioLevel *= 0.60f; // Very fast decay
 // Force to zero when low to prevent lingering
 if (smoothedAudioLevel < 20) {
 smoothedAudioLevel = 0;
 }
 }
 
 // When transitioning away from AUDIO_REACTIVE, quickly fade out any residual levels
 if (currentLEDMode != LED_AUDIO_REACTIVE && currentLEDMode != LED_RECORDING && 
 currentLEDMode != LED_AMBIENT_VU && smoothedAudioLevel > 0) {
 smoothedAudioLevel *= 0.4f; // Very rapid fade
 if (smoothedAudioLevel < 5) {
 smoothedAudioLevel = 0;
 currentAudioLevel = 0;
 }
 }

    switch(currentLEDMode) {
        case LED_BOOT:            renderLedBoot(leds);              break;
        case LED_IDLE:            renderLedIdle(leds);              break;
        case LED_RECORDING:       renderLedRecording(leds);         break;
        case LED_PROCESSING:      renderLedProcessing(leds);        break;
        case LED_AMBIENT_VU:      renderLedAmbientVU(leds);         break;
        case LED_AUDIO_REACTIVE:  renderLedAudioReactive(leds);     break;
        case LED_TIDE:            renderLedTide(leds);              break;
        case LED_TIMER:           renderLedTimer(leds);             break;
        case LED_MOON:            renderLedMoon(leds);              break;
        case LED_AMBIENT:         ambientRenderer.render(leds, NUM_LEDS); break;
        case LED_RADIO:           renderLedRadio(leds);             break;
        case LED_POMODORO:        renderLedPomodoro(leds);          break;
        case LED_MEDITATION:      renderLedMeditation(leds);        break;
        case LED_LAMP:            renderLedLamp(leds);              break;
        case LED_SEA_GOOSEBERRY:  seaGooseberry.render(leds, NUM_LEDS); break;
        case LED_EYES:            eyeAnimation.render(leds);        break;
        case LED_ALARM:           renderLedAlarm(leds);             break;
        case LED_CONVERSATION_WINDOW: renderLedConversationWindow(leds); break;
        case LED_CONNECTED:       renderLedConnected(leds);         break;
        case LED_ERROR:           renderLedError(leds);             break;
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
                Serial.printf("Memory baseline: Heap=%u KB, PSRAM=%u KB\n", freeHeap/1024, freePsram/1024);
            }
            if (freeHeap < lowestHeap) {
                lowestHeap = freeHeap;
                Serial.printf("New low heap: %u KB (lost %u KB since startup)\n", 
                             freeHeap/1024, (startupHeap - freeHeap)/1024);
            }
            
            // Hourly summary for leak detection
            uint32_t uptime = (millis() - startTime) / 1000;
            if (uptime > 0 && uptime % 3600 == 0) {
                Serial.printf("\n\n");
                Serial.printf("HOURLY MEMORY REPORT - %u hours runtime  \n", uptime/3600);
                Serial.printf("\n");
                Serial.printf("Current Heap:  %6u KB                \n", freeHeap/1024);
                Serial.printf("Startup Heap:  %6u KB                \n", startupHeap/1024);
                Serial.printf("Lowest Heap:   %6u KB                \n", lowestHeap/1024);
                Serial.printf("Heap Lost:     %6u KB                \n", (startupHeap - freeHeap)/1024);
                Serial.printf("PSRAM Free:    %6u KB                \n", freePsram/1024);
                Serial.printf("Mode: %-30s    \n", 
                    currentLEDMode == LED_IDLE ? "IDLE" :
                    currentLEDMode == LED_AMBIENT ? "AMBIENT" :
                    currentLEDMode == LED_AMBIENT_VU ? "VU METER" :
                    currentLEDMode == LED_SEA_GOOSEBERRY ? "SEA GOOSEBERRY" :
                    currentLEDMode == LED_POMODORO ? "POMODORO" :
                    currentLEDMode == LED_MEDITATION ? "MEDITATION" : "OTHER");
                Serial.printf("\n\n");
            }
            
            lastRSSI = rssi;
            lastHealthLog = millis();
            
            // Warn if heap is getting low
            if (freeHeap < 50000) {  // Less than 50KB free
                Serial.printf("LOW HEAP WARNING: Only %u KB free!\n", freeHeap/1024);
            }
            
            // Attempt to reconnect WiFi if signal is very poor but still connected
            if (rssi < -80 && WiFi.status() == WL_CONNECTED) {
                Serial.println("Very weak signal detected - WiFi may drop soon");
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
            Serial.printf("LED task stalled #%d: %dms since last update\n", 
                         ledTaskStalls, now - lastLedUpdate);
        }
        
        // Mutex-protected LED rendering
        if (xSemaphoreTake(ledMutex, portMAX_DELAY) == pdTRUE) {
            // Track updateLEDs calls
            static uint32_t updateCount = 0;
            static uint32_t lastUpdateLog = 0;
            updateCount++;
            
            if (millis() - lastUpdateLog > 30000) {
                // Serial.printf("LED task: %u updates in 30s (expect ~990)\n", updateCount);  // Disabled for clean logging
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