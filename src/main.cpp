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
#include <lwip/sockets.h>
#include "Config.h"
#include "display_mapping.h"
#include "front_text_marquee.h"

// Debug logging macro - controlled by Config.h DEBUG_LOGS flag
#ifdef DEBUG_LOGS
    #define DEBUG_PRINT(...) Serial.printf(__VA_ARGS__)
    #define DEBUG_PRINTLN(...) Serial.println(__VA_ARGS__)
#else
    #define DEBUG_PRINT(...) ((void)0)
    #define DEBUG_PRINTLN(...) ((void)0)
#endif

// Chakra names array (used across multiple functions)
static const char* CHAKRA_NAMES[NUM_CHAKRAS] = {
    "ROOT", "SACRAL", "SOLAR", "HEART", "THROAT", "THIRD_EYE", "CROWN"
};

// ============== GLOBAL STATE ==============
WiFiClientSecure wifiClient;
WebSocketsClient webSocket;
CRGB leds[NUM_LEDS];

// Front text marquee system
FrontTextMarquee frontMarquee;

bool isWebSocketConnected = false;
bool recordingActive = false;
bool isPlayingResponse = false;
bool isPlayingAmbient = false;  // Track ambient sound playback separately
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
uint32_t lastWiFiCheck = 0;  // Track WiFi health monitoring
int32_t lastRSSI = 0;  // Track signal strength changes
bool firstAudioChunk = true;
float volumeMultiplier = 0.25f;  // Volume control (25% for testing)
int32_t currentAudioLevel = 0;  // Current audio amplitude for VU meter
float smoothedAudioLevel = 0.0f;  // Smoothed audio level for stable VU meter
bool conversationMode = false;  // Track if we're in conversation window
uint32_t conversationWindowStart = 0;  // Timestamp when conversation window opened
bool conversationRecording = false;  // Track if current recording was triggered from conversation mode

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

struct AudioChunk {
    uint8_t data[2048];
    size_t length;
};

enum LEDMode { LED_BOOT, LED_IDLE, LED_RECORDING, LED_PROCESSING, LED_AUDIO_REACTIVE, LED_CONNECTED, LED_ERROR, LED_TIDE, LED_TIMER, LED_MOON, LED_AMBIENT_VU, LED_AMBIENT, LED_POMODORO, LED_MEDITATION, LED_CLOCK, LED_LAMP, LED_ALARM, LED_CONVERSATION_WINDOW, LED_MARQUEE };
LEDMode currentLEDMode = LED_IDLE;  // Start directly in idle mode
LEDMode targetLEDMode = LED_IDLE;  // Mode to switch to after marquee finishes
bool ambientVUMode = false;  // Toggle for ambient sound VU meter mode

// Ambient sound type (for cycling within AMBIENT mode)
enum AmbientSoundType { SOUND_RAIN, SOUND_OCEAN, SOUND_RAINFOREST };
AmbientSoundType currentAmbientSoundType = SOUND_RAIN;

// Tide visualization state
struct TideState {
    String state;  // "flooding" or "ebbing"
    float waterLevel;  // 0.0 to 1.0
    int nextChangeMinutes;
    uint32_t displayStartTime;
    bool active;
} tideState = {"", 0.0, 0, 0, false};

// Timer visualization state
struct TimerState {
    int totalSeconds;
    uint32_t startTime;
    bool active;
} timerState = {0, 0, false};

// Moon phase visualization state
struct MoonState {
    String phaseName;
    int illumination;  // 0-100%
    float moonAge;
    uint32_t displayStartTime;
    bool active;
} moonState = {"", 0, 0.0, 0, false};

// LED mutex for thread-safe access
SemaphoreHandle_t ledMutex = NULL;

// Ambient sound state
struct AmbientSound {
    String name;  // "rain", "ocean", "rainforest"
    bool active;
    uint16_t sequence;  // Increments each time we request a new sound
    uint16_t discardedCount;  // Count discarded chunks to reduce log spam
    uint32_t drainUntil;  // Timestamp until which we silently drain stale packets
} ambientSound = {"", false, 0, 0, 0};

// Pomodoro timer state
struct PomodoroState {
    enum Session { FOCUS, SHORT_BREAK, LONG_BREAK };
    Session currentSession;
    int sessionCount;        // Track completed focus sessions (0-3, resets at 4)
    int totalSeconds;        // Duration of current session
    uint32_t startTime;      // When current timer started (0 if not started)
    uint32_t pausedTime;     // Time remaining when paused (0 if not paused)
    bool active;             // Pomodoro mode active
    bool paused;             // Timer paused
    // Custom durations (in minutes)
    int focusDuration;       // Focus session duration (default: 25)
    int shortBreakDuration;  // Short break duration (default: 5)
    int longBreakDuration;   // Long break duration (default: 15)
    // Flash animation state (non-blocking)
    bool flashing;           // Currently flashing completion indicator
    uint8_t flashCount;      // Number of flashes completed (0-6: on/off/on/off/on/off)
    uint32_t flashStartTime; // When current flash state started
} pomodoroState = {PomodoroState::FOCUS, 0, 25 * 60, 0, 0, false, false, 25, 5, 15, false, 0, 0};

// Meditation mode state
struct MeditationState {
    enum Chakra { ROOT, SACRAL, SOLAR, HEART, THROAT, THIRD_EYE, CROWN };
    enum BreathPhase { INHALE, HOLD_TOP, EXHALE, HOLD_BOTTOM };
    Chakra currentChakra;
    BreathPhase phase;
    uint32_t phaseStartTime;
    bool active;             // Meditation mode active
    bool paused;             // Breathing paused
    bool streaming;          // Currently streaming meditation audio
    float savedVolume;       // User's volume before meditation
} meditationState = {MeditationState::ROOT, MeditationState::INHALE, 0, false, true, false, 1.0f};

// Clock display state
struct ClockState {
    int lastHour;            // Last displayed hour
    int lastMinute;          // Last displayed minute
    int scrollPosition;      // Horizontal scroll position for rotating display
    uint32_t lastScrollUpdate; // Last time scroll position updated
    bool active;             // Clock mode active
} clockState = {-1, -1, 0, 0, false};

// Lamp mode state
struct LampState {
    enum Color { WHITE, RED, GREEN, BLUE };
    Color currentColor;      // Current target color
    Color previousColor;     // Color being replaced
    int currentRow;          // Current row being lit (0-11)
    int currentCol;          // Current column within row (0-11)
    uint32_t lastUpdate;     // Last LED update time
    uint32_t ledStartTimes[NUM_LEDS]; // Start time for each LED fade
    bool active;             // Lamp mode active
    bool fullyLit;           // All LEDs fully lit
    bool transitioning;      // Currently transitioning between colors
} lampState = {LampState::WHITE, LampState::WHITE, 0, 0, 0, {0}, false, false, false};

// Alarm state
#define MAX_ALARMS 10
struct Alarm {
    uint32_t alarmID;        // Unique ID from server
    time_t triggerTime;      // Unix timestamp when alarm should trigger
    bool enabled;            // Alarm is active
    bool triggered;          // Alarm has been triggered (prevent re-trigger)
    bool snoozed;            // Currently snoozed
    time_t snoozeUntil;      // Wake up from snooze at this time
} alarms[MAX_ALARMS];

struct AlarmState {
    bool ringing;            // Alarm currently ringing
    uint32_t ringStartTime;  // When alarm started ringing
    uint32_t pulseStartTime; // For LED animation timing
    float pulseRadius;       // Current radius for center-outward pulse
    bool active;             // Alarm system active
    LEDMode previousMode;    // Mode before alarm triggered (to restore after dismissal)
    bool wasRecording;       // If recording was active when alarm triggered
    bool wasPlayingResponse; // If playing response when alarm triggered
} alarmState = {false, 0, 0, 0.0f, false, LED_IDLE, false, false};

// ============== FORWARD DECLARATIONS ==============
void onWebSocketEvent(WStype_t type, uint8_t * payload, size_t length);
void websocketTask(void * parameter);
void ledTask(void * parameter);
void audioTask(void * parameter);
void updateLEDs();
bool initI2SMic();
bool initI2SSpeaker();
void handleWebSocketMessage(uint8_t* payload, size_t length);
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
    DEBUG_PRINT("📜 Starting marquee: '%s' -> mode %d\n", text.c_str(), nextMode);
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
        Serial.println("ERROR: Failed to create LED mutex!");
    }
    Serial.println("\n\n========================================");
    Serial.println("=== JELLYBERRY BOOT STARTING ===");
    Serial.println("========================================");
    Serial.flush();

    // Initialize LED strip (144 LEDs on GPIO 1)
    Serial.write("LED_INIT_START\r\n", 16);
    FastLED.addLeds<LED_CHIPSET, LED_DATA_PIN, LED_COLOR_ORDER>(leds, NUM_LEDS);
    FastLED.setBrightness(LED_BRIGHTNESS);
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
        Serial.println("✗ Failed to create audio queue");
        currentLEDMode = LED_ERROR;
        return;
    }
    Serial.println("✓ Audio queue created");
    Serial.flush();
    
    // Raw PCM streaming - no codec initialization needed
    Serial.println("✓ Audio pipeline: Raw PCM (16-bit, 16kHz mic → 24kHz speaker)");

    // Initialize I2S audio
    if (!initI2SMic()) {
        Serial.println("✗ Microphone init failed");
        currentLEDMode = LED_ERROR;
        return;
    }
    Serial.println("✓ Microphone initialized");

    if (!initI2SSpeaker()) {
        Serial.println("✗ Speaker init failed");
        currentLEDMode = LED_ERROR;
        return;
    }
    Serial.println("✓ Speaker initialized");

    // Initialize touch pads
    pinMode(TOUCH_PAD_START_PIN, INPUT_PULLDOWN);
    pinMode(TOUCH_PAD_STOP_PIN, INPUT_PULLDOWN);
    Serial.printf("✓ Touch pads initialized (START=%d, STOP=%d)\n", 
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
            Serial.println("\n✓ WiFi connected");
            Serial.printf("IP: %s\n", WiFi.localIP().toString().c_str());
            Serial.printf("Signal: %d dBm\n", WiFi.RSSI());
            
            // Configure NTP time sync (GMT timezone)
            configTime(0, 0, "pool.ntp.org", "time.nist.gov");
            Serial.println("⏰ NTP time sync configured");
        } else {
            retryCount++;
            Serial.printf("\n✗ Connection attempt failed (status: %d)\n", WiFi.status());
        }
    }
    
    if (!connected) {
        Serial.println("✗ WiFi connection failed after all retries");
        currentLEDMode = LED_ERROR;
        return;
    }

    // Initialize WebSocket to edge server
    wifiClient.setInsecure(); // Skip certificate validation
    String wsPath = String(EDGE_SERVER_PATH) + "?device_id=" + String(DEVICE_ID);
    
    #if USE_SSL
    webSocket.beginSSL(EDGE_SERVER_HOST, EDGE_SERVER_PORT, wsPath.c_str(), "", "wss");
    Serial.printf("✓ WebSocket initialized to wss://%s:%d%s\n", EDGE_SERVER_HOST, EDGE_SERVER_PORT, wsPath.c_str());
    #else
    webSocket.begin(EDGE_SERVER_HOST, EDGE_SERVER_PORT, wsPath.c_str());
    Serial.printf("✓ WebSocket initialized to ws://%s:%d%s\n", EDGE_SERVER_HOST, EDGE_SERVER_PORT, wsPath.c_str());
    #endif
    
    webSocket.onEvent(onWebSocketEvent);
    webSocket.setReconnectInterval(WS_RECONNECT_INTERVAL);
    webSocket.enableHeartbeat(60000, 30000, 5);  // Ping every 60s, timeout 30s, 5 retries = ~210s tolerance
    Serial.println("✓ WebSocket initialized with relaxed keepalive");
    
    // Note: TCP buffer sizes are controlled by lwIP configuration, not runtime changeable
    Serial.println("✓ Using default TCP buffers (configured in sdkconfig)");

    // Start FreeRTOS tasks
    // WebSocket needs high priority (3) and larger stack for heavy audio streaming
    xTaskCreatePinnedToCore(websocketTask, "WebSocket", 16384, NULL, 3, &websocketTaskHandle, CORE_1);  // Increased from 8KB to 16KB
    xTaskCreatePinnedToCore(ledTask, "LEDs", 4096, NULL, 0, &ledTaskHandle, CORE_0);
    xTaskCreatePinnedToCore(audioTask, "Audio", 32768, NULL, 2, &audioTaskHandle, CORE_1);  // 32KB for audio buffers + processing
    Serial.println("✓ Tasks created on dual cores");

    // Play startup sound
    Serial.println("🔊 Playing startup sound...");
    playStartupSound();
    
    Serial.printf("=== Initialization Complete ===  [LEDMode: IDLE]\n");
    Serial.println("Touch START pad to begin recording");
}

