#pragma once

#include <Arduino.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>
#include <FastLED.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/queue.h>
#include <driver/i2s.h>
#include <time.h>
#include "Config.h"
#include "types.h"

// ── Globals defined in main.cpp that handleWebSocketMessage accesses ──
extern WebSocketsClient        webSocket;
extern CRGB                    leds[];
extern SemaphoreHandle_t       ledMutex;
extern SemaphoreHandle_t       wsSendMutex;
extern SemaphoreHandle_t       i2sSpeakerMutex;

extern volatile LEDMode    currentLEDMode;

extern volatile bool turnComplete;
extern volatile bool responseInterrupted;
extern bool     firstAudioChunk;
extern bool     isPlayingAlarm;
extern volatile bool  recordingActive;
extern volatile bool  isPlayingResponse;
extern volatile bool  isPlayingAmbient;
extern volatile float volumeMultiplier;
extern volatile uint32_t lastAudioChunkTime;
extern ConvState convState;
extern uint32_t waitingEnteredAt;
extern QueueHandle_t audioOutputQueue;

extern TideState       tideState;
extern DayNightData    dayNightData;
extern TimerState      timerState;
extern AlarmState      alarmState;
extern Alarm           alarms[];
extern MoonState       moonState;
extern AmbientSound    ambientSound;
extern MeditationState meditationState;
extern PomodoroState   pomodoroState;
extern LampState          lampState;
extern AmbientSoundType   currentAmbientSoundType;
extern RadioState         radioState;

// Chakra names (defined in main.cpp)
extern const char* CHAKRA_NAMES[];

// ── Helper functions defined in main.cpp ──
void playVolumeChime();
void updateDayNightBrightness();
void playShutdownSound();

// ── Function declarations ──
void handleWebSocketMessage(uint8_t* payload, size_t length);

// Drain audioOutputQueue, zero I2S DMA, and set a drain window.
// Call after sending stopAmbient to prevent audio tail on voice-commanded stops.
// windowMs: how long to suppress incoming audio (default 500ms, use 2000ms after radio).
void drainAudioAndSilence(uint32_t windowMs = 500);

// Mutex-guarded i2s_zero_dma_buffer — prevents DMA race with audioTask's i2s_write.
// Use instead of bare i2s_zero_dma_buffer(I2S_NUM_1) everywhere outside audioTask.
static inline void i2sZeroSafe() {
    if (i2sSpeakerMutex && xSemaphoreTake(i2sSpeakerMutex, pdMS_TO_TICKS(200)) == pdTRUE) {
        i2s_zero_dma_buffer(I2S_NUM_1);
        xSemaphoreGive(i2sSpeakerMutex);
    }
}

// Central ConvState transition function — sets universally-required entry actions.
// Callers still set LEDs and other context-specific flags after calling this.
void transitionConvState(ConvState newState);

// Safe WebSocket send wrapper — logs on failure, returns sendTXT result.
bool wsSendMessage(const String& msg);

// Alarm persistence to NVS (defined in main.cpp)
void saveAlarmsToNVS();
