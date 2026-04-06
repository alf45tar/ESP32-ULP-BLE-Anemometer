/**
 * ======================================================================================
 * ESP32 ULTRA-LOW POWER (ULP) BLE ANEMOMETER
 * ======================================================================================
 * * DESCRIPTION:
 * This firmware transforms an ESP32 into a high-efficiency wind speed sensor.
 * It uses the ULP (Ultra-Low Power) co-processor to count pulses from a reed
 * switch/hall sensor while the main ESP32 cores are in Deep Sleep.
 * * HOW IT WORKS:
 * 1. SLEEP: The ESP32 enters Deep Sleep (consuming ~10-15µA).
 * 2. ULP MONITORING: The ULP co-processor remains active, sampling GPIO 32
 * every few milliseconds to detect magnet passes (edge detection).
 * 3. WAKEUP: Every 5 seconds, the ESP32 wakes up to process the ULP data.
 * 4. LOGIC ENGINE:
 * - If Wind Speed > 0: Calculates speed and broadcasts immediately via BLE.
 * - If Speed Changes (e.g., 2m/s -> 0m/s): Broadcasts immediately to show the drop.
 * - If No Wind: Skips BLE transmission to save battery, incrementing a counter.
 * - Heartbeat: Every 60 seconds, it forces a broadcast (even if 0m/s) so Home
 * Assistant knows the sensor is still online.
 * * DATA PROTOCOL:
 * Uses BTHome V2 (Bluetooth Low Energy). Compatible with Home Assistant
 * "Bluetooth" and "BTHome" integrations out of the box.
 * * HARDWARE NOTES:
 * - Sensor Pin: GPIO 32 (Must be an RTC-capable pin).
 * - Pulses: 2 pulses per revolution (assuming 2 magnets).
 * - Radius: 0.071m (Center to cup middle).
 * - Calibration: 2.5x factor to compensate for cup drag/aerodynamics.
 * * POWER CONSUMPTION:
 * - Deep Sleep: ~15µA
 * - BLE Broadcast (1.5s): ~100mA
 * - Estimated Battery Life (1000mAh Li-ion): >1 year with 1-minute heartbeats.
 * ======================================================================================
 */

#include <Arduino.h>
#include <BtHomeV2Device.h>
#include "NimBLEDevice.h"
#include "esp32/ulp.h"
#include "driver/rtc_io.h"
#include "soc/rtc_io_reg.h"
#include "esp_log.h"

// Logging tag
static const char *TAG = "ANEMOMETER";

// --------------------------------------------------------------------------------------
// CONFIGURATION & CALIBRATION
// --------------------------------------------------------------------------------------
#define SENSOR_GPIO            GPIO_NUM_4   // Must be RTC-capable
                                            // ESP32:    GPIO 0, 2, 4, 12-15, 25-27, 32-39
                                            // ESP32-S3: GPIO 0-21
const float RADIUS_METERS      = 0.071;     // Distance from center to the middle of the cup (e.g., 5cm = 0.05m)
const float CALIBRATION_FACTOR = 2.5;       // Aerodynamic compensation (usually between 2.0 and 3.0)
const int   PULSES_PER_REV     = 2;         // Set to 2 because we have 2 magnets
const uint64_t SLEEP_TIME_US   = 5000000;   // 5 seconds sleep
const int   HEARTBEAT_CYCLES   = 12;        // 12 * 5s = 60s update

#define BATTERY_ADC_PIN         14          // Must be an ADC-capable pin
                                            // Battery measurement (expects a resistor divider to keep ADC voltage <= 3.3V)
const float ADC_REFERENCE_VOLT  = 3.3;
const float ADC_MAX_READING     = 4095.0;
const float VOLTAGE_DIV_RATIO   = 2.0;      // Resistor divider -> battery voltage is ADC voltage * 2
const float BATTERY_MIN_VOLT    = 3.2;      // 0%
const float BATTERY_MAX_VOLT    = 4.2;      // 100%