// ============== MAIN LOOP ==============
void loop() {
    static uint32_t lastPrint = 0;
    static uint32_t lastWiFiCheck = 0;
    static uint32_t lastAlarmCheck = 0;
    
    if (millis() - lastPrint > 5000) {
        Serial.write("LOOP_TICK\r\n", 11);
        lastPrint = millis();
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
                            
                            alarmState.ringing = true;
                            alarmState.ringStartTime = millis();
                            alarmState.pulseStartTime = millis();
                            alarmState.pulseRadius = 0.0f;
                            currentLEDMode = LED_ALARM;
                            DEBUG_PRINT("⏰ Alarm %u ringing after snooze! (interrupted mode: %d)\n", alarms[i].alarmID, alarmState.previousMode);
                            
                            // Play alarm sound
                            isPlayingAlarm = true;
                            isPlayingResponse = true;
                            firstAudioChunk = true;
                            lastAudioChunkTime = millis();
                            JsonDocument alarmDoc;
                            alarmDoc["action"] = "requestAlarm";
                            String alarmMsg;
                            serializeJson(alarmDoc, alarmMsg);
                            DEBUG_PRINTLN("🔔 Requesting alarm sound from server");
                            webSocket.sendTXT(alarmMsg);
                            
                            break;
                        }
                    } else if (now >= alarms[i].triggerTime) {
                        // Alarm time reached!
                        
                        // Save current state before switching to alarm
                        alarmState.previousMode = currentLEDMode;
                        alarmState.wasRecording = recordingActive;
                        alarmState.wasPlayingResponse = isPlayingResponse;
                        
                        alarmState.ringing = true;
                        alarmState.ringStartTime = millis();
                        alarmState.pulseStartTime = millis();
                        alarmState.pulseRadius = 0.0f;
                        currentLEDMode = LED_ALARM;
                        DEBUG_PRINT("⏰ Alarm %u triggered at %s (interrupted mode: %d)\n", alarms[i].alarmID, asctime(&timeinfo), alarmState.previousMode);
                        
                        // Play alarm sound
                        isPlayingAlarm = true;
                        isPlayingResponse = true;
                        firstAudioChunk = true;
                        lastAudioChunkTime = millis();
                        JsonDocument alarmDoc;
                        alarmDoc["action"] = "requestAlarm";
                        String alarmMsg;
                        serializeJson(alarmDoc, alarmMsg);
                        DEBUG_PRINTLN("🔔 Requesting alarm sound from server");
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
        
        // Detect button 2 press start
        if (stopTouch && !stopPressed) {
            button2PressStart = millis();
        }
        
        // Button 2 long-press: Return to IDLE and start Gemini recording
        if (!stopTouch && stopPressed && !recordingActive && 
            (millis() - button2PressStart) >= BUTTON2_LONG_PRESS) {
            DEBUG_PRINTLN("🏠 Button 2 long-press: Returning to IDLE + starting recording");
            
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
                meditationState.active = false;
                meditationState.paused = false;
                meditationState.phaseStartTime = 0;
                meditationState.streaming = false;
                volumeMultiplier = meditationState.savedVolume;  // Restore volume
                Serial.printf("🔊 Volume restored to %.0f%%\n", volumeMultiplier * 100);
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
            DEBUG_PRINTLN("🎤 Recording started via long-press");
            
            stopPressed = stopTouch;
            lastDebounceTime = millis();
            return;  // Skip normal button handling
        }
        
        // STOP button short press: Cycle through modes
        // IDLE → AMBIENT_VU → AMBIENT → POMODORO → MEDITATION → IDLE
        // Allow during ambient modes, block only during Gemini responses (non-ambient)
        if (stopTouch && !stopPressed && !recordingActive && 
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
                DEBUG_PRINTLN("🎵 Ambient VU meter mode enabled");
            } else if (modeToCheck == LED_AMBIENT_VU) {
                ambientVUMode = false;
                currentAmbientSoundType = SOUND_RAIN;  // Start with rain
                ambientSound.name = "rain";
                ambientSound.active = true;
                ambientSound.sequence++;
                isPlayingAmbient = true;
                isPlayingResponse = false;
                firstAudioChunk = true;
                lastAudioChunkTime = millis();  // Initialize timing
                DEBUG_PRINT("🌧️  Ambient mode: Rain (seq %d)\n", ambientSound.sequence);
                // Show marquee first
                startMarquee("RAIN", CRGB(0, 100, 255), LED_AMBIENT);  // Blue for rain
                // Request rain sounds from server (sequence number handles overlap)
                JsonDocument ambientDoc;
                ambientDoc["action"] = "requestAmbient";
                ambientDoc["sound"] = "rain";
                ambientDoc["sequence"] = ambientSound.sequence;
                String ambientMsg;
                serializeJson(ambientDoc, ambientMsg);
                DEBUG_PRINT("📤 Sending ambient request: %s\n", ambientMsg.c_str());
                webSocket.sendTXT(ambientMsg);
            } else if (modeToCheck == LED_AMBIENT) {
                // Stop ambient sound and switch to Pomodoro mode
                DEBUG_PRINTLN("🔄 Mode transition: AMBIENT → POMODORO (cleaning up...)");
                
                // Clear audio buffer to prevent bleed
                i2s_zero_dma_buffer(I2S_NUM_1);
                delay(50);
                i2s_zero_dma_buffer(I2S_NUM_1);  // Double clear for safety
                
                // Clear LED buffer to remove rain/ocean effects (with mutex)
                if (xSemaphoreTake(ledMutex, portMAX_DELAY) == pdTRUE) {
                    if (currentLEDMode == LED_MEDITATION) {
                        DEBUG_PRINT("⚠️ INTERRUPTING meditation: mode transition clear\n");
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
                
                DEBUG_PRINTLN("🍅 Pomodoro mode activated (will auto-start after marquee)");
                startMarquee("POMODORO", CRGB(255, 100, 0), LED_POMODORO);  // Orange for pomodoro
            } else if (modeToCheck == LED_POMODORO) {
                // Exit Pomodoro and go to Meditation
                DEBUG_PRINTLN("⏹️  Pomodoro mode stopped");
                
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
                        DEBUG_PRINT("⚠️ INTERRUPTING meditation: Pomodoro mode transition (2x clear)\n");
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
                meditationState.phaseStartTime = 0;  // Don't start yet - wait for marquee
                meditationState.active = true;
                meditationState.paused = true;  // Start paused until marquee completes
                meditationState.streaming = false;
                
                // Lower volume for meditation (prevents vibration/distortion)
                meditationState.savedVolume = volumeMultiplier;
                volumeMultiplier = 0.05f;  // 5% volume for meditation (low freq sounds)
                DEBUG_PRINT("🔊 Volume: %.0f%% → 5%% for meditation\n", meditationState.savedVolume * 100);
                
                Serial.println("🧘 Meditation mode activated (waiting for marquee)");
                startMarquee("MEDITATION", CRGB(255, 0, 255), LED_MEDITATION);  // Magenta
            } else if (modeToCheck == LED_MEDITATION) {
                // Exit Meditation and go to Clock
                DEBUG_PRINTLN("⏹️  Meditation mode stopped");
                
                // Clear meditation state
                meditationState.active = false;
                meditationState.paused = false;
                meditationState.phaseStartTime = 0;
                meditationState.streaming = false;
                volumeMultiplier = meditationState.savedVolume;  // Restore volume
                Serial.printf("🔊 Volume restored to %.0f%%\n", volumeMultiplier * 100);
                
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
                
                DEBUG_PRINTLN("🕐 Clock mode activated");
                startMarquee("CLOCK", CRGB::White, LED_CLOCK);
            } else if (modeToCheck == LED_CLOCK) {
                // Exit Clock and go to Lamp
                DEBUG_PRINTLN("⏹️  Clock mode stopped");
                
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
                
                DEBUG_PRINTLN("💡 Lamp mode activated");
                startMarquee("LAMP", CRGB::White, LED_LAMP);
            } else if (modeToCheck == LED_LAMP) {
                // Exit Lamp and return to IDLE
                DEBUG_PRINTLN("⏹️  Lamp mode stopped");
                
                // Clear lamp state
                lampState.active = false;
                lampState.fullyLit = false;
                
                DEBUG_PRINTLN("💤 Returning to IDLE mode");
                currentLEDMode = LED_IDLE;
                targetLEDMode = LED_IDLE;
                meditationState.paused = false;
                meditationState.phaseStartTime = 0;
                meditationState.streaming = false;
                
                // Restore original volume
                volumeMultiplier = meditationState.savedVolume;
                DEBUG_PRINT("🔊 Volume restored to %.0f%%\n", volumeMultiplier * 100);
                
                // Clear LED buffer to remove meditation breathing visualization (with mutex)
                if (xSemaphoreTake(ledMutex, portMAX_DELAY) == pdTRUE) {
                    DEBUG_PRINT("🧘 Exiting meditation: clearing LEDs\n");
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
        
        // Ambient mode: Button 1 cycles between sounds (rain → ocean → rainforest)
        static uint32_t lastAmbientCycle = 0;
        if (currentLEDMode == LED_AMBIENT && startTouch && !startPressed && 
            (millis() - lastAmbientCycle) > 500) {  // 500ms debounce
            
            // Stop current audio
            JsonDocument stopDoc;
            stopDoc["action"] = "stopAmbient";
            String stopMsg;
            serializeJson(stopDoc, stopMsg);
            webSocket.sendTXT(stopMsg);
            i2s_zero_dma_buffer(I2S_NUM_1);
            
            // Cycle to next sound
            if (currentAmbientSoundType == SOUND_RAIN) {
                currentAmbientSoundType = SOUND_OCEAN;
                ambientSound.name = "ocean";
                DEBUG_PRINT("🌊 Ambient: Switching to Ocean (seq %d)\n", ambientSound.sequence + 1);
                startMarquee("OCEAN", CRGB(0, 150, 200), LED_AMBIENT);  // Cyan
            } else if (currentAmbientSoundType == SOUND_OCEAN) {
                currentAmbientSoundType = SOUND_RAINFOREST;
                ambientSound.name = "rainforest";
                DEBUG_PRINT("🌿 Ambient: Switching to Rainforest (seq %d)\n", ambientSound.sequence + 1);
                startMarquee("FOREST", CRGB(50, 255, 50), LED_AMBIENT);  // Green
            } else {  // SOUND_RAINFOREST
                currentAmbientSoundType = SOUND_RAIN;
                ambientSound.name = "rain";
                DEBUG_PRINT("🌧️  Ambient: Switching to Rain (seq %d)\n", ambientSound.sequence + 1);
                startMarquee("RAIN", CRGB(0, 100, 255), LED_AMBIENT);  // Blue
            }
            
            // Update state
            ambientSound.sequence++;
            isPlayingAmbient = true;
            isPlayingResponse = false;
            firstAudioChunk = true;
            lastAudioChunkTime = millis();
            
            // Request new sound from server (will be sent after marquee completes)
            lastAmbientCycle = millis();
        }
        
        // Pomodoro mode: Button 1 long press = pause/resume, short press = Gemini
        // Button 2 cycles modes as usual
        static uint32_t button1PressStart = 0;
        static uint32_t lastPomodoroAction = 0;
        const uint32_t LONG_PRESS_DURATION = 2000;  // 2 seconds
        const uint32_t ACTION_DEBOUNCE = 500;  // 500ms between actions
        
        if (currentLEDMode == LED_POMODORO && pomodoroState.active) {
            // Detect button 1 press start
            if (startTouch && !startPressed) {
                button1PressStart = millis();
            }
            
            // On button 1 release, check duration
            if (!startTouch && startPressed && (millis() - lastPomodoroAction) > ACTION_DEBOUNCE) {
                uint32_t pressDuration = millis() - button1PressStart;
                
                if (pressDuration >= LONG_PRESS_DURATION) {
                    // Long press: Toggle pause/resume
                    if (pomodoroState.paused) {
                        // Resume from paused state
                        // Calculate how much time has already elapsed before pause
                        int sessionDuration;
                        if (pomodoroState.currentSession == PomodoroState::FOCUS) {
                            sessionDuration = pomodoroState.focusDuration * 60;
                        } else if (pomodoroState.currentSession == PomodoroState::SHORT_BREAK) {
                            sessionDuration = pomodoroState.shortBreakDuration * 60;
                        } else {
                            sessionDuration = pomodoroState.longBreakDuration * 60;
                        }
                        
                        // Restore original session duration
                        pomodoroState.totalSeconds = sessionDuration;
                        
                        // Adjust startTime so elapsed time accounts for time already consumed
                        int timeAlreadyElapsed = sessionDuration - pomodoroState.pausedTime;
                        pomodoroState.startTime = millis() - (timeAlreadyElapsed * 1000);
                        pomodoroState.pausedTime = 0;
                        pomodoroState.paused = false;
                        
                        DEBUG_PRINT("▶️  Pomodoro resumed from %d seconds remaining (long press)\n", sessionDuration - timeAlreadyElapsed);
                        // No sound on resume - user will source alternative
                    } else {
                        // Pause and save current position
                        uint32_t elapsed = (millis() - pomodoroState.startTime) / 1000;
                        pomodoroState.pausedTime = max(0, pomodoroState.totalSeconds - (int)elapsed);
                        pomodoroState.startTime = 0;
                        pomodoroState.paused = true;
                        DEBUG_PRINT("⏸️  Pomodoro paused at %d seconds remaining (long press)\n", pomodoroState.pausedTime);
                        // No sound on pause - user will source alternative
                    }
                    lastPomodoroAction = millis();
                }
                // Short press falls through to recording trigger below
            }
        }
        
        // Meditation mode: Button 1 controls pause/play and long-press chakra change
        static uint32_t lastMeditationAction = 0;
        
        if (currentLEDMode == LED_MEDITATION && meditationState.active) {
            // Detect button 1 press start (reuse button1PressStart from Pomodoro)
            if (startTouch && !startPressed) {
                button1PressStart = millis();
            }
            
            // On button 1 release, check duration
            if (!startTouch && startPressed && (millis() - lastMeditationAction) > ACTION_DEBOUNCE) {
                uint32_t pressDuration = millis() - button1PressStart;
                
                if (pressDuration >= LONG_PRESS_DURATION) {
                    // Long press: Next chakra
                    meditationState.currentChakra = (MeditationState::Chakra)((meditationState.currentChakra + 1) % 7);
                    meditationState.phase = MeditationState::INHALE;
                    meditationState.phaseStartTime = 0;
                    
                    // Stop current audio
                    JsonDocument stopDoc;
                    stopDoc["action"] = "stopAmbient";
                    String stopMsg;
                    serializeJson(stopDoc, stopMsg);
                    webSocket.sendTXT(stopMsg);
                    ambientSound.drainUntil = millis() + 500;  // Short drain
                    
                    // Request new chakra audio
                    JsonDocument reqDoc;
                    reqDoc["action"] = "requestAmbient";
                    char soundName[16];
                    sprintf(soundName, "om%03d", meditationState.currentChakra + 1);
                    reqDoc["sound"] = soundName;
                    reqDoc["sequence"] = ++ambientSound.sequence;
                    String reqMsg;
                    serializeJson(reqDoc, reqMsg);
                    webSocket.sendTXT(reqMsg);
                    meditationState.streaming = true;
                    
                    // Update ambient sound state
                    ambientSound.name = soundName;
                    firstAudioChunk = true;
                    lastAudioChunkTime = millis();
                    
                    // Show chakra name
                    DEBUG_PRINT("🧘 Chakra changed to %s\n", CHAKRA_NAMES[meditationState.currentChakra]);
                    startMarquee(CHAKRA_NAMES[meditationState.currentChakra], CRGB(255, 0, 255), LED_MEDITATION);
                    
                    playVolumeChime();
                    lastMeditationAction = millis();
                } else {
                    // Short press: Toggle pause/play
                    if (meditationState.paused) {
                        // Resume breathing
                        meditationState.phaseStartTime = millis();
                        meditationState.paused = false;
                        DEBUG_PRINTLN("▶️  Meditation resumed");
                        playVolumeChime();
                        lastMeditationAction = millis();
                    } else {
                        // Pause breathing
                        meditationState.phaseStartTime = 0;
                        meditationState.paused = true;
                        DEBUG_PRINTLN("⏸️  Meditation paused");
                        playVolumeChime();
                        lastMeditationAction = millis();
                    }
                }
            }
        }
        
        // Interrupt feature: START button during active playback stops audio and starts recording
        // Only interrupt if we've received audio recently (within 500ms) and turn is not complete
        // Note: Pomodoro now allowed - button 1 can interrupt responses during Pomodoro
        if (currentLEDMode != LED_MEDITATION && startTouch && !startPressed && isPlayingResponse && !turnComplete && 
            (millis() - lastAudioChunkTime) < 500) {
            DEBUG_PRINTLN("⏸️  Interrupted response - starting new recording");
            responseInterrupted = true;  // Flag to ignore remaining audio chunks
            isPlayingResponse = false;
            i2s_zero_dma_buffer(I2S_NUM_1);  // Stop audio immediately
            
            recordingActive = true;
            recordingStartTime = millis();
            lastVoiceActivityTime = millis();
            currentLEDMode = LED_RECORDING;
            DEBUG_PRINT("🎤 Recording started... (START=%d, STOP=%d)\n", startTouch, stopTouch);
        }
        // ALARM MODE: Button 1 or 2 dismisses alarm
        else if (currentLEDMode == LED_ALARM && alarmState.ringing && (startTouch || stopTouch) && !startPressed && !stopPressed) {
            DEBUG_PRINTLN("⏰ Alarm dismissed");
            
            // Clear alarm from memory
            for (int i = 0; i < MAX_ALARMS; i++) {
                if (alarms[i].enabled && !alarms[i].triggered) {
                    DEBUG_PRINT("✓ Alarm %u dismissed and cleared from slot %d\n", alarms[i].alarmID, i);
                    
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
            DEBUG_PRINTLN("🔕 Sent stop alarm request to server");
            
            // Stop I2S output - let buffered audio drain naturally
            i2s_zero_dma_buffer(I2S_NUM_1);
            
            // Don't clear the queue - let audio task handle cleanup naturally
            // (clearing queue while audio task is active causes crashes)
            
            // Restore previous mode
            DEBUG_PRINT("↩️  Restoring previous mode: %d (recording=%d, playing=%d)\n", 
                         alarmState.previousMode, alarmState.wasRecording, alarmState.wasPlayingResponse);
            
            currentLEDMode = alarmState.previousMode;
            
            // Restore recording state if it was active
            if (alarmState.wasRecording) {
                recordingActive = true;
                DEBUG_PRINTLN("↩️  Resuming recording");
            }
            
            // Restore playback state if it was active
            if (alarmState.wasPlayingResponse) {
                isPlayingResponse = true;
                lastAudioChunkTime = millis();  // Reset timeout
                DEBUG_PRINTLN("↩️  Resuming audio playback");
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
        // LAMP MODE: Button 1 cycles colors (WHITE → RED → GREEN → BLUE)
        else if (currentLEDMode == LED_LAMP && lampState.active && startTouch && !startPressed) {
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
            
            DEBUG_PRINT("🎨 Lamp color: %s → %s\n", 
                         (const char*[]){"WHITE", "RED", "GREEN", "BLUE"}[lampState.previousColor], 
                         (const char*[]){"WHITE", "RED", "GREEN", "BLUE"}[lampState.currentColor]);
        }
        // Start recording on rising edge (normal case - not interrupting)
        // Block if: recording active, playing response, OR in ambient sound mode, OR conversation window is open, OR in Meditation/Ambient/Lamp mode
        // Special handling for Pomodoro: only trigger on button release after short press
        else if (currentLEDMode != LED_MEDITATION && currentLEDMode != LED_AMBIENT && currentLEDMode != LED_LAMP && !recordingActive && !isPlayingResponse && !isPlayingAmbient && !conversationMode) {
            // Pomodoro mode: only start recording on button release after SHORT press
            bool shouldStartRecording = false;
            if (currentLEDMode == LED_POMODORO) {
                // Wait for button release
                if (!startTouch && startPressed && (millis() - lastPomodoroAction) > ACTION_DEBOUNCE) {
                    uint32_t pressDuration = millis() - button1PressStart;
                    // Only start recording if SHORT press (long press handled above for pause/resume)
                    if (pressDuration < LONG_PRESS_DURATION) {
                        shouldStartRecording = true;
                        DEBUG_PRINTLN("🎤 Short press detected in Pomodoro - starting Gemini");
                    }
                }
            } else {
                // Other modes: start recording on button press as usual
                shouldStartRecording = (startTouch && !startPressed);
            }
            
            if (shouldStartRecording) {
            // Additional safety: don't start recording if alarm is ringing
            if (alarmState.ringing) {
                DEBUG_PRINTLN("⚠️  Cannot start recording - alarm is ringing");
                startPressed = true;
                return;
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
                DEBUG_PRINTLN("✓ Cancelled drain timer - ready for new audio");
                ambientSound.drainUntil = 0;
            }
            
            // Exit ambient VU mode
            if (ambientVUMode) {
                ambientVUMode = false;
                DEBUG_PRINTLN("🎵 Ambient VU meter mode disabled");
            }
            
            recordingActive = true;
            recordingStartTime = millis();
            lastVoiceActivityTime = millis();
            currentLEDMode = LED_RECORDING;
            DEBUG_PRINT("🎤 Recording started... (START=%d, STOP=%d)\n", startTouch, stopTouch);
            }  // End of shouldStartRecording block
        }
        startPressed = startTouch;
        
        // Stop recording only on timeout (manual stop removed - rely on VAD)
        if (recordingActive && (millis() - recordingStartTime) > MAX_RECORDING_DURATION_MS) {
            recordingActive = false;
            // Don't change LED mode if ambient sound is already active
            if (!ambientSound.active) {
                currentLEDMode = LED_PROCESSING;
                processingStartTime = millis();
            }
            DEBUG_PRINT("⏹️  Recording stopped - Duration: %dms (max duration reached)\n", millis() - recordingStartTime);
        }
        stopPressed = stopTouch;
        
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
        DEBUG_PRINTLN("⏹️  Recording stopped - Silence detected");
    }
    
    // Show thinking animation if response is taking too long (after delay)
    if (currentLEDMode == LED_RECORDING && processingStartTime > 0 && 
        (millis() - processingStartTime) > THINKING_ANIMATION_DELAY_MS && 
        (millis() - processingStartTime) < 10000) {
        currentLEDMode = LED_PROCESSING;
        DEBUG_PRINTLN("⏳ Response delayed - showing thinking animation");
    }
    
    // Timeout PROCESSING mode if no response after 10 seconds
    if ((currentLEDMode == LED_PROCESSING || currentLEDMode == LED_RECORDING) && 
        processingStartTime > 0 && (millis() - processingStartTime) > 10000) {
        DEBUG_PRINT("⚠️  Processing timeout after 10s - no response received (mode was %d)\n", currentLEDMode);
        processingStartTime = 0;
        
        // Return to visualizations if active, otherwise IDLE
        if (pomodoroState.active) {
            currentLEDMode = LED_POMODORO;
            DEBUG_PRINTLN("↩️  Timeout - returning to POMODORO display");
        } else if (timerState.active) {
            currentLEDMode = LED_TIMER;
            DEBUG_PRINTLN("↩️  Timeout - returning to TIMER display");
        } else if (moonState.active) {
            currentLEDMode = LED_MOON;
            moonState.displayStartTime = millis();
            DEBUG_PRINTLN("↩️  Timeout - returning to MOON display");
        } else if (tideState.active) {
            currentLEDMode = LED_TIDE;
            tideState.displayStartTime = millis();
            DEBUG_PRINTLN("↩️  Timeout - returning to TIDE display");
        } else {
            currentLEDMode = LED_IDLE;
            DEBUG_PRINTLN("↩️  Timeout - returning to IDLE");
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
            DEBUG_PRINT("⏹️  Audio playback complete (timeout + queue drained to %u), turnComplete=%d\n", queueDepth, turnComplete);
        
        // Check if turn is complete - if so, decide what to show
        // Skip conversation mode for startup greeting
        if (turnComplete && !waitingForGreeting) {
            // Always open conversation window first (10 second listening period)
            // Visualizations will show after conversation window closes
            conversationMode = true;
            conversationWindowStart = millis();
            currentLEDMode = LED_CONVERSATION_WINDOW;
            Serial.println("💬 Conversation window opened - speak anytime in next 10 seconds");
        } else {
            // Turn not complete - show visualizations or return to idle/ambient
            // Priority: Pomodoro > Timer > Moon > Tide > Ambient VU > Idle
            if (pomodoroState.active) {
                currentLEDMode = LED_POMODORO;
                DEBUG_PRINTLN("✓ Audio playback complete - switching to POMODORO display");
            } else if (timerState.active) {
                currentLEDMode = LED_TIMER;
                DEBUG_PRINTLN("✓ Audio playback complete - switching to TIMER display");
            } else if (moonState.active) {
                currentLEDMode = LED_MOON;
                moonState.displayStartTime = millis();
                DEBUG_PRINTLN("✓ Audio playback complete - switching to MOON display");
            } else if (tideState.active) {
                currentLEDMode = LED_TIDE;
                tideState.displayStartTime = millis();
                DEBUG_PRINT("✓ Audio playback complete - switching to TIDE display (state=%s, level=%.2f)\n", 
                             tideState.state.c_str(), tideState.waterLevel);
            } else if (ambientVUMode) {
                currentLEDMode = LED_AMBIENT_VU;
                DEBUG_PRINTLN("✓ Audio playback complete - returning to AMBIENT VU mode");
            } else {
                currentLEDMode = LED_IDLE;
                DEBUG_PRINTLN("✓ Audio playback complete - switching to IDLE");
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
    
    // Check for Pomodoro session completion and auto-advance
    if (currentLEDMode == LED_POMODORO && pomodoroState.active && !pomodoroState.paused && pomodoroState.startTime > 0) {
        uint32_t elapsed = (millis() - pomodoroState.startTime) / 1000;
        int secondsRemaining = pomodoroState.totalSeconds - (int)elapsed;
        
        if (secondsRemaining <= 0) {
            DEBUG_PRINTLN("⏰ Pomodoro session complete!");
            
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
                    DEBUG_PRINT("🍅 → 🟦 Focus complete! Starting long break (%d min)\n", pomodoroState.longBreakDuration);
                    pomodoroState.currentSession = PomodoroState::LONG_BREAK;
                    pomodoroState.totalSeconds = pomodoroState.longBreakDuration * 60;
                    startMarquee("LONG BREAK", CRGB(0, 100, 255), LED_POMODORO);
                    
                    // Start immediately (don't pause)
                    pomodoroState.startTime = millis();
                    pomodoroState.pausedTime = 0;
                    pomodoroState.paused = false;
                } else {
                    // Normal short break after focus
                    DEBUG_PRINT("🍅 → 🟩 Focus complete! Starting short break (%d min) [%d/4]\n", pomodoroState.shortBreakDuration, pomodoroState.sessionCount);
                    pomodoroState.currentSession = PomodoroState::SHORT_BREAK;
                    pomodoroState.totalSeconds = pomodoroState.shortBreakDuration * 60;
                    startMarquee("SHORT BREAK", CRGB(0, 255, 0), LED_POMODORO);
                    
                    // Start immediately (don't pause)
                    pomodoroState.startTime = millis();
                    pomodoroState.pausedTime = 0;
                    pomodoroState.paused = false;
                }
            } else if (pomodoroState.currentSession == PomodoroState::LONG_BREAK) {
                // Long break complete - END OF CYCLE, return to paused state
                DEBUG_PRINTLN("🟦 → 🛑 Long break complete! Pomodoro cycle finished - press button to restart");
                pomodoroState.currentSession = PomodoroState::FOCUS;
                pomodoroState.totalSeconds = pomodoroState.focusDuration * 60;
                pomodoroState.sessionCount = 0;  // Reset counter for next cycle
                startMarquee("CYCLE DONE", CRGB(255, 255, 0), LED_POMODORO);  // Yellow = complete
                
                // Stop and wait for user to press button
                pomodoroState.startTime = 0;
                pomodoroState.pausedTime = pomodoroState.totalSeconds;
                pomodoroState.paused = true;
            } else {
                // Short break complete, return to focus
                DEBUG_PRINT("🟩 → 🍅 Break complete! Starting focus session (%d min)\n", pomodoroState.focusDuration);
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
        Serial.printf("✓ Ambient sound completed: %s - returning to IDLE\n", ambientSound.name.c_str());
        
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
                Serial.println("🌊 Tide display complete - opening conversation window");
                tideState.active = false;
                shouldOpenConversation = true;
            }
        } else if (currentLEDMode == LED_MOON && moonState.active) {
            if (millis() - moonState.displayStartTime > 10000) {
                Serial.println("🌙 Moon display complete - opening conversation window");
                moonState.active = false;
                shouldOpenConversation = true;
            }
        }
        // Note: Timer has its own expiry logic and shouldn't auto-transition
        
        if (shouldOpenConversation) {
            Serial.printf("🔄 Transition to conversation: LED=%d, recording=%d, playing=%d, alarm=%d\n",
                         currentLEDMode, recordingActive, isPlayingResponse, alarmState.ringing);
            conversationMode = true;
            conversationWindowStart = millis();
            currentLEDMode = LED_CONVERSATION_WINDOW;
            Serial.println("💬 Conversation window opened - speak anytime in next 10 seconds");
        }
    }
    
    // Conversation window monitoring
    if (conversationMode && !isPlayingResponse && !recordingActive && !alarmState.ringing) {
        uint32_t elapsed = millis() - conversationWindowStart;
        
        // Debug every 2 seconds - show current state
        static uint32_t lastDebugPrint = 0;
        if (millis() - lastDebugPrint > 2000) {
            Serial.printf("💬 [CONV] active, window=%ums/%u, LED=%d, turnComplete=%d\n", 
                         elapsed, CONVERSATION_WINDOW_MS, currentLEDMode, turnComplete);
            lastDebugPrint = millis();
        }
        
        if (elapsed < CONVERSATION_WINDOW_MS) {
            // Window is open - check for voice activity
            static int16_t windowBuffer[320];  // 20ms at 16kHz for quick VAD check
            size_t bytes_read = 0;
            
            // Safety: Only read if not playing alarm
            if (isPlayingAlarm) {
                return;
            }
            
            esp_err_t result = i2s_read(I2S_NUM_0, windowBuffer, sizeof(windowBuffer), &bytes_read, 10);
            
            if (result == ESP_OK && bytes_read > 0) {
                size_t samples_read = bytes_read / sizeof(int16_t);
                
                // Apply same 16x gain as normal recording (INMP441 has low output)
                const int16_t GAIN = 16;
                for (size_t i = 0; i < samples_read; i++) {
                    int32_t amplified = (int32_t)windowBuffer[i] * GAIN;
                    // Clamp to int16_t range
                    if (amplified > 32767) amplified = 32767;
                    if (amplified < -32768) amplified = -32768;
                    windowBuffer[i] = (int16_t)amplified;
                }
                
                // Calculate amplitude
                int32_t sum = 0;
                for (size_t i = 0; i < samples_read; i++) {
                    sum += abs(windowBuffer[i]);
                }
                int32_t avgAmplitude = sum / samples_read;
                
                // Debug sound detection
                static uint32_t lastSoundPrint = 0;
                if (avgAmplitude > 100 && millis() - lastSoundPrint > 500) {
                    Serial.printf("🔊 Sound: %d (threshold=%d, bytes=%d)\n", avgAmplitude, VAD_CONVERSATION_THRESHOLD, bytes_read);
                    lastSoundPrint = millis();
                }
                
                // Use lower threshold during conversation window for faster response
                if (samples_read > 0 && avgAmplitude > VAD_CONVERSATION_THRESHOLD) {
                    // Voice detected! Start recording immediately
                    Serial.printf("🎤 Voice detected in conversation window - avgAmp=%d, starting recording\n", avgAmplitude);
                    
                    // Set audio level immediately for instant VU meter feedback
                    currentAudioLevel = avgAmplitude;
                    
                    // Exit conversation mode and start recording
                    conversationMode = false;  // Exit window mode
                    conversationRecording = true;  // Flag for longer silence timeout
                    recordingActive = true;
                    recordingStartTime = millis();
                    lastVoiceActivityTime = millis();
                    processingStartTime = 0;  // CRITICAL: Reset processing timer to prevent immediate timeout
                    currentLEDMode = LED_RECORDING;
                    lastDebounceTime = millis();  // Reset debounce to prevent immediate stop
                    
                    Serial.printf("✅ Recording mode activated: LED=%d, audioLevel=%d\n", currentLEDMode, currentAudioLevel);
                }
            } else {
                static uint32_t lastErrorPrint = 0;
                if (millis() - lastErrorPrint > 3000) {
                    Serial.printf("⚠️  I2S error: result=%d, bytes=%d\n", result, bytes_read);
                    lastErrorPrint = millis();
                }
            }
        } else {
            // Window expired with no voice - return to visualizations or idle
            Serial.println("💬 Conversation window expired");
            conversationMode = false;
            
            // Priority: Pomodoro > Timer > Moon > Tide > Idle
            if (pomodoroState.active) {
                currentLEDMode = LED_POMODORO;
                Serial.println("↩️  Returning to POMODORO display");
            } else if (timerState.active) {
                currentLEDMode = LED_TIMER;
                Serial.println("↩️  Returning to TIMER display");
            } else if (moonState.active) {
                currentLEDMode = LED_MOON;
                moonState.displayStartTime = millis();
                Serial.println("↩️  Returning to MOON display");
            } else if (tideState.active) {
                currentLEDMode = LED_TIDE;
                tideState.displayStartTime = millis();
                Serial.println("↩️  Returning to TIDE display");
            } else {
                currentLEDMode = LED_IDLE;
                Serial.println("↩️  Returning to IDLE");
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
                
                // Convert mono → stereo with volume
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
                    Serial.printf("⚠️  I2S write failed: result=%d, wrote=%u/%u\n", result, bytes_written, numSamples*4);
                    // Continue anyway - don't get stuck
                }
                
                // Update last audio chunk time to prevent timeout while queue has data
                lastAudioChunkTime = millis();
                
                // Debug periodically
                static uint32_t lastPlaybackDebug = 0;
                if (millis() - lastPlaybackDebug > 1000) {
                    Serial.printf("[PLAYBACK] Raw PCM: %d bytes → %d samples, level=%d, queue=%d\n", 
                                 playbackChunk.length, numSamples, currentAudioLevel, uxQueueMessagesWaiting(audioOutputQueue));
                    lastPlaybackDebug = millis();
                }
            } else {
                Serial.printf("❌ Invalid PCM chunk: %d bytes (%d samples)\n", playbackChunk.length, numSamples);
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
        } else {
            // Nothing to do - sleep briefly
            vTaskDelay(pdMS_TO_TICKS(1));
        }
    }
}

// ============== AUDIO FEEDBACK ==============
void playStartupSound() {
    // Play a pleasant ascending melody on startup
    const int sampleRate = 24000;
    const int noteDuration = 120;  // 120ms per note
    const int numSamples = (sampleRate * noteDuration) / 1000;
    
    // Musical notes: C5, E5, G5, C6 (major chord arpeggio)
    const float frequencies[] = {523.25f, 659.25f, 783.99f, 1046.50f};
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
        i2s_write(I2S_NUM_1, toneBuffer, numSamples * 4, &bytes_written, portMAX_DELAY);
    }
}

void playZenBell() {
    // Request zen bell sound from server
    if (!isWebSocketConnected) {
        Serial.println("⚠️  Cannot play zen bell - WebSocket not connected");
        return;
    }
    
    JsonDocument bellDoc;
    bellDoc["action"] = "requestZenBell";
    String bellMsg;
    serializeJson(bellDoc, bellMsg);
    webSocket.sendTXT(bellMsg);
    Serial.println("🔔 Requesting zen bell from server");
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
    
    // Simple Base64 encode
    const char* base64_chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    String encoded = "";
    int val = 0, valb = -6;
    for (size_t i = 0; i < length; i++) {
        val = (val << 8) + data[i];
        valb += 8;
        while (valb >= 0) {
            encoded += base64_chars[(val >> valb) & 0x3F];
            valb -= 6;
        }
    }
    if (valb > -6) encoded += base64_chars[((val << 8) >> (valb + 8)) & 0x3F];
    while (encoded.length() % 4) encoded += '=';
    
    // Live API format: realtimeInput.audio as Blob
    JsonObject audio = doc["realtimeInput"]["audio"].to<JsonObject>();
    audio["data"] = encoded;
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
                Serial.printf("✓ WebSocket Connected to Edge Server! (disconnect count: %d)\n", disconnectCount);
                isWebSocketConnected = true;
                shutdownSoundPlayed = false;  // Reset flag on successful connection
                currentLEDMode = LED_CONNECTED;
                
                // Edge server handles Gemini setup automatically
                Serial.println("✓ Waiting for 'ready' message from server");
                
                // Show connection on LEDs (with mutex)
                if (xSemaphoreTake(ledMutex, portMAX_DELAY) == pdTRUE) {
                    fill_solid(leds, NUM_LEDS, CRGB::Green);
                    FastLED.show();
                    xSemaphoreGive(ledMutex);
                }
                delay(500);
                
                // Resume ambient mode if it was active before disconnect
                if (ambientSound.active && !ambientSound.name.isEmpty()) {
                    Serial.printf("▶️  Resuming ambient sound: %s (seq %d)\n", 
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
                    Serial.println("▶️  Resuming VU meter mode");
                } else {
                    currentLEDMode = LED_IDLE;
                }
            }
            break;
            
        case WStype_TEXT:
            Serial.printf("📥 Received TEXT: %d bytes: %.*s\n", length, (int)min(length, (size_t)200), (char*)payload);
            handleWebSocketMessage(payload, length);
            break;
            
        case WStype_BIN:
            {
                // 🔍 DIAGNOSTIC: Track packet timing to detect bursting
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
                    Serial.printf("📊 [STREAM] %u packets, %.1fms avg interval, %u fast (<20ms), %u KB/s, queue=%u\n", 
                                 packetCount, avgInterval, fastPackets, bytesPerSec/1024, queueDepth);
                    packetCount = 0;
                    fastPackets = 0;
                    binaryBytesReceived = 0;
                    lastBinaryRateLog = now;
                }
                
                // Drain mode: silently discard all packets for 2s after stopping ambient
                if (ambientSound.drainUntil > 0 && millis() < ambientSound.drainUntil) {
                    // Silently discard - no logging to prevent spam
                    static uint32_t drainCount = 0;
                    static uint32_t lastDrainLog = 0;
                    drainCount++;
                    if (millis() - lastDrainLog > 1000) {
                        Serial.printf("🚰 Draining buffered chunks... (%u drained so far)\n", drainCount);
                        lastDrainLog = millis();
                    }
                    break;  // Discard this packet
                }
                
                // Clear drain timer once period expires
                if (ambientSound.drainUntil > 0 && millis() >= ambientSound.drainUntil) {
                    Serial.println("✓ Drain complete - ready for new audio");
                    ambientSound.drainUntil = 0;
                }
                
                // Check for ambient magic header + sequence number
                // Magic bytes 0xA5 0x5A are very unlikely to appear in PCM audio
                if (length >= 4 && payload != nullptr && payload[0] == 0xA5 && payload[1] == 0x5A) {
                    // This has ambient magic header - extract sequence number
                    uint16_t chunkSequence = payload[2] | (payload[3] << 8);
                    
                    if (ambientSound.active && chunkSequence == ambientSound.sequence) {
                        // Valid ambient chunk - strip magic header + sequence
                        payload += 4;
                        length -= 4;
                    } else {
                        // Stale ambient chunk (wrong sequence or ambient not active)
                        // Rate-limit logging to prevent spam (max 1 every 10 seconds)
                        static uint32_t lastDiscardLog = 0;
                        static uint32_t discardsSinceLog = 0;
                        discardsSinceLog++;
                        
                        if (millis() - lastDiscardLog > 10000) {  // Changed from 1000 to 10000 (10 seconds)
                            if (discardsSinceLog > 0) {
                                Serial.printf("🚫 Discarded %u stale ambient chunks in last 10s (seq %d, active=%d, expected=%d)\n", 
                                             discardsSinceLog, chunkSequence, ambientSound.active, ambientSound.sequence);
                            }
                            discardsSinceLog = 0;
                            lastDiscardLog = millis();
                        }
                        break;  // Discard this chunk
                    }
                }
                // else: Gemini audio (no magic header) - continue to play
                
                // Ignore audio if response was interrupted (but not for ambient/alarm sounds)
                if (responseInterrupted && !isPlayingAmbient && !isPlayingAlarm) {
                    Serial.println("🚫 Discarding audio chunk (response was interrupted)");
                    break;
                }
                
                // Raw PCM audio data from server (16-bit mono samples)
                // This handles BOTH Gemini responses and ambient sounds
                if (!isPlayingResponse) {
                    // CRITICAL: Stop recording immediately when response arrives (even during prebuffer)
                    // This prevents connection overload from bidirectional audio traffic
                    if (recordingActive) {
                        Serial.println("⏹️  Stopping recording - response arriving");
                        recordingActive = false;
                    }
                    
                    // Wait for prebuffer before starting playback to eliminate initial stutter
                    uint32_t queueDepth = uxQueueMessagesWaiting(audioOutputQueue);
                    const uint32_t MIN_PREBUFFER = 8;  // ~320ms buffer (8 packets × 40ms)
                    
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
                            Serial.printf("🔊 Starting ambient audio stream: %s (prebuffered %u packets)\n", 
                                         ambientSound.name.c_str(), queueDepth);
                        } else if (isPlayingAlarm) {
                            Serial.printf("🔔 Starting alarm audio playback (prebuffered %u packets)\n", queueDepth);
                        } else {
                            Serial.printf("🔊 Starting audio playback with %u packets prebuffered\n", queueDepth);
                        }
                    } else {
                        // Silent prebuffering phase - log once per stream
                        static uint32_t lastPrebufferLog = 0;
                        if (millis() - lastPrebufferLog > 1000) {
                            Serial.printf("⏳ Prebuffering... (%u/%u packets)\n", queueDepth, MIN_PREBUFFER);
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
                    
                    // 🔍 DIAGNOSTIC: Track queue depth before send
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
                                Serial.printf("⚠️  Blocked on queue for 100ms+ (%u times, queue=%u/%u) - audio system may be frozen\n", 
                                             dropsince, queueBefore, AUDIO_QUEUE_SIZE);
                                dropsince = 0;
                            }
                            lastDropWarning = millis();
                        }
                    } else {
                        // Queue growth logging disabled (too noisy during meditation)
                        // if (queueBefore == 0 || queueBefore >= AUDIO_QUEUE_SIZE - 5) {
                        //     Serial.printf("📈 Queue: %u → %u/%u\n", queueBefore, queueBefore + 1, AUDIO_QUEUE_SIZE);
                        // }
                    }
                } else {
                    Serial.printf("❌ PCM chunk too large: %d bytes\n", length);
                }
            }
            break;
            
        case WStype_DISCONNECTED:
            disconnectCount++;
            lastDisconnectTime = millis();
            Serial.printf("✗ WebSocket Disconnected (#%d) - isPlaying=%d, recording=%d, uptime=%lus\n",
                         disconnectCount, isPlayingResponse, recordingActive, millis()/1000);
            isWebSocketConnected = false;
            
            // Pause ambient playback but keep mode state for resume on reconnect
            if (isPlayingAmbient || ambientSound.active) {
                Serial.printf("⏸️  Pausing ambient sound due to disconnect: %s (will resume)\n", ambientSound.name.c_str());
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
            Serial.println("✗ WebSocket Error");
            currentLEDMode = LED_ERROR;
            break;
            
        default:
            break;
    }
}

void handleWebSocketMessage(uint8_t* payload, size_t length) {
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, payload, length);
    
    if (error) {
        Serial.printf("JSON parse error: %s\n", error.c_str());
        return;
    }
    
    // Handle server ready message
    if (doc["type"].is<const char*>() && doc["type"] == "ready") {
        Serial.printf("✓ Server: %s\n", doc["message"].as<const char*>());
        return;
    }
    
    // Handle setup complete message
    if (doc["type"].is<const char*>() && doc["type"] == "setupComplete") {
        Serial.println("📦 Setup complete - ready for interaction");
        // Greeting feature removed for simplicity - ready immediately
        return;
    }
    
    // Handle turn complete
    if (doc["type"].is<const char*>() && doc["type"] == "turnComplete") {
        Serial.println("✓ Turn complete");
        turnComplete = true;  // Mark turn as finished
        // Don't change LED mode here - let the audio finish playing naturally
        // isPlayingResponse will be set to false when audio actually stops
        // Conversation window will open after audio completes
        
        // Clear greeting flag ONLY if this was the startup greeting
        if (waitingForGreeting) {
            waitingForGreeting = false;
            Serial.println("👋 Startup greeting complete!");
        }
        
        // Clear interrupt flag - old turn is done, ready for new response
        if (responseInterrupted) {
            Serial.println("✅ Old turn complete, cleared interrupt flag");
            responseInterrupted = false;
        }
        return;
    }
    
    // Handle function calls
    if (doc["type"].is<const char*>() && doc["type"] == "functionCall") {
        String funcName = doc["name"].as<String>();
        Serial.printf("🔧 Function call: %s\n", funcName.c_str());
        
        if (funcName == "set_volume") {
            String direction = doc["args"]["direction"].as<String>();
            if (direction == "up") {
                volumeMultiplier = min(2.0f, volumeMultiplier + 0.2f);
                Serial.printf("🔊 Volume up: %.0f%%\n", volumeMultiplier * 100);
            } else if (direction == "down") {
                volumeMultiplier = max(0.1f, volumeMultiplier - 0.2f);
                Serial.printf("🔉 Volume down: %.0f%%\n", volumeMultiplier * 100);
            }
            // Play a brief chime at the new volume
            playVolumeChime();
            
        } else if (funcName == "set_volume_percent") {
            int percent = doc["args"]["percent"].as<int>();
            volumeMultiplier = constrain(percent / 100.0f, 0.1f, 2.0f);
            Serial.printf("🔊 Volume set: %d%%\n", percent);
            
            // Play a brief chime at the new volume
            playVolumeChime();
        }
        
        return;
    }
    
    // Handle tide data from server
    if (doc["type"].is<const char*>() && doc["type"] == "tideData") {
        Serial.println("🌊 Received tide data - storing for display after speech");
        tideState.state = doc["state"].as<String>();
        tideState.waterLevel = doc["waterLevel"].as<float>();
        tideState.nextChangeMinutes = doc["nextChangeMinutes"].as<int>();
        tideState.active = true;
        // Don't switch LED mode yet - let it display after audio finishes
        
        Serial.printf("🌊 Tide: %s, water level: %.1f%%, next change in %d minutes\n",
                      tideState.state.c_str(), 
                      tideState.waterLevel * 100,
                      tideState.nextChangeMinutes);
        return;
    }
    
    // Handle timer set from server
    if (doc["type"].is<const char*>() && doc["type"] == "timerSet") {
        Serial.println("⏱️  Timer set - storing for display after speech");
        timerState.totalSeconds = doc["durationSeconds"].as<int>();
        timerState.startTime = millis();
        timerState.active = true;
        // Don't switch LED mode yet - let it display after audio finishes
        
        Serial.printf("⏱️  Timer: %d seconds (%d minutes)\n",
                      timerState.totalSeconds,
                      timerState.totalSeconds / 60);
        return;
    }
    
    // Handle alarm set from server
    if (doc["type"].is<const char*>() && doc["type"] == "setAlarm") {
        uint32_t alarmID = doc["alarmID"].as<uint32_t>();
        time_t triggerTime = doc["triggerTime"].as<long long>() / 1000;  // Convert ms to seconds
        
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
            
            Serial.printf("⏰ Alarm set: ID=%u, time=%s (slot %d)\n", alarmID, timeStr, slot);
            alarmState.active = true;
        } else {
            Serial.println("⚠️  No alarm slots available!");
        }
        return;
    }
    
    // Handle timer cancelled
    if (doc["type"].is<const char*>() && doc["type"] == "timerCancelled") {
        Serial.println("⏱️  Timer cancelled");
        timerState.active = false;
        if (currentLEDMode == LED_TIMER) {
            currentLEDMode = LED_IDLE;
        }
        return;
    }
    
    // Handle timer expired
    if (doc["type"].is<const char*>() && doc["type"] == "timerExpired") {
        Serial.println("⏰ Timer expired!");
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
        // Brief delay to allow first audio chunk to arrive, then switch to playback mode
        delay(300);
        currentLEDMode = LED_AUDIO_REACTIVE;
        isPlayingResponse = true;
        lastAudioChunkTime = millis();  // Reset audio timeout
        processingStartTime = 0;  // Clear processing timeout
        Serial.println("✓ Ready for audio notification from Gemini...");
        return;
    }
    
    // Handle cancel alarm from server
    if (doc["type"].is<const char*>() && doc["type"] == "cancelAlarm") {
        String which = doc["which"].as<String>();
        Serial.printf("🚫 Cancel alarm request: %s\n", which.c_str());
        
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
            Serial.printf("✓ Cancelled %d alarm(s)\n", cancelledCount);
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
                    
                    Serial.printf("✓ Cancelled next alarm ID=%u from slot %d\n", cancelledID, earliestSlot);
                    
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
                    Serial.println("⚠️  No active alarms to cancel");
                }
            }
        }
        return;
    }
    
    // Handle list alarms request
    if (doc["type"].is<const char*>() && doc["type"] == "listAlarms") {
        Serial.println("📋 List alarms request");
        
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
                    
                    Serial.printf("  Alarm %u: %s (isPast=%d)\n", alarms[i].alarmID, timeStr, (alarms[i].triggerTime <= now));
                }
            }
            
            String responseMsg;
            serializeJson(responseDoc, responseMsg);
            webSocket.sendTXT(responseMsg);
            Serial.printf("📤 Sent alarm list: %d alarm(s)\n", alarmArray.size());
        }
        return;
    }
    
    // Handle moon data
    if (doc["type"].is<const char*>() && doc["type"] == "moonData") {
        Serial.println("🌙 Received moon data - storing for display after speech");
        moonState.phaseName = doc["phaseName"].as<String>();
        moonState.illumination = doc["illumination"].as<int>();
        moonState.moonAge = doc["moonAge"].as<float>();
        moonState.active = true;
        // Don't switch LED mode yet - let it display after audio finishes
        
        Serial.printf("🌙 Moon: %s (%d%% illuminated, %.1f days old)\n", 
                      moonState.phaseName.c_str(), moonState.illumination, moonState.moonAge);
        return;
    }
    
    // Handle ambient stream completion
    if (doc["type"].is<const char*>() && doc["type"] == "ambientComplete") {
        String soundName = doc["sound"].as<String>();
        uint16_t sequence = doc["sequence"].as<uint16_t>();
        Serial.printf("🎵 Ambient track complete: %s (seq %d)\n", soundName.c_str(), sequence);
        Serial.printf("🧘 Meditation state: active=%d, streaming=%d, mode=%d, chakra=%d, currentSound=%s\n", 
                     meditationState.active, meditationState.streaming, currentLEDMode, 
                     meditationState.currentChakra, ambientSound.name.c_str());
        
        // Validate: Only process if this completion matches what we're currently playing
        if (soundName != ambientSound.name) {
            Serial.printf("⚠️  Ignoring stale completion: expected '%s', got '%s'\n", 
                         ambientSound.name.c_str(), soundName.c_str());
            return;
        }
        
        // If meditation mode, auto-advance to next chakra
        if (meditationState.active && meditationState.streaming && currentLEDMode == LED_MEDITATION) {
            if (meditationState.currentChakra < MeditationState::CROWN) {
                // Advance to next chakra
                meditationState.currentChakra = (MeditationState::Chakra)(meditationState.currentChakra + 1);
                meditationState.phase = MeditationState::INHALE;
                meditationState.phaseStartTime = millis();  // Restart breathing cycle
                
                // Request next chakra audio
                JsonDocument reqDoc;
                reqDoc["action"] = "requestAmbient";
                char nextSound[16];
                sprintf(nextSound, "om%03d", meditationState.currentChakra + 1);
                reqDoc["sound"] = nextSound;
                reqDoc["sequence"] = ++ambientSound.sequence;
                String reqMsg;
                serializeJson(reqDoc, reqMsg);
                webSocket.sendTXT(reqMsg);
                
                // Update ambient sound state
                ambientSound.name = nextSound;
                firstAudioChunk = true;
                lastAudioChunkTime = millis();
                
                Serial.printf("🧘 Auto-advancing to %s chakra (color will smoothly transition)\n", CHAKRA_NAMES[meditationState.currentChakra]);
                // DON'T show marquee - let LED color smoothly transition in breathing visualization
            } else {
                // All chakras complete - show completion message
                Serial.println("✨ Meditation sequence complete!");
                meditationState.streaming = false;
                meditationState.paused = true;
                startMarquee("COMPLETE", CRGB(255, 255, 255), LED_MEDITATION);  // White for completion
            }
        }
        return;
    }
    
    // Handle Pomodoro commands
    if (doc["type"].is<const char*>() && doc["type"] == "pomodoroStart") {
        // Get custom durations if provided, otherwise use current settings
        if (doc["focusMinutes"].is<int>()) {
            pomodoroState.focusDuration = doc["focusMinutes"].as<int>();
            Serial.printf("🍅 Custom focus duration: %d minutes\n", pomodoroState.focusDuration);
        }
        if (doc["shortBreakMinutes"].is<int>()) {
            pomodoroState.shortBreakDuration = doc["shortBreakMinutes"].as<int>();
            Serial.printf("🍅 Custom short break: %d minutes\n", pomodoroState.shortBreakDuration);
        }
        if (doc["longBreakMinutes"].is<int>()) {
            pomodoroState.longBreakDuration = doc["longBreakMinutes"].as<int>();
            Serial.printf("🍅 Custom long break: %d minutes\n", pomodoroState.longBreakDuration);
        }
        
        Serial.println("🍅 Pomodoro started via voice command");
        currentLEDMode = LED_POMODORO;
        targetLEDMode = LED_POMODORO;
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
        Serial.println("🍅 Pomodoro paused via voice command");
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
        Serial.println("🍅 Pomodoro resumed via voice command");
        if (pomodoroState.active && pomodoroState.paused) {
            pomodoroState.startTime = millis();
            pomodoroState.paused = false;
            playVolumeChime();
        }
        return;
    }
    
    if (doc["type"].is<const char*>() && doc["type"] == "pomodoroStop") {
        Serial.println("🍅 Pomodoro stopped via voice command");
        pomodoroState.active = false;
        pomodoroState.paused = false;
        pomodoroState.sessionCount = 0;
        currentLEDMode = LED_IDLE;
        targetLEDMode = LED_IDLE;
        playShutdownSound();
        return;
    }
    
    if (doc["type"].is<const char*>() && doc["type"] == "pomodoroSkip") {
        Serial.println("🍅 Skipping to next Pomodoro session");
        if (pomodoroState.active) {
            // Trigger session transition by setting remaining time to 0
            pomodoroState.startTime = millis() - (pomodoroState.totalSeconds * 1000);
            pomodoroState.paused = false;
        }
        return;
    }
    
    if (doc["type"].is<const char*>() && doc["type"] == "pomodoroStatusRequest") {
        Serial.println("🍅 Pomodoro status requested");
        
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
            const char* sessionName = (pomodoroState.currentSession == PomodoroState::FOCUS) ? "Focus" :
                                     (pomodoroState.currentSession == PomodoroState::SHORT_BREAK) ? "Short Break" : "Long Break";
            
            statusDoc["active"] = true;
            statusDoc["session"] = sessionName;
            statusDoc["minutesRemaining"] = minutes;
            statusDoc["secondsRemaining"] = seconds;
            statusDoc["paused"] = pomodoroState.paused;
            statusDoc["cycleNumber"] = pomodoroState.sessionCount + 1;
            
            Serial.printf("🍅 Status: %s session, %d:%02d remaining, %s, cycle %d/4\n",
                         sessionName, minutes, seconds, pomodoroState.paused ? "paused" : "running", pomodoroState.sessionCount + 1);
        } else {
            statusDoc["active"] = false;
            Serial.println("🍅 Pomodoro not active");
        }
        
        String statusMsg;
        serializeJson(statusDoc, statusMsg);
        webSocket.sendTXT(statusMsg);
        Serial.println("📤 Sent Pomodoro status to server");
        return;
    }
    
    // Handle text responses
    if (doc["type"].is<const char*>() && doc["type"] == "text") {
        Serial.printf("📝 Text: %s\n", doc["text"].as<const char*>());
        return;
    }
    
    // Handle errors
    if (doc["error"].is<const char*>()) {
        Serial.printf("❌ Error: %s\n", doc["error"].as<const char*>());
        currentLEDMode = LED_ERROR;
        return;
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
                    Serial.println("🔶 LED_BOOT: Orange pulsing (connecting...)");
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
                // 5.3 second cycle (50% faster than 8s), bounces up and down
                float t = (millis() % 5333) / 5333.0;
                
                // Create bouncing wave: stays within LED range (0-11) with fade margins
                // Range 0→14→0 where 14 gives smooth fade at top (doesn't hit full 16)
                float wavePos;
                if (t < 0.5) {
                    // First half: bottom to top (0→14)
                    wavePos = (t * 2.0) * 14.0;
                } else {
                    // Second half: top to bottom (14→0)
                    wavePos = ((1.0 - t) * 2.0) * 14.0;
                }
                
                // Debug every 2 seconds
                static uint32_t lastIdleDebug = 0;
                if (millis() - lastIdleDebug > 2000) {
                    Serial.printf("💙 IDLE: t=%.2f, wavePos=%.2f, hue=160 (blue)\n", t, wavePos);
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
            // Traditional VU meter colors: green → yellow → red
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
                            Serial.printf("⚠️ LED overflow: col=%d, row=%d, idx=%d\n", col, row, ledIndex);
                            continue;
                        }
                        
                        if (row < numRows) {
                            // Traditional VU meter gradient based on row height
                            // Green at bottom, yellow in middle, red at top
                            float progress = (float)row / (float)LEDS_PER_COLUMN;
                            
                            if (progress < 0.4) {
                                leds[ledIndex] = CRGB(0, 255, 0);  // Green (bottom 40%)
                            } else if (progress < 0.7) {
                                leds[ledIndex] = CRGB(255, 255, 0);  // Yellow (40-70%)
                            } else {
                                leds[ledIndex] = CRGB(255, 0, 0);  // Red (top 70-100%)
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
            // Ambient sound VU meter - vertical bars on all strips with fade trail
            // Each strip is a vertical VU meter synced together
            {
                static int16_t ambientBuffer[640];  // 40ms at 16kHz (640 samples)
                size_t bytes_read = 0;
                
                // Fade all LEDs slightly for trail effect (instead of clearing)
                for (int i = 0; i < NUM_LEDS; i++) {
                    leds[i].fadeToBlackBy(80);  // Gentle fade creates smooth trail
                }
                
                // Read from microphone
                if (i2s_read(I2S_NUM_0, ambientBuffer, sizeof(ambientBuffer), &bytes_read, 0) == ESP_OK) {
                    size_t samples_read = bytes_read / sizeof(int16_t);
                    
                    // Calculate RMS amplitude
                    int64_t sum = 0;
                    for (size_t i = 0; i < samples_read; i++) {
                        int32_t sample = ambientBuffer[i];
                        sum += (int64_t)sample * sample;
                    }
                    float rms = sqrt(sum / samples_read);
                    
                    // Auto-gain control with peak limiting
                    static float autoGain = 25.0f;  // Start with high gain for ambient sounds
                    static float peakRMS = 100.0f;  // Track peak levels
                    static uint32_t lastGainAdjust = 0;
                    
                    // Update peak tracker (decay slowly)
                    peakRMS = peakRMS * 0.995f + rms * 0.005f;
                    if (rms > peakRMS) peakRMS = rms;
                    
                    // Adjust gain based on recent peak levels (every 50ms for faster response)
                    if (millis() - lastGainAdjust > 50) {
                        // Target peak around 800-1500 for good range
                        if (peakRMS < 150 && autoGain < 50.0f) {
                            autoGain *= 1.15f;  // Increase gain faster for quiet sounds
                        } else if (peakRMS > 2500 && autoGain > 3.0f) {
                            autoGain *= 0.85f;  // Decrease gain for loud sounds
                        }
                        lastGainAdjust = millis();
                    }
                    
                    // Apply auto-gain
                    float gainedRMS = rms * autoGain;
                    
                    // Soft compression: reduce peaks above threshold
                    if (gainedRMS > 1500) {
                        // Logarithmic compression for peaks
                        gainedRMS = 1500 + (gainedRMS - 1500) * 0.3f;
                    }
                    
                    // Smooth the value for more stable display and better visibility of sustained sounds
                    static float smoothedRMS = 0.0f;
                    smoothedRMS = smoothedRMS * 0.80f + gainedRMS * 0.20f;  // 80/20 for more smoothing
                    
                    // Map to row count (0-12 rows) instead of total LED count
                    // Adjusted range: 150-1600 for good sensitivity
                    int numRows = map(constrain((int)smoothedRMS, 150, 1600), 150, 1600, 0, LEDS_PER_COLUMN);
                    
                    // Debug output every 2 seconds
                    static uint32_t lastDebug = 0;
                    if (millis() - lastDebug > 2000) {
                        Serial.printf("🎵 VU: raw=%.0f gain=%.1f gained=%.0f smooth=%.0f rows=%d\n", 
                                     rms, autoGain, gainedRMS, smoothedRMS, numRows);
                        lastDebug = millis();
                    }
                    
                    // VU meter gradient - vertical on all strips (column-based)
                    for (int col = 0; col < LED_COLUMNS; col++) {
                        for (int row = 0; row < LEDS_PER_COLUMN; row++) {
                            int ledIndex = col * LEDS_PER_COLUMN + row;
                            
                            if (row < numRows) {
                                // Gradient based on row height
                                if (row < LEDS_PER_COLUMN * 0.6) {
                                    leds[ledIndex] = CRGB(0, 255, 0);  // Green (bottom 60%)
                                } else if (row < LEDS_PER_COLUMN * 0.85) {
                                    leds[ledIndex] = CRGB(255, 255, 0);  // Yellow (60-85%)
                                } else {
                                    leds[ledIndex] = CRGB(255, 0, 0);  // Red (top 85-100%)
                                }
                            }
                            // LEDs above numRows will keep their faded value from fadeToBlackBy
                        }
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
                            Serial.printf("⚠️ LED overflow: col=%d, row=%d, idx=%d\n", col, row, ledIndex);
                            continue;
                        }
                        
                        if (row < numRows) {
                            // Blue → cyan → magenta gradient based on row height
                            // More pronounced color differences for better visibility
                            float progress = (float)row / (float)LEDS_PER_COLUMN;
                            
                            if (progress < 0.4) {
                                leds[ledIndex] = CRGB(0, 150, 255);  // Sky blue (bottom 40%)
                            } else if (progress < 0.7) {
                                leds[ledIndex] = CRGB(0, 255, 200);  // Cyan/aqua (40-70%)
                            } else {
                                leds[ledIndex] = CRGB(150, 0, 255);  // Magenta/purple (top 70-100%)
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
                    Serial.printf("🌊 LED_TIDE active: state=%s, level=%.2f, mode=%d\n", 
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
                    // Falling rain effect - random blue drops falling down
                    static uint32_t lastDrop = 0;
                    static uint8_t dropBrightness[144] = {0};  // Track drop brightness (max LEDs)
                    static bool rainInitialized = false;
                    
                    // Reset on first entry to rain mode
                    if (!rainInitialized) {
                        memset(dropBrightness, 0, sizeof(dropBrightness));
                        lastDrop = millis();
                        rainInitialized = true;
                    }
                    
                    // Fade all LEDs
                    for (int i = 0; i < NUM_LEDS; i++) {
                        leds[i].fadeToBlackBy(40);
                    }
                    
                    // Add new drops randomly
                    if (millis() - lastDrop > RAIN_DROP_SPAWN_INTERVAL_MS) {
                        if (random(100) < RAIN_DROP_SPAWN_CHANCE) {  // 30% chance each 100ms
                            int pos = random(NUM_LEDS);
                            dropBrightness[pos] = 255;
                        }
                        lastDrop = millis();
                    }
                    
                    // Render drops
                    for (int i = 0; i < NUM_LEDS; i++) {
                        if (dropBrightness[i] > 0) {
                            leds[i] = CHSV(160, 255, dropBrightness[i]);  // Blue
                            dropBrightness[i] = dropBrightness[i] * RAIN_DROP_FADE_RATE;  // Fade out
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
                    
                    // Smooth the wave height for fluid motion (slower smoothing for ocean waves)
                    smoothedWave = smoothedWave * 0.90f + instantLevel * 0.10f;
                    
                    // Debug log every 2 seconds
                    if (millis() - lastDebugLog > 2000) {
                        Serial.printf("🌊 Ocean: Level=%d, Smoothed=%.0f, Rows=%d/%d\n", 
                                     currentAudioLevel, smoothedWave, 
                                     (int)(constrain(smoothedWave / 400.0f, 0.15f, 0.95f) * LEDS_PER_COLUMN), 
                                     LEDS_PER_COLUMN);
                        lastDebugLog = millis();
                    }
                    
                    // Map audio level to wave height with dramatic swell
                    // Ocean sound typically has levels 50-300 (based on PCM amplitude)
                    // Use higher divisor for more dynamic range: low = 2 LEDs, high = 11 LEDs
                    float normalizedWave = constrain(smoothedWave / 400.0f, 0.15f, 0.95f);
                    
                    // Convert to number of rows (vertical wave on all columns)
                    int waveRows = (int)(normalizedWave * LEDS_PER_COLUMN);
                    
                    // Add MORE traveling wave phase for dramatic effect
                    float time = millis() / 3000.0;
                    
                    for (int col = 0; col < LED_COLUMNS; col++) {
                        // Phase offset for traveling wave effect
                        float phaseOffset = (float)col / (float)LED_COLUMNS * TWO_PI;
                        float phaseWave = sin(time + phaseOffset) * 3.0;  // ±3 rows variation (was ±1.5)
                        
                        int colWaveRows = constrain(waveRows + (int)phaseWave, 1, LEDS_PER_COLUMN);
                        
                        // Light column from bottom to wave height
                        for (int row = 0; row < LEDS_PER_COLUMN; row++) {
                            int ledIndex = col * LEDS_PER_COLUMN + row;
                            
                            if (ledIndex >= NUM_LEDS) continue;
                            
                            if (row < colWaveRows) {
                                // Gradient from deep blue (bottom) to cyan (top)
                                float progress = (float)row / (float)LEDS_PER_COLUMN;
                                uint8_t hue = 160 + (uint8_t)(progress * 10);  // 160-170 range
                                uint8_t brightness = 150 + (uint8_t)(progress * 105);  // Brighter at top
                                leds[ledIndex] = CHSV(hue, 255, brightness);
                            } else {
                                leds[ledIndex] = CRGB::Black;
                            }
                        }
                    }
                } else {  // SOUND_RAINFOREST
                    // Pulsing green canopy - gentle breathing effect
                    float pulse = 0.6 + 0.4 * sin(millis() / 3000.0);  // 0.6 to 1.0
                    uint8_t brightness = (uint8_t)(200 * pulse);
                    
                    for (int i = 0; i < NUM_LEDS; i++) {
                        // Green with slight variation per LED
                        uint8_t hue = 96 + (i * 2);  // Green range 96-114
                        uint8_t sat = 220 + (i * 3);  // Varying saturation
                        leds[i] = CHSV(hue, sat, brightness);
                    }
                }
            }
            break;
            
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
                        litRows = (int)(progress * LEDS_PER_COLUMN);
                    } else {
                        // Focus: count DOWN (draining = time running out)
                        litRows = (int)((1.0 - progress) * LEDS_PER_COLUMN);
                    }
                    litRows = constrain(litRows, 0, LEDS_PER_COLUMN);
                    
                    // Pulse effect for active LED (slow breathing) - smooth sine wave
                    float activePulse = 1.0;
                    if (pomodoroState.paused) {
                        // When paused, ALL LEDs breathe together
                        float breathe = sin(millis() / 3000.0 * PI);
                        activePulse = 0.30 + 0.70 * ((breathe + 1.0) / 2.0);
                    } else {
                        // When running, only active LED breathes
                        float breathe = sin(millis() / 2000.0 * PI);  // Faster 4-second cycle
                        activePulse = 0.40 + 0.60 * ((breathe + 1.0) / 2.0);  // 40-100% range
                    }
                    
                    // Calculate which LED is currently "active" (the moving indicator)
                    // Use floor for discrete LED positions
                    int activeLED;
                    
                    if (isBreakSession) {
                        // Countup: active LED moves from bottom (0) to top (11)
                        activeLED = constrain((int)(progress * LEDS_PER_COLUMN), 0, LEDS_PER_COLUMN - 1);
                    } else {
                        // Countdown: active LED moves from top (11) to bottom (0)
                        // As progress goes 0→1, active LED goes 11→0
                        activeLED = constrain(LEDS_PER_COLUMN - 1 - (int)(progress * LEDS_PER_COLUMN), 0, LEDS_PER_COLUMN - 1);
                    }
                    
                    // Debug logging (every 5 seconds)
                    static uint32_t lastPomodoroDebug = 0;
                    if (millis() - lastPomodoroDebug > 5000) {
                        Serial.printf("🍅 Progress: %.1f%%, Active LED row: %d, Pulse: %.2f, Paused: %d, Remaining: %ds\n",
                                     progress * 100, activeLED, activePulse, pomodoroState.paused, secondsRemaining);
                        lastPomodoroDebug = millis();
                    }
                    
                    // Light all columns identically (vertical bars)
                    for (int col = 0; col < LED_COLUMNS; col++) {
                        for (int row = 0; row < LEDS_PER_COLUMN; row++) {
                            int ledIndex = col * LEDS_PER_COLUMN + row;
                            if (ledIndex >= NUM_LEDS) continue;
                            
                            float ledBrightness;
                            
                            if (pomodoroState.paused) {
                                // When paused: all LEDs breathe together at same brightness
                                ledBrightness = activePulse;
                            } else {
                                // When running: active LED breathes, others stay at 5%
                                if (row == activeLED) {
                                    ledBrightness = activePulse;  // 40-100% breathing
                                } else {
                                    ledBrightness = 0.05;  // Constant 5% for inactive LEDs (barely visible background)
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
                        CRGB(0, 150, 255),    // THROAT - Blue
                        CRGB(75, 0, 130),     // THIRD_EYE - Indigo
                        CRGB(148, 0, 211)     // CROWN - Violet
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
                        
                        Serial.printf("🎨 Chakra changed to %s: RGB(%d,%d,%d) - starting 3s color fade\n", 
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
                    
                    if (!meditationState.paused && meditationState.phaseStartTime > 0) {
                        // Calculate phase progress
                        const uint32_t PHASE_DURATION = 4000;  // 4 seconds per phase
                        uint32_t phaseElapsed = millis() - meditationState.phaseStartTime;
                        
                        // Check if phase is complete and advance
                        if (phaseElapsed >= PHASE_DURATION) {
                            meditationState.phase = (MeditationState::BreathPhase)((meditationState.phase + 1) % 4);
                            meditationState.phaseStartTime = millis();
                            phaseElapsed = 0;
                            
                            const char* phaseNames[] = {"INHALE", "HOLD_TOP", "EXHALE", "HOLD_BOTTOM"};
                            Serial.printf("🧘 Breath phase: %s\n", phaseNames[meditationState.phase]);
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
                        // Paused: Show static chakra color at 30%
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
                            
                            Serial.printf("🕐 Clock updated: %02d:%02d\n", currentHour, currentMinute);
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
            // Button 1: cycle colors (WHITE → RED → GREEN → BLUE)
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
                                    Serial.println("💡 Lamp fully lit");
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
                    Serial.printf("📜 Marquee complete, switching to mode %d\n", targetLEDMode);
                    
                    // If switching to ambient mode, request the audio now
                    if (targetLEDMode == LED_AMBIENT) {
                        // Request the current ambient sound from server
                        JsonDocument ambientDoc;
                        ambientDoc["action"] = "requestAmbient";
                        ambientDoc["sound"] = ambientSound.name;  // rain/ocean/rainforest
                        ambientDoc["sequence"] = ambientSound.sequence;
                        String ambientMsg;
                        serializeJson(ambientDoc, ambientMsg);
                        Serial.printf("📤 Ambient audio request: %s (seq %d)\n", ambientMsg.c_str(), ambientSound.sequence);
                        webSocket.sendTXT(ambientMsg);
                    }
                    
                    // If switching to Pomodoro mode, auto-start the timer
                    if (targetLEDMode == LED_POMODORO && pomodoroState.active && pomodoroState.paused) {
                        pomodoroState.startTime = millis();
                        pomodoroState.paused = false;
                        Serial.println("▶️  Pomodoro timer auto-started");
                        playZenBell();  // Play zen bell on start
                    }
                    
                    // If switching to meditation mode, start the breathing and audio now
                    if (targetLEDMode == LED_MEDITATION && meditationState.active && meditationState.paused) {
                        // Start breathing animation
                        meditationState.phaseStartTime = millis();
                        meditationState.paused = false;
                        
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
                        Serial.printf("📤 Meditation starting: %s (seq %d)\n", reqMsg.c_str(), ambientSound.sequence);
                        webSocket.sendTXT(reqMsg);
                        meditationState.streaming = true;
                        
                        Serial.println("🧘 Meditation breathing and audio started (ROOT chakra)");
                    }
                }
            }
            break;
            
        case LED_CONNECTED:
            {
                static uint32_t lastConnDebug = 0;
                if (millis() - lastConnDebug > 500) {
                    Serial.println("✅ LED_CONNECTED: Solid green");
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
    while(1) {
        webSocket.loop();
        
        // Health monitoring every 5s (more frequent for weak signal detection)
        if (millis() - lastHealthLog > 5000) {
            int32_t rssi = WiFi.RSSI();
            uint32_t timeSinceLastSend = millis() - lastWebSocketSendTime;
            
            // Get memory statistics
            uint32_t freeHeap = ESP.getFreeHeap();
            uint32_t minFreeHeap = ESP.getMinFreeHeap();
            uint32_t heapSize = ESP.getHeapSize();
            uint32_t freePsram = ESP.getFreePsram();
            
            // Warn if signal is degrading rapidly
            if (lastRSSI != 0 && (lastRSSI - rssi) > 10) {
                Serial.printf("⚠️  WiFi signal dropped %d dBm! (%d → %d)\n", lastRSSI - rssi, lastRSSI, rssi);
            }
            
            Serial.printf("[WS Health] Connected=%d, WiFi=%d dBm, LastSend=%us ago, Failures=%u\n",
                         isWebSocketConnected, rssi, timeSinceLastSend/1000, webSocketSendFailures);
            Serial.printf("[Memory] Heap: %u/%u KB (min=%u KB), PSRAM: %u KB, Playing=%d\n",
                         freeHeap/1024, heapSize/1024, minFreeHeap/1024, freePsram/1024, isPlayingResponse);
            lastRSSI = rssi;
            lastHealthLog = millis();
            
            // Warn if heap is getting low
            if (freeHeap < 50000) {  // Less than 50KB free
                Serial.printf("⚠️  LOW HEAP WARNING: Only %u KB free!\n", freeHeap/1024);
            }
            
            // Attempt to reconnect WiFi if signal is very poor but still connected
            if (rssi < -80 && WiFi.status() == WL_CONNECTED) {
                Serial.println("⚠️  Very weak signal detected - WiFi may drop soon");
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
            Serial.printf("⚠️ LED task stalled #%d: %dms since last update\n", 
                         ledTaskStalls, now - lastLedUpdate);
        }
        
        // Mutex-protected LED rendering
        if (xSemaphoreTake(ledMutex, portMAX_DELAY) == pdTRUE) {
            // Track updateLEDs calls
            static uint32_t updateCount = 0;
            static uint32_t lastUpdateLog = 0;
            updateCount++;
            
            if (millis() - lastUpdateLog > 30000) {
                Serial.printf("🔄 LED task: %u updates in 30s (expect ~990)\n", updateCount);
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
