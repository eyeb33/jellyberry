#include "LedModes.h"

// Global instance of the ambient renderer (owns all ambient animation state)
AmbientLedRenderer ambientRenderer;

// ============================================================
// Shared VU-meter helper
// ============================================================
void renderVUMeter(CRGB* leds, int numRows, uint8_t style) {
    // Fade all LEDs for trail effect
    for (int i = 0; i < NUM_LEDS; i++) {
        leds[i].fadeToBlackBy(80);
    }
    for (int col = 0; col < LED_COLUMNS; col++) {
        for (int row = 0; row < LEDS_PER_COLUMN; row++) {
            int idx = col * LEDS_PER_COLUMN + row;
            if (idx >= NUM_LEDS) continue;
            if (row < numRows) {
                float progress = (float)row / (float)LEDS_PER_COLUMN;
                switch (style) {
                    case 0: // Recording  green→yellow→red
                    case 2: // Ambient VU (same palette)
                        if      (progress < 0.50f) leds[idx] = CRGB(0,   255, 0);
                        else if (progress < 0.83f) leds[idx] = CRGB(255, 255, 0);
                        else                       leds[idx] = CRGB(255, 0,   0);
                        break;
                    case 1: // Audio-reactive  blue→cyan→magenta
                        if      (progress < 0.50f) leds[idx] = CRGB(0,   100, 200);
                        else if (progress < 0.83f) leds[idx] = CRGB(0,   255, 150);
                        else                       leds[idx] = CRGB(200, 0,   255);
                        break;
                    case 3: // Radio teal
                    {
                        uint8_t g = (uint8_t)(progress * 200);
                        uint8_t b = (uint8_t)(100 + progress * 155);
                        leds[idx] = CRGB(0, g, b);
                        break;
                    }
                }
            }
        }
    }
}

// ============================================================
// LED_BOOT
// ============================================================
void renderLedBoot(CRGB* leds) {
    static uint32_t lastDebug = 0;
    if (millis() - lastDebug > 1000) {
        Serial.println("LED_BOOT: Orange pulsing (connecting...)");
        lastDebug = millis();
    }
    uint8_t b = constrain(100 + (int)(50 * sin(millis() / 500.0)), 0, 255);
    fill_solid(leds, NUM_LEDS, CHSV(25, 255, b));
}

// ============================================================
// LED_IDLE
// ============================================================
void renderLedIdle(CRGB* leds) {
    // 5.9-second bouncing wave, all strips in sync
    float t = (millis() % 5866) / 5866.0f;
    float wavePos;
    if (t < 0.5f) {
        wavePos = (t * 2.0f) * 16.0f - 2.5f;
    } else {
        wavePos = ((1.0f - t) * 2.0f) * 16.0f - 2.5f;
    }

    static uint32_t lastDebug = 0;
    if (millis() - lastDebug > 2000) {
        Serial.printf("IDLE: t=%.2f, wavePos=%.2f, hue=160 (blue)\n", t, wavePos);
        lastDebug = millis();
    }

    for (int col = 0; col < LED_COLUMNS; col++) {
        for (int row = 0; row < LEDS_PER_COLUMN; row++) {
            int idx = col * LEDS_PER_COLUMN + row;
            float dist = abs(wavePos - row);
            if (dist < IDLE_WAVE_SPREAD) {
                float wb = 1.0f - (dist / IDLE_WAVE_SPREAD);
                wb = wb * wb;
                uint8_t b = (uint8_t)(wb * (IDLE_WAVE_BRIGHTNESS_MAX - IDLE_WAVE_BRIGHTNESS_MIN)
                                      + IDLE_WAVE_BRIGHTNESS_MIN);
                leds[idx] = CHSV(160, 200, b);
            } else {
                leds[idx] = CHSV(160, 200, IDLE_WAVE_BRIGHTNESS_MIN);
            }
        }
    }
}

// ============================================================
// LED_RECORDING  (VU meter, green→yellow→red)
// ============================================================
void renderLedRecording(CRGB* leds) {
    int levelAboveNoise = max(0, (int)smoothedAudioLevel - RECORDING_NOISE_FLOOR);
    int numRows = map(constrain(levelAboveNoise, 0, RECORDING_LEVEL_MAX), 0, RECORDING_LEVEL_MAX, 0, LEDS_PER_COLUMN);
    renderVUMeter(leds, numRows, 0);
}

// ============================================================
// LED_PROCESSING
// ============================================================
void renderLedProcessing(CRGB* leds) {
    fill_solid(leds, NUM_LEDS, CRGB::Black);
}

// ============================================================
// LED_AMBIENT_VU  (green→yellow→red using mic rows computed by audioTask)
// ============================================================
void renderLedAmbientVU(CRGB* leds) {
    renderVUMeter(leds, ambientMicRows, 2);
}

