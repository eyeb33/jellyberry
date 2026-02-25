#pragma once

#include <Arduino.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>
#include <FastLED.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <time.h>
#include "Config.h"
#include "types.h"

// ── Globals defined in main.cpp that handleWebSocketMessage accesses ──
extern WebSocketsClient        webSocket;
extern CRGB                    leds[];
extern SemaphoreHandle_t       ledMutex;

extern LEDMode    currentLEDMode;
extern LEDMode    targetLEDMode;

extern bool     startupSoundPlayed;
extern bool     turnComplete;
extern bool     waitingForGreeting;
extern bool     responseInterrupted;
extern bool     firstAudioChunk;
extern bool     isPlayingAlarm;
extern volatile bool  isPlayingResponse;
extern volatile bool  isPlayingAmbient;
extern volatile float volumeMultiplier;
extern uint32_t lastAudioChunkTime;
extern uint32_t processingStartTime;

extern TideState       tideState;
extern DayNightData    dayNightData;
extern TimerState      timerState;
extern AlarmState      alarmState;
extern Alarm           alarms[];
extern MoonState       moonState;
extern AmbientSound    ambientSound;
extern MeditationState meditationState;
extern PomodoroState   pomodoroState;

// Chakra names (defined in main.cpp)
extern const char* CHAKRA_NAMES[];

// ── Helper functions defined in main.cpp ──
void playStartupSound();
void playVolumeChime();
void updateDayNightBrightness();
void startMarquee(String text, CRGB color, LEDMode returnMode);
void playShutdownSound();

// ── Function declaration ──
void handleWebSocketMessage(uint8_t* payload, size_t length);
