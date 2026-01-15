# Sea Gooseberry Mode - Implementation Guide

## Overview

**Sea Gooseberry Mode** transforms your JellyBerry device into a living comb jelly (Pleurobrachia spp.) with mesmerizing downward-traveling waves of iridescent color. This mode accurately recreates the natural bioluminescence patterns of real sea gooseberries drifting in dark ocean water.

---

## 🎨 Visual Design

### Biological Accuracy
Real sea gooseberries have 8 meridional comb rows running from pole to pole. Each row contains tiny beating cilia that refract light, creating traveling waves of rainbow color (blue, cyan, green, magenta) flowing downward. This is **NOT** true bioluminescence, but rather white light refracted through glass-like comb plates.

### Our Implementation
- **8 Primary Comb Rows** (strips 0, 2, 3, 5, 6, 8, 9, 11) - bright, dominant ribs
- **4 Dimmer Ribs** (strips 1, 4, 7, 10) - 40% brightness, in-between glow
- **2 Overlapping Waves** - different speeds for visual depth
- **Phase-Shifted Rotation** - each rib slightly ahead/behind neighbors
- **Organic Timing** - ±10% speed variation per rib, never mechanical
- **Slow Breathing** - 30-second global brightness cycle (±30%)

---

## 🔧 Technical Architecture

### File Structure
```
src/
├── SeaGooseberryVisualizer.h      # Class definition & constants
├── SeaGooseberryVisualizer.cpp    # Animation implementation
└── main.cpp                        # Integration & mode switching
```

### Key Components

#### 1. **SeaGooseberryVisualizer Class**
```cpp
class SeaGooseberryVisualizer {
public:
    void begin();                    // Initialize state
    void update(uint32_t nowMs);     // Non-blocking animation update
    void render(CRGB* leds, int ledCount);  // Write to LED buffer
    
    // Runtime configuration
    void setWaveSpeed(float speed);      // 0.5-2.0x multiplier
    void setBrightness(float brightness); // 0.0-1.0
    void setBreathingEnabled(bool enabled);
};
```

#### 2. **Per-Rib State Tracking**
```cpp
struct RibState {
    float phaseOffset;       // Rotation effect (0.0-1.0)
    float speedVariation;    // Organic speed (0.9-1.1)
    float brightnessScale;   // Primary vs dimmer ribs
    float hueOffset;         // Color diversity
    bool isPrimaryComb;      // True for main 8 ribs
};
```

#### 3. **Wave System**
- **Wave 1:** Base speed (1.5s per full travel)
- **Wave 2:** 1.3x faster (secondary ripples)
- **Color:** HSV interpolation with sine-based rainbow gradients
- **Brightness:** Gaussian-like falloff from wave center

---

## 📐 LED Mapping

### Physical Layout
```
12 vertical strips × 12 LEDs each = 144 total
Strips arranged around spherical shell (30° apart)
Each strip wired bottom→top (LED 0=bottom, LED 11=top)
Data flows: strip 0 → 1 → 2 ... → 11 → repeat
```

### Display Coordinates
```cpp
int ledIndexForStripAndRow(int strip, int row) {
    // strip: 0-11 (around circumference)
    // row: 0-11 (0=top pole, 11=bottom pole)
    int physicalRow = (LEDS_PER_STRIP - 1) - row;
    return (strip * LEDS_PER_STRIP) + physicalRow;
}
```

### Primary Comb Row Distribution
```
Strips:  0  1  2  3  4  5  6  7  8  9  10 11
Type:    P  D  P  P  D  P  P  D  P  P  D  P
         ^     ^  ^     ^  ^     ^  ^     ^
         Primary comb rows (bright, like real comb jelly)
```
(P = Primary bright rib, D = Dimmer in-between rib)

---

## ⚙️ Configuration Constants

### Wave Parameters
```cpp
BASE_WAVE_SPEED = 0.0015f         // 1.5 seconds per full wave
NUM_WAVES = 2                      // Multiple overlapping waves
```

### Breathing Effect
```cpp
BREATHING_PERIOD = 30000.0f       // 30 second breathing cycle
BREATHING_DEPTH = 0.3f            // ±30% brightness variation
```

### Color Palette (HSV hues 0-255)
```cpp
HUE_BLUE = 160      // Dominant base color
HUE_CYAN = 128      // Center hue
HUE_GREEN = 96      // Warm accent
HUE_MAGENTA = 192   // Cool accent
HUE_RANGE = 64      // Total rainbow spread
```

### Performance
- **Update Rate:** Every frame (~20-50ms)
- **CPU Usage:** Minimal (no blocking calls)
- **Memory:** ~300 bytes (12 RibState structs + wave arrays)

---

## 🎮 User Controls

### Activation
1. Press **Button 2** repeatedly to cycle through modes
2. Mode order: Idle → Ambient → ... → Lamp → **Sea Gooseberry** → Idle
3. Marquee displays "SEA JELLY" during mode entry

### During Mode
- **Button 1 (short press):** Talk to Gemini (animation continues in background)
  - Returns to Sea Gooseberry after 10s conversation window
- **Button 2 (short press):** Exit to Idle mode

### Voice Commands
All standard Gemini functions work while animation plays:
- "What mode is this?"
- "Tell me about comb jellies"
- "Set a timer for 5 minutes" (timer overlays, then returns to animation)

---