// ============================================================
// LED_AUDIO_REACTIVE  (blue→cyan→magenta)
// ============================================================
void renderLedAudioReactive(CRGB* leds) {
    int numRows = map(constrain((int)smoothedAudioLevel, 0, AUDIO_REACTIVE_LEVEL_MAX), 0, AUDIO_REACTIVE_LEVEL_MAX, 0, LEDS_PER_COLUMN);
    renderVUMeter(leds, numRows, 1);
}

// ============================================================
// LED_TIDE
// ============================================================
void renderLedTide(CRGB* leds) {
    static uint32_t lastDebug = 0;
    if (millis() - lastDebug > 5000) {
        Serial.printf("LED_TIDE active: state=%s, level=%.2f, mode=%d\n",
                      tideState.state, tideState.waterLevel, currentLEDMode);
        lastDebug = millis();
    }

    int baseRows   = max(1, (int)(tideState.waterLevel * LEDS_PER_COLUMN));
    CRGB tideColor = strcmp(tideState.state, "flooding") == 0
                   ? CRGB(0, 100, 255) : CRGB(255, 100, 0);
    float time = millis() / 1000.0f;

    for (int col = 0; col < LED_COLUMNS; col++) {
        float phaseOffset = (float)col / (float)LED_COLUMNS * TWO_PI;
        float wave        = sin(time * 1.5f + phaseOffset) * 2.0f;
        int waterRows     = constrain(baseRows + (int)wave, 0, LEDS_PER_COLUMN);

        for (int row = 0; row < LEDS_PER_COLUMN; row++) {
            int idx = col * LEDS_PER_COLUMN + row;
            if (idx >= NUM_LEDS) continue;
            if (row < waterRows) {
                float shimmer = 0.7f + 0.3f * sin(time * 3.0f + phaseOffset * 2.0f);
                leds[idx] = CRGB((uint8_t)(tideColor.r * shimmer),
                                 (uint8_t)(tideColor.g * shimmer),
                                 (uint8_t)(tideColor.b * shimmer));
            } else {
                leds[idx] = CRGB::Black;
            }
        }
    }
}

// ============================================================
// LED_TIMER
// ============================================================
void renderLedTimer(CRGB* leds) {
    if (!timerState.active) {
        fill_solid(leds, NUM_LEDS, CRGB::Black);
        return;
    }

    uint32_t elapsed  = (millis() - timerState.startTime) / 1000;
    int remaining = timerState.totalSeconds - (int)elapsed;

    if (remaining <= 0) {
        fill_solid(leds, NUM_LEDS, CRGB::Black);
        return;
    }

    float progress     = (float)remaining / (float)timerState.totalSeconds;
    float exactLEDs    = progress * NUM_LEDS;
    int numLEDs        = (int)exactLEDs;
    float frac         = exactLEDs - numLEDs;

    uint8_t hue;
    if      (progress > 0.66f) hue = 96;
    else if (progress > 0.33f) hue = 64;
    else if (progress > 0.15f) hue = 32;
    else                       hue = 0;

    uint8_t baseBrightness = 255;
    if (progress < 0.15f) {
        baseBrightness = 128 + (uint8_t)(127 * sin(millis() / 200.0));
    }

    for (int i = 0; i < NUM_LEDS; i++) {
        if (i < numLEDs) {
            leds[i] = CHSV(hue, 255, baseBrightness);
        } else if (i == numLEDs && frac > 0) {
            leds[i] = CHSV(hue, 255, (uint8_t)(baseBrightness * frac));
        } else {
            leds[i] = CRGB::Black;
        }
    }
}

// ============================================================
// LED_MOON
// ============================================================
void renderLedMoon(CRGB* leds) {
    if (!moonState.active) {
        fill_solid(leds, NUM_LEDS, CRGB::Black);
        return;
    }

    float pulse         = 0.85f + 0.15f * sin(millis() / 1500.0f);
    uint8_t baseBr      = (uint8_t)(220 * pulse);
    int numColumns      = max(1, (int)((moonState.illumination / 100.0f) * LED_COLUMNS));
    int centerCol       = LED_COLUMNS / 2;
    int leftMost        = centerCol - (numColumns / 2);
    int rightMost       = leftMost + numColumns - 1;

    fill_solid(leds, NUM_LEDS, CRGB::Black);
    for (int col = 0; col < LED_COLUMNS; col++) {
        if (col < leftMost || col > rightMost) continue;
        int dist          = abs(col - centerCol);
        float brightFac   = 1.0f - (dist / (float)LED_COLUMNS * 0.3f);
        uint8_t colBr     = (uint8_t)(baseBr * brightFac);
        for (int row = 0; row < LEDS_PER_COLUMN; row++) {
            int idx = col * LEDS_PER_COLUMN + row;
            if (idx < NUM_LEDS) leds[idx] = CHSV(160, 80, colBr);
        }
    }
}

