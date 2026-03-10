#include "EyeAnimationVisualizer.h"

EyeAnimationVisualizer::EyeAnimationVisualizer() 
    : eyeColor(CRGB::White),
      currentExpression(NORMAL),
      targetExpression(NORMAL),
      lastUpdateMs(0),
      lastBlinkMs(0),
      blinkInterval(3000),  // Blink every 3 seconds
      transitionProgress(1.0f),
      isTransitioning(false)
{
}

void EyeAnimationVisualizer::begin() {
    lastUpdateMs = millis();
    lastBlinkMs = millis();
    currentExpression = NORMAL;
    Serial.println("Eye Animation started - Robot eyes active!");
}

void EyeAnimationVisualizer::update(unsigned long currentMs) {
    unsigned long deltaMs = currentMs - lastUpdateMs;
    lastUpdateMs = currentMs;
    
    // Auto-blink periodically
    if (currentMs - lastBlinkMs > blinkInterval && !isTransitioning) {
        lastBlinkMs = currentMs;
        // Trigger a quick blink
        if (currentExpression == NORMAL) {
            setExpression(BLINK);
        }
    }
    
    // Random expression changes
    if (!isTransitioning && random(0, 100) < 1) {  // 1% chance per update
        Expression expressions[] = {NORMAL, SQUINT, WIDE, HAPPY, WINK_LEFT, LOOK_LEFT, LOOK_RIGHT};
        targetExpression = expressions[random(0, 7)];
        isTransitioning = true;
        transitionProgress = 0.0f;
    }
    
    // Update transition
    if (isTransitioning) {
        transitionProgress += deltaMs / 300.0f;  // 300ms transition
        if (transitionProgress >= 1.0f) {
            transitionProgress = 1.0f;
            currentExpression = targetExpression;
            isTransitioning = false;
            
            // If blink finished, return to normal
            if (currentExpression == BLINK) {
                targetExpression = NORMAL;
                isTransitioning = true;
                transitionProgress = 0.0f;
            }
        }
    }
}

void EyeAnimationVisualizer::render(CRGB* leds) {
    clearEyes(leds);
    drawExpression(leds, currentExpression, transitionProgress);
}

void EyeAnimationVisualizer::clearEyes(CRGB* leds) {
    // Clear all eye strips
    for (int s = LEFT_EYE_STRIP_START; s < LEFT_EYE_STRIP_START + EYE_WIDTH; s++) {
        for (int h = 0; h < EYE_HEIGHT; h++) {
            int idx = ledIndexForCoord(s, h);
            if (idx >= 0) leds[idx] = CRGB::Black;
        }
    }
    for (int s = RIGHT_EYE_STRIP_START; s < RIGHT_EYE_STRIP_START + EYE_WIDTH; s++) {
        for (int h = 0; h < EYE_HEIGHT; h++) {
            int idx = ledIndexForCoord(s, h);
            if (idx >= 0) leds[idx] = CRGB::Black;
        }
    }
}

