#include <Arduino.h>
#include <WS2812FX.h>
#include <driver/i2s.h>

// ===============================
// CONFIGURATION
// ===============================

// Microphone I2S pins
#define I2S_WS 25    // LRCL / WS
#define I2S_SD 33    // DOUT
#define I2S_SCK 32   // BCLK / SCK

// LED configuration
#define LED_PIN     4
#define LED_COUNT   60
#define BRIGHTNESS  100

// Audio processing
#define I2S_PORT I2S_NUM_0
#define BUFFER_LEN 64
#define MAX_VOLUME_TARGET 3000  // Target maximum volume

int32_t sBuffer[BUFFER_LEN];
float volume = 0;
float smoothVolume = 0;
float maxVolume = MAX_VOLUME_TARGET;  // For serial plotter display

// Moving average filter for additional stability
#define FILTER_SIZE 5
float volumeFilter[FILTER_SIZE] = {0};
int filterIndex = 0;

// Calibration variables
float baselineNoise = 15000;  // Auto-calibrated on startup
#define CALIBRATION_SAMPLES 100
float dynamicScaleFactor = 2.0;  // Adjusted every 5 seconds

#define MIN_VOLUME 1500
#define SMOOTHING_FACTOR 0.8
#define UPDATE_INTERVAL 5

// Dynamic calibration (5 seconds)
#define CALIBRATION_WINDOW 5000
#define VOLUME_SAMPLES 500
float rawVolumeHistory[VOLUME_SAMPLES];
int volumeIndex = 0;
unsigned long lastCalibration = 0;

// Peak averaging for stable max range
#define PEAK_HISTORY_SIZE 10
float peakHistory[PEAK_HISTORY_SIZE];
int peakIndex = 0;
int peakCount = 0;

// LED FX engine
WS2812FX ws2812fx = WS2812FX(LED_COUNT, LED_PIN, NEO_GRB + NEO_KHZ800);

// ===============================
// PEAK AVERAGING
// ===============================
void addPeakToHistory(float peak) {
  peakHistory[peakIndex] = peak;
  peakIndex = (peakIndex + 1) % PEAK_HISTORY_SIZE;
  if (peakCount < PEAK_HISTORY_SIZE) {
    peakCount++;
  }
}

float getAveragePeak() {
  if (peakCount == 0) return 0;
  
  float sum = 0;
  for (int i = 0; i < peakCount; i++) {
    sum += peakHistory[i];
  }
  return sum / peakCount;
}

// ===============================
// CALIBRATION
// ===============================
void calibrateBaseline() {
  Serial.println("Calibrating baseline noise level...");
  Serial.println("Please keep quiet for 3 seconds...");
  
  float totalNoise = 0;
  int validSamples = 0;
  
  for (int sample = 0; sample < CALIBRATION_SAMPLES; sample++) {
    size_t bytesIn = 0;
    esp_err_t result = i2s_read(I2S_PORT, &sBuffer, BUFFER_LEN * sizeof(int32_t), &bytesIn, portMAX_DELAY);
    
    if (result == ESP_OK && bytesIn > 0) {
      int16_t samples_read = bytesIn / sizeof(int32_t);
      float sum = 0;
      int actualSamples = 0;
      
      for (int16_t i = 0; i < samples_read; ++i) {
        int32_t sample = sBuffer[i] >> 14;
        // Filter out obvious spikes during calibration
        if (abs(sample) < 50000) {
          sum += (sample * sample);
          actualSamples++;
        }
      }
      
      if (actualSamples > 0) {
        float rms = sqrt(sum / actualSamples);
        totalNoise += rms;
        validSamples++;
      }
    }
    delay(30); // 30ms delay between samples
  }
  
  if (validSamples > 0) {
    baselineNoise = totalNoise / validSamples;
    Serial.print("Baseline calibrated to: ");
    Serial.println(baselineNoise);
  } else {
    Serial.println("Calibration failed, using default");
  }
}