// ============================================================
// LED_RADIO
// ============================================================
void renderLedRadio(CRGB* leds) {
    if (!radioState.streaming) {
        // Discovery mode: slow teal sine pulse
        static float phase = 0.0f;
        phase += 0.004f;
        if (phase > TWO_PI) phase -= TWO_PI;
        float b = 0.30f + 0.15f * sinf(phase);
        uint8_t bv = (uint8_t)(b * 255);
        fill_solid(leds, NUM_LEDS, CRGB(0, (uint8_t)(bv * 0.7f), bv));
    } else if (radioState.isHLS && !isPlayingAmbient) {
        // HLS buffering: slow orange pulse
        static float hlsPhase = 0.0f;
        hlsPhase += 0.003f;
        if (hlsPhase > TWO_PI) hlsPhase -= TWO_PI;
        float b = 0.25f + 0.20f * sinf(hlsPhase);
        uint8_t bv = (uint8_t)(b * 255);
        fill_solid(leds, NUM_LEDS, CRGB(bv, (uint8_t)(bv * 0.5f), 0));
    } else if (!radioState.visualsActive) {
        // Visuals off: static teal 40%
        fill_solid(leds, NUM_LEDS, CRGB(0, (uint8_t)(0.4f * 180), (uint8_t)(0.4f * 255)));
    } else {
        // VU meter streaming
    int numRows = map(constrain((int)smoothedAudioLevel, 0, AUDIO_REACTIVE_LEVEL_MAX), 0, AUDIO_REACTIVE_LEVEL_MAX, 0, LEDS_PER_COLUMN);
        renderVUMeter(leds, numRows, 3);
    }
}

// ============================================================
// LED_POMODORO
// ============================================================
void renderLedPomodoro(CRGB* leds) {
    if (!pomodoroState.active) {
        fill_solid(leds, NUM_LEDS, CRGB::Black);
        return;
    }

    int secondsRemaining;
    if (pomodoroState.paused) {
        secondsRemaining = pomodoroState.pausedTime > 0 ? pomodoroState.pausedTime : pomodoroState.totalSeconds;
    } else if (pomodoroState.startTime > 0) {
        uint32_t elapsed = (millis() - pomodoroState.startTime) / 1000;
        secondsRemaining = max(0, pomodoroState.totalSeconds - (int)elapsed);
    } else {
        secondsRemaining = pomodoroState.totalSeconds;
    }

    float progress = 1.0f - ((float)secondsRemaining / (float)pomodoroState.totalSeconds);

    bool isBreak = (pomodoroState.currentSession == PomodoroState::SHORT_BREAK ||
                    pomodoroState.currentSession == PomodoroState::LONG_BREAK);

    CRGB sessionColor;
    if      (pomodoroState.currentSession == PomodoroState::FOCUS)       sessionColor = CRGB(255, 0,   0);
    else if (pomodoroState.currentSession == PomodoroState::SHORT_BREAK) sessionColor = CRGB(0,   255, 0);
    else                                                                   sessionColor = CRGB(0,   100, 255);

    int activeLED;
    if (isBreak) {
        activeLED = constrain((int)(progress * LEDS_PER_COLUMN), 0, LEDS_PER_COLUMN - 1);
    } else {
        activeLED = constrain(LEDS_PER_COLUMN - 1 - (int)(progress * LEDS_PER_COLUMN), 0, LEDS_PER_COLUMN - 1);
    }

    float activePulse;
    if (pomodoroState.paused) {
        float breathe = sin(millis() / 3000.0 * PI);
        activePulse = 0.30f + 0.70f * ((breathe + 1.0f) / 2.0f);
    } else {
        float breathe = sin(millis() / 2000.0 * PI);
        activePulse = 0.70f + 0.30f * ((breathe + 1.0f) / 2.0f);
    }

    static uint32_t lastDebug = 0;
    if (millis() - lastDebug > 5000) {
        Serial.printf("Pomodoro progress: %.1f%%, Active row: %d, Pulse: %.2f, Paused: %d, Remaining: %ds\n",
                      progress * 100, activeLED, activePulse, pomodoroState.paused, secondsRemaining);
        lastDebug = millis();
    }

    for (int col = 0; col < LED_COLUMNS; col++) {
        for (int row = 0; row < LEDS_PER_COLUMN; row++) {
            int idx = col * LEDS_PER_COLUMN + row;
            if (idx >= NUM_LEDS) continue;

            if (row > activeLED) {
                leds[idx] = CRGB::Black;
                continue;
            }

            float ledBr = pomodoroState.paused ? activePulse
                        : (row == activeLED ? activePulse : 0.10f);

            leds[idx] = CRGB((uint8_t)(sessionColor.r * ledBr),
                             (uint8_t)(sessionColor.g * ledBr),
                             (uint8_t)(sessionColor.b * ledBr));
        }
    }
}