## 🔬 Algorithm Details

### Wave Position Calculation
```cpp
// For each LED on each rib:
float wavePhase = wavePhases[wave] + rib.phaseOffset;
float wavePos = fmod(wavePhase - position + 1.0f, 1.0f);

// wavePhase: current wave position (0.0-1.0)
// position: LED position on strip (0.0=top, 1.0=bottom)
// Result: wave travels downward over time
```

### Color Generation
```cpp
CRGB getWaveColor(float position, float hueOffset) {
    float hue = HUE_CYAN;
    float hueShift = sin(position * PI) * HUE_RANGE;
    hue += hueShift + (hueOffset * HUE_RANGE);
    return CHSV((uint8_t)hue, 200, 255);
}
```

### Brightness Falloff
```cpp
float getWaveBrightness(float position) {
    // Gaussian-like with secondary peak
    float distance = abs(position - 0.5f) * 2.0f;
    float brightness = easeInOutSine(1.0f - distance);
    float secondaryPeak = sin(position * PI * 2.0f) * 0.2f;
    return pow(brightness + secondaryPeak, 0.8f);
}
```

---

## 🧪 Testing & Tuning

### Visual Verification Checklist
- [ ] Waves travel smoothly downward (not jumping)
- [ ] Neighboring ribs are phase-shifted (rotation visible)
- [ ] Primary ribs (0,2,3,5,6,8,9,11) are brighter than others
- [ ] Colors shift through blue→cyan→green→magenta
- [ ] No harsh whites or strobing
- [ ] Motion feels organic, not mechanical
- [ ] Breathing effect is subtle (30s cycle)

### Performance Testing
```cpp
// In loop(), measure render time:
uint32_t start = micros();
seaGooseberry.render(leds, NUM_LEDS);
uint32_t duration = micros() - start;
// Should be < 5ms on ESP32-S3
```

### Tuning Parameters
**To make waves faster:**
```cpp
seaGooseberry.setWaveSpeed(1.5f);  // 50% faster
```

**To make dimmer/brighter:**
```cpp
seaGooseberry.setBrightness(0.8f);  // 80% brightness
```

**To disable breathing:**
```cpp
seaGooseberry.setBreathingEnabled(false);
```

---

## 🐛 Troubleshooting

### Waves not moving
- Check that `seaGooseberry.update(millis())` is called in `loop()`
- Verify `currentLEDMode == LED_SEA_GOOSEBERRY`

### All ribs synchronize (no rotation)
- Verify `initializeRibs()` sets different `phaseOffset` per strip
- Check that `rib.phaseOffset` is added to `wavePhases[wave]`

### Colors are harsh/white
- Reduce `brightnessMultiplier` (default 0.6)
- Verify HSV saturation is 200 (not 255)
- Check that `getWaveColor()` returns CHSV, not CRGB

### LEDs in wrong positions
- Verify `ledIndexForStripAndRow()` matches physical wiring
- Test with single strip lit: `leds[strip * 12 + row] = CRGB::Red`

### Performance issues
- Reduce `NUM_WAVES` from 2 to 1
- Simplify `getWaveBrightness()` curve
- Use `FastLED.setMaxRefreshRate(200)` to limit updates

---

## 📚 References

### Biological Resources
- **Sea Gooseberry (Pleurobrachia):** https://en.wikipedia.org/wiki/Pleurobrachia
- **Comb Jelly Bioluminescence:** https://www.mbari.org/products/creature-feature/comb-jellies/
- **Video Reference:** Search YouTube for "comb jelly slow motion" to see real wave patterns

### Code Integration Points
1. **main.cpp** lines ~12-14: Header includes
2. **main.cpp** line ~88: LED mode enum
3. **main.cpp** line ~36: Visualizer instance
4. **main.cpp** line ~890: Mode cycling (Lamp → Sea Gooseberry)
5. **main.cpp** line ~1423: Update call in loop()
6. **main.cpp** line ~3656: Render case in updateLEDs()

### Related Modes
- **LED_IDLE:** Simple blue wave (simpler animation)
- **LED_AMBIENT_VU:** Audio-reactive (different input)
- **LED_MEDITATION:** Breathing effect (similar slow modulation)

---

## ✅ Success Criteria

A successful implementation should:
1. ✅ Create smooth, downward-traveling waves
2. ✅ Show clear rotation effect around sphere
3. ✅ Display iridescent rainbow colors (no harsh white)
4. ✅ Feel organic and relaxing to watch
5. ✅ Run without blocking (audio/buttons still work)
6. ✅ Match real sea gooseberry behavior
7. ✅ Integrate seamlessly with existing modes

---

## 🚀 Future Enhancements

### Possible Additions
- **Audio reactivity:** Subtle speed variation with ambient sound
- **Touch sensitivity:** Ripple waves on button press
- **Multiple patterns:** "Active hunting" vs "drifting" modes
- **Color themes:** Deep sea (blues) vs shallow (greens)
- **Tentacle simulation:** Occasional trailing light strands

### Performance Optimizations
- Pre-compute sine/cosine lookup tables
- Use fixed-point math instead of float
- Batch LED updates with FastLED.setBrightness()

---

*This mode represents the most biologically accurate visualization in the JellyBerry firmware, closely modeling the natural light refraction patterns of real Pleurobrachia sea gooseberries. Enjoy watching your device come alive!* 🌊✨
