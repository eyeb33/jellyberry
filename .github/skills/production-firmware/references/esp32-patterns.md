# ESP32-S3 / FreeRTOS Common Patterns

Quick reference for production-ready ESP32-S3/FreeRTOS implementations.

## ESP32-S3 Specific Notes

- **Dual-core Xtensa LX7**: Core 0 (protocol/WiFi), Core 1 (application) - use `xTaskCreatePinnedToCore`
- **PSRAM support**: Up to 8MB external PSRAM for large buffers (use `heap_caps_malloc(size, MALLOC_CAP_SPIRAM)`)
- **USB-OTG**: Native USB support for CDC, MSC, HID without external chip
- **I2S improvements**: Parallel I2S for LCD displays, improved audio capabilities
- **Security features**: Secure boot, flash encryption, digital signature peripheral
- **Extended GPIO**: 45 GPIO pins (vs 34 on ESP32)

## Task Creation

```cpp
// ✅ Production pattern with stack monitoring
TaskHandle_t audioTaskHandle = NULL;
xTaskCreatePinnedToCore(
    audioTask,                  // Function
    "audioTask",                // Name (for debugging)
    40960,                      // Stack size (bytes) - liberal for audio buffers
    NULL,                       // Parameters
    2,                          // Priority (1=low, 2=normal, 3=high)
    &audioTaskHandle,           // Task handle for monitoring
    1                           // Pin to core 1 (0=protocol core, 1=app core)
);

// Monitor stack usage periodically
void monitorStacks() {
    if (audioTaskHandle != NULL) {
        UBaseType_t audioStack = uxTaskGetStackHighWaterMark(audioTaskHandle);
        Serial.printf("[STACK] audioTask: %u words free (%u bytes)\n", 
                      audioStack, audioStack * 4);
        if (audioStack < 512) {  // <2KB warning threshold
            Serial.println("[STACK] WARNING: audioTask critically low!");
        }
    }
}
```

## Mutex Patterns

```cpp
// ✅ Create mutex in global scope
SemaphoreHandle_t dataMutex = NULL;

void setup() {
    dataMutex = xSemaphoreCreateMutex();
    if (dataMutex == NULL) {
        Serial.println("FATAL: Failed to create mutex");
        while(1) { delay(1000); }  // Halt on critical failure
    }
}

// ✅ Use mutex with timeout and recovery
bool accessSharedData() {
    if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        Serial.println("CRITICAL: dataMutex deadlock - rebooting");
        esp_restart();  // Nuclear option: reboot to recover
        return false;   // Never reached
    }
    
    // Critical section - keep SHORT
    sharedData.value = calculateNewValue();
    
    xSemaphoreGive(dataMutex);
    return true;
}

// ❌ NEVER do this (deadlock risk)
xSemaphoreTake(mutex1, portMAX_DELAY);
xSemaphoreTake(mutex2, portMAX_DELAY);  // If another task has mutex2->mutex1, DEADLOCK
```

## Queue Patterns

```cpp
// ✅ Create queue with appropriate size
QueueHandle_t audioQueue = NULL;

void setup() {
    audioQueue = xQueueCreate(30, sizeof(AudioChunk));  // 30 packets = ~1.2s buffer
    if (audioQueue == NULL) {
        Serial.println("FATAL: Failed to create audio queue");
        while(1) { delay(1000); }
    }
}

// ✅ Producer: use blocking send with backpressure
bool sendAudioChunk(AudioChunk* chunk) {
    static uint32_t consecutiveDrops = 0;
    
    if (xQueueSend(audioQueue, chunk, pdMS_TO_TICKS(100)) != pdTRUE) {
        consecutiveDrops++;
        
        if (consecutiveDrops > 20) {
            Serial.println("CRITICAL: Queue blocked - consumer stalled");
            currentLEDMode = LED_ERROR;
            
            // Drain queue to recover
            AudioChunk dummy;
            while (xQueueReceive(audioQueue, &dummy, 0) == pdTRUE) {
                // Discard stale packets
            }
            consecutiveDrops = 0;
        }
        return false;
    }
    
    consecutiveDrops = 0;  // Reset on success
    return true;
}

// ✅ Consumer: use blocking receive with timeout
void audioTask(void* param) {
    AudioChunk chunk;
    while(1) {
        if (xQueueReceive(audioQueue, &chunk, pdMS_TO_TICKS(1000)) == pdTRUE) {
            // Process chunk
            processAudio(&chunk);
        } else {
            // Timeout - queue empty for 1 second
            esp_task_wdt_reset();  // Feed watchdog during idle
        }
    }
}
```