void EyeAnimationVisualizer::drawExpression(CRGB* leds, Expression expr, float progress) {
    switch (expr) {
        case NORMAL:
            // Simple rectangular eyes - centered
            drawRectEye(leds, LEFT_EYE_STRIP_START, 4, 7);
            drawRectEye(leds, RIGHT_EYE_STRIP_START, 4, 7);
            break;
            
        case BLINK:
            {
                float openness = 1.0f - progress;
                if (progress > 0.5f) openness = progress - 0.5f;  // Reopen after half
                int height = (int)(4 * openness);  // 4 LEDs tall when open
                if (height > 0) {
                    int center = 5;  // Center around LED 5-6
                    drawRectEye(leds, LEFT_EYE_STRIP_START, center - height/2, center + height/2);
                    drawRectEye(leds, RIGHT_EYE_STRIP_START, center - height/2, center + height/2);
                }
            }
            break;
            
        case SQUINT:
            // Very narrow horizontal slits
            drawRectEye(leds, LEFT_EYE_STRIP_START, 5, 6);
            drawRectEye(leds, RIGHT_EYE_STRIP_START, 5, 6);
            break;
            
        case WIDE:
            // Large tall rectangular eyes
            drawRectEye(leds, LEFT_EYE_STRIP_START, 2, 9);
            drawRectEye(leds, RIGHT_EYE_STRIP_START, 2, 9);
            break;
            
        case HAPPY:
            // Two separate blocks with gap in middle (smile-like)
            drawRectEye(leds, LEFT_EYE_STRIP_START, 3, 4);
            drawRectEye(leds, LEFT_EYE_STRIP_START, 7, 8);
            drawRectEye(leds, RIGHT_EYE_STRIP_START, 3, 4);
            drawRectEye(leds, RIGHT_EYE_STRIP_START, 7, 8);
            break;
            
        case ANGRY:
            // Single block in center (focused look)
            drawRectEye(leds, LEFT_EYE_STRIP_START, 5, 7);
            drawRectEye(leds, RIGHT_EYE_STRIP_START, 5, 7);
            break;
            
        case WINK_LEFT:
            // Left closed, right open
            drawRectEye(leds, LEFT_EYE_STRIP_START, 5, 6);  // Thin line
            drawRectEye(leds, RIGHT_EYE_STRIP_START, 4, 7);  // Normal
            break;
            
        case WINK_RIGHT:
            // Right closed, left open
            drawRectEye(leds, LEFT_EYE_STRIP_START, 4, 7);   // Normal
            drawRectEye(leds, RIGHT_EYE_STRIP_START, 5, 6);  // Thin line
            break;
            
        case HEARTS:
            drawHeartEye(leds, LEFT_EYE_STRIP_START);
            drawHeartEye(leds, RIGHT_EYE_STRIP_START);
            break;
            
        case LOOK_LEFT:
            // Both rectangles on left strip only
            for (int h = 4; h <= 7; h++) {
                leds[ledIndexForCoord(LEFT_EYE_STRIP_START, h)] = eyeColor;
                leds[ledIndexForCoord(RIGHT_EYE_STRIP_START, h)] = eyeColor;
            }
            break;
            
        case LOOK_RIGHT:
            // Both rectangles on right strip only
            for (int h = 4; h <= 7; h++) {
                leds[ledIndexForCoord(LEFT_EYE_STRIP_START + 1, h)] = eyeColor;
                leds[ledIndexForCoord(RIGHT_EYE_STRIP_START + 1, h)] = eyeColor;
            }
            break;
            
        case LOOK_UP:
            // Shift rectangles up (toward LED 11 - top of strip)
            drawRectEye(leds, LEFT_EYE_STRIP_START, 7, 10);
            drawRectEye(leds, RIGHT_EYE_STRIP_START, 7, 10);
            break;
            
        case LOOK_DOWN:
            // Shift rectangles down (toward LED 0 - bottom/data line)
            drawRectEye(leds, LEFT_EYE_STRIP_START, 1, 4);
            drawRectEye(leds, RIGHT_EYE_STRIP_START, 1, 4);
            break;
    }
}

void EyeAnimationVisualizer::drawRectEye(CRGB* leds, int stripStart, int top, int bottom) {
    for (int s = 0; s < EYE_WIDTH; s++) {
        for (int h = top; h <= bottom; h++) {
            int idx = ledIndexForCoord(stripStart + s, h);
            if (idx >= 0) leds[idx] = eyeColor;
        }
    }
}

void EyeAnimationVisualizer::drawHeartEye(CRGB* leds, int stripStart) {
    // Draw heart shape - guard every index against ledIndexForCoord returning -1
    auto safeSet = [&](int strip, int height, CRGB color) {
        int idx = ledIndexForCoord(strip, height);
        if (idx >= 0) leds[idx] = color;
    };

    // Top bumps
    safeSet(stripStart,     3, eyeColor);
    safeSet(stripStart,     4, eyeColor);
    safeSet(stripStart + 1, 3, eyeColor);
    safeSet(stripStart + 1, 4, eyeColor);
    
    // Middle
    for (int h = 5; h <= 7; h++) {
        safeSet(stripStart,     h, eyeColor);
        safeSet(stripStart + 1, h, eyeColor);
    }
    
    // Bottom point
    safeSet(stripStart,     8, eyeColor);
    safeSet(stripStart + 1, 8, eyeColor);
}

int EyeAnimationVisualizer::ledIndexForCoord(int strip, int height) {
    if (strip < 0 || strip >= NUM_STRIPS) return -1;
    if (height < 0 || height >= LEDS_PER_STRIP) return -1;
    
    // Uniform wiring: all strips run bottom→top, consistent with display_mapping.cpp
    return strip * LEDS_PER_STRIP + height;
}

void EyeAnimationVisualizer::setExpression(Expression expr) {
    targetExpression = expr;
    isTransitioning = true;
    transitionProgress = 0.0f;
}
