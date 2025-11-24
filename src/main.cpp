#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>
#include <FastLED.h>
#include <driver/i2s.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
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
bool responseInterrupted = false;  // Flag to ignore audio after interrupt
uint32_t recordingStartTime = 0;
uint32_t lastVoiceActivityTime = 0;
uint32_t lastAudioChunkTime = 0;  // Track when we last received audio
bool firstAudioChunk = true;
float volumeMultiplier = 0.25f;  // Volume control (25% for testing)
int32_t currentAudioLevel = 0;  // Current audio amplitude for VU meter
float smoothedAudioLevel = 0.0f;  // Smoothed audio level for stable VU meter

// Audio level delay buffer for LED sync (compensates for I2S buffer latency)
#define AUDIO_DELAY_BUFFER_SIZE 6  // ~180ms delay to match I2S playback buffer
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

enum LEDMode { LED_BOOT, LED_IDLE, LED_RECORDING, LED_PROCESSING, LED_AUDIO_REACTIVE, LED_CONNECTED, LED_ERROR };
LEDMode currentLEDMode = LED_BOOT;

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

    // Connect WiFi
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    Serial.print("Connecting to WiFi");
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 20) {
        delay(500);
        Serial.print(".");
        attempts++;
    }
    if (WiFi.status() == WL_CONNECTED) {
        Serial.println("\n✓ WiFi connected");
        Serial.printf("IP: %s\n", WiFi.localIP().toString().c_str());
        Serial.printf("Signal: %d dBm\n", WiFi.RSSI());
    } else {
        Serial.println("\n✗ WiFi connection failed");
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
    webSocket.enableHeartbeat(15000, 3000, 2);  // Ping every 15s, timeout 3s, 2 retries
    Serial.println("✓ WebSocket initialized with keepalive");

    // Start FreeRTOS tasks
    xTaskCreatePinnedToCore(websocketTask, "WebSocket", 8192, NULL, 1, &websocketTaskHandle, CORE_1);
    xTaskCreatePinnedToCore(ledTask, "LEDs", 4096, NULL, 0, &ledTaskHandle, CORE_0);
    xTaskCreatePinnedToCore(audioTask, "Audio", 32768, NULL, 2, &audioTaskHandle, CORE_1);  // Increased to 32KB for Opus + buffers
    Serial.println("✓ Tasks created on dual cores");

    currentLEDMode = LED_IDLE;
    Serial.printf("=== Initialization Complete ===  [LEDMode set to %d]\n", currentLEDMode);
    Serial.println("Touch START pad to begin recording");
}