## I2S Audio Patterns

```cpp
// ✅ I2S configuration with DMA
i2s_config_t i2s_config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate = 16000,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 8,                    // Number of DMA buffers
    .dma_buf_len = 64,                     // Samples per buffer
    .use_apll = false,
    .tx_desc_auto_clear = false,
    .fixed_mclk = 0
};

// ✅ Install driver with error handling
esp_err_t err = i2s_driver_install(I2S_NUM_0, &i2s_config, 0, NULL);
if (err != ESP_OK) {
    Serial.printf("I2S driver install failed: %d\n", err);
    return false;
}

// ✅ Read with error handling and recovery
static uint32_t i2sErrorCount = 0;
size_t bytes_read = 0;

esp_err_t err = i2s_read(I2S_NUM_0, buffer, bufferSize, 
                         &bytes_read, pdMS_TO_TICKS(100));

if (err != ESP_OK || bytes_read == 0) {
    i2sErrorCount++;
    
    if (i2sErrorCount > 50) {
        Serial.println("I2S persistent failure - reinstalling driver");
        i2s_driver_uninstall(I2S_NUM_0);
        delay(100);
        i2s_driver_install(I2S_NUM_0, &i2s_config, 0, NULL);
        i2sErrorCount = 0;
    }
    return 0;  // Return 0 bytes read
}

i2sErrorCount = 0;  // Reset on success
return bytes_read;
```

## WiFi Connection Patterns

```cpp
// ✅ WiFi connection with watchdog feeding
bool connectWiFi(const char* ssid, const char* password) {
    WiFi.begin(ssid, password);
    
    int retries = 0;
    const int MAX_RETRIES = 20;  // 20 * 500ms = 10 seconds
    
    while (WiFi.status() != WL_CONNECTED && retries < MAX_RETRIES) {
        delay(500);
        Serial.print(".");
        retries++;
        
        esp_task_wdt_reset();  // Feed watchdog during connection attempts
        
        if (retries % 10 == 0) {
            Serial.printf("\n[WiFi] Attempt %d/%d...\n", retries, MAX_RETRIES);
        }
    }
    
    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("\n[WiFi] Connected: %s\n", WiFi.localIP().toString().c_str());
        return true;
    }
    
    Serial.println("\n[WiFi] Connection failed - will retry");
    return false;
}

// ✅ Reconnection with state recovery
void handleWiFiDisconnect() {
    Serial.println("[WiFi] Disconnected - attempting reconnection");
    
    // Preserve critical state before reconnect
    bool wasPlaying = isPlayingAudio;
    LEDMode savedMode = currentLEDMode;
    
    // Attempt reconnection
    if (connectWiFi(WIFI_SSID, WIFI_PASSWORD)) {
        // Restore after successful reconnection
        if (wasPlaying) {
            Serial.println("[WiFi] Resuming audio after reconnect");
            resumeAudio();
        }
    } else {
        // Enter error state
        currentLEDMode = LED_ERROR;
    }
}
```

## Watchdog Patterns

```cpp
// ✅ Initialize watchdog in setup()
void setup() {
    // 30-second timeout, panic on expiry
    esp_task_wdt_init(30, true);
    esp_task_wdt_add(NULL);  // Subscribe main task
    
    Serial.println("Watchdog initialized: 30s timeout");
}

// ✅ Feed watchdog in main loop
void loop() {
    esp_task_wdt_reset();  // Feed at start of loop iteration
    
    // Your loop code here...
    
    // Feed again before long operations
    if (needsLongOperation()) {
        esp_task_wdt_reset();
        performLongOperation();
    }
}

// ✅ Feed watchdog in task loops
void audioTask(void* param) {
    while(1) {
        // Process audio
        AudioChunk chunk;
        if (xQueueReceive(audioQueue, &chunk, pdMS_TO_TICKS(1000)) == pdTRUE) {
            processAudio(&chunk);
        }
        
        esp_task_wdt_reset();  // Feed at end of iteration
    }
}
```

