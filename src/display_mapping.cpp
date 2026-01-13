#include "display_mapping.h"
#include "Config.h"

// ============== FULL CIRCUMFERENCE DISPLAY ==============
// 
// Text scrolls around all 12 strips, visible from any angle.
// Strips numbered 0-11 going anti-clockwise around shell.
// Data flows anti-clockwise (strip 0→1→2...→11→0)

// ============== LED MAPPING FUNCTION ==============

CRGB& getDisplayLED(int x, int y, CRGB* leds) {
    // Validate coordinates
    if (!isValidDisplayCoord(x, y)) {
        // Return dummy LED (first pixel) if out of bounds
        // Caller should validate before calling, but this prevents crashes
        return leds[0];
    }
    
    // Map display coordinates to physical LED
    // x: strip number [0..11] going anti-clockwise
    // y: position on strip [0..11] where 0=top, 11=bottom
    
    // All strips wired bottom→top (NOT serpentine)
    // Physical row 0 = bottom, physical row 11 = top
    // Display y=0 should be top, so we invert:
    int physicalRow = 11 - y;
    
    // Calculate LED index
    // Each strip starts at (stripNumber * 12)
    // Within strip: LED index increases from bottom to top
    int ledIndex = (x * LEDS_PER_COLUMN) + physicalRow;
    
    return leds[ledIndex];
}
