# 🌀 ESP32 ULP BLE Anemometer (BTHome V2)

A high-efficiency, battery-powered wind speed sensor (Anemometer) using the ESP32. This project leverages the **ULP (Ultra-Low Power) co-processor** to monitor wind pulses while the main CPU is in Deep Sleep, allowing for years of battery life.

## 🚀 Key Features

  * **Ultra-Low Power:** Can be as low as **\~15µA** in deep sleep on the ESP32 alone. Actual current depends on the board and any attached hardware. The main cores only wake up to calculate and transmit data.
  * **BTHome V2 Protocol:** Works natively with **Home Assistant** via Bluetooth—no custom integration or ESPHome YAML required. See [bthome.io](https://bthome.io).
  * **Intelligent Reporting:**
      * **Wind Detected:** Updates every 5 seconds.
      * **Change Detection:** Updates immediately if the wind stops (e.g., 2 m/s → 0 m/s).
      * **Heartbeat:** Sends a "still alive" update every 60 seconds during calm periods.
  * **Hardware Debouncing:** ULP-based software sampling filters out mechanical reed switch "bounce."

-----

## 🛠 Hardware Requirements

1.  **ESP32 Development Board** (e.g., DevKit V1, WROOM-32).
2.  **Anemometer** (3-cup type with a Reed Switch or Hall Effect sensor).
3.  **Battery:** Li-ion 18650 or LiPo (recommended).
4.  **Resistor:** 10kΩ (if your sensor doesn't have a built-in pull-up).

### Wiring

| Component | ESP32 Pin | Note |
| :--- | :--- | :--- |
| **Anemometer Signal** | **GPIO 32** | Must be an RTC-capable pin |
| **Anemometer GND** | **GND** | |

*Note: Avoid GPIO 0, 2, 12, and 15 for the sensor as they are "strapping pins" and can prevent the ESP32 from booting if held LOW/HIGH by the magnet.*

-----

## 💻 Installation

### 1\. Libraries Required

Install the following libraries via the Arduino Library Manager:

  * [**NimBLE-Arduino**](https://github.com/h2zero/NimBLE-Arduino) (Lightweight Bluetooth LE)
  * [**BTHomeV2**](https://www.google.com/search?q=https://github.com/Chreece/BTHomeV2) (BTHome data formatting)

### 2\. Configuration

Adjust the constants at the top of the sketch to match your hardware:

```cpp
const float RADIUS_METERS      = 0.071; // Center to cup middle
const float CALIBRATION_FACTOR = 2.5;   // Aerodynamic factor
const int   PULSES_PER_REV     = 2;     // Number of magnets in your sensor
```

### 3\. Flash

Upload the code to your ESP32. Open the Serial Monitor (115200 baud) to verify the "Fresh Boot" and "Deep Sleep" cycles.

-----

## 🏠 Home Assistant Integration

1.  Ensure the **Bluetooth** integration is active in Home Assistant.
2.  The sensor will be automatically discovered as a **BTHome** device named **"Wind Sensor"**.
3.  Add it to your dashboard. The wind speed will appear as a sensor entity in `m/s`.

-----

## 🔌 Shelly Integration

If you are using **Shelly** devices, the anemometer can also be configured as a **virtual component** in your Shelly setup. This does **not** require Home Assistant.

### Example Use Case

Use the anemometer to automatically close an awning with a **Shelly 2PM Gen3** when wind speed gets too high, without any extra wiring between the wind sensor and the Shelly device. The anemometer broadcasts wind data over BLE, and Shelly handles the automation logic on its side.

-----

## 📉 Power Consumption Profile

  * **Deep Sleep (ULP Active):** \~15µA minimum, depending on the board and attached hardware
  * **BLE Transmission (1.5s):** \~100mA
  * **Total Battery Life:** \~1-2 years on a 2500mAh 18650 cell (depending on wind frequency).

-----

## 📜 License

This project is licensed under the MIT License - see the LICENSE file for details.

-----

## 🙌 Credits

  * **NimBLE-Arduino** by [h2zero](https://github.com/h2zero)
  * **BTHome V2** protocol by [bthome.io](https://bthome.io).