// ============================================================
// LED_MEDITATION
// ============================================================
void renderLedMeditation(CRGB* leds) {
    if (!meditationState.active) {
        fill_solid(leds, NUM_LEDS, CRGB::Black);
        return;
    }

    const CRGB chakraColors[7] = {
        CRGB(255, 0,   0),    // ROOT
        CRGB(255, 100, 0),    // SACRAL
        CRGB(255, 200, 0),    // SOLAR
        CRGB(0,   255, 0),    // HEART
        CRGB(0,   100, 255),  // THROAT
        CRGB(75,  0,   130),  // THIRD_EYE
        CRGB(180, 0,   255)   // CROWN
    };

    CRGB currentColor = chakraColors[meditationState.currentChakra];

    static int    lastChakra          = -1;
    static CRGB   lastColor           = CRGB::Black;
    static uint32_t colorTransStart   = 0;

    if (lastChakra == -1) {
        lastChakra       = meditationState.currentChakra;
        lastColor        = currentColor;
        colorTransStart  = millis() - COLOR_TRANSITION_MS;
    }

    if (meditationState.currentChakra != lastChakra) {
        lastChakra      = meditationState.currentChakra;
        colorTransStart = millis();
        Serial.printf("Chakra changed to %s: RGB(%d,%d,%d) - starting %ds colour fade\n",
                      CHAKRA_NAMES[meditationState.currentChakra],
                      currentColor.r, currentColor.g, currentColor.b,
                      COLOR_TRANSITION_MS / 1000);
    }

    CRGB displayColor;
    uint32_t transAge = millis() - colorTransStart;
    if (transAge < (uint32_t)COLOR_TRANSITION_MS) {
        float t   = (float)transAge / COLOR_TRANSITION_MS;
        displayColor = CRGB(
            lastColor.r + (int)((currentColor.r - lastColor.r) * t),
            lastColor.g + (int)((currentColor.g - lastColor.g) * t),
            lastColor.b + (int)((currentColor.b - lastColor.b) * t));
    } else {
        displayColor = currentColor;
        lastColor    = currentColor;
    }

    if (meditationState.phaseStartTime == 0) {
        // Not yet started — show 30% static colour
        for (int i = 0; i < NUM_LEDS; i++) {
            leds[i] = CRGB((uint8_t)(displayColor.r * 77 / 255),
                           (uint8_t)(displayColor.g * 77 / 255),
                           (uint8_t)(displayColor.b * 77 / 255));
        }
        return;
    }

    const uint32_t PHASE_DURATION = MEDITATION_PHASE_DURATION_MS;
    uint32_t phaseElapsed = millis() - meditationState.phaseStartTime;

    if (phaseElapsed >= PHASE_DURATION) {
        meditationState.phase =
            (MeditationState::BreathPhase)((meditationState.phase + 1) % 4);
        meditationState.phaseStartTime = millis();
        phaseElapsed = 0;
        const char* phaseNames[] = {"INHALE", "HOLD_TOP", "EXHALE", "HOLD_BOTTOM"};
        Serial.printf("Breath phase: %s\n", phaseNames[meditationState.phase]);
    }

    float phaseProgress  = (float)phaseElapsed / PHASE_DURATION;
    float breathBrightness;
    switch (meditationState.phase) {
        case MeditationState::INHALE:
            breathBrightness = MEDITATION_BREATH_MIN +
                (MEDITATION_BREATH_MAX - MEDITATION_BREATH_MIN) * phaseProgress;
            break;
        case MeditationState::HOLD_TOP:
            breathBrightness = MEDITATION_BREATH_MAX;
            break;
        case MeditationState::EXHALE:
            breathBrightness = MEDITATION_BREATH_MAX -
                (MEDITATION_BREATH_MAX - MEDITATION_BREATH_MIN) * phaseProgress;
            break;
        default: // HOLD_BOTTOM
            breathBrightness = MEDITATION_BREATH_MIN;
            break;
    }

    float eased = (1.0f - cos(breathBrightness * PI)) / 2.0f;
    uint8_t b   = (uint8_t)(eased * 255);

    fill_solid(leds, NUM_LEDS,
               CRGB((displayColor.r * b) / 255,
                    (displayColor.g * b) / 255,
                    (displayColor.b * b) / 255));
}

