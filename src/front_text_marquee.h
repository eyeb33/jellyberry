#ifndef FRONT_TEXT_MARQUEE_H
#define FRONT_TEXT_MARQUEE_H

#include <Arduino.h>
#include <FastLED.h>
#include "display_mapping.h"

// ============== FRONT TEXT MARQUEE ==============
//
// Self-contained scrolling text system for the full circumference display.
// Uses simple 3×8 glyphs (centered vertically in 12-row strips)
// matching the Arduino prototype design.
//
// Text scrolls around all 12 strips, visible from any viewing angle.
//
// Usage:
//   FrontTextMarquee marquee;
//   marquee.setText("POMODORO");
//   marquee.setColor(CRGB::Red);
//   marquee.start();
//   marquee.onComplete([]() { Serial.println("Done!"); });
//   
//   // In main loop:
//   marquee.update();  // advances scroll position
//   marquee.render(leds);  // draws to display
//
// ================================================

class FrontTextMarquee {
public:
    FrontTextMarquee();
    
    // Configuration
    void setText(const String& text);
    void setColor(CRGB color);
    void setSpeed(int columnsPerSecond);  // Default: 4 columns/sec
    
    // Control
    void start();
    void stop();
    bool isActive() const { return active; }
    bool isComplete() const;  // Has text fully scrolled off screen?
    
    // Update and render (call from main loop)
    void update();                // Advances scroll position based on time
    void render(CRGB* leds);      // Draws current frame to display
    
    // Callback when marquee completes
    void onComplete(void (*callback)());
    
private:
    String text;
    CRGB color;
    int scrollPosition;      // Current X position of text (can be negative)
    uint32_t lastUpdate;
    int updateInterval;      // Milliseconds between scroll steps
    bool active;
    void (*completeCallback)();
    
    // Calculate total width of text in columns
    int getTextWidth() const;
    
    // Draw a single character at position x (in display coordinates)
    void drawChar(char c, int x, CRGB* leds);
    
    // Get glyph data for a character (returns pointer to 3-column bitmap)
    const uint8_t* getGlyph(char c);
};

#endif // FRONT_TEXT_MARQUEE_H
