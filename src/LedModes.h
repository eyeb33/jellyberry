#pragma once

#include <Arduino.h>
#include <FastLED.h>
#include "Config.h"
#include "types.h"

// ── Globals defined in main.cpp that LED mode renderers read ──
extern CRGB                    leds[];
extern volatile LEDMode        currentLEDMode;
extern volatile float          smoothedAudioLevel;
extern volatile int32_t        currentAudioLevel;
extern volatile int32_t        ambientMicRows;
extern volatile bool           conversationMode;
extern volatile bool           isPlayingAmbient;
extern uint32_t                conversationWindowStart;
extern AmbientSoundType        currentAmbientSoundType;
extern TideState               tideState;
extern TimerState              timerState;
extern MoonState               moonState;
extern RadioState              radioState;
extern PomodoroState           pomodoroState;
extern MeditationState         meditationState;
extern LampState               lampState;
extern AlarmState              alarmState;
extern const char*             CHAKRA_NAMES[];

// ── Shared VU-meter helper ──
// style: 0 = recording (green→yellow→red), 1 = audio-reactive (blue→cyan→magenta)
//        2 = ambient-VU  (green→yellow→red), 3 = radio teal
void renderVUMeter(CRGB* leds, int numRows, uint8_t style);

// ── Per-mode render functions ──
void renderLedBoot(CRGB* leds);
void renderLedIdle(CRGB* leds);
void renderLedRecording(CRGB* leds);
void renderLedProcessing(CRGB* leds);
void renderLedReconnecting(CRGB* leds);
void renderLedAmbientVU(CRGB* leds);
void renderLedAudioReactive(CRGB* leds);
void renderLedTide(CRGB* leds);
void renderLedTimer(CRGB* leds);
void renderLedMoon(CRGB* leds);
void renderLedRadio(CRGB* leds);
void renderLedPomodoro(CRGB* leds);
void renderLedMeditation(CRGB* leds);
void renderLedLamp(CRGB* leds);
void renderLedAlarm(CRGB* leds);
void renderLedConversationWindow(CRGB* leds);
void renderLedConnected(CRGB* leds);
void renderLedError(CRGB* leds);

// ── Ambient sound renderer ──
// Handles Rain, Ocean, Rainforest, and Fire sub-modes.  State is encapsulated
// in the class so that switching between sound types resets each mode cleanly,
// and the static data is not scattered across updateLEDs() local scopes.
class AmbientLedRenderer {
public:
    void render(CRGB* leds, int ledCount);

private:
    void renderRain(CRGB* leds, int ledCount);
    void renderOcean(CRGB* leds, int ledCount);
    void renderRainforest(CRGB* leds, int ledCount);
    void renderFire(CRGB* leds, int ledCount);

    AmbientSoundType lastType = (AmbientSoundType)-1;

    // Rain
    bool     rainInit             = false;
    float    dropPosition[LED_COLUMNS] = {};
    float    dropSpeed[LED_COLUMNS]    = {};
    uint32_t lastDrop             = 0;

    // Ocean
    bool     oceanInit            = false;
    float    smoothedWave         = 0.0f;
    uint32_t lastOceanDebug       = 0;

    // Rainforest
    bool     rainforestInit       = false;
    float    fireflyPos[6][3]     = {};   // [i][0]=strip [i][1]=row [i][2]=brightness
    uint32_t fireflyTimers[6]     = {};
    float    eyePair[3]           = {};   // [0]=strip [1]=row [2]=expiryMs

    // Fire
    bool     fireInit             = false;
    float    flameHeights[LED_COLUMNS]    = {};
    float    flamePhases[LED_COLUMNS]     = {};
    float    sparkPositions[LED_COLUMNS]  = {};
    float    sparkBrightness[LED_COLUMNS] = {};
};

// Global instance — declared here, defined in LedModes.cpp, used in main.cpp updateLEDs()
extern AmbientLedRenderer ambientRenderer;
