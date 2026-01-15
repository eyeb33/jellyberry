#ifndef SEA_GOOSEBERRY_VISUALIZER_H
#define SEA_GOOSEBERRY_VISUALIZER_H

#include <Arduino.h>
#include <FastLED.h>

// ============== SEA GOOSEBERRY VISUALIZER ==============
//
// Accurately mimics real comb jelly (Pleurobrachia) metachronal waves.
//
// BIOLOGY: Real comb jellies have 8 comb rows with beating cilia.
// Each cilia plate beats slightly after its neighbor, creating a fast
// "chase" pulse (metachronal wave) that races down each row. The colors
// are REFRACTED WHITE LIGHT (not bioluminescence) creating narrow,
// glassy rainbow bands on transparent tissue.
//
// IMPLEMENTATION: Fast (0.3-0.6s per strip), tight pulses (3-7 LEDs)
// with sharp head and fading tail. Neighboring strips are phase-shifted
// to create helical spiral motion. Dark background with bright pulses.
//
// Hardware: 12 vertical LED strips, 12 LEDs each (144 total)
//           Strips 0-11 arranged around spherical shell
//           Wiring: Serpentine (alternating bottom-top, top-bottom)
//           Display: h=0 is bottom, h=11 is top
//
// =======================================================

class SeaGooseberryVisualizer {
public:
    SeaGooseberryVisualizer();
    
    // Initialize the visualizer
    void begin();
    
    // Update animation state (non-blocking, call every frame)
    void update(uint32_t nowMs);
    
    // Render current frame to LED buffer
    void render(CRGB* leds, int ledCount);
    
    // Configuration (can be called at runtime to adjust behavior)
    void setWaveSpeed(float speed);      // 0.5-2.0, default 1.0
    void setBrightness(float brightness); // 0.0-1.0, default 0.8
    
private:
    // Configuration constants (authentic comb jelly parameters)
    static constexpr int NUM_STRIPS = 12;
    static constexpr int LEDS_PER_STRIP = 12;
    static constexpr int NUM_MAIN_RIBS = 8;  // Main bright ribs (strips 0,1,3,4,6,7,9,10)
    
    // Metachronal wave timing (1-2 seconds traverse as observed)
    static constexpr float BASE_WAVE_SPEED = 0.00057f;  // ~1.75s per strip traverse (85% of original)
    static constexpr float PHASE_SHIFT_PER_STRIP = 0.08f;  // Phase offset for circulation
    static constexpr int NUM_WAVES_PER_STRIP = 2;  // 2 bands per rib with good spacing
    static constexpr float SPEED_VARIATION = 0.20f;  // ±20% per-rib speed variation
    
    // Pulse shape (2-3 LED vertical bands - wider for visibility)
    static constexpr float BAND_CENTER_WIDTH = 1.0f;  // Gaussian center
    static constexpr float BAND_FALLOFF = 1.5f;  // Medium falloff for 2-3 LED bands
    
    // Color palette (cyan→green→turquoise with blue accents)
    // Pastel/constrained, not fully saturated
    static constexpr uint8_t HUE_BASE = 110;  // Green-cyan base (more green)
    static constexpr uint8_t HUE_RANGE = 70;  // Through cyan→green→turquoise→blue
    static constexpr uint8_t HUE_ACCENT = 224;  // Pink/magenta accent (rare)
    static constexpr uint8_t SATURATION_BASE = 160;  // Medium saturation
    
    // Background (dark blue, almost black)
    static constexpr uint8_t BACKGROUND_BRIGHTNESS = 0;
    
    // Brightness (30-60% of full LED power)
    static constexpr float BRIGHTNESS_MIN = 0.3f;
    static constexpr float BRIGHTNESS_MAX = 0.6f;
    
    // Macro-scale variation (slow 20-30s breathing)
    static constexpr float BREATHING_PERIOD = 25000.0f;  // 25 second cycle
    
    // Per-strip state
    struct StripState {
        float phaseOffset;      // Phase delay for circulation effect (0.0-1.0)
        float speedVariation;   // Per-rib speed jitter (0.85-1.15)
        float hueOffset;        // Per-rib hue variation
        bool isDimRib;          // True for dimmer structural ribs
        int waveCount;          // Number of waves on this strip (1-3, varies)
        float waveSpacing;      // Random spacing between waves (0.3-0.5)
    };
    
    StripState strips[NUM_STRIPS];
    
    // Animation state
    float globalPhase;           // Master wave position (0.0-1.0, wraps)
    float breathingPhase;        // Slow breathing brightness (0.0-1.0)
    uint32_t lastUpdateMs;
    uint32_t lastShuffleMs;      // Last time patterns were shuffled
    uint32_t nextShuffleInterval; // Time until next shuffle (3-5 seconds)
    
    // Configuration
    float speedMultiplier;
    float brightnessMultiplier;
    
    // Helper functions
    void initializeStrips();
    void shufflePatterns();      // Randomize strip patterns periodically
    int ledIndexForCoord(int strip, int height);  // Handle serpentine wiring
    CRGB getBandColor(float bandPhase, float ledPositionInBand, int stripIndex);  // Color with vertical gradient
    float getGaussianBrightness(float distance);  // Gaussian brightness curve
};

#endif // SEA_GOOSEBERRY_VISUALIZER_H