// ============== MAIN LOOP ==============
void loop() {
    static uint32_t lastPrint = 0;
    static uint32_t lastTouchDebug = 0;
    
    if (millis() - lastPrint > 5000) {
        Serial.write("LOOP_TICK\r\n", 11);
        lastPrint = millis();
    }
    
    // Debug touch pad states every 2 seconds
    if (millis() - lastTouchDebug > 2000) {
        Serial.printf("Touch: START=%d, STOP=%d, Recording=%d, LEDMode=%d\n", 
                      digitalRead(TOUCH_PAD_START_PIN), 
                      digitalRead(TOUCH_PAD_STOP_PIN),
                      recordingActive,
                      currentLEDMode);
        lastTouchDebug = millis();
    }
    
    // Poll touch pads with debouncing
    static bool startPressed = false;
    static bool stopPressed = false;
    static uint32_t lastDebounceTime = 0;
    const uint32_t debounceDelay = 50;
    
    if ((millis() - lastDebounceTime) > debounceDelay) {
        bool startTouch = digitalRead(TOUCH_PAD_START_PIN) == HIGH;
        bool stopTouch = digitalRead(TOUCH_PAD_STOP_PIN) == HIGH;
        
        // Interrupt feature: START button during playback stops audio and starts recording
        if (startTouch && !startPressed && isPlayingResponse) {
            Serial.println("⏸️  Interrupted response - starting new recording");
            isPlayingResponse = false;
            responseInterrupted = true;  // Flag to ignore remaining audio chunks
            firstAudioChunk = true;  // Reset for next playback
            
            // Don't send turnComplete - just ignore remaining audio locally
            // This keeps Gemini connection alive for the next question
            
            // Start new recording
            recordingActive = true;
            recordingStartTime = millis();
            lastVoiceActivityTime = millis();
            currentLEDMode = LED_RECORDING;
            // Clear I2S speaker buffer to stop audio immediately
            i2s_zero_dma_buffer(I2S_NUM_1);
            Serial.printf("🎤 Recording started... (START=%d, STOP=%d)\n", startTouch, stopTouch);
        }
        // Start recording on rising edge (when not already playing)
        else if (startTouch && !startPressed && !recordingActive && !isPlayingResponse) {
            responseInterrupted = false;  // Clear interrupt flag for new conversation
            recordingActive = true;
            recordingStartTime = millis();
            lastVoiceActivityTime = millis();
            currentLEDMode = LED_RECORDING;
            Serial.printf("🎤 Recording started... (START=%d, STOP=%d)\n", startTouch, stopTouch);
        }
        startPressed = startTouch;
        
        // Stop recording on rising edge or timeout
        if ((stopTouch && !stopPressed && recordingActive) || 
            (recordingActive && (millis() - recordingStartTime) > MAX_RECORDING_DURATION_MS)) {
            recordingActive = false;
            currentLEDMode = LED_PROCESSING;
            uint32_t duration = millis() - recordingStartTime;
            Serial.printf("⏹️  Recording stopped - Duration: %dms (START=%d, STOP=%d)\n", duration, startTouch, stopTouch);
        }
        stopPressed = stopTouch;
        
        lastDebounceTime = millis();
    }
    
    // Auto-stop on silence (VAD)
    if (recordingActive && (millis() - lastVoiceActivityTime) > VAD_SILENCE_MS) {
        recordingActive = false;
        currentLEDMode = LED_PROCESSING;
        Serial.println("⏹️  Recording stopped - Silence detected");
    }
    
    // Auto-return to IDLE when playback finishes (no audio chunks for 2 seconds)
    // Longer timeout to handle natural pauses in speech
    if (isPlayingResponse && (millis() - lastAudioChunkTime) > 2000) {
        isPlayingResponse = false;
        // Clear LEDs before switching mode
        fill_solid(leds, NUM_LEDS, CRGB::Black);
        FastLED.show();
        delay(50);  // Brief delay to ensure clean transition
        currentLEDMode = LED_IDLE;
        Serial.println("✓ Audio playback complete - switching to IDLE");
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
    webSocket.sendTXT(output);
}

void onWebSocketEvent(WStype_t type, uint8_t * payload, size_t length) {
    switch(type) {
        case WStype_CONNECTED:
            Serial.println("✓ WebSocket Connected to Edge Server!");
            isWebSocketConnected = true;
            currentLEDMode = LED_CONNECTED;
            
            // Edge server handles Gemini setup automatically
            Serial.println("✓ Waiting for 'ready' message from server");
            
            // Show connection on LEDs
            fill_solid(leds, NUM_LEDS, CRGB::Green);
            FastLED.show();
            delay(500);
            currentLEDMode = LED_IDLE;
            break;
            
        case WStype_TEXT:
            Serial.printf("📥 Received: %d bytes\n", length);
            handleWebSocketMessage(payload, length);
            break;
            
        case WStype_BIN:
            {
                // Ignore audio if response was interrupted
                if (responseInterrupted) {
                    Serial.println("🚫 Discarding audio chunk (response was interrupted)");
                    break;
                }
                
                // Raw PCM audio data from server (16-bit mono samples)
                // Play immediately with minimal processing
                if (!isPlayingResponse) {
                    isPlayingResponse = true;
                    recordingActive = false;  // Ensure recording is stopped
                    currentLEDMode = LED_AUDIO_REACTIVE;
                    firstAudioChunk = true;
                    // Clear all LEDs immediately when starting playback
                    fill_solid(leds, NUM_LEDS, CRGB::Black);
                    FastLED.show();
                    // Clear delay buffer for clean LED sync
                    for (int i = 0; i < AUDIO_DELAY_BUFFER_SIZE; i++) {
                        audioLevelBuffer[i] = 0;
                    }
                    audioBufferIndex = 0;
                    Serial.println("🔊 Starting audio playback...");
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
                i2s_write(I2S_NUM_1, stereoBuffer, numSamples * 4, &bytes_written, portMAX_DELAY);
            }
            break;
            
        case WStype_DISCONNECTED:
            Serial.println("✗ WebSocket Disconnected");
            isWebSocketConnected = false;
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
        Serial.println("📦 Setup complete");
        return;
    }
    
    // Handle turn complete
    if (doc["type"].is<const char*>() && doc["type"] == "turnComplete") {
        Serial.println("✓ Turn complete");
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
    
    // Decay smoothed level slowly when no new audio to prevent flickering
    if (currentAudioLevel == 0) {
        smoothedAudioLevel *= 0.95f;  // Gentle decay
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
            
        case LED_AUDIO_REACTIVE:
            // VU meter during playback - same green to yellow to red
            {
                // Map audio level to LED count (0-9 LEDs)
                // Playback audio range: 0-3000, map to 0-9 LEDs
                int numLEDs = map(constrain((int)smoothedAudioLevel, 0, 3000), 0, 3000, 0, NUM_LEDS);
                
                // Set ALL LEDs explicitly - no partial updates
                for (int i = 0; i < NUM_LEDS; i++) {
                    if (i < numLEDs) {
                        // Universal VU meter gradient (same as recording)
                        if (i < NUM_LEDS * 0.6) {
                            leds[i] = CRGB(0, 255, 0);  // Pure green
                        } else if (i < NUM_LEDS * 0.85) {
                            leds[i] = CRGB(255, 255, 0);  // Pure yellow
                        } else {
                            leds[i] = CRGB(255, 0, 0);  // Pure red
                        }
                    } else {
                        leds[i] = CRGB(0, 0, 0);  // Pure black (completely off)
                    }
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
    while(1) {
        webSocket.loop();
        vTaskDelay(10 / portTICK_PERIOD_MS);
    }
}

void ledTask(void * parameter) {
    while(1) {
        updateLEDs();
        FastLED.show();
        vTaskDelay(30 / portTICK_PERIOD_MS);  // 33Hz update rate - matches audio chunk timing better
    }
}
