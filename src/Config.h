#ifndef CONFIG_H
#define CONFIG_H

// WiFi Configuration
#define WIFI_SSID "YOUR_WIFI_SSID"
#define WIFI_PASSWORD "YOUR_WIFI_PASSWORD"

// Gemini API Configuration
#define GEMINI_API_KEY "AIzaSyDV0Qa53W3S9-jC7xsi0jtgm7DsVRF64Kk"
#define GEMINI_WS_HOST "generativelanguage.googleapis.com"
#define GEMINI_WS_PORT 443
#define GEMINI_MODEL "models/gemini-2.5-pro-exp-11-05"

// GPIO PIN ASSIGNMENTS - ESP32-S3
// Microphone I2S (INMP441)
#define I2S_MIC_SCK_PIN 8
#define I2S_MIC_WS_PIN 9
#define I2S_MIC_SD_PIN 10

// Speaker I2S (MAX98357A) 
#define I2S_SPEAKER_LRC_PIN 5
#define I2S_SPEAKER_BCLK_PIN 6
#define I2S_SPEAKER_DIN_PIN 7

// LED Strip (WS2812B)
#define LED_DATA_PIN 1

// Touch Sensors (TTP223)
#define TOUCH_PAD_START_PIN 3
#define TOUCH_PAD_STOP_PIN 4

// Audio Configuration
#define AUDIO_SAMPLE_RATE 16000
#define AUDIO_BITS 16
#define AUDIO_CHANNELS 1
#define AUDIO_BUFFER_SIZE 4096
#define MAX_AUDIO_BUFFER_SIZE 32768

// LED Configuration
#define NUM_LEDS 144
#define LED_BRIGHTNESS 200
#define LED_COLOR_ORDER GRB
#define LED_CHIPSET WS2812B
#define LED_COLUMNS 12
#define LEDS_PER_COLUMN 12

// Recording Configuration
#define MAX_RECORDING_DURATION_MS 10000
#define MIN_RECORDING_DURATION_MS 1000

// WebSocket Configuration
#define WS_RECONNECT_INTERVAL 5000
#define WS_TIMEOUT 30000

// System Constants
#define SERIAL_BAUD_RATE 115200
#define CORE_0 0
#define CORE_1 1

#endif // CONFIG_H
