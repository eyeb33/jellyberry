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

// ============== GLOBAL STATE ==============
WiFiClientSecure wifiClient;
WebSocketsClient webSocket;
CRGB leds[NUM_LEDS];
bool isWebSocketConnected = false;
bool recordingActive = false;
bool isPlayingResponse = false;
bool isPlayingAmbient = false;  // Track ambient sound playback separately
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
#define AUDIO_DELAY_BUFFER_SIZE 12  // ~360ms delay to match I2S playback buffer + speaker latency
int audioLevelBuffer[AUDIO_DELAY_BUFFER_SIZE] = {0};
int audioBufferIndex = 0;

TaskHandle_t websocketTaskHandle = NULL;
TaskHandle_t ledTaskHandle = NULL;
TaskHandle_t audioTaskHandle = NULL;

// Audio processing (raw PCM - no codec needed)

// Audio buffers
QueueHandle_t audioOutputQueue;   // Queue for playback audio
#define AUDIO_QUEUE_SIZE 30  // 30 packets = ~1.2s buffer (good jitter tolerance with paced delivery)

struct AudioChunk {
    uint8_t data[2048];
    size_t length;
};

enum LEDMode { LED_BOOT, LED_IDLE, LED_RECORDING, LED_PROCESSING, LED_AUDIO_REACTIVE, LED_CONNECTED, LED_ERROR, LED_TIDE, LED_TIMER, LED_MOON, LED_AMBIENT_VU, LED_AMBIENT_RAIN, LED_AMBIENT_OCEAN, LED_AMBIENT_RAINFOREST, LED_CONVERSATION_WINDOW, LED_MARQUEE };
LEDMode currentLEDMode = LED_IDLE;  // Start directly in idle mode
LEDMode targetLEDMode = LED_IDLE;  // Mode to switch to after marquee finishes
bool ambientVUMode = false;  // Toggle for ambient sound VU meter mode

// Marquee text scrolling state
struct MarqueeState {
    String text;           // Text to display
    int scrollPosition;    // Current horizontal scroll offset
    uint32_t lastUpdate;   // Last time we scrolled
    bool active;
    CRGB color;            // Text color
} marqueeState = {"", 0, 0, false, CRGB::White};

// Simple 5x7 pixel font (stored as columns for easy vertical display)
// Each character is 5 columns wide, 7 rows tall (we have 12 rows, so we'll center it)
const uint8_t FONT_WIDTH = 5;
const uint8_t FONT_HEIGHT = 7;
const uint8_t FONT_SPACING = 1;  // Space between characters

// Font bitmap - each byte represents a column of the character (7 bits used)
const uint8_t font5x7[][5] = {
    {0x7E, 0x89, 0x91, 0xA1, 0x7E}, // A
    {0xFF, 0x91, 0x91, 0x91, 0x6E}, // B
    {0x7E, 0x81, 0x81, 0x81, 0x42}, // C
    {0xFF, 0x81, 0x81, 0x81, 0x7E}, // D
    {0xFF, 0x91, 0x91, 0x91, 0x81}, // E
    {0xFF, 0x90, 0x90, 0x90, 0x80}, // F
    {0x7E, 0x81, 0x89, 0x89, 0x4E}, // G
    {0xFF, 0x10, 0x10, 0x10, 0xFF}, // H
    {0x00, 0x81, 0xFF, 0x81, 0x00}, // I
    {0x06, 0x01, 0x01, 0x01, 0xFE}, // J
    {0xFF, 0x10, 0x28, 0x44, 0x82}, // K
    {0xFF, 0x01, 0x01, 0x01, 0x01}, // L
    {0xFF, 0x40, 0x20, 0x40, 0xFF}, // M
    {0xFF, 0x40, 0x20, 0x10, 0xFF}, // N
    {0x7E, 0x81, 0x81, 0x81, 0x7E}, // O
    {0xFF, 0x88, 0x88, 0x88, 0x70}, // P
    {0x7E, 0x81, 0x85, 0x82, 0x7D}, // Q
    {0xFF, 0x88, 0x8C, 0x8A, 0x71}, // R
    {0x62, 0x91, 0x91, 0x91, 0x8C}, // S
    {0x80, 0x80, 0xFF, 0x80, 0x80}, // T
    {0xFE, 0x01, 0x01, 0x01, 0xFE}, // U
    {0xF8, 0x04, 0x02, 0x04, 0xF8}, // V
    {0xFF, 0x02, 0x04, 0x02, 0xFF}, // W
    {0xC3, 0x24, 0x18, 0x24, 0xC3}, // X
    {0xE0, 0x10, 0x0F, 0x10, 0xE0}, // Y
    {0x83, 0x85, 0x99, 0xA1, 0xC1}, // Z
    {0x00, 0x00, 0x00, 0x00, 0x00}, // Space (index 26)
    {0x3E, 0x51, 0x49, 0x45, 0x3E}, // 0
    {0x00, 0x42, 0x7F, 0x40, 0x00}, // 1
    {0x42, 0x43, 0x45, 0x49, 0x31}, // 2
    {0x22, 0x41, 0x49, 0x49, 0x36}, // 3
    {0x18, 0x28, 0x48, 0x7F, 0x08}, // 4
    {0x71, 0x49, 0x49, 0x49, 0x46}, // 5
    {0x3E, 0x49, 0x49, 0x49, 0x06}, // 6
    {0x40, 0x47, 0x48, 0x50, 0x60}, // 7
    {0x36, 0x49, 0x49, 0x49, 0x36}, // 8
    {0x30, 0x49, 0x49, 0x49, 0x3E}, // 9
};

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