// ============================================================
// LED_LAMP
// ============================================================
void renderLedLamp(CRGB* leds) {
    if (!lampState.active) {
        fill_solid(leds, NUM_LEDS, CRGB::Black);
        return;
    }

    const uint32_t FADE_DURATION_MS = 150;
    const uint32_t LED_INTERVAL_MS  = 40;
    uint32_t now = millis();

    auto getColorRGB = [](LampState::Color c) -> CRGB {
        switch (c) {
            case LampState::RED:   return CRGB(128, 0,   0);
            case LampState::GREEN: return CRGB(0,   128, 0);
            case LampState::BLUE:  return CRGB(0,   0,   128);
            default:               return CRGB(128, 128, 128);
        }
    };

    CRGB targetColor   = getColorRGB(lampState.currentColor);
    CRGB previousColor = getColorRGB(lampState.previousColor);

    if (!lampState.fullyLit && (now - lampState.lastUpdate) >= LED_INTERVAL_MS) {
        int idx = lampState.currentCol * LEDS_PER_COLUMN + lampState.currentRow;
        if (idx < NUM_LEDS) {
            lampState.ledStartTimes[idx] = now;
            lampState.lastUpdate = now;
            lampState.currentCol++;
            if (lampState.currentCol >= LED_COLUMNS) {
                lampState.currentCol = 0;
                lampState.currentRow++;
                if (lampState.currentRow >= LEDS_PER_COLUMN) {
                    lampState.fullyLit    = true;
                    lampState.transitioning = false;
                    Serial.println("Lamp fully lit");
                }
            }
        }
    }

    for (int i = 0; i < NUM_LEDS; i++) {
        if (lampState.transitioning && lampState.ledStartTimes[i] == 0) {
            leds[i] = previousColor;
        } else if (lampState.ledStartTimes[i] > 0) {
            uint32_t elapsed = now - lampState.ledStartTimes[i];
            if (elapsed < FADE_DURATION_MS) {
                float p = (float)elapsed / FADE_DURATION_MS;
                p = p * p;  // ease-in
                if (lampState.transitioning) {
                    leds[i] = CRGB(
                        previousColor.r + (int)((targetColor.r - previousColor.r) * p),
                        previousColor.g + (int)((targetColor.g - previousColor.g) * p),
                        previousColor.b + (int)((targetColor.b - previousColor.b) * p));
                } else {
                    leds[i] = CRGB((uint8_t)(targetColor.r * p),
                                   (uint8_t)(targetColor.g * p),
                                   (uint8_t)(targetColor.b * p));
                }
            } else {
                leds[i] = targetColor;
            }
        } else {
            leds[i] = CRGB::Black;
        }
    }
}

// ============================================================
// LED_ALARM
// ============================================================
void renderLedAlarm(CRGB* leds) {
    if (!alarmState.ringing) {
        fill_solid(leds, NUM_LEDS, CRGB::Black);
        return;
    }

    const uint32_t PULSE_DURATION_MS = 1500;
    const float    MAX_RADIUS        = 8.0f;
    const float    WAVE_THICKNESS    = 2.5f;

    uint32_t now     = millis();
    uint32_t elapsed = now - alarmState.pulseStartTime;
    if (elapsed >= PULSE_DURATION_MS) {
        alarmState.pulseStartTime = now;
        elapsed = 0;
    }

    float progress  = (float)elapsed / PULSE_DURATION_MS;
    alarmState.pulseRadius = progress * MAX_RADIUS;

    const float centerX = LED_COLUMNS / 2.0f;
    const float centerY = LEDS_PER_COLUMN / 2.0f;

    for (int col = 0; col < LED_COLUMNS; col++) {
        for (int row = 0; row < LEDS_PER_COLUMN; row++) {
            int idx = col * LEDS_PER_COLUMN + row;
            float dx = col - centerX;
            float dy = row - centerY;
            float dist = sqrt(dx * dx + dy * dy);
            float wf   = alarmState.pulseRadius;
            float dfw  = abs(dist - wf);

            float intensity = 0.0f;
            if (dfw < WAVE_THICKNESS) {
                intensity = 1.0f - (dfw / WAVE_THICKNESS);
                intensity = intensity * intensity;
            } else if (dist < wf) {
                intensity = 0.2f;
            }

            uint8_t green = (uint8_t)(120 * (1.0f - intensity * 0.5f));
            leds[idx] = CRGB((uint8_t)(255 * intensity),
                             (uint8_t)(green * intensity),
                             0);
        }
    }
}

