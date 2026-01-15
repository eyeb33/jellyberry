#include "SeaGooseberryVisualizer.h"
#include <math.h>

// ============== CONSTRUCTOR ==============

SeaGooseberryVisualizer::SeaGooseberryVisualizer()
    : globalPhase(0.0f),
      breathingPhase(0.0f),
      lastUpdateMs(0),
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
        
        // Phase offset for circulation effect with minimal randomization
        strip.phaseOffset = s * PHASE_SHIFT_PER_STRIP + (random(0, 80) / 500.0f);  // Add 0-0.16 random offset
        
        // Per-rib speed variation ±12% for organic feel
        strip.speedVariation = 0.88f + (random(0, 240) / 1000.0f);
        
        // Per-rib hue offset for color variation (-10 to +10 to stay centered)
        strip.hueOffset = random(-10, 10);
        
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
        
        // Render 1-3 bands per rib (using 2)
        for (int w = 0; w < NUM_WAVES_PER_STRIP; w++) {
            // Calculate band center position (0.0-1.0 along strip)
            float waveOffset = (float)w / (float)NUM_WAVES_PER_STRIP;
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