## Memory Management

```cpp
// ✅ Check available heap before allocation
void allocateLargeBuffer() {
    size_t freeHeap = esp_get_free_heap_size();
    size_t requiredSize = 8192;
    
    if (freeHeap < requiredSize + 10000) {  // Keep 10KB safety margin
        Serial.printf("Insufficient heap: %u bytes (need %u)\n", 
                      freeHeap, requiredSize);
        return;
    }
    
    uint8_t* buffer = (uint8_t*)malloc(requiredSize);
    if (buffer == NULL) {
        Serial.println("Malloc failed despite sufficient heap!");
        return;
    }
    
    // Use buffer...
    
    free(buffer);
}

// ✅ Monitor heap fragmentation
void printHeapStats() {
    Serial.printf("[HEAP] Free: %u bytes, Largest block: %u bytes, Min free: %u bytes\n",
                  esp_get_free_heap_size(),
                  heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
                  esp_get_minimum_free_heap_size());
}

// ✅ Use heap_caps_malloc for large buffers in tasks
void audioTask(void* param) {
    int16_t* stereoBuffer = (int16_t*)heap_caps_malloc(
        1920 * sizeof(int16_t), 
        MALLOC_CAP_8BIT  // 8-bit capable memory (allows DMA)
    );
    
    if (!stereoBuffer) {
        Serial.println("FATAL: Failed to allocate stereoBuffer");
        vTaskDelete(NULL);
        return;
    }
    
    // Task loop...
    
    free(stereoBuffer);  // Cleanup (never reached for infinite loop tasks)
}
```

## Time Handling

```cpp
// ✅ millis() wraparound-safe comparisons
uint32_t lastEventTime = 0;

void checkTimeout() {
    const uint32_t TIMEOUT_MS = 5000;
    
    // Cast to int32_t to handle wraparound at 49 days
    if ((int32_t)(millis() - lastEventTime) > TIMEOUT_MS) {
        Serial.println("Timeout occurred");
        handleTimeout();
        lastEventTime = millis();
    }
}

// ✅ Rate-limiting pattern
void rateLimitedLog(const char* message) {
    static uint32_t lastLogTime = 0;
    const uint32_t MIN_INTERVAL_MS = 2000;  // Max once per 2 seconds
    
    if ((int32_t)(millis() - lastLogTime) > MIN_INTERVAL_MS) {
        Serial.println(message);
        lastLogTime = millis();
    }
}

// ✅ Periodic execution pattern
void periodicTask() {
    static uint32_t lastExecutionTime = 0;
    const uint32_t INTERVAL_MS = 60000;  // Once per minute
    
    if ((int32_t)(millis() - lastExecutionTime) > INTERVAL_MS) {
        performPeriodicWork();
        lastExecutionTime = millis();
    }
}
```

## Error Handling

```cpp
// ✅ Comprehensive error handling with recovery
bool sendWebSocketMessage(const char* message) {
    if (!isConnected()) {
        Serial.println("[WS] Not connected - buffering message");
        return bufferMessage(message);
    }
    
    if (strlen(message) > MAX_MESSAGE_SIZE) {
        Serial.printf("[WS] Message too large: %u bytes (max %u)\n",
                      strlen(message), MAX_MESSAGE_SIZE);
        return false;
    }
    
    bool success = webSocket.sendTXT(message);
    
    if (!success) {
        wsErrorCount++;
        Serial.printf("[WS] Send failed (error count: %u)\n", wsErrorCount);
        
        if (wsErrorCount > 10) {
            Serial.println("[WS] Persistent errors - reconnecting");
            reconnectWebSocket();
            wsErrorCount = 0;
        }
    } else {
        wsErrorCount = 0;  // Reset on success
    }
    
    return success;
}

// ✅ Retry pattern with exponential backoff
bool retryWithBackoff(bool (*operation)(), int maxRetries) {
    for (int attempt = 0; attempt < maxRetries; attempt++) {
        if (operation()) {
            return true;  // Success
        }
        
        uint32_t backoffMs = 100 * (1 << attempt);  // 100, 200, 400, 800...
        Serial.printf("Retry %d/%d after %ums\n", attempt + 1, maxRetries, backoffMs);
        
        delay(backoffMs);
        esp_task_wdt_reset();  // Feed watchdog during retries
    }
    
    Serial.println("Operation failed after all retries");
    return false;
}
```