float readBatteryVoltage() {
  uint16_t raw = analogRead(BATTERY_ADC_PIN);
  float adcVoltage = (raw / ADC_MAX_READING) * ADC_REFERENCE_VOLT;
  return adcVoltage * VOLTAGE_DIV_RATIO;
}

uint8_t batteryPercentFromVoltage(float batteryVoltage) {
  if (batteryVoltage <= BATTERY_MIN_VOLT) {
    return 0;
  }
  if (batteryVoltage >= BATTERY_MAX_VOLT) {
    return 100;
  }
  float scaled = (batteryVoltage - BATTERY_MIN_VOLT) * 100.0 / (BATTERY_MAX_VOLT - BATTERY_MIN_VOLT);
  return (uint8_t)(scaled + 0.5);
}

// --------------------------------------------------------------------------------------
// RTC MEMORY MAPPING (Persists during Deep Sleep)
// --------------------------------------------------------------------------------------
enum {
  VAR_COUNTER,      // RTC_SLOW_MEM[0] - Pulse count from ULP
  VAR_LAST_STATE,   // RTC_SLOW_MEM[1] - ULP internal pin state
  VAR_SKIP_COUNT,   // RTC_SLOW_MEM[2] - Heartbeat timer
  VAR_LAST_SPEED,   // RTC_SLOW_MEM[3] - Speed of the previous wake cycle
  PROG_START        // RTC_SLOW_MEM[4] - Start of ULP code
};

// --------------------------------------------------------------------------------------
// ULP ASSEMBLY PROGRAM
// --------------------------------------------------------------------------------------
void init_ulp_program() {
  int rtc_pin = rtc_io_number_get(SENSOR_GPIO);

  const ulp_insn_t ulp_program[] = {
    M_LABEL(0),
    // 1. Read current state of RTC GPIO
    I_RD_REG(RTC_GPIO_IN_REG, RTC_GPIO_IN_NEXT_S + rtc_pin, RTC_GPIO_IN_NEXT_S + rtc_pin),

    // 2. Load last state
    I_MOVI(R2, VAR_LAST_STATE),
    I_LD(R1, R2, 0),

    // 3. Compare current (R0) and last (R1)
    I_SUBR(R3, R0, R1),
    M_BXZ(1),           // If no change, jump to LABEL 1

    // 4. Update last state and increment counter
    I_ST(R0, R2, 0),
    I_MOVI(R2, VAR_COUNTER),
    I_LD(R1, R2, 0),
    I_ADDI(R1, R1, 1),
    I_ST(R1, R2, 0),

    M_LABEL(1),
    I_DELAY(30000),     // Sampling rate (approx 3-5ms debounce)
    M_BX(0),
  };

  memset(RTC_SLOW_MEM, 0, CONFIG_ULP_COPROC_RESERVE_MEM);
  size_t size = sizeof(ulp_program) / sizeof(ulp_insn_t);
  ulp_process_macros_and_load(PROG_START, ulp_program, &size);

  rtc_gpio_init(SENSOR_GPIO);
  rtc_gpio_set_direction(SENSOR_GPIO, RTC_GPIO_MODE_INPUT_ONLY);
  rtc_gpio_pullup_en(SENSOR_GPIO);
  rtc_gpio_pulldown_dis(SENSOR_GPIO);

  ulp_run(PROG_START);
}

