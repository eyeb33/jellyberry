#include "front_text_marquee.h"

// ============== FONT DEFINITION ==============
//
// Simple 3×8 glyphs matching Arduino prototype.
// Each glyph is 3 columns wide, 8 rows tall.
// Bits read bottom-to-top within each column.
//
// Bitmap format: 3 bytes per character, one per column
// Bit 0 = row 0 (bottom), Bit 7 = row 7 (top)

// Font lookup table
struct Glyph {
    char character;
    uint8_t columns[3];
};

const Glyph FONT[] = {
    // Simple readable letters - bottom to top (bit 0 = bottom, bit 7 = top)
    {'J', {0b01111110, 0b10000001, 0b01111110}},  // J
    {'E', {0b11111111, 0b10010001, 0b10000001}},  // E
    {'L', {0b11111111, 0b10000000, 0b10000000}},  // L
    {'Y', {0b00000111, 0b11111000, 0b00000111}},  // Y
    {'B', {0b11111111, 0b10010001, 0b01101110}},  // B
    {'R', {0b11111111, 0b00010001, 0b11101110}},  // R
    
    {'A', {0b11111110, 0b00010001, 0b11111110}},  // A
    {'C', {0b01111110, 0b10000001, 0b10000001}},  // C
    {'D', {0b11111111, 0b10000001, 0b01111110}},  // D
    {'F', {0b11111111, 0b00010001, 0b00000001}},  // F
    {'G', {0b01111110, 0b10000001, 0b11100001}},  // G
    {'H', {0b11111111, 0b00010000, 0b11111111}},  // H
    {'I', {0b11111111, 0b11111111, 0b11111111}},  // I
    {'K', {0b11111111, 0b00111000, 0b11000111}},  // K
    {'M', {0b11111111, 0b00001110, 0b11111111}},  // M
    {'N', {0b11111111, 0b00011100, 0b11111111}},  // N
    {'O', {0b01111110, 0b10000001, 0b01111110}},  // O
    {'P', {0b11111111, 0b00010001, 0b00001110}},  // P
    {'T', {0b00000001, 0b11111111, 0b00000001}},  // T
    {'U', {0b01111111, 0b10000000, 0b01111111}},  // U
    
    {'0', {0b01111110, 0b10000001, 0b01111110}},  // 0
    {'1', {0b01000001, 0b11111111, 0b10000000}},  // 1
    {'2', {0b11000001, 0b10100001, 0b10011111}},  // 2
    {'3', {0b01000010, 0b10010001, 0b01101110}},  // 3
    {'4', {0b00111111, 0b00100000, 0b11111111}},  // 4
    {'5', {0b10011111, 0b10010001, 0b01100001}},  // 5
    {'6', {0b01111110, 0b10010001, 0b01100010}},  // 6
    {'7', {0b00000001, 0b11100001, 0b00011111}},  // 7
    {'8', {0b01101110, 0b10010001, 0b01101110}},  // 8
    {'9', {0b01001110, 0b10010001, 0b01111110}},  // 9
    
    {' ', {0b00000000, 0b00000000, 0b00000000}},  // Space
    {':', {0b01100110, 0b01100110, 0b01100110}},  // :
    
    {'*', {0b11111111, 0b11111111, 0b11111111}}   // Fallback
};

const int FONT_SIZE = sizeof(FONT) / sizeof(Glyph);

// ============== MARQUEE IMPLEMENTATION ==============

FrontTextMarquee::FrontTextMarquee() 
    : text(""), 
      color(CRGB::White), 
      scrollPosition(0),
      lastUpdate(0),
      updateInterval(250),  // Default: 4 columns/sec
      active(false),
      completeCallback(nullptr)
{
}

void FrontTextMarquee::setText(const String& newText) {
    text = newText;
    text.toUpperCase();  // Font only has uppercase
}

void FrontTextMarquee::setColor(CRGB newColor) {
    color = newColor;
}

void FrontTextMarquee::setSpeed(int columnsPerSecond) {
    if (columnsPerSecond > 0) {
        updateInterval = 1000 / columnsPerSecond;
    }
}

void FrontTextMarquee::start() {
    scrollPosition = DISPLAY_WIDTH;  // Start off right edge
    lastUpdate = millis();
    active = true;
}

void FrontTextMarquee::stop() {
    active = false;
}

bool FrontTextMarquee::isComplete() const {
    // Text has scrolled completely off the left edge
    int textWidth = getTextWidth();
    return scrollPosition < -textWidth;
}

void FrontTextMarquee::onComplete(void (*callback)()) {
    completeCallback = callback;
}

void FrontTextMarquee::update() {
    if (!active) return;
    
    uint32_t now = millis();
    if (now - lastUpdate >= updateInterval) {
        scrollPosition--;
        lastUpdate = now;
        
        // Check if complete and trigger callback
        if (isComplete()) {
            active = false;
            if (completeCallback) {
                completeCallback();
            }
        }
    }
}

void FrontTextMarquee::render(CRGB* leds) {
    if (!active) return;
    
    // Clear display region
    for (int x = 0; x < DISPLAY_WIDTH; x++) {
        for (int y = 0; y < DISPLAY_HEIGHT; y++) {
            getDisplayLED(x, y, leds) = CRGB::Black;
        }
    }
    
    // Draw each character
    int xPos = scrollPosition;
    for (size_t i = 0; i < text.length(); i++) {
        char c = text[i];
        
        // Only draw if any part of character is visible
        if (xPos + 3 >= 0 && xPos < DISPLAY_WIDTH) {
            drawChar(c, xPos, leds);
        }
        
        xPos += 4;  // 3 columns for char + 1 column spacing
    }
}

int FrontTextMarquee::getTextWidth() const {
    // Each character is 3 columns wide + 1 column spacing
    // Last character doesn't need trailing space
    if (text.length() == 0) return 0;
    return text.length() * 4 - 1;
}

void FrontTextMarquee::drawChar(char c, int x, CRGB* leds) {
    const uint8_t* glyph = getGlyph(c);
    
    // Center 8-row glyphs at rows 2-9 (leaving rows 0-1 and 10-11 blank)
    const int Y_OFFSET = 2;
    
    // Draw 3 columns of the glyph
    for (int col = 0; col < 3; col++) {
        int displayX = x + col;
        if (displayX < 0 || displayX >= DISPLAY_WIDTH) continue;
        
        uint8_t columnData = glyph[col];
        
        // Draw 8 rows (bit 0 = bottom, bit 7 = top)
        for (int row = 0; row < 8; row++) {
            if (columnData & (1 << row)) {
                int displayY = Y_OFFSET + row;
                getDisplayLED(displayX, displayY, leds) = color;
            }
        }
    }
}

const uint8_t* FrontTextMarquee::getGlyph(char c) {
    // Search font table
    for (int i = 0; i < FONT_SIZE; i++) {
        if (FONT[i].character == c) {
            return FONT[i].columns;
        }
    }
    
    // Return fallback glyph (solid block) for unknown characters
    return FONT[FONT_SIZE - 1].columns;
}