## Debugging Patterns

```cpp
// ✅ Conditional debug logging (compile-time)
#ifdef DEBUG_VERBOSE
  #define DEBUG_LOG(fmt, ...) Serial.printf("[DEBUG] " fmt "\n", ##__VA_ARGS__)
#else
  #define DEBUG_LOG(fmt, ...) do {} while(0)  // No-op in release
#endif

void processData() {
    DEBUG_LOG("Processing data: size=%u", dataSize);  // Only in debug builds
    // ...
}

// ✅ Task monitoring dashboard
void printSystemStatus() {
    Serial.println("\n=== SYSTEM STATUS ===");
    
    // Uptime
    uint32_t uptimeSeconds = millis() / 1000;
    Serial.printf("Uptime: %02u:%02u:%02u\n",
                  uptimeSeconds / 3600,
                  (uptimeSeconds % 3600) / 60,
                  uptimeSeconds % 60);
    
    // Memory
    Serial.printf("Heap: %u bytes free (min: %u bytes)\n",
                  esp_get_free_heap_size(),
                  esp_get_minimum_free_heap_size());
    
    // Task stacks
    if (audioTaskHandle != NULL) {
        UBaseType_t stack = uxTaskGetStackHighWaterMark(audioTaskHandle);
        Serial.printf("audioTask stack: %u bytes free\n", stack * 4);
    }
    
    // Queue depths
    uint32_t queueDepth = uxQueueMessagesWaiting(audioQueue);
    uint32_t queueSpaces = uxQueueSpacesAvailable(audioQueue);
    Serial.printf("Audio queue: %u/%u packets\n", queueDepth, queueDepth + queueSpaces);
    
    // Network
    Serial.printf("WiFi: %s (%s)\n",
                  WiFi.status() == WL_CONNECTED ? "Connected" : "Disconnected",
                  WiFi.localIP().toString().c_str());
    Serial.printf("WebSocket: %s\n",
                  isWebSocketConnected ? "Connected" : "Disconnected");
    
    Serial.println("====================\n");
}
```

## Anti-Patterns to Avoid

### ❌ Delays in ISRs
```cpp
// NEVER do this
void IRAM_ATTR buttonISR() {
    delay(50);  // FATAL: Blocks all interrupts!
    handleButton();
}

// ✅ Use flag and debounce in main loop
volatile bool buttonPressed = false;

void IRAM_ATTR buttonISR() {
    buttonPressed = true;  // Set flag only
}

void loop() {
    if (buttonPressed) {
        buttonPressed = false;
        delay(50);  // Debounce in main loop is fine
        if (digitalRead(BUTTON_PIN) == LOW) {
            handleButton();
        }
    }
}
```

### ❌ Serial.print in ISRs
```cpp
// NEVER do this
void IRAM_ATTR timerISR() {
    Serial.println("Timer fired");  // FATAL: Serial not ISR-safe!
}

// ✅ Use xQueueSendFromISR or set flag
void IRAM_ATTR timerISR() {
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    xQueueSendFromISR(eventQueue, &event, &xHigherPriorityTaskWoken);
    portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
}
```

### ❌ Float operations in ISRs
```cpp
// Avoid if possible (slow in ISR context)
void IRAM_ATTR adcISR() {
    float voltage = analogRead(PIN) * 3.3 / 4095.0;  // Heavy computation in ISR
}

// ✅ Defer computation to task
void IRAM_ATTR adcISR() {
    uint16_t rawValue = analogRead(PIN);
    xQueueSendFromISR(adcQueue, &rawValue, NULL);
}
```

### ❌ Malloc/free in ISRs
```cpp
// NEVER do this
void IRAM_ATTR dataISR() {
    uint8_t* buffer = (uint8_t*)malloc(256);  // FATAL: Non-deterministic!
    // ...
    free(buffer);
}

// ✅ Use pre-allocated ring buffer
uint8_t ringBuffer[256];
volatile uint8_t ringHead = 0;

void IRAM_ATTR dataISR() {
    ringBuffer[ringHead++] = getData();
    if (ringHead >= 256) ringHead = 0;
}
```
