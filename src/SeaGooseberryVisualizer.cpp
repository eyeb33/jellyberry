#include "SeaGooseberryVisualizer.h"
#include <math.h>

// ============== CONSTRUCTOR ==============

SeaGooseberryVisualizer::SeaGooseberryVisualizer()
    : globalPhase(0.0f),
      breathingPhase(0.0f),
      lastUpdateMs(0),
      lastShuffleMs(0),
      nextShuffleInterval(3000 + random(0, 2000)),  // 3-5 seconds
      speedMultiplier(1.0f),
      brightnessMultiplier(0.5f)  // Medium-low brightness (30-60% range)
{
}

// ============== INITIALIZATION ==============

void SeaGooseberryVisualizer::begin() {
    initializeStrips();
    lastUpdateMs = millis();
    Serial.println("🌊 Sea Gooseberry Mode: Authentic comb jelly animation");
    Serial.println("   8 main ribs, 2-4 LED bands, blue→cyan→green→turquoise");
}

// ============== STRIP INITIALIZATION ==============

void SeaGooseberryVisualizer::initializeStrips() {
    // Initialize 8 main bright ribs + 4 dimmer structural ribs
    // Main ribs: 0, 1, 3, 4, 6, 7, 9, 10 (evenly distributed)
    // Dim ribs: 2, 5, 8, 11 (every third)
    
    for (int s = 0; s < NUM_STRIPS; s++) {
        StripState& strip = strips[s];
        
        // Phase offset for circulation effect with increased randomization
        strip.phaseOffset = s * PHASE_SHIFT_PER_STRIP + (random(0, 200) / 500.0f);  // Add 0-0.4 random offset (was 0-0.16)
        
        // Per-rib speed variation with wider range and some slower waves mixed in
        // 70% chance normal speed, 30% chance slower
        if (random(100) < 30) {
            strip.speedVariation = 0.50f + (random(0, 400) / 1000.0f);  // Slower: 0.50-0.90
        } else {
            strip.speedVariation = 0.85f + (random(0, 350) / 1000.0f);  // Normal: 0.85-1.20
        }
        
        // Per-rib hue offset for more color variation
        strip.hueOffset = random(-15, 15);  // ±15 (was ±10)
        
        // Random wave count (1-3 waves per strip for variety)
        int waveChoice = random(100);
        if (waveChoice < 30) {
            strip.waveCount = 1;  // 30% single wave
        } else if (waveChoice < 70) {
            strip.waveCount = 2;  // 40% double wave
        } else {
            strip.waveCount = 3;  // 30% triple wave
        }
        
        // Random spacing between waves
        strip.waveSpacing = 0.3f + (random(0, 200) / 1000.0f);  // 0.3-0.5
        
        // Mark dimmer structural ribs (every third: 2, 5, 8, 11)
        strip.isDimRib = (s % 3 == 2);
    }
}

// ============== CONFIGURATION ==============

void SeaGooseberryVisualizer::setWaveSpeed(float speed) {
    speedMultiplier = constrain(speed, 0.5f, 2.0f);
}

void SeaGooseberryVisualizer::setBrightness(float brightness) {
    brightnessMultiplier = constrain(brightness, 0.0f, 1.0f);
}

// ============== UPDATE (NON-BLOCKING) ==============

void SeaGooseberryVisualizer::update(uint32_t nowMs) {
    if (lastUpdateMs == 0) {
        lastUpdateMs = nowMs;
        return;
    }
    
    float deltaMs = (float)(nowMs - lastUpdateMs);
    lastUpdateMs = nowMs;
    
    // Update slow breathing modulation (20-30s cycle)
    breathingPhase += deltaMs / BREATHING_PERIOD;
    if (breathingPhase >= 1.0f) {
        breathingPhase -= 1.0f;
    }
    
    // Periodically shuffle patterns to break repetition (every 3-5s)
    if (nowMs - lastShuffleMs >= nextShuffleInterval) {
        shufflePatterns();
        lastShuffleMs = nowMs;
        nextShuffleInterval = 3000 + random(0, 2000);  // Next shuffle in 3-5s
    }
    
    // Calculate current speed (constant, per-rib variation applied in render)
    float currentSpeed = BASE_WAVE_SPEED * speedMultiplier;
    
    // Advance global wave phase (continuous downward comb waves)
    globalPhase += currentSpeed * deltaMs;
    if (globalPhase >= 1.0f) {
        globalPhase -= 1.0f;
    }
}

// ============== RENDER ==============

