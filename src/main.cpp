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

// Opus codec
extern "C" {
    #include "opus.h"
}

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
bool firstAudioChunk = true;
float volumeMultiplier = 0.25f;  // Volume control (25% for testing)
int32_t currentAudioLevel = 0;  // Current audio amplitude for VU meter
float smoothedAudioLevel = 0.0f;  // Smoothed audio level for stable VU meter

// Audio level delay buffer for LED sync (compensates for I2S buffer latency)
#define AUDIO_DELAY_BUFFER_SIZE 12  // ~360ms delay to match I2S playback buffer + speaker latency
int audioLevelBuffer[AUDIO_DELAY_BUFFER_SIZE] = {0};
int audioBufferIndex = 0;

TaskHandle_t websocketTaskHandle = NULL;
TaskHandle_t ledTaskHandle = NULL;
TaskHandle_t audioTaskHandle = NULL;

// Opus encoder/decoder handles
OpusEncoder *encoder = NULL;
OpusDecoder *decoder = NULL;

// Audio buffers
QueueHandle_t audioOutputQueue;   // Queue for playback audio
#define AUDIO_QUEUE_SIZE 30

struct AudioChunk {
    uint8_t data[2048];
    size_t length;
};

enum LEDMode { LED_BOOT, LED_IDLE, LED_RECORDING, LED_PROCESSING, LED_AUDIO_REACTIVE, LED_CONNECTED, LED_ERROR, LED_TIDE, LED_TIMER, LED_MOON, LED_AMBIENT_VU, LED_AMBIENT_RAIN, LED_AMBIENT_OCEAN, LED_AMBIENT_RAINFOREST };
LEDMode currentLEDMode = LED_BOOT;
bool ambientVUMode = false;  // Toggle for ambient sound VU meter mode

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

    // Initialize LED strip (9 LEDs on GPIO 1)
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
    
    // Initialize Opus encoder
    Serial.println("Creating Opus encoder...");
    Serial.flush();
    int error = 0;
    
    // Allocate encoder in PSRAM (board has 2MB PSRAM)
    size_t encoder_size = opus_encoder_get_size(AUDIO_CHANNELS);
    Serial.printf("Encoder size needed: %d bytes\n", encoder_size);
    encoder = (OpusEncoder*)ps_malloc(encoder_size);
    if (!encoder) {
        Serial.println("✗ Failed to allocate Opus encoder memory");
        currentLEDMode = LED_ERROR;
        return;
    }
    
    error = opus_encoder_init(encoder, OPUS_SAMPLE_RATE, AUDIO_CHANNELS, OPUS_APPLICATION_VOIP);
    if (error != OPUS_OK) {
        Serial.printf("✗ Failed to initialize Opus encoder: %d\n", error);
        free(encoder);
        encoder = NULL;
        currentLEDMode = LED_ERROR;
        return;
    }
    opus_encoder_ctl(encoder, OPUS_SET_BITRATE(OPUS_BITRATE));
    opus_encoder_ctl(encoder, OPUS_SET_COMPLEXITY(OPUS_COMPLEXITY));
    Serial.println("✓ Opus encoder initialized");
    
    // Initialize Opus decoder
    size_t decoder_size = opus_decoder_get_size(AUDIO_CHANNELS);
    Serial.printf("Decoder size needed: %d bytes\n", decoder_size);
    decoder = (OpusDecoder*)ps_malloc(decoder_size);
    if (!decoder) {
        Serial.println("✗ Failed to allocate Opus decoder memory");
        currentLEDMode = LED_ERROR;
        return;
    }
    
    error = opus_decoder_init(decoder, OPUS_SAMPLE_RATE, AUDIO_CHANNELS);
    if (error != OPUS_OK || !decoder) {
        Serial.printf("✗ Failed to create Opus decoder: %d\n", error);
        currentLEDMode = LED_ERROR;
        return;
    }
    Serial.println("✓ Opus decoder initialized");

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
    webSocket.enableHeartbeat(15000, 3000, 2);  // Ping every 15s, timeout 3s, 2 retries (faster detection)
    Serial.println("✓ WebSocket initialized with keepalive");

    // Start FreeRTOS tasks
    // WebSocket needs high priority (3) and larger stack for heavy audio streaming
    xTaskCreatePinnedToCore(websocketTask, "WebSocket", 16384, NULL, 3, &websocketTaskHandle, CORE_1);  // Increased from 8KB to 16KB
    xTaskCreatePinnedToCore(ledTask, "LEDs", 4096, NULL, 0, &ledTaskHandle, CORE_0);
    xTaskCreatePinnedToCore(audioTask, "Audio", 32768, NULL, 2, &audioTaskHandle, CORE_1);  // Increased to 32KB for Opus + buffers
    Serial.println("✓ Tasks created on dual cores");

    // Play startup sound
    Serial.println("🔊 Playing startup sound...");
    playStartupSound();
    
    currentLEDMode = LED_IDLE;
    Serial.printf("=== Initialization Complete ===  [LEDMode set to %d]\n", currentLEDMode);
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
                currentLEDMode = LED_AMBIENT_VU;
                ambientVUMode = true;
                ambientSound.sequence++;  // Increment for mode change
                Serial.println("🎵 Ambient VU meter mode enabled");
            } else if (currentLEDMode == LED_AMBIENT_VU) {
                currentLEDMode = LED_AMBIENT_RAIN;
                ambientVUMode = false;
                ambientSound.name = "rain";
                ambientSound.active = true;
                ambientSound.sequence++;
                isPlayingAmbient = true;
                isPlayingResponse = false;
                firstAudioChunk = true;
                lastAudioChunkTime = millis();  // Initialize timing
                Serial.printf("🌧️  Rain ambient sound mode (seq %d)\n", ambientSound.sequence);
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
                currentLEDMode = LED_AMBIENT_OCEAN;
                ambientSound.name = "ocean";
                ambientSound.active = true;
                ambientSound.sequence++;
                isPlayingAmbient = true;
                isPlayingResponse = false;
                firstAudioChunk = true;
                lastAudioChunkTime = millis();  // Initialize timing
                Serial.printf("🌊 Ocean ambient sound mode (seq %d)\n", ambientSound.sequence);
                // Request ocean sounds from server
                JsonDocument ambientDoc;
                ambientDoc["action"] = "requestAmbient";
                ambientDoc["sound"] = "ocean";
                ambientDoc["sequence"] = ambientSound.sequence;
                String ambientMsg;
                serializeJson(ambientDoc, ambientMsg);
                webSocket.sendTXT(ambientMsg);
            } else if (currentLEDMode == LED_AMBIENT_OCEAN) {
                currentLEDMode = LED_AMBIENT_RAINFOREST;
                ambientSound.name = "rainforest";
                ambientSound.active = true;
                ambientSound.sequence++;
                isPlayingAmbient = true;
                isPlayingResponse = false;
                firstAudioChunk = true;
                lastAudioChunkTime = millis();  // Initialize timing
                Serial.printf("🌿 Rainforest ambient sound mode (seq %d)\n", ambientSound.sequence);
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
                currentLEDMode = LED_IDLE;
                
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
        // Block if: recording active, playing response, OR in ambient sound mode
        else if (startTouch && !startPressed && !recordingActive && !isPlayingResponse && !isPlayingAmbient) {
            // Clear all previous state
            responseInterrupted = false;
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
    
    // Auto-stop on silence (VAD)
    if (recordingActive && (millis() - lastVoiceActivityTime) > VAD_SILENCE_MS) {
        recordingActive = false;
        // Don't change LED mode if ambient sound is already active
        if (!ambientSound.active) {
            currentLEDMode = LED_PROCESSING;
            processingStartTime = millis();
        }
        Serial.println("⏹️  Recording stopped - Silence detected");
    }
    
    // Timeout PROCESSING mode if no response after 10 seconds
    if (currentLEDMode == LED_PROCESSING && processingStartTime > 0 && (millis() - processingStartTime) > 10000) {
        Serial.println("⚠️  Processing timeout - no response received, returning to IDLE");
        currentLEDMode = LED_IDLE;
    }
    
    // Auto-return to IDLE when Gemini playback finishes (no audio chunks for 2 seconds)
    // NOTE: This should NOT trigger for ambient sounds - they may have gaps in streaming
    if (isPlayingResponse && !isPlayingAmbient && (millis() - lastAudioChunkTime) > 2000) {
        isPlayingResponse = false;
        Serial.println("⏹️  Audio playback complete (2s timeout)");
        
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
    int16_t inputBuffer[OPUS_FRAME_SIZE];
    size_t bytes_read = 0;
    static uint32_t lastDebug = 0;
    
    while(1) {
        // Capture and send audio when recording (but not during playback)
        if (recordingActive && !isPlayingResponse) {
            if (i2s_read(I2S_NUM_0, inputBuffer, OPUS_FRAME_SIZE * sizeof(int16_t), &bytes_read, 100) == ESP_OK) {
                if (bytes_read == OPUS_FRAME_SIZE * sizeof(int16_t)) {
                    // Apply software gain (amplify by 16x to compensate for low INMP441 output)
                    const int16_t GAIN = 16;
                    for (size_t i = 0; i < OPUS_FRAME_SIZE; i++) {
                        int32_t amplified = (int32_t)inputBuffer[i] * GAIN;
                        // Clamp to int16_t range
                        if (amplified > 32767) amplified = 32767;
                        if (amplified < -32768) amplified = -32768;
                        inputBuffer[i] = (int16_t)amplified;
                    }
                    
                    // Calculate amplitude for VAD
                    int32_t sum = 0;
                    for (size_t i = 0; i < OPUS_FRAME_SIZE; i++) {
                        sum += abs(inputBuffer[i]);
                    }
                    int32_t avgAmplitude = sum / OPUS_FRAME_SIZE;
                    
                    // Update current audio level for LED VU meter
                    currentAudioLevel = avgAmplitude;
                    
                    // VAD check
                    bool hasVoice = detectVoiceActivity(inputBuffer, OPUS_FRAME_SIZE);
                    
                    // Debug output every 2 seconds
                    if (millis() - lastDebug > 2000) {
                        Serial.printf("[AUDIO] Recording: bytes_read=%d, hasVoice=%d, avgAmp=%d, threshold=%d\n", 
                                      bytes_read, hasVoice, avgAmplitude, VAD_THRESHOLD);
                        lastDebug = millis();
                    }
                    
                    // Send raw PCM audio directly (Live API handles encoding)
                    sendAudioChunk((uint8_t*)inputBuffer, bytes_read);
                    
                    // Update last voice activity time for VAD
                    if (hasVoice) {
                        lastVoiceActivityTime = millis();
                    }
                }
            }
        } else {
            // Not recording - just sleep
            vTaskDelay(100 / portTICK_PERIOD_MS);
        }
        
        vTaskDelay(1 / portTICK_PERIOD_MS);
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
        i2s_write(I2S_NUM_1, toneBuffer, numSamples * 4, &bytes_written, portMAX_DELAY);
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
    i2s_write(I2S_NUM_1, toneBuffer, numSamples * 4, &bytes_written, portMAX_DELAY);
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
                    isPlayingResponse = true;
                    
                    // Only reset turn state for non-ambient audio
                    if (!isPlayingAmbient) {
                        turnComplete = false;  // New Gemini turn starting
                    }
                    
                    recordingActive = false;  // Ensure recording is stopped
                    
                    // Set LED mode (ambient sounds set their own mode in ambientStart)
                    if (!ambientSound.active) {
                        currentLEDMode = LED_AUDIO_REACTIVE;
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
                        Serial.printf("🔊 Starting ambient audio stream: %s\n", ambientSound.name.c_str());
                    } else {
                        Serial.println("🔊 Starting audio playback...");
                    }
                }
                
                // Update last audio chunk time
                lastAudioChunkTime = millis();
                
                // Clear greeting flag on first audio chunk
                if (waitingForGreeting) {
                    waitingForGreeting = false;
                    Serial.println("👋 Startup greeting received!");
                }
                
                // Debug first chunk
                if (firstAudioChunk) {
                    Serial.print("First bytes (hex): ");
                    for (int i = 0; i < min(8, (int)length); i++) {
                        Serial.printf("%02X ", payload[i]);
                    }
                    Serial.println();
                    firstAudioChunk = false;
                }
                
                int16_t* samples = (int16_t*)payload;
                int numSamples = length / 2;
                
                // Calculate amplitude for VU meter
                int32_t sum = 0;
                for (int i = 0; i < numSamples; i++) {
                    sum += abs(samples[i]);
                }
                int instantLevel = sum / numSamples;
                
                // Add to delay buffer and get delayed value for LED sync
                audioLevelBuffer[audioBufferIndex] = instantLevel;
                audioBufferIndex = (audioBufferIndex + 1) % AUDIO_DELAY_BUFFER_SIZE;
                currentAudioLevel = audioLevelBuffer[audioBufferIndex];  // Use delayed value for LEDs
                
                // Debug audio level periodically
                static uint32_t lastLevelDebug = 0;
                if (millis() - lastLevelDebug > 1000) {
                    Serial.printf("[PLAYBACK] avgAmp=%d, smoothed=%.0f, mode=%d, volume=%.2f\n", 
                                  currentAudioLevel, smoothedAudioLevel, currentLEDMode, volumeMultiplier);
                    lastLevelDebug = millis();
                }
                
                // Convert mono to stereo using static buffer (no malloc delay)
                static int16_t stereoBuffer[1024];  // Max 512 samples = 1024 bytes input
                
                for (int i = 0; i < numSamples && i < 512; i++) {
                    // Apply volume control
                    int32_t sample = (int32_t)(samples[i] * volumeMultiplier);
                    // Clamp to int16 range
                    if (sample > 32767) sample = 32767;
                    if (sample < -32768) sample = -32768;
                    stereoBuffer[i * 2] = (int16_t)sample;
                    stereoBuffer[i * 2 + 1] = (int16_t)sample;
                }
                
                size_t bytes_written;
                
                // Determine if we should play this audio
                // Play if: (1) Not in ambient mode (Gemini audio), OR (2) In ambient mode AND actively playing
                bool shouldPlay = !ambientSound.active || (ambientSound.active && isPlayingAmbient);
                
                if (shouldPlay) {
                    esp_err_t result = i2s_write(I2S_NUM_1, stereoBuffer, numSamples * 4, &bytes_written, portMAX_DELAY);
                    
                    if (result != ESP_OK || bytes_written < numSamples * 4) {
                        Serial.printf("[I2S WARNING] Write issue: result=%d, requested=%d, written=%d\n", 
                                     result, numSamples * 4, bytes_written);
                    }
                } else {
                    bytes_written = numSamples * 4;  // Pretend we wrote it
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
            brightness = constrain(100 + (int)(50 * sin(millis() / 500.0)), 0, 255);
            fill_solid(leds, NUM_LEDS, CHSV(160, 255, brightness));
            break;
            
        case LED_IDLE:
            // Jellyfish-inspired pulse animation - wave travels up with fade trail
            {
                float t = (millis() % 3000) / 3000.0;  // 3 second cycle
                // Create wave position (travels from LED 0 to LED 8)
                float wavePos = t * (NUM_LEDS + 3);  // +3 for smooth exit
                
                for (int i = 0; i < NUM_LEDS; i++) {
                    // Calculate distance from wave center
                    float distance = wavePos - i;
                    
                    if (distance >= 0 && distance < 4) {
                        // Active wave (bright blue with trail)
                        float waveBrightness = (1.0 - (distance / 4.0)) * 255;
                        leds[i] = CHSV(160, 200, (uint8_t)waveBrightness);
                    } else {
                        // Fade trail (dims gradually)
                        uint8_t currentBrightness = leds[i].getLuma();
                        if (currentBrightness > 30) {
                            leds[i].fadeToBlackBy(25);  // Gentle fade
                        } else {
                            leds[i] = CHSV(160, 200, 30);  // Minimum ambient glow
                        }
                    }
                }
            }
            break;
            
        case LED_RECORDING:
            // VU meter during recording - green to yellow to red
            {
                // Map audio level to LED count (0-9 LEDs)
                // Typical range: 0-5000 (after 16x gain), map to 0-9 LEDs
                int numLEDs = map(constrain((int)smoothedAudioLevel, 0, 5000), 0, 5000, 0, NUM_LEDS);
                for (int i = 0; i < NUM_LEDS; i++) {
                    if (i < numLEDs) {
                        // Universal VU meter gradient
                        if (i < NUM_LEDS * 0.6) {
                            leds[i] = CRGB::Green;  // 0-60% = green (safe)
                        } else if (i < NUM_LEDS * 0.85) {
                            leds[i] = CRGB::Yellow;  // 60-85% = yellow (approaching limit)
                        } else {
                            leds[i] = CRGB::Red;  // 85-100% = red (near max)
                        }
                    } else {
                        leds[i] = CRGB::Black;  // Turn off unused LEDs
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
            // Ambient sound VU meter - uses microphone to visualize external sounds
            // Read microphone continuously and display real-time VU meter
            {
                static int16_t ambientBuffer[640];  // 40ms at 16kHz (640 samples)
                size_t bytes_read = 0;
                
                // Clear all LEDs first to ensure clean state
                fill_solid(leds, NUM_LEDS, CRGB::Black);
                
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
                    
                    // Map to LED count - lower threshold for better verse visibility
                    // Adjusted range: 150-1600 (was 200-1600)
                    int numLEDs = map(constrain((int)smoothedRMS, 150, 1600), 150, 1600, 0, NUM_LEDS);
                    
                    // Debug output every 2 seconds
                    static uint32_t lastDebug = 0;
                    if (millis() - lastDebug > 2000) {
                        Serial.printf("🎵 VU: raw=%.0f gain=%.1f gained=%.0f smooth=%.0f LEDs=%d\n", 
                                     rms, autoGain, gainedRMS, smoothedRMS, numLEDs);
                        lastDebug = millis();
                    }
                    
                    // VU meter gradient (green to yellow to red)
                    for (int i = 0; i < NUM_LEDS; i++) {
                        if (i < numLEDs) {
                            if (i < NUM_LEDS * 0.6) {
                                leds[i] = CRGB(0, 255, 0);  // Pure green
                            } else if (i < NUM_LEDS * 0.85) {
                                leds[i] = CRGB(255, 255, 0);  // Pure yellow
                            } else {
                                leds[i] = CRGB(255, 0, 0);  // Pure red
                            }
                        } else {
                            leds[i] = CRGB(0, 0, 0);  // Off
                        }
                    }
                }
            }
            break;
            
        case LED_AUDIO_REACTIVE:
            // VU meter during playback - same green to yellow to red
            {
                // Clear all LEDs first
                fill_solid(leds, NUM_LEDS, CRGB::Black);
                
                // Map audio level to LED count (0-144 LEDs)
                // Playback audio range: 0-3000, map to 0-144 LEDs
                int numLEDs = map(constrain((int)smoothedAudioLevel, 0, 3000), 0, 3000, 0, NUM_LEDS);
                
                // Set LEDs up to numLEDs with gradient
                for (int i = 0; i < numLEDs; i++) {
                    // Universal VU meter gradient (same as recording)
                    if (i < NUM_LEDS * 0.6) {
                        leds[i] = CRGB(0, 255, 0);  // Pure green
                    } else if (i < NUM_LEDS * 0.85) {
                        leds[i] = CRGB(255, 255, 0);  // Pure yellow
                    } else {
                        leds[i] = CRGB(255, 0, 0);  // Pure red
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
            
        case LED_CONNECTED:
            fill_solid(leds, NUM_LEDS, CHSV(128, 255, LED_BRIGHTNESS));
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
        
        // Health monitoring every 10s
        if (millis() - lastHealthLog > 10000) {
            int32_t rssi = WiFi.RSSI();
            uint32_t timeSinceLastSend = millis() - lastWebSocketSendTime;
            Serial.printf("[WS Health] Connected=%d, WiFi=%d dBm, LastSend=%us ago, Failures=%u\n",
                         isWebSocketConnected, rssi, timeSinceLastSend/1000, webSocketSendFailures);
            lastHealthLog = millis();
        }
        
        // Monitor WiFi connection and log if disconnected
        if (millis() - lastConnCheck > 10000) {  // Check every 10s
            if (WiFi.status() != WL_CONNECTED) {
                Serial.printf("[WebSocket Task] WiFi disconnected! Status: %d\n", WiFi.status());
            } else if (!isWebSocketConnected) {
                Serial.printf("[WebSocket Task] WebSocket not connected. WiFi RSSI: %d dBm\n", WiFi.RSSI());
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