// ============================================================
// LED_CONVERSATION_WINDOW
// ============================================================
void renderLedConversationWindow(CRGB* leds) {
    if (!conversationMode) {
        fill_solid(leds, NUM_LEDS, CRGB::Black);
        return;
    }

    uint32_t elapsed   = millis() - conversationWindowStart;
    uint32_t remaining = CONVERSATION_WINDOW_MS > elapsed ? CONVERSATION_WINDOW_MS - elapsed : 0;

    if (remaining == 0) {
        fill_solid(leds, NUM_LEDS, CRGB::Black);
        return;
    }

    float progress  = (float)remaining / (float)CONVERSATION_WINDOW_MS;
    int numRows     = (int)(progress * LEDS_PER_COLUMN);
    uint8_t brightness = 255;
    if (remaining < 3000) {
        float pulse = 0.5f + 0.5f * sin(millis() / 150.0f);
        brightness  = (uint8_t)(255 * pulse);
    }

    for (int col = 0; col < LED_COLUMNS; col++) {
        for (int row = 0; row < LEDS_PER_COLUMN; row++) {
            int idx = col * LEDS_PER_COLUMN + row;
            if (row < numRows) leds[idx] = CHSV(160, 200, brightness);
            else             leds[idx] = CRGB::Black;
        }
    }
}

// ============================================================
// LED_CONNECTED
// ============================================================
void renderLedConnected(CRGB* leds) {
    static uint32_t lastDebug = 0;
    if (millis() - lastDebug > 500) {
        Serial.println("LED_CONNECTED: Solid green");
        lastDebug = millis();
    }
    fill_solid(leds, NUM_LEDS, CRGB(0, 255, 0));
}

// ============================================================
// LED_ERROR
// ============================================================
void renderLedError(CRGB* leds) {
    uint8_t b = (millis() / 200) % 2 ? 255 : 50;
    fill_solid(leds, NUM_LEDS, CHSV(0, 255, b));
}

// ============================================================
// AmbientLedRenderer — Rain
// ============================================================
void AmbientLedRenderer::renderRain(CRGB* leds, int ledCount) {
    if (!rainInit) {
        for (int i = 0; i < LED_COLUMNS; i++) {
            dropPosition[i] = -1.0f;
            dropSpeed[i]    = 0.0f;
        }
        lastDrop  = millis();
        rainInit  = true;
    }

    fill_solid(leds, ledCount, CRGB::Black);

    if (millis() - lastDrop > RAIN_DROP_SPAWN_INTERVAL_MS) {
        if (random(100) < RAIN_DROP_SPAWN_CHANCE) {
            int strip = random(LED_COLUMNS);
            if (dropPosition[strip] < 0.0f) {
                dropPosition[strip] = 0.0f;
                dropSpeed[strip]    = 0.08f + (random(0, 100) / 1000.0f);
            }
        }
        lastDrop = millis();
    }

    for (int strip = 0; strip < LED_COLUMNS; strip++) {
        if (dropPosition[strip] < 0.0f) continue;

        dropPosition[strip] += dropSpeed[strip];
        if (dropPosition[strip] >= (float)LEDS_PER_COLUMN) {
            dropPosition[strip] = -1.0f;
            dropSpeed[strip]    = 0.0f;
            continue;
        }

        int cur = (int)dropPosition[strip];

        if (dropPosition[strip] < 0.5f && cur == 0) {
            leds[strip * LEDS_PER_COLUMN + cur] = CRGB(200, 220, 255);
        } else if (cur < LEDS_PER_COLUMN) {
            leds[strip * LEDS_PER_COLUMN + cur] = CHSV(160, 255, 255);
        }
        if (cur > 0 && cur - 1 < LEDS_PER_COLUMN)
            leds[strip * LEDS_PER_COLUMN + (cur - 1)] = CHSV(160, 255, 150);
        if (cur > 1 && cur - 2 < LEDS_PER_COLUMN)
            leds[strip * LEDS_PER_COLUMN + (cur - 2)] = CHSV(160, 255, 80);
    }
}

