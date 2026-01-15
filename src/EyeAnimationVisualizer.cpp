#include "EyeAnimationVisualizer.h"

EyeAnimationVisualizer::EyeAnimationVisualizer() 
    : eyeColor(CRGB::White),
      currentExpression(NORMAL),
      targetExpression(NORMAL),
      lastUpdateMs(0),
      lastBlinkMs(0),
      blinkInterval(3000),  // Blink every 3 seconds
      transitionProgress(1.0f),
      isTransitioning(false),
      leftEyeOpenness(1.0f),
      rightEyeOpenness(1.0f),
      pupilX(0.0f),
      pupilY(0.0f)
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

void EyeAnimationVisualizer::drawOvalEye(CRGB* leds, int stripStart, float openness) {
    if (openness <= 0.0f) return;
    
    int centerY = EYE_HEIGHT / 2;
    int height = (int)(EYE_HEIGHT * openness * 0.7f);  // 70% of full height
    
    int top = centerY - height / 2;
    int bottom = centerY + height / 2;
    
    // Draw oval shape (fuller in middle)
    for (int s = 0; s < EYE_WIDTH; s++) {
        for (int h = top; h <= bottom; h++) {
            int idx = ledIndexForCoord(stripStart + s, h);
            if (idx >= 0) leds[idx] = eyeColor;
        }
    }
}

void EyeAnimationVisualizer::drawHeartEye(CRGB* leds, int stripStart) {
    // Draw heart shape
    // Top bumps
    leds[ledIndexForCoord(stripStart, 3)] = CRGB::Red;
    leds[ledIndexForCoord(stripStart, 4)] = CRGB::Red;
    leds[ledIndexForCoord(stripStart + 1, 3)] = CRGB::Red;
    leds[ledIndexForCoord(stripStart + 1, 4)] = CRGB::Red;
    
    // Middle
    for (int h = 5; h <= 7; h++) {
        leds[ledIndexForCoord(stripStart, h)] = CRGB::Red;
        leds[ledIndexForCoord(stripStart + 1, h)] = CRGB::Red;
    }
    
    // Bottom point
    leds[ledIndexForCoord(stripStart, 8)] = CRGB::Red;
    leds[ledIndexForCoord(stripStart + 1, 8)] = CRGB::Red;
}

void EyeAnimationVisualizer::drawPupil(CRGB* leds, int stripStart, float pupilX, float pupilY, float eyeOpenness) {
    if (eyeOpenness < 0.3f) return;  // Don't draw pupil if eye too closed
    
    int centerY = EYE_HEIGHT / 2;
    int pupilH = centerY + (int)(pupilY * 2.0f);
    int pupilS = pupilX > 0 ? 1 : 0;  // Left or right strip
    
    // Draw small dark pupil
    int idx = ledIndexForCoord(stripStart + pupilS, pupilH);
    if (idx >= 0) {
        leds[idx] = CRGB::Black;
        // Add a dim pixel above/below for size
        int idx2 = ledIndexForCoord(stripStart + pupilS, pupilH + 1);
        if (idx2 >= 0) leds[idx2] = CRGB(40, 40, 40);
    }
}

int EyeAnimationVisualizer::ledIndexForCoord(int strip, int height) {
    if (strip < 0 || strip >= NUM_STRIPS) return -1;
    if (height < 0 || height >= LEDS_PER_STRIP) return -1;
    
    int baseIndex = strip * LEDS_PER_STRIP;
    
    if (strip % 2 == 0) {
        // Even strips: wired bottom→top
        return baseIndex + height;
    } else {
        // Odd strips: wired top→bottom
        return baseIndex + (LEDS_PER_STRIP - 1 - height);
    }
}

void EyeAnimationVisualizer::setExpression(Expression expr) {
    targetExpression = expr;
    isTransitioning = true;
    transitionProgress = 0.0f;
}

void EyeAnimationVisualizer::setEyeColor(CRGB color) {
    eyeColor = color;
}

void EyeAnimationVisualizer::setBlinkInterval(unsigned long interval) {
    blinkInterval = interval;
}
