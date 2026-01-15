#pragma once

#include <FastLED.h>

// Eye Animation Visualizer - Expressive robot eyes
// Uses strips 2-3 for left eye, strips 11-12 for right eye
// Strip 1 is a spacer (stays dark)
class EyeAnimationVisualizer {
public:
    static constexpr int NUM_STRIPS = 12;
    static constexpr int LEDS_PER_STRIP = 12;
    static constexpr int TOTAL_LEDS = NUM_STRIPS * LEDS_PER_STRIP;
    
    // Eye layout: each eye is 2 strips × 12 LEDs (roughly square)
    // Strip 0 is the spacer (stays dark)
    static constexpr int LEFT_EYE_STRIP_START = 1;   // Strips 1-2
    static constexpr int RIGHT_EYE_STRIP_START = 10;  // Strips 10-11
    static constexpr int EYE_WIDTH = 2;   // 2 strips wide
    static constexpr int EYE_HEIGHT = 12; // 12 LEDs tall
    
    // Eye expressions
    enum Expression {
        NORMAL,
        BLINK,
        SQUINT,
        WIDE,
        HAPPY,
        ANGRY,
        WINK_LEFT,
        WINK_RIGHT,
        HEARTS,
        LOOK_LEFT,
        LOOK_RIGHT,
        LOOK_UP,
        LOOK_DOWN
    };
    
    EyeAnimationVisualizer();
    
    void begin();
    void update(unsigned long currentMs);
    void render(CRGB* leds);
    
    void setExpression(Expression expr);
    void setEyeColor(CRGB color);
    void setBlinkInterval(unsigned long interval);
    
private:
    CRGB eyeColor;
    Expression currentExpression;
    Expression targetExpression;
    
    // Animation state
    unsigned long lastUpdateMs;
    unsigned long lastBlinkMs;
    unsigned long blinkInterval;
    float transitionProgress;  // 0.0 to 1.0
    bool isTransitioning;
    
    // Eye state
    float leftEyeOpenness;   // 0.0 (closed) to 1.0 (open)
    float rightEyeOpenness;
    float pupilX, pupilY;    // Pupil position (-1.0 to 1.0)
    
    // Helper functions
    void drawEye(CRGB* leds, int stripStart, float openness, float pupilX, float pupilY);
    void drawExpression(CRGB* leds, Expression expr, float progress);
    void updateAnimation(unsigned long currentMs);
    void clearEyes(CRGB* leds);
    
    // LED mapping (serpentine wiring)
    int ledIndexForCoord(int strip, int height);
    
    // Eye shape helpers
    void drawRectEye(CRGB* leds, int stripStart, int top, int bottom);
    void drawOvalEye(CRGB* leds, int stripStart, float openness);
    void drawHeartEye(CRGB* leds, int stripStart);
    void drawPupil(CRGB* leds, int stripStart, float pupilX, float pupilY, float eyeOpenness);
};
