# ESP32 BLE + I²C workshop firmware

ESP-IDF 5.2 example for **ESP32-C3, ESP32-C6 and ESP32-H2**. ESP32-S2 cannot run this BLE firmware because it has no Bluetooth radio. The phone uses **nRF Connect for Mobile**, so a custom phone app is not required. This is a BLE peripheral/server example; firmware for a second ESP32 BLE central/client is not included.

## Hardware

- ESP32-C3, C6 or H2 development board and USB cable.
- External LED **with series resistor** connected to the configured LED GPIO and GND (or set the board's onboard LED GPIO and active-low setting). Never drive a bare LED without a resistor.
- **BH1750** light sensor breakout, with 3.3 V supply, common GND, SDA and SCL. Its I²C address is usually `0x23` (or `0x5c` depending on the ADDR connection). Use external SDA/SCL pull-ups to **3.3 V**, typically 4.7 kΩ, unless the breakout already includes them. Do not connect 5 V signals to ESP32 GPIO.
- Default *example* pins: LED GPIO 7, SDA GPIO 4, SCL GPIO 5. **Check the pinout and boot/USB/strapping pin restrictions on your exact board before wiring.** Change pins using `idf.py menuconfig` → `Workshop hardware`. Built-in LEDs may be addressable RGB LEDs rather than simple GPIO LEDs; use an external LED if so.

## Build and flash

Open this folder in VS Code with the Espressif IDF extension and select the installed ESP-IDF 5.2.x toolchain. Choose the target for the actual BLE board, then adjust the three pins in `Workshop hardware`. In an activated ESP-IDF terminal:

```sh
idf.py set-target esp32c3      # or esp32c6 / esp32h2; creates target-specific sdkconfig
idf.py menuconfig              # Workshop hardware: GPIOs, LED polarity, sensor address
idf.py build
idf.py -p YOUR_PORT flash monitor
```

When switching targets, `set-target` resets `sdkconfig`; recheck the workshop GPIO and address settings. For different board variants, use separate builds or record each board's configuration.

At startup the serial monitor lists responding 7-bit I²C addresses and prints a unique BLE name (`WS-XXXXXX`). The sensor task retries if no BH1750 is detected: LED control still works and the sensor characteristic reports `FF FF` until a valid measurement is received. `FF FF` is an unavailable-sensor marker, **not** a lux value. A responding I²C address alone does not establish that a device is a BH1750.

## nRF Connect test

1. Enable Bluetooth on your phone, open nRF Connect, scan for the exact `WS-XXXXXX` name printed on the serial monitor and connect.
2. Find service `e1630000-7cbe-4c7c-9eb3-74769682d091`.
3. Write a single byte `01` (hex) to characteristic `e1630001-7cbe-4c7c-9eb3-74769682d091` to turn the LED on, or `00` to turn it off. Read it back to check the current state. Values other than 0/1 and writes longer than one byte are rejected.
4. Read characteristic `e1630002-7cbe-4c7c-9eb3-74769682d091` or enable notifications. Its two bytes are an **unsigned little-endian lux value** (for example, `64 00` means 100 lux). `FF FF` means unavailable. Shine a light on the BH1750 to see the value change; compare it with serial logs.
5. Disconnect and scan again to check that advertising restarts.

The GATT operations are deliberately unauthenticated for a supervised classroom demo. Anyone nearby can connect and write the LED value; add pairing/access controls before using this design for a real light or actuator. Use a suitable driver, isolation and power supply for a mains-powered lamp; **never attach mains voltage directly to an ESP32 GPIO**.

## When the sensor is selected

The sensor-specific logic is in `sensor_task()` in `main/workshop_main.c`: replace the BH1750 initialization and raw-to-lux conversion with the selected sensor's driver, and update the characteristic's documented units/payload. The BLE LED flow and GAP/GATT service can remain the same. The I²C address scan helps confirm the wiring but does not read arbitrary unknown sensors.