// ===============================
// I2S SETUP
// ===============================
void i2s_install() {
  const i2s_config_t i2s_config = {
    .mode = i2s_mode_t(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate = 44100,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,  // Changed from I2S_COMM_FORMAT_I2S
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 8,
    .dma_buf_len = BUFFER_LEN,
    .use_apll = false,
    .tx_desc_auto_clear = false,
    .fixed_mclk = 0
  };

  esp_err_t err = i2s_driver_install(I2S_PORT, &i2s_config, 0, NULL);
  if (err != ESP_OK) {
    Serial.print("I2S driver install failed: ");
    Serial.println(err);
  } else {
    Serial.println("I2S driver installed successfully");
  }
}

void i2s_setpin() {
  const i2s_pin_config_t pin_config = {
    .bck_io_num = I2S_SCK,
    .ws_io_num = I2S_WS,
    .data_out_num = -1,
    .data_in_num = I2S_SD
  };

  esp_err_t err = i2s_set_pin(I2S_PORT, &pin_config);
  if (err != ESP_OK) {
    Serial.print("I2S pin config failed: ");
    Serial.println(err);
  } else {
    Serial.println("I2S pins configured successfully");
  }
}

// ===============================
// Volume-based LED animation
// ===============================
void updateLedsByVolume() {
  ws2812fx.stop();
  ws2812fx.clear();

  int numLedsToLight = 0;

  if (smoothVolume > MIN_VOLUME) {
    // Map volume to LED count
    if (smoothVolume >= MAX_VOLUME_TARGET * 0.95f) {
      numLedsToLight = LED_COUNT;  // Full LEDs at 95% of max
    } else {
      float normalized = (smoothVolume - MIN_VOLUME) / (MAX_VOLUME_TARGET - MIN_VOLUME);
      normalized = pow(normalized, 0.7f);  // Gentle curve
      numLedsToLight = constrain(round(normalized * LED_COUNT), 1, LED_COUNT);
    }
  }

  // Light LEDs from center outward
  int center = LED_COUNT / 2;  // Center point (30 for 60 LEDs)
  
  for (int i = 0; i < numLedsToLight; i++) {
    int ledIndex;
    
    // Alternate between left and right of center
    if (i % 2 == 0) {
      // Even indices: go right from center
      ledIndex = center + (i / 2);
    } else {
      // Odd indices: go left from center
      ledIndex = center - 1 - (i / 2);
    }
    
    // Make sure we don't go out of bounds
    if (ledIndex >= 0 && ledIndex < LED_COUNT) {
      // Calculate color based on distance from center (for gradient effect)
      float distanceFromCenter = abs(ledIndex - center) / (float)(LED_COUNT / 2);
      uint32_t color = (distanceFromCenter < 0.33) ? 0x00FF00 :   // green (center)
                       (distanceFromCenter < 0.66) ? 0xFFFF00 :   // yellow (middle)
                                                      0xFF0000;    // red (edges)
      ws2812fx.setPixelColor(ledIndex, color);
    }
  }

  ws2812fx.show();
}

// ===============================
// SETUP
// ===============================
void setup() {
  Serial.begin(115200);
  delay(1000);

  // Initialize volume history array
  for (int i = 0; i < VOLUME_SAMPLES; i++) {
    rawVolumeHistory[i] = 0;
  }
  
  // Initialize peak history array
  for (int i = 0; i < PEAK_HISTORY_SIZE; i++) {
    peakHistory[i] = 0;
  }

  ws2812fx.init();
  ws2812fx.setBrightness(BRIGHTNESS);
  ws2812fx.setColor(0);              // black/off
  ws2812fx.setMode(FX_MODE_STATIC);
  ws2812fx.start();

  Serial.println("Initializing I2S for SPH0645 microphone...");
  i2s_install();
  i2s_setpin();
  
  esp_err_t err = i2s_start(I2S_PORT);
  if (err != ESP_OK) {
    Serial.print("I2S start failed: ");
    Serial.println(err);
  } else {
    Serial.println("I2S started successfully");
  }
  
  // Calibrate baseline noise level
  calibrateBaseline();
  
  Serial.println("Setup complete. Monitoring audio...");
}

// ===============================
// LOOP
// ===============================
void loop() {
  static unsigned long lastUpdate = 0;
  unsigned long now = millis();

  // Read audio data
  size_t bytesIn = 0;
  esp_err_t result = i2s_read(I2S_PORT, &sBuffer, BUFFER_LEN * sizeof(int32_t), &bytesIn, portMAX_DELAY);
  
  if (result == ESP_OK && bytesIn > 0) {
    // Calculate RMS (Root Mean Square) for better noise handling
    float sum = 0;
    int16_t samples_read = bytesIn / sizeof(int32_t);
    int validSamples = 0;
    
    for (int16_t i = 0; i < samples_read; ++i) {
      int32_t sample = sBuffer[i] >> 14;  // SPH0645: shift 14 bits
      
      // Basic spike filter - ignore extreme outliers
      if (abs(sample) < 100000) {  // Reasonable upper limit
        sum += (sample * sample);  // Square for RMS
        validSamples++;
      }
    }
    
    if (validSamples > 0) {
      float rms = sqrt(sum / validSamples);  // RMS calculation
      
      // Apply calibration and scaling
      float calibratedVolume = max(0.0f, rms - baselineNoise);
      
      // Additional noise gate - ignore very small changes
      if (calibratedVolume < 100) {
        calibratedVolume = 0;
      }
      
      float rawVolume = constrain(calibratedVolume * dynamicScaleFactor, 0.0f, MAX_VOLUME_TARGET);
      
      // Apply moving average filter
      volumeFilter[filterIndex] = rawVolume;
      filterIndex = (filterIndex + 1) % FILTER_SIZE;
      
      float filteredVolume = 0;
      for (int i = 0; i < FILTER_SIZE; i++) {
        filteredVolume += volumeFilter[i];
      }
      volume = filteredVolume / FILTER_SIZE;
      
      // Extra smoothing for stability
      static float previousVolume = 0;
      float volumeDelta = abs(volume - previousVolume);
      
      // If change is too dramatic, limit it
      if (volumeDelta > MAX_VOLUME_TARGET * 0.3f) {
        volume = previousVolume + (volume > previousVolume ? MAX_VOLUME_TARGET * 0.05f : -MAX_VOLUME_TARGET * 0.05f);
      }
      previousVolume = volume;
      
      smoothVolume = (smoothVolume * (1.0 - SMOOTHING_FACTOR)) + (volume * SMOOTHING_FACTOR);
      
      // Store for dynamic calibration
      rawVolumeHistory[volumeIndex] = calibratedVolume;
      volumeIndex = (volumeIndex + 1) % VOLUME_SAMPLES;
    }
  }

  // Update LEDs and recalibrate periodically
  if (now - lastUpdate > UPDATE_INTERVAL) {
    updateLedsByVolume();
    
    // Recalibrate scale factor every 5 seconds
    if (now - lastCalibration >= CALIBRATION_WINDOW) {
      // Find current peak in this window
      float currentMaxDetected = 0;
      for (int i = 0; i < VOLUME_SAMPLES; i++) {
        if (rawVolumeHistory[i] > currentMaxDetected) {
          currentMaxDetected = rawVolumeHistory[i];
        }
      }
      
      // Add this peak to our history (only if it's a meaningful peak)
      if (currentMaxDetected > baselineNoise * 2) {  // Only add significant peaks
        addPeakToHistory(currentMaxDetected);
      }
      
      // Use average of recent peaks for scaling instead of single max
      float averagePeak = getAveragePeak();
      if (averagePeak > 0) {
        float newScale = MAX_VOLUME_TARGET / averagePeak;
        dynamicScaleFactor = (dynamicScaleFactor * 0.8) + (newScale * 0.2);  // Even smoother transition
        maxVolume = averagePeak * dynamicScaleFactor;  // Update max for plotter
      }
      
      lastCalibration = now;
    }
    
    // Serial plotter output
    Serial.print("MinRange:");
    Serial.print(-1000);
    Serial.print("Volume:");
    Serial.print(volume);
    Serial.print(",SmoothVolume:");
    Serial.print(smoothVolume);
    Serial.print(",MaxVolume:");
    Serial.print(maxVolume);
    Serial.print(",MaxRange:");
    Serial.println(5000);
    
    lastUpdate = now;
  }

  ws2812fx.service();
}