// ============================================================
// AmbientLedRenderer — Ocean
// ============================================================
void AmbientLedRenderer::renderOcean(CRGB* leds, int ledCount) {
    if (!oceanInit) {
        smoothedWave = 0.0f;
        oceanInit    = true;
    }

    smoothedWave = smoothedWave * 0.80f + (float)currentAudioLevel * 0.20f;

    if (millis() - lastOceanDebug > 2000) {
        int rows = (int)(constrain(smoothedWave / 500.0f, 0.15f, 0.75f) * LEDS_PER_COLUMN);
        Serial.printf("Ocean: Level=%d, Smoothed=%.0f, Rows=%d/%d\n",
                      (int)currentAudioLevel, smoothedWave, rows, LEDS_PER_COLUMN);
        lastOceanDebug = millis();
    }

    float normalizedWave = constrain(smoothedWave / 500.0f, 0.15f, 0.75f);
    int waveRows = (int)(normalizedWave * LEDS_PER_COLUMN);
    float time   = millis() / 3000.0f;

    for (int col = 0; col < LED_COLUMNS; col++) {
        float phaseOffset = (float)col / (float)LED_COLUMNS * TWO_PI;
        float phaseWave   = sin(time + phaseOffset) * 3.0f;
        int colWaveRows   = constrain(waveRows + (int)phaseWave, 1, LEDS_PER_COLUMN);

        for (int row = 0; row < LEDS_PER_COLUMN; row++) {
            int idx = col * LEDS_PER_COLUMN + row;
            if (idx >= ledCount) continue;
            if (row < colWaveRows) {
                float progress = (float)row / (float)colWaveRows;
                uint8_t hue = 170 - (uint8_t)(progress * 30);
                uint8_t sat = 255 - (uint8_t)(progress * 40);
                uint8_t bri = 80  + (uint8_t)(progress * 175);
                leds[idx] = CHSV(hue, sat, bri);
            } else {
                leds[idx] = CRGB::Black;
            }
        }
    }
}

// ============================================================
// AmbientLedRenderer — Rainforest
// ============================================================
void AmbientLedRenderer::renderRainforest(CRGB* leds, int ledCount) {
    if (!rainforestInit) {
        for (int i = 0; i < 6; i++) {
            fireflyPos[i][0] = -1.0f;
            fireflyTimers[i] = 0;
        }
        eyePair[0]       = -1.0f;
        rainforestInit   = true;
    }

    uint32_t now = millis();

    // Update fireflies
    for (int i = 0; i < 6; i++) {
        if (fireflyPos[i][0] < 0.0f) {
            if (random(100) < 3) {
                fireflyPos[i][0] = random(0, LED_COLUMNS);
                fireflyPos[i][1] = random(3, LEDS_PER_COLUMN - 2);
                fireflyPos[i][2] = 1.0f;
                fireflyTimers[i] = now + 2000 + random(0, 1000);
            }
        } else {
            fireflyPos[i][2] -= 0.008f;
            fireflyPos[i][1] += random(-1, 2) * 0.05f;
            if (now > fireflyTimers[i] || fireflyPos[i][2] <= 0.0f)
                fireflyPos[i][0] = -1.0f;
        }
    }

    // Update eye pair
    if (eyePair[0] < 0.0f) {
        if (random(1000) < 5) {
            eyePair[0] = random(0, LED_COLUMNS - 3);
            eyePair[1] = random(5, LEDS_PER_COLUMN - 4);
            eyePair[2] = (float)(now + 3000 + random(0, 2000));
        }
    } else if (now > (uint32_t)eyePair[2]) {
        eyePair[0] = -1.0f;
    }

    // Render canopy
    for (int strip = 0; strip < LED_COLUMNS; strip++) {
        float stripPhase = strip * 0.2f;
        float pulse      = 0.7f + 0.3f * sin((now / 5000.0f) + stripPhase);
        for (int row = 0; row < LEDS_PER_COLUMN; row++) {
            int idx = strip * LEDS_PER_COLUMN + row;
            float vp = (float)row / (LEDS_PER_COLUMN - 1.0f);
            uint8_t hue = 85  + (uint8_t)(vp * 15.0f);
            uint8_t sat = 255 - (uint8_t)(vp * 40.0f);
            uint8_t bri = 60  + (uint8_t)(vp * 80.0f * pulse);
            leds[idx] = CHSV(hue, sat, bri);
        }
    }

    // Render fireflies
    for (int i = 0; i < 6; i++) {
        if (fireflyPos[i][0] < 0.0f) continue;
        int s = (int)fireflyPos[i][0];
        int r = (int)fireflyPos[i][1];
        if (r >= 0 && r < LEDS_PER_COLUMN && s >= 0 && s < LED_COLUMNS) {
            leds[s * LEDS_PER_COLUMN + r] =
                CHSV(70, 200, (uint8_t)(255 * fireflyPos[i][2]));
        }
    }

    // Render eye pair
    if (eyePair[0] >= 0.0f) {
        int s1  = (int)eyePair[0];
        int s2  = s1 + 3;
        int row = (int)eyePair[1];
        if (s1 < LED_COLUMNS && s2 < LED_COLUMNS && row >= 0 && row < LEDS_PER_COLUMN - 1) {
            uint32_t totalDur = 4000;
            uint32_t age = totalDur - ((uint32_t)eyePair[2] - now);
            uint8_t bri = 255;
            if ((age > 1000 && age < 1150) || (age > 2500 && age < 2650)) bri = 30;
            leds[s1 * LEDS_PER_COLUMN + row]     = CHSV(30, 220, bri);
            leds[s1 * LEDS_PER_COLUMN + row + 1] = CHSV(30, 220, bri);
            leds[s2 * LEDS_PER_COLUMN + row]     = CHSV(30, 220, bri);
            leds[s2 * LEDS_PER_COLUMN + row + 1] = CHSV(30, 220, bri);
        }
    }
}

