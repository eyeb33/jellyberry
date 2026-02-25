#pragma once

#include "Config.h"
#include <Arduino.h>
#include <FastLED.h>
#include <time.h>

#define MAX_ALARMS 10

struct AudioChunk {
    uint8_t data[2048];
    size_t length;
};

enum LEDMode { LED_BOOT, LED_IDLE, LED_RECORDING, LED_PROCESSING, LED_AUDIO_REACTIVE, LED_CONNECTED, LED_ERROR, LED_TIDE, LED_TIMER, LED_MOON, LED_AMBIENT_VU, LED_AMBIENT, LED_POMODORO, LED_MEDITATION, LED_CLOCK, LED_LAMP, LED_SEA_GOOSEBERRY, LED_EYES, LED_ALARM, LED_CONVERSATION_WINDOW, LED_MARQUEE };

enum AmbientSoundType { SOUND_RAIN, SOUND_OCEAN, SOUND_RAINFOREST, SOUND_FIRE };

// Tide visualization state
struct TideState {
    String state;  // "flooding" or "ebbing"
    float waterLevel;  // 0.0 to 1.0
    int nextChangeMinutes;
    uint32_t displayStartTime;
    bool active;
};

// Timer visualization state
struct TimerState {
    int totalSeconds;
    uint32_t startTime;
    bool active;
};

// Moon phase visualization state
struct MoonState {
    String phaseName;
    int illumination;  // 0-100%
    float moonAge;
    uint32_t displayStartTime;
    bool active;
};

// Ambient sound state
struct AmbientSound {
    String name;  // "rain", "ocean", "rainforest", "fire"
    bool active;
    uint16_t sequence;  // Increments each time we request a new sound
    uint16_t discardedCount;  // Count discarded chunks to reduce log spam
    uint32_t drainUntil;  // Timestamp until which we silently drain stale packets
};

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
};

// Meditation mode state
struct MeditationState {
    enum Chakra { ROOT, SACRAL, SOLAR, HEART, THROAT, THIRD_EYE, CROWN };
    enum BreathPhase { INHALE, HOLD_TOP, EXHALE, HOLD_BOTTOM };
    Chakra currentChakra;
    BreathPhase phase;
    uint32_t phaseStartTime;
    bool active;             // Meditation mode active
    bool streaming;          // Currently streaming meditation audio
    float savedVolume;       // User's volume before meditation
};

// Clock display state
struct ClockState {
    int lastHour;            // Last displayed hour
    int lastMinute;          // Last displayed minute
    int scrollPosition;      // Horizontal scroll position for rotating display
    uint32_t lastScrollUpdate; // Last time scroll position updated
    bool active;             // Clock mode active
};

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
};

// Alarm
struct Alarm {
    uint32_t alarmID;        // Unique ID from server
    time_t triggerTime;      // Unix timestamp when alarm should trigger
    bool enabled;            // Alarm is active
    bool triggered;          // Alarm has been triggered (prevent re-trigger)
    bool snoozed;            // Currently snoozed
    time_t snoozeUntil;      // Wake up from snooze at this time
};

struct AlarmState {
    bool ringing;            // Alarm currently ringing
    uint32_t ringStartTime;  // When alarm started ringing
    uint32_t pulseStartTime; // For LED animation timing
    float pulseRadius;       // Current radius for center-outward pulse
    bool active;             // Alarm system active
    LEDMode previousMode;    // Mode before alarm triggered (to restore after dismissal)
    bool wasRecording;       // If recording was active when alarm triggered
    bool wasPlayingResponse; // If playing response when alarm triggered
};

// Day/Night brightness control
struct DayNightData {
    bool valid;            // Have we received sunrise/sunset times?
    uint32_t sunriseTime;  // Unix timestamp of today's sunrise
    uint32_t sunsetTime;   // Unix timestamp of today's sunset
    uint32_t lastUpdate;   // When we last received this data
    bool isDaytime;        // Current day/night state
    uint8_t currentBrightness; // Active brightness level
};