// --------------------------------------------------------------------------------------
// MAIN EXECUTION (Wakeup Logic)
// --------------------------------------------------------------------------------------
void setup() {
  // Logging is already initialized by ESP-IDF at boot
  ESP_LOGI(TAG, "Anemometer setup starting...");

  analogSetPinAttenuation(BATTERY_ADC_PIN, ADC_11db);

  esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();

  if (cause == ESP_SLEEP_WAKEUP_TIMER) {
    uint32_t transitions = RTC_SLOW_MEM[VAR_COUNTER] & 0xFFFF;
    uint32_t skip_count = RTC_SLOW_MEM[VAR_SKIP_COUNT] & 0xFFFF;
    uint32_t last_speed_int = RTC_SLOW_MEM[VAR_LAST_SPEED] & 0xFFFF;

    // 1. Calculate current speed
    float totalPulses = (float)transitions / 2.0;
    float totalTurns = totalPulses / (float)PULSES_PER_REV;
    float rps = totalTurns / (SLEEP_TIME_US / 1000000.0);
    float speedMPS = rps * (2 * PI * RADIUS_METERS) * CALIBRATION_FACTOR;

    // Convert to int for easy comparison (e.g., 2.45 m/s becomes 245)
    uint32_t current_speed_int = (uint32_t)(speedMPS * 100);

    // 2. Evaluate Conditions
    bool speed_changed = (current_speed_int != last_speed_int);
    bool heartbeat = (skip_count >= (HEARTBEAT_CYCLES - 1));

    if (speed_changed || heartbeat) {
      // Broadcast logic
      RTC_SLOW_MEM[VAR_COUNTER] = 0;
      RTC_SLOW_MEM[VAR_SKIP_COUNT] = 0;
      RTC_SLOW_MEM[VAR_LAST_SPEED] = current_speed_int;

      // Start BLE advertisement
      NimBLEDevice::init("Wind Sensor");
      NimBLEAdvertising *pAdvertising = NimBLEDevice::getAdvertising();

      // Battery is sampled only when you actually advertise, not on every wake cycle
      /*
      * 1. ADC Measurement Cost:
      *    A single analog read is negligible in terms of power consumption
      *    compared to a BLE transmission. It is not the primary battery drain.
      * 2. Resistor Divider Static Drain:
      *    Assuming a 100k/100k divider, the continuous leakage
      *    current is calculated as:
      *         I ≈ 4.2V / 200,000Ω ≈ 21µA
      *    Assuming a 470k/470k divider, the continuous leakage
      *    current is calculated as:
      *         I ≈ 4.2V / 940,000Ω ≈ 4.5µA
      * This 4.5µA drain is CONTINUOUS, even during deep sleep.
      * This is such a low value that the internal chemical self-discharge
      * of the battery itself probably consumes more.
      */
      float batteryVoltage = readBatteryVoltage();
      uint8_t batteryPercent = batteryPercentFromVoltage(batteryVoltage);

      BtHomeV2Device btHome("Anemometer", "Wind Sensor", false);
      btHome.addSpeedMs(speedMPS);
      btHome.addBatteryPercentage(batteryPercent);

      uint8_t advDataRaw[MAX_ADVERTISEMENT_SIZE];
      size_t size = btHome.getAdvertisementData(advDataRaw);
      NimBLEAdvertisementData pAdvData;
      std::vector<uint8_t> data(advDataRaw, advDataRaw + size);
      pAdvData.addData(data);

      pAdvertising->setAdvertisementData(pAdvData);
      pAdvertising->start();

      ESP_LOGI(TAG, "Pulses: %d | RPS: %.2f | Wind Speed: %.2f m/s | Battery: %d%% (%.2fV) | MAC: %s (Reason: %s)",
          transitions/2, rps, speedMPS, batteryPercent, batteryVoltage, NimBLEDevice::getAddress().toString().c_str(), speed_changed ? "Change" : "Heartbeat");

      delay(1500); // Wait for scanner to catch the change
      pAdvertising->stop();
      NimBLEDevice::deinit(true);
    } else {
      // Silent cycle: increment heartbeat timer
      RTC_SLOW_MEM[VAR_SKIP_COUNT] = skip_count + 1;
    }

  } else {
    // Fresh boot
    init_ulp_program();
  }

  // Go back to sleep
  ESP_LOGI(TAG, "Entering Deep Sleep...");
  esp_sleep_enable_timer_wakeup(SLEEP_TIME_US);
  esp_deep_sleep_start();
}


void loop() {} // Unused in Deep Sleep flow