// ============================================================
// AmbientLedRenderer — Fire
// ============================================================
void AmbientLedRenderer::renderFire(CRGB* leds, int ledCount) {
    if (!fireInit) {
        for (int s = 0; s < LED_COLUMNS; s++) {
            flameHeights[s]   = 0.3f + (random(0, 300) / 1000.0f);
            flamePhases[s]    = random(0, 1000) / 1000.0f;
            sparkPositions[s] = -1.0f;
            sparkBrightness[s]= 0.0f;
        }
        fireInit = true;
    }

    float globalTime = millis() / 1000.0f;
    for (int s = 0; s < LED_COLUMNS; s++) {
        float freq   = 0.3f + (s * 0.03f);
        float target = 0.35f + 0.12f * sin((globalTime * freq) + flamePhases[s]);
        flameHeights[s] += (target - flameHeights[s]) * 0.05f;

        if (sparkPositions[s] < 0.0f && random(100) < 2 && flameHeights[s] > 0.3f) {
            sparkPositions[s]  = flameHeights[s] * LEDS_PER_COLUMN;
            sparkBrightness[s] = 1.0f;
        }
        if (sparkPositions[s] >= 0.0f) {
            sparkPositions[s]  += 0.18f + (random(0, 70) / 1000.0f);
            sparkBrightness[s] -= 0.12f;
            if (sparkPositions[s] >= LEDS_PER_COLUMN || sparkBrightness[s] <= 0.0f) {
                sparkPositions[s]  = -1.0f;
                sparkBrightness[s] = 0.0f;
            }
        }
    }

    fill_solid(leds, ledCount, CRGB::Black);

    for (int strip = 0; strip < LED_COLUMNS; strip++) {
        int maxFlameRow = constrain((int)(flameHeights[strip] * LEDS_PER_COLUMN), 0, LEDS_PER_COLUMN / 2);

        for (int row = 0; row < LEDS_PER_COLUMN; row++) {
            int idx = strip * LEDS_PER_COLUMN + row;
            if (idx < 0 || idx >= ledCount) continue;

            // Spark
            if (sparkPositions[strip] >= 0.0f) {
                int sparkRow = (int)sparkPositions[strip];
                if (row == sparkRow && sparkBrightness[strip] > 0.0f) {
                    uint8_t sh = 25 + random(0, 10);
                    uint8_t sb = (uint8_t)(255 * sparkBrightness[strip]);
                    leds[idx] = CHSV(sh, 220, sb);
                    continue;
                }
            }

            if (row <= maxFlameRow) {
                float prog = (maxFlameRow > 0) ? (float)row / (float)maxFlameRow : 0.0f;
                uint8_t hue;
                if      (prog < 0.4f) hue = 0  + (uint8_t)(prog * 2.5f * 5.0f);
                else if (prog < 0.7f) hue = 5  + (uint8_t)((prog - 0.4f) * 3.33f * 10.0f);
                else                  hue = 15 + (uint8_t)((prog - 0.7f) * 3.33f * 10.0f);
                hue += (uint8_t)random(-1, 2);

                uint8_t bri;
                if (prog < 0.5f) bri = 150 + (uint8_t)(prog * 2.0f * 50.0f);
                else             bri = 200 + (uint8_t)((prog - 0.5f) * 2.0f * 55.0f);
                bri += (uint8_t)random(-5, 6);
                bri = constrain(bri, 100, 255);

                leds[idx] = CHSV(hue, 255, bri);
            }
        }
    }
}

// ============================================================
// AmbientLedRenderer::render  (dispatches by currentAmbientSoundType)
// ============================================================
void AmbientLedRenderer::render(CRGB* leds, int ledCount) {
    // Reset init flags when sound type changes
    if (currentAmbientSoundType != lastType) {
        rainInit       = false;
        oceanInit      = false;
        rainforestInit = false;
        fireInit       = false;
        lastType       = currentAmbientSoundType;
    }

    switch (currentAmbientSoundType) {
        case SOUND_RAIN:       renderRain(leds, ledCount);       break;
        case SOUND_OCEAN:      renderOcean(leds, ledCount);      break;
        case SOUND_RAINFOREST: renderRainforest(leds, ledCount); break;
        case SOUND_FIRE:       renderFire(leds, ledCount);       break;
    }
}
