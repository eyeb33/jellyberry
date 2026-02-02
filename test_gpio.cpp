// Simple GPIO test - upload this to check if basic GPIO output works
// This will help determine if the board is damaged or if it's a FastLED issue

#include <Arduino.h>

#define TEST_PIN_1 16  // Test pin 1
#define TEST_PIN_2 17  // Test pin 2
#define LED_PIN 2      // Your LED data pin

void setup() {
    Serial.begin(115200);
    delay(1000);
    Serial.println("\n\n=== GPIO OUTPUT TEST ===");
    
    // Configure test pins as outputs
    pinMode(TEST_PIN_1, OUTPUT);
    pinMode(TEST_PIN_2, OUTPUT);
    pinMode(LED_PIN, OUTPUT);
    
    // Set all pins HIGH
    digitalWrite(TEST_PIN_1, HIGH);
    digitalWrite(TEST_PIN_2, HIGH);
    digitalWrite(LED_PIN, HIGH);
    
    Serial.println("✓ Test pins configured as OUTPUT");
    Serial.println("✓ All pins set HIGH (3.3V)");
    Serial.println("\nMeasure voltages:");
    Serial.printf("  GPIO %d to GND: should be ~3.3V\n", TEST_PIN_1);
    Serial.printf("  GPIO %d to GND: should be ~3.3V\n", TEST_PIN_2);
    Serial.printf("  GPIO %d to GND: should be ~3.3V (LED pin)\n", LED_PIN);
    Serial.println("\nPins will toggle every 2 seconds...");
}

void loop() {
    static bool state = true;
    static uint32_t lastToggle = 0;
    
    if (millis() - lastToggle > 2000) {
        state = !state;
        
        digitalWrite(TEST_PIN_1, state);
        digitalWrite(TEST_PIN_2, state);
        digitalWrite(LED_PIN, state);
        
        Serial.printf("[%lu ms] Pins set to: %s (%s)\n", 
                     millis(), 
                     state ? "HIGH" : "LOW",
                     state ? "3.3V" : "0V");
        
        lastToggle = millis();
    }
    
    delay(10);
}
