#ifndef DISPLAY_MAPPING_H
#define DISPLAY_MAPPING_H

#include <FastLED.h>

// ============== DISPLAY MAPPING FOR JELLYBERRY ==============
//
// Hardware: 12 vertical strips of 12 LEDs each (144 total)
//           Wired as single chain, ALL strips bottom→top:
//           Strip 0: bottom→top (LEDs 0-11, where 0=bottom, 11=top)
//           Strip 1: bottom→top (LEDs 12-23, where 12=bottom, 23=top)
//           Strip 2: bottom→top (LEDs 24-35, where 24=bottom, 35=top)
//           ... continues for all 12 strips
//           Data flows anti-clockwise around shell
//
// Display: Text scrolls around full circumference (all 12 strips)
//          Visible from any viewing angle
//
// Coordinate System:
//   x: horizontal position around circumference [0..11]
//   y: vertical position on strip [0..11]
//      y=0 is TOP of strip (physical LED row 11)
//      y=11 is BOTTOM of strip (physical LED row 0)
//
// ============================================================

// Display dimensions
constexpr int DISPLAY_WIDTH = 12;  // All 12 strips around circumference
constexpr int DISPLAY_HEIGHT = 12; // Full height of each strip

// All strips used for text display (scrolls around full circumference)

// Get LED reference at display coordinates (x, y)
// Returns a reference to the LED in the global leds[] array
// 
// Parameters:
//   x: horizontal position [0..11], around circumference
//   y: vertical position [0..11], top to bottom (0=top, 11=bottom)
//   leds: the global FastLED array
//
// Returns reference to the appropriate LED, or leds[0] if out of bounds
// (calling code should validate coordinates before drawing)
CRGB& getDisplayLED(int x, int y, CRGB* leds);

// Validate display coordinates
inline bool isValidDisplayCoord(int x, int y) {
    return (x >= 0 && x < DISPLAY_WIDTH && y >= 0 && y < DISPLAY_HEIGHT);
}

#endif // DISPLAY_MAPPING_H