void SeaGooseberryVisualizer::render(CRGB* leds, int ledCount) {
    // Calculate global breathing brightness (30-60% over 25s cycle)
    float breathingMult = BRIGHTNESS_MIN + (BRIGHTNESS_MAX - BRIGHTNESS_MIN) * (0.5f + 0.5f * sin(breathingPhase * 2.0f * PI));
    
    // Start with pure black background (bands stand out clearly)
    CRGB background = CRGB(0, 0, 0);
    fill_solid(leds, ledCount, background);
    
    // Render each rib
    for (int s = 0; s < NUM_STRIPS; s++) {
        StripState& strip = strips[s];
        
        // Dim structural ribs have higher brightness (was too dim)
        float ribBrightness = strip.isDimRib ? 0.4f : 1.0f;
        
        // Render variable number of bands per rib (1-3, randomized)
        for (int w = 0; w < strip.waveCount; w++) {
            // Calculate band center position (0.0-1.0 along strip)
            float waveOffset = (float)w * strip.waveSpacing;
            float bandPhase = globalPhase + strip.phaseOffset + waveOffset;
            bandPhase *= strip.speedVariation;  // Per-rib speed variation
            
            // Ensure phase wraps properly (keep in 0-1 range)
            bandPhase = bandPhase - floor(bandPhase);
            
            // Convert to LED position (0-11)
            float bandCenter = bandPhase * LEDS_PER_STRIP;
            
            // Render each LED with Gaussian falloff around band center
            for (int h = 0; h < LEDS_PER_STRIP; h++) {
                int ledIdx = ledIndexForCoord(s, h);
                if (ledIdx < 0 || ledIdx >= ledCount) continue;
                
                // Distance from band center
                float distance = abs(h - bandCenter);
                if (distance > LEDS_PER_STRIP / 2) {
                    distance = LEDS_PER_STRIP - distance;  // Wrap distance
                }
                
                // Gaussian brightness curve (2-3 LED width)
                float brightness = getGaussianBrightness(distance);
                
                if (brightness > 0.10f) {  // Slightly higher threshold to prevent dim purple
                    // Position within band for color gradient (0=center, 1=edge)
                    float posInBand = distance / BAND_FALLOFF;
                    
                    // Get color (varies along band height and per rib)
                    CRGB bandColor = getBandColor(bandPhase, posInBand, s);
                    
                    // Apply brightness multipliers
                    float finalBrightness = brightness * ribBrightness * breathingMult * brightnessMultiplier;
                    bandColor.nscale8_video((uint8_t)(finalBrightness * 255));
                    
                    // Additive blend (bands can overlap)
                    leds[ledIdx] += bandColor;
                }
            }
        }
        
        // No additional glow - ribs should be dark except for traveling bands
    }
}

// ============== HELPER FUNCTIONS ==============

// LED index mapping with serpentine wiring correction
int SeaGooseberryVisualizer::ledIndexForCoord(int strip, int height) {
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

// Get color for band with vertical gradient and per-rib variation
CRGB SeaGooseberryVisualizer::getBandColor(float bandPhase, float posInBand, int stripIndex) {
    StripState& strip = strips[stripIndex];
    
    // STRICTLY green→cyan→blue palette (96-160 hue)
    // Build hue from safe components only
    float hue = 110.0f;  // Start at green-cyan base
    
    // Add small controlled variations (each component kept small)
    hue += (bandPhase * 0.3f) * 30.0f;  // Max +9 from phase (keep bandPhase effect small)
    hue += strip.hueOffset;  // ±10 from strip
    hue += posInBand * 8.0f;  // Max +8 from position
    
    // HARD LIMIT: Cannot exceed green-cyan-blue range
    // 96=green, 128=cyan, 160=light blue
    if (hue < 96.0f) hue = 96.0f;
    if (hue > 160.0f) hue = 160.0f;
    
    // Medium saturation (not pastel, not full)
    uint8_t saturation = SATURATION_BASE + (uint8_t)((1.0f - posInBand) * 40);
    
    return CHSV((uint8_t)hue, saturation, 255);
}

// Gaussian brightness curve for 2-3 LED bands
float SeaGooseberryVisualizer::getGaussianBrightness(float distance) {
    // Medium Gaussian falloff for visible 2-3 LED bands
    float exponent = -(distance * distance) / (2.0f * BAND_FALLOFF * BAND_FALLOFF);
    float brightness = exp(exponent);
    
    // Threshold to prevent dim glow
    if (brightness < 0.10f) brightness = 0.0f;
    
    return brightness;
}

// Shuffle patterns periodically to break repetitive patterns
void SeaGooseberryVisualizer::shufflePatterns() {
    for (int s = 0; s < NUM_STRIPS; s++) {
        StripState& strip = strips[s];
        
        // Re-randomize wave count (1-3 waves)
        int waveChoice = random(100);
        if (waveChoice < 30) {
            strip.waveCount = 1;  // 30% single wave
        } else if (waveChoice < 70) {
            strip.waveCount = 2;  // 40% double wave
        } else {
            strip.waveCount = 3;  // 30% triple wave
        }
        
        // Re-randomize spacing
        strip.waveSpacing = 0.3f + (random(0, 200) / 1000.0f);  // 0.3-0.5
        
        // Add random phase jump (0-0.3)
        strip.phaseOffset += random(0, 300) / 1000.0f;
        if (strip.phaseOffset > 1.0f) strip.phaseOffset -= 1.0f;
        
        // Occasionally swap speed ranges
        if (random(100) < 40) {
            if (random(100) < 30) {
                strip.speedVariation = 0.50f + (random(0, 400) / 1000.0f);  // Slower: 0.50-0.90
            } else {
                strip.speedVariation = 0.85f + (random(0, 350) / 1000.0f);  // Normal: 0.85-1.20
            }
        }
    }
}