// Ambient sound state
struct AmbientSound {
    String name;  // "rain", "ocean", "rainforest"
    bool active;
    uint16_t sequence;  // Increments each time we request a new sound
    uint16_t discardedCount;  // Count discarded chunks to reduce log spam
    uint32_t drainUntil;  // Timestamp until which we silently drain stale packets
} ambientSound = {"", false, 0, 0, 0};

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
void playShutdownSound();
void startMarquee(String text, CRGB color, LEDMode nextMode);
int getCharIndex(char c);

// ============== MARQUEE FUNCTIONS ==============

// Get font index for a character (A-Z = 0-25, space = 26, 0-9 = 27-36)
int getCharIndex(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a';  // Convert lowercase to uppercase
    if (c >= '0' && c <= '9') return 27 + (c - '0');
    return 26;  // Space for any other character
}

// Start a marquee animation before switching modes
void startMarquee(String text, CRGB color, LEDMode nextMode) {
    marqueeState.text = text;
    marqueeState.text.toUpperCase();  // Convert to uppercase for font
    marqueeState.scrollPosition = LED_COLUMNS;  // Start off right edge
    marqueeState.lastUpdate = millis();
    marqueeState.active = true;
    marqueeState.color = color;
    targetLEDMode = nextMode;
    currentLEDMode = LED_MARQUEE;
    Serial.printf("📜 Starting marquee: '%s'\n", text.c_str());
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
    Serial.println("\n\n========================================");
    Serial.println("=== JELLYBERRY BOOT STARTING ===");
    Serial.println("========================================");
    Serial.flush();

    // Initialize LED strip (144 LEDs on GPIO 1)
    Serial.write("LED_INIT_START\r\n", 16);
    FastLED.addLeds<LED_CHIPSET, LED_DATA_PIN, LED_COLOR_ORDER>(leds, NUM_LEDS);
    FastLED.setBrightness(LED_BRIGHTNESS);
    FastLED.setDither(0);  // Disable dithering to prevent faint colors
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
    
    // Ignore touch pads for first 5 seconds after boot to avoid false triggers
    static const uint32_t bootIgnoreTime = 5000;
    
    // Poll touch pads with debouncing
    static bool startPressed = false;
    static bool stopPressed = false;
    static uint32_t lastDebounceTime = 0;
    const uint32_t debounceDelay = 10;  // 10ms - TTP223 has hardware debounce
    
    if (millis() > bootIgnoreTime && (millis() - lastDebounceTime) > debounceDelay) {
        bool startTouch = digitalRead(TOUCH_PAD_START_PIN) == HIGH;
        bool stopTouch = digitalRead(TOUCH_PAD_STOP_PIN) == HIGH;
        
        // STOP button: Cycle through ambient modes
        // IDLE → AMBIENT_VU → RAIN → OCEAN → RAINFOREST → IDLE
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
            
            // Cycle to next mode
            if (currentLEDMode == LED_IDLE || currentLEDMode == LED_MOON || 
                currentLEDMode == LED_TIDE || currentLEDMode == LED_TIMER) {
                // Show marquee before switching
                ambientVUMode = true;
                ambientSound.sequence++;  // Increment for mode change
                startMarquee("VU MODE", CRGB::Green, LED_AMBIENT_VU);
                Serial.println("🎵 Ambient VU meter mode enabled");
            } else if (currentLEDMode == LED_AMBIENT_VU) {
                ambientVUMode = false;
                ambientSound.name = "rain";
                ambientSound.active = true;
                ambientSound.sequence++;
                isPlayingAmbient = true;
                isPlayingResponse = false;
                firstAudioChunk = true;
                lastAudioChunkTime = millis();  // Initialize timing
                Serial.printf("🌧️  Rain ambient sound mode (seq %d)\n", ambientSound.sequence);
                // Show marquee first
                startMarquee("RAIN MODE", CRGB(0, 100, 255), LED_AMBIENT_RAIN);  // Blue for rain
                // Brief delay to let server cancel previous stream
                delay(200);
                // Request rain sounds from server
                JsonDocument ambientDoc;
                ambientDoc["action"] = "requestAmbient";
                ambientDoc["sound"] = "rain";
                ambientDoc["sequence"] = ambientSound.sequence;
                String ambientMsg;
                serializeJson(ambientDoc, ambientMsg);
                webSocket.sendTXT(ambientMsg);
            } else if (currentLEDMode == LED_AMBIENT_RAIN) {
                ambientSound.name = "ocean";
                ambientSound.active = true;
                ambientSound.sequence++;
                isPlayingAmbient = true;
                isPlayingResponse = false;
                firstAudioChunk = true;
                lastAudioChunkTime = millis();  // Initialize timing
                Serial.printf("🌊 Ocean ambient sound mode (seq %d)\n", ambientSound.sequence);
                // Show marquee first
                startMarquee("OCEAN MODE", CRGB(0, 150, 200), LED_AMBIENT_OCEAN);  // Cyan for ocean
                // Request ocean sounds from server
                JsonDocument ambientDoc;
                ambientDoc["action"] = "requestAmbient";
                ambientDoc["sound"] = "ocean";
                ambientDoc["sequence"] = ambientSound.sequence;
                String ambientMsg;
                serializeJson(ambientDoc, ambientMsg);
                webSocket.sendTXT(ambientMsg);
            } else if (currentLEDMode == LED_AMBIENT_OCEAN) {
                ambientSound.name = "rainforest";
                ambientSound.active = true;
                ambientSound.sequence++;
                isPlayingAmbient = true;
                isPlayingResponse = false;
                firstAudioChunk = true;
                lastAudioChunkTime = millis();  // Initialize timing
                Serial.printf("🌿 Rainforest ambient sound mode (seq %d)\n", ambientSound.sequence);
                // Show marquee first
                startMarquee("FOREST MODE", CRGB(50, 255, 50), LED_AMBIENT_RAINFOREST);  // Bright green
                // Request rainforest sounds from server
                JsonDocument ambientDoc;
                ambientDoc["action"] = "requestAmbient";
                ambientDoc["sound"] = "rainforest";
                ambientDoc["sequence"] = ambientSound.sequence;
                String ambientMsg;
                serializeJson(ambientDoc, ambientMsg);
                webSocket.sendTXT(ambientMsg);
            } else if (currentLEDMode == LED_AMBIENT_RAINFOREST) {
                // Stop ambient and return to IDLE
                Serial.println("⏹️  Ambient mode stopped - draining buffered chunks for 2s...");
                
                // Clear ambient state
                isPlayingAmbient = false;
                isPlayingResponse = false;
                ambientSound.active = false;
                ambientSound.name = "";
                ambientSound.sequence++;  // Increment to invalidate in-flight chunks
                
                // Show "IDLE MODE" marquee before returning to idle
                startMarquee("IDLE MODE", CRGB(100, 100, 255), LED_IDLE);  // Blue for idle
                
                // Send stop request to server
                JsonDocument stopDoc;
                stopDoc["action"] = "stopAmbient";
                String stopMsg;
                serializeJson(stopDoc, stopMsg);
                webSocket.sendTXT(stopMsg);
                
                // Set 2-second drain period to discard buffered server chunks
                ambientSound.drainUntil = millis() + 2000;
            }
        }
        
        // Interrupt feature: START button during active playback stops audio and starts recording
        // Only interrupt if we've received audio recently (within 500ms) and turn is not complete
        if (startTouch && !startPressed && isPlayingResponse && !turnComplete && 
            (millis() - lastAudioChunkTime) < 500) {
            Serial.println("⏸️  Interrupted response - starting new recording");
            responseInterrupted = true;  // Flag to ignore remaining audio chunks
            isPlayingResponse = false;
            i2s_zero_dma_buffer(I2S_NUM_1);  // Stop audio immediately
            
            recordingActive = true;
            recordingStartTime = millis();
            lastVoiceActivityTime = millis();
            currentLEDMode = LED_RECORDING;
            Serial.printf("🎤 Recording started... (START=%d, STOP=%d)\n", startTouch, stopTouch);
        }
        // Start recording on rising edge (normal case - not interrupting)
        // Block if: recording active, playing response, OR in ambient sound mode, OR conversation window is open
        else if (startTouch && !startPressed && !recordingActive && !isPlayingResponse && !isPlayingAmbient && !conversationMode) {
            // Clear all previous state
            responseInterrupted = false;
            conversationRecording = false;  // Button press = normal recording timeout
            tideState.active = false;
            moonState.active = false;
            timerState.active = false;
            
            // CRITICAL: Cancel drain timer so Gemini responses can play
            if (ambientSound.drainUntil > 0) {
                Serial.println("✓ Cancelled drain timer - ready for new audio");
                ambientSound.drainUntil = 0;
            }
            
            // Exit ambient VU mode
            if (ambientVUMode) {
                ambientVUMode = false;
                Serial.println("🎵 Ambient VU meter mode disabled");
            }
            
            recordingActive = true;
            recordingStartTime = millis();
            lastVoiceActivityTime = millis();
            currentLEDMode = LED_RECORDING;
            Serial.printf("🎤 Recording started... (START=%d, STOP=%d)\n", startTouch, stopTouch);
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
            uint32_t duration = millis() - recordingStartTime;
            Serial.printf("⏹️  Recording stopped - Duration: %dms (max duration reached)\n", duration);
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
        Serial.println("⏹️  Recording stopped - Silence detected");
    }
    
    // Show thinking animation if response is taking too long (after delay)
    if (currentLEDMode == LED_RECORDING && processingStartTime > 0 && 
        (millis() - processingStartTime) > THINKING_ANIMATION_DELAY_MS && 
        (millis() - processingStartTime) < 10000) {
        currentLEDMode = LED_PROCESSING;
        Serial.println("⏳ Response delayed - showing thinking animation");
    }
    
    // Timeout PROCESSING mode if no response after 10 seconds
    if ((currentLEDMode == LED_PROCESSING || currentLEDMode == LED_RECORDING) && 
        processingStartTime > 0 && (millis() - processingStartTime) > 10000) {
        Serial.println("⚠️  Processing timeout - no response received, returning to IDLE");
        currentLEDMode = LED_IDLE;
        processingStartTime = 0;
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
            Serial.printf("⏹️  Audio playback complete (timeout + queue drained to %u)\n", queueDepth);
        
        // Check if turn is complete - if so, open conversation window
        // Skip conversation mode for startup greeting
        if (turnComplete && !waitingForGreeting) {
            // Start conversation window for follow-up questions
            conversationMode = true;
            conversationWindowStart = millis();
            currentLEDMode = LED_CONVERSATION_WINDOW;
            
            // Clear any visualization states - conversation mode takes priority
            moonState.active = false;
            tideState.active = false;
            
            Serial.println("💬 Conversation window opened - speak anytime in next 10 seconds");
        } else {
            // Priority: Timer > Moon > Tide > Ambient VU > Idle
            if (timerState.active) {
                currentLEDMode = LED_TIMER;
                Serial.println("✓ Audio playback complete - switching to TIMER display");
            } else if (moonState.active) {
                currentLEDMode = LED_MOON;
                moonState.displayStartTime = millis();
                Serial.println("✓ Audio playback complete - switching to MOON display");
            } else if (tideState.active) {
                currentLEDMode = LED_TIDE;
                tideState.displayStartTime = millis();
                Serial.printf("✓ Audio playback complete - switching to TIDE display (state=%s, level=%.2f)\n", 
                             tideState.state.c_str(), tideState.waterLevel);
            } else if (ambientVUMode) {
                currentLEDMode = LED_AMBIENT_VU;
                Serial.println("✓ Audio playback complete - returning to AMBIENT VU mode");
            } else {
                currentLEDMode = LED_IDLE;
                Serial.println("✓ Audio playback complete - switching to IDLE");
            }
        }
        }
    }
    
    // Check for ambient sound streaming completion
    // When stream ends, return to IDLE mode (no looping)
    if (isPlayingAmbient && ambientSound.active && !firstAudioChunk && 
        (millis() - lastAudioChunkTime) > 7000) {
        Serial.printf("✓ Ambient sound completed: %s - returning to IDLE\n", ambientSound.name.c_str());
        
        // Return to IDLE mode
        currentLEDMode = LED_IDLE;
        isPlayingAmbient = false;
        isPlayingResponse = false;
        ambientSound.active = false;
        ambientSound.name = "";
    }
    
    // Conversation window monitoring
    if (conversationMode && !isPlayingResponse && !recordingActive) {
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
                    Serial.println("🎤 Voice detected in conversation window - starting recording");
                    conversationMode = false;  // Exit window mode
                    conversationRecording = true;  // Flag for longer silence timeout
                    recordingActive = true;
                    recordingStartTime = millis();
                    lastVoiceActivityTime = millis();
                    currentLEDMode = LED_RECORDING;
                    lastDebounceTime = millis();  // Reset debounce to prevent immediate stop
                }
            } else {
                static uint32_t lastErrorPrint = 0;
                if (millis() - lastErrorPrint > 3000) {
                    Serial.printf("⚠️  I2S error: result=%d, bytes=%d\n", result, bytes_read);
                    lastErrorPrint = millis();
                }
            }
        } else {
            // Window expired with no voice - return to idle
            Serial.println("💬 Conversation window expired - returning to IDLE");
            conversationMode = false;
            currentLEDMode = LED_IDLE;
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
    const int durationMs = 100;  // Short 100ms beep
    const int numSamples = (sampleRate * durationMs) / 1000;
    const float frequency = 1000.0f;  // 1kHz tone
    
    static int16_t toneBuffer[4800];  // 100ms at 24kHz stereo (2400 samples * 2 channels)
    
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
                
                // Show connection on LEDs
                fill_solid(leds, NUM_LEDS, CRGB::Green);
                FastLED.show();
                delay(500);
                
                // Resume ambient mode if it was active before disconnect
                if (ambientSound.active && !ambientSound.name.isEmpty()) {
                    Serial.printf("▶️  Resuming ambient sound: %s (seq %d)\n", 
                                 ambientSound.name.c_str(), ambientSound.sequence);
                    
                    // Restore LED mode based on ambient sound
                    if (ambientSound.name == "rain") {
                        currentLEDMode = LED_AMBIENT_RAIN;
                    } else if (ambientSound.name == "ocean") {
                        currentLEDMode = LED_AMBIENT_OCEAN;
                    } else if (ambientSound.name == "rainforest") {
                        currentLEDMode = LED_AMBIENT_RAINFOREST;
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
            Serial.printf("📥 Received: %d bytes\n", length);
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
                        // Rate-limit logging to prevent spam (max 1/second)
                        static uint32_t lastDiscardLog = 0;
                        static uint32_t discardsSinceLog = 0;
                        discardsSinceLog++;
                        
                        if (millis() - lastDiscardLog > 1000) {
                            if (discardsSinceLog > 0) {
                                Serial.printf("🚫 Discarded %u stale ambient chunks (seq %d, active=%d, expected=%d)\n", 
                                             discardsSinceLog, chunkSequence, ambientSound.active, ambientSound.sequence);
                            }
                            discardsSinceLog = 0;
                            lastDiscardLog = millis();
                        }
                        break;  // Discard this chunk
                    }
                }
                // else: Gemini audio (no magic header) - continue to play
                
                // Ignore audio if response was interrupted (but not for ambient sounds)
                if (responseInterrupted && !isPlayingAmbient) {
                    Serial.println("🚫 Discarding audio chunk (response was interrupted)");
                    break;
                }
                
                // Raw PCM audio data from server (16-bit mono samples)
                // This handles BOTH Gemini responses and ambient sounds
                if (!isPlayingResponse) {
                    // Wait for prebuffer before starting playback to eliminate initial stutter
                    uint32_t queueDepth = uxQueueMessagesWaiting(audioOutputQueue);
                    const uint32_t MIN_PREBUFFER = 8;  // ~320ms buffer (8 packets × 40ms)
                    
                    if (queueDepth >= MIN_PREBUFFER) {
                        isPlayingResponse = true;
                        
                        // Only reset turn state for non-ambient audio
                        if (!isPlayingAmbient) {
                            turnComplete = false;  // New Gemini turn starting
                        }
                        
                        recordingActive = false;  // Ensure recording is stopped
                        
                        // Set LED mode - prioritize visualizations during playback
                        if (!ambientSound.active) {
                            if (moonState.active) {
                                currentLEDMode = LED_MOON;
                                moonState.displayStartTime = millis();
                                Serial.println("🌙 Showing moon visualization during playback");
                            } else if (tideState.active) {
                                currentLEDMode = LED_TIDE;
                                tideState.displayStartTime = millis();
                                Serial.println("🌊 Showing tide visualization during playback");
                            } else {
                                currentLEDMode = LED_AUDIO_REACTIVE;
                            }
                        }
                        
                        firstAudioChunk = true;
                        // NOTE: Don't clear responseInterrupted here! Only clear it on turnComplete
                        // to prevent buffered chunks from interrupted response playing through
                        // Clear all LEDs immediately when starting playback
                        fill_solid(leds, NUM_LEDS, CRGB::Black);
                        FastLED.show();
                        // Clear delay buffer for clean LED sync
                        for (int i = 0; i < AUDIO_DELAY_BUFFER_SIZE; i++) {
                            audioLevelBuffer[i] = 0;
                        }
                        audioBufferIndex = 0;
                        
                        if (isPlayingAmbient) {
                            Serial.printf("🔊 Starting ambient audio stream: %s (prebuffered %u packets)\n", 
                                         ambientSound.name.c_str(), queueDepth);
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
                        // 🔍 DIAGNOSTIC: Log queue growth (only when significant)
                        if (queueBefore == 0 || queueBefore >= AUDIO_QUEUE_SIZE - 5) {
                            Serial.printf("📈 Queue: %u → %u/%u\n", queueBefore, queueBefore + 1, AUDIO_QUEUE_SIZE);
                        }
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
        Serial.println("⏱️  Timer set - starting countdown");
        timerState.totalSeconds = doc["durationSeconds"].as<int>();
        timerState.startTime = millis();
        timerState.active = true;
        currentLEDMode = LED_TIMER;  // Switch to timer display immediately
        
        Serial.printf("⏱️  Timer: %d seconds (%d minutes)\n",
                      timerState.totalSeconds,
                      timerState.totalSeconds / 60);
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
        // Flash LEDs for completion
        for (int i = 0; i < 3; i++) {
            fill_solid(leds, NUM_LEDS, CRGB::Green);
            FastLED.show();
            delay(200);
            fill_solid(leds, NUM_LEDS, CRGB::Black);
            FastLED.show();
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
    
    // Handle moon data
    if (doc["type"].is<const char*>() && doc["type"] == "moonData") {
        moonState.phaseName = doc["phaseName"].as<String>();
        moonState.illumination = doc["illumination"].as<int>();
        moonState.moonAge = doc["moonAge"].as<float>();
        moonState.displayStartTime = millis();
        moonState.active = true;
        currentLEDMode = LED_MOON;
        Serial.printf("🌙 Moon: %s (%d%% illuminated, %.1f days old)\n", 
                      moonState.phaseName.c_str(), moonState.illumination, moonState.moonAge);
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
    static uint32_t lastModeLog = 0;
    
    // Debug: log mode every 3 seconds
    if (millis() - lastModeLog > 3000) {
        Serial.printf("🎨 LED Mode: %d (IDLE=1, RECORDING=2, PROCESSING=3, AUDIO_REACTIVE=4)\n", currentLEDMode);
        lastModeLog = millis();
    }
    
    // Smooth the audio level with exponential moving average
    const float smoothing = 0.15f;  // Lower = more smoothing (was 0.3)
    smoothedAudioLevel = smoothedAudioLevel * (1.0f - smoothing) + currentAudioLevel * smoothing;
    
    // Faster decay when no audio to prevent LEDs lingering after speech ends
    if (currentAudioLevel == 0) {
        smoothedAudioLevel *= 0.85f;  // Faster decay (was 0.95)
        // Force to zero when very low to prevent lingering
        if (smoothedAudioLevel < 50) {
            smoothedAudioLevel = 0;
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
                        if (distance < 6.0) {
                            float waveBrightness = (1.0 - (distance / 6.0));
                            waveBrightness = waveBrightness * waveBrightness;  // Squared for softer falloff
                            uint8_t brightness = (uint8_t)(waveBrightness * 180 + 30);  // 30-210 range (not too bright)
                            leds[ledIndex] = CHSV(160, 200, brightness);  // Blue hue locked at 160
                        } else {
                            // Base ambient glow when wave is far away
                            leds[ledIndex] = CHSV(160, 200, 30);  // Dim blue
                        }
                    }
                }
            }
            break;
            
        case LED_RECORDING:
            // VU meter during recording - vertical bars on all strips with fade trail
            // Orange gradient for person speaking (warm, friendly)
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
                        
                        if (row < numRows) {
                            // Orange gradient for person
                            if (row < LEDS_PER_COLUMN * 0.6) {
                                leds[ledIndex] = CRGB(255, 140, 0);  // Soft orange (bottom 60%)
                            } else if (row < LEDS_PER_COLUMN * 0.85) {
                                leds[ledIndex] = CRGB(255, 100, 0);  // Deeper orange (60-85%)
                            } else {
                                leds[ledIndex] = CRGB(255, 50, 0);  // Red-orange (top 85-100%)
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
            // Purple gradient for Gemini speaking (AI, calm)
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
                        
                        if (row < numRows) {
                            // Purple gradient for Gemini
                            if (row < LEDS_PER_COLUMN * 0.6) {
                                leds[ledIndex] = CRGB(120, 60, 200);  // Soft purple (bottom 60%)
                            } else if (row < LEDS_PER_COLUMN * 0.85) {
                                leds[ledIndex] = CRGB(140, 40, 220);  // Brighter purple (60-85%)
                            } else {
                                leds[ledIndex] = CRGB(160, 20, 240);  // Vibrant purple (top 85-100%)
                            }
                        }
                        // LEDs above numRows keep their faded value
                    }
                }
            }
            break;
            
        case LED_TIDE:
            // Tide visualization: water level represented by LED count
            // Blue = flooding (incoming), Red = ebbing (outgoing)
            // Stays active until next user interaction
            {
                // Debug: log mode switch
                static uint32_t lastDebugLog = 0;
                if (millis() - lastDebugLog > 5000) {
                    Serial.printf("🌊 LED_TIDE active: state=%s, level=%.2f, mode=%d\n", 
                                 tideState.state.c_str(), tideState.waterLevel, currentLEDMode);
                    lastDebugLog = millis();
                }
                
                // Calculate number of LEDs to light based on water level (0.0 to 1.0)
                int numLEDs = max(1, (int)(tideState.waterLevel * NUM_LEDS));
                
                // Choose color based on tide state
                CRGB tideColor = tideState.state == "flooding" ? CRGB(0, 100, 255) : CRGB(255, 50, 0);
                
                // Add gentle wave animation
                float wave = sin((millis() / 1000.0) * 2.0) * 0.3 + 0.7; // 0.4 to 1.0
                
                for (int i = 0; i < NUM_LEDS; i++) {
                    if (i < numLEDs) {
                        // Apply wave brightness modulation
                        uint8_t r = (uint8_t)(tideColor.r * wave);
                        uint8_t g = (uint8_t)(tideColor.g * wave);
                        uint8_t b = (uint8_t)(tideColor.b * wave);
                        leds[i] = CRGB(r, g, b);
                    } else {
                        leds[i] = CRGB(0, 0, 0);
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
            // Moon phase visualization - brightness based on illumination
            {
                if (moonState.active) {
                    // Soft blue-white color (low saturation for pale moon glow)
                    uint8_t moonHue = 160;  // Blue-cyan
                    uint8_t moonSat = 80;   // Low saturation for pale white-blue
                    
                    // Map illumination (0-100%) to brightness
                    uint8_t brightness = map(moonState.illumination, 0, 100, 20, 255);
                    
                    // Gentle pulse effect (slower for moon)
                    float pulse = 0.85 + 0.15 * sin(millis() / 1500.0);
                    brightness = (uint8_t)(brightness * pulse);
                    
                    // Number of LEDs lit based on illumination phase
                    int numLEDs = map(moonState.illumination, 0, 100, 1, NUM_LEDS);
                    
                    for (int i = 0; i < NUM_LEDS; i++) {
                        if (i < numLEDs) {
                            // Gradient fade from center
                            float fadePosition = (float)i / numLEDs;
                            uint8_t ledBrightness = (uint8_t)(brightness * (1.0 - fadePosition * 0.3));
                            leds[i] = CHSV(moonHue, moonSat, ledBrightness);
                        } else {
                            leds[i] = CRGB::Black;
                        }
                    }
                    
                    // Auto-return to IDLE after 20 seconds
                    if (millis() - moonState.displayStartTime > 20000) {
                        moonState.active = false;
                        currentLEDMode = LED_IDLE;
                        Serial.println("🌙 Moon display timeout - returning to IDLE");
                    }
                } else {
                    fill_solid(leds, NUM_LEDS, CRGB::Black);
                }
            }
            break;
            
        case LED_AMBIENT_RAIN:
            // Falling rain effect - random blue drops falling down
            {
                static uint32_t lastDrop = 0;
                static uint8_t dropBrightness[144] = {0};  // Track drop brightness (max LEDs)
                
                // Fade all LEDs
                for (int i = 0; i < NUM_LEDS; i++) {
                    leds[i].fadeToBlackBy(40);
                }
                
                // Add new drops randomly
                if (millis() - lastDrop > 100) {
                    if (random(100) < 30) {  // 30% chance each 100ms
                        int pos = random(NUM_LEDS);
                        dropBrightness[pos] = 255;
                    }
                    lastDrop = millis();
                }
                
                // Render drops
                for (int i = 0; i < NUM_LEDS; i++) {
                    if (dropBrightness[i] > 0) {
                        leds[i] = CHSV(160, 255, dropBrightness[i]);  // Blue
                        dropBrightness[i] = dropBrightness[i] * 0.85;  // Fade out
                    }
                }
            }
            break;
            
        case LED_AMBIENT_OCEAN:
            // Swelling ocean waves - blue wave that rises and falls
            {
                float wave = (sin(millis() / 2000.0) + 1.0) / 2.0;  // 0.0 to 1.0
                int numLit = (int)(wave * NUM_LEDS);
                
                for (int i = 0; i < NUM_LEDS; i++) {
                    if (i < numLit) {
                        // Gradient from deep blue to cyan
                        uint8_t hue = 160 + (i * 10 / NUM_LEDS);  // 160-170 range
                        uint8_t brightness = 150 + (i * 105 / NUM_LEDS);  // Brighter at top
                        leds[i] = CHSV(hue, 255, brightness);
                    } else {
                        leds[i] = CRGB::Black;
                    }
                }
            }
            break;
            
        case LED_AMBIENT_RAINFOREST:
            // Pulsing green canopy - gentle breathing effect
            {
                float pulse = 0.6 + 0.4 * sin(millis() / 3000.0);  // 0.6 to 1.0
                uint8_t brightness = (uint8_t)(200 * pulse);
                
                for (int i = 0; i < NUM_LEDS; i++) {
                    // Green with slight variation per LED
                    uint8_t hue = 96 + (i * 2);  // Green range 96-114
                    uint8_t sat = 220 + (i * 3);  // Varying saturation
                    leds[i] = CHSV(hue, sat, brightness);
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
            // Scrolling text marquee around the circle
            {
                // Update scroll position every 240ms for readable scrolling (1/3 speed)
                if (millis() - marqueeState.lastUpdate > 240) {
                    marqueeState.scrollPosition--;
                    marqueeState.lastUpdate = millis();
                    
                    // Calculate total width of text
                    int totalWidth = marqueeState.text.length() * (FONT_WIDTH + FONT_SPACING);
                    
                    // If text has scrolled completely off the left, finish marquee
                    if (marqueeState.scrollPosition < -totalWidth) {
                        marqueeState.active = false;
                        currentLEDMode = targetLEDMode;
                        Serial.printf("📜 Marquee complete, switching to mode %d\n", targetLEDMode);
                    }
                }
                
                // Clear all LEDs
                fill_solid(leds, NUM_LEDS, CRGB::Black);
                
                // Draw each character of the text
                int xPos = marqueeState.scrollPosition;  // Current x position for drawing
                
                for (int charIdx = 0; charIdx < marqueeState.text.length(); charIdx++) {
                    char c = marqueeState.text[charIdx];
                    int fontIdx = getCharIndex(c);
                    
                    // Draw this character column by column
                    for (int col = 0; col < FONT_WIDTH; col++) {
                        int screenCol = xPos + col;
                        
                        // Only draw if within visible area (0 to LED_COLUMNS-1)
                        if (screenCol >= 0 && screenCol < LED_COLUMNS) {
                            uint8_t columnBits = font5x7[fontIdx][col];
                            
                            // Draw pixels vertically - center the 7-pixel font in 12-pixel height
                            int yOffset = (LEDS_PER_COLUMN - FONT_HEIGHT) / 2;  // Center vertically
                            
                            for (int row = 0; row < FONT_HEIGHT; row++) {
                                if (columnBits & (1 << (6 - row))) {  // Check bit (top to bottom)
                                    int ledIndex = screenCol * LEDS_PER_COLUMN + (yOffset + row);
                                    if (ledIndex >= 0 && ledIndex < NUM_LEDS) {
                                        leds[ledIndex] = marqueeState.color;
                                    }
                                }
                            }
                        }
                    }
                    
                    // Move to next character position (width + spacing)
                    xPos += FONT_WIDTH + FONT_SPACING;
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
    while(1) {
        updateLEDs();
        FastLED.show();
        vTaskDelay(30 / portTICK_PERIOD_MS);  // 33Hz update rate - matches audio chunk timing better
    }
}
