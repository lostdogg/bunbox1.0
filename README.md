Below is the definitive pinout guide to ensure your wiring matches the code perfectly, followed by a summary of everything your machine can do.
Part 1: The Master Pinout GuideAll buttons are configured with INPUT_PULLUP in the code,
meaning you wire one side of the button/pad to the ESP32 Pin, and the other side directly to GND (no external resistors needed).
1. Audio & Display
2. 1. Connections / Component / ESP32 PinNotes
3. Audio Left (Mono) / GPIO 25 / Connect to Tip of audio jack (or left speaker)
4. Audio Right / GPIO 26 / Connect to Ring of audio jack (or right speaker)
5. OLED SDA / GPIO 21 / Standard ESP32 I2C Data Pin
6. OLED SCL / GPIO 22 / Standard ESP32 I2C Clock Pin
7. Audio/Screen Ground / GNDCommon / ground for audio jacks and screen

The 12 Drum Pads
(Button Pins)
Pad Number / ESP32 Pin  / Function
Pad 1 / GPIO 13-
Pad 2 / GPIO 12-
Pad 3 / GPIO 14-
Pad 4 / GPIO 27-
Pad 5 / GPIO 15-
Pad 6 / GPIO 4-
Pad 7 / GPIO 33-
Pad 8 / GPIO 32-
Pad 9 / GPIO 19-
Pad 10 / GPIO 18-
Pad 11 / GPIO 5Game Over / Crash Sound
Pad 12 / GPIO 17 / "Shift" Key, Jump Sound

3 Control Buttons
Component / ESP32 Pin / Function
Bank Button / GPIO 0 / Action button, Jump, Game Toggle

Part 2: Funbox OS Feature Summary
🎧 1. The Audio Engine (Core 0)
Dual-Core Processing: The audio rendering is isolated entirely on ESP32 Core 0. This ensures that web traffic and screen updates never cause the audio to stutter or "pop."

10-Voice Polyphony: The sampler can play up to 10 sounds simultaneously. You can rapidly drum on the pads without cutting off the previous sounds.

Hardware Timer DAC: Audio is pushed out to Pins 25 and 26 via a strict hardware interrupt timer running at a high frequency, guaranteeing stable playback.

Flash Storage: Uses the ESP32's internal memory (LittleFS) to permanently store your uploaded .wav samples.

🌐 2. Wireless & UI (Core 1)
Standalone Wi-Fi Hotspot: The Funbox broadcasts its own network called "Funbox-Modular". You don't need a router; you connect your phone or laptop directly to the instrument.

Drag-and-Drop Web UI: Access the sampler interface via a web browser. You can grab any file saved on the machine and visually drag it onto a digital drum pad to instantly remap the sound without writing any code.

Bluetooth MIDI (BLE): The Funbox broadcasts itself as "Funbox MIDI" to your phone or computer. Any MIDI notes from C4 to B4 (Notes 60-71) will wirelessly trigger the 12 drum pads with zero latency.

Smooth OLED Refresh: The I2C bus is overclocked to 400kHz, allowing the screen to maintain a fast frame rate without bottlenecking the CPU.

🕹️ 3. The "Secret Mode" (Flappy Box)
Hardware Toggle: Holding Pad 12 and tapping the Bank Button hides the synth UI and boots up a fully playable Flappy Bird clone.

Physics Engine: Features gravity, acceleration, random pipe generation, and collision detection, running at roughly 30 Frames Per Second.

Audio Integration: The game physically talks to the sampler engine. It automatically plays whatever .wav file is loaded into Pad 12 when you jump, and plays Pad 11 when you crash.

---

## Part 3: Flashing the Funbox OS to Your ESP32

### Prerequisites

**Hardware:**
- ESP-WROOM-32S v1.1 (or compatible ESP32 dev board)
- USB data cable (Micro-USB or USB-C — must be a data cable, not charge-only)

**Software:**
- [VS Code](https://code.visualstudio.com/) with the [PlatformIO extension](https://platformio.org/install/ide?install=vscode) installed

---

### Step 1: Install VS Code + PlatformIO

1. Download and install **VS Code**.
2. Open VS Code → go to the **Extensions** tab (Ctrl+Shift+X).
3. Search for **PlatformIO IDE** and install it.
4. Restart VS Code when prompted.

---

### Step 2: Open the Project

1. In VS Code, click the **PlatformIO icon** (alien head) in the left sidebar.
2. Click **"Open Project"** and navigate to the `funbox_os/` folder in this repo.
3. PlatformIO will read `platformio.ini` and automatically detect:
   - Board: `esp32dev`
   - Framework: Arduino
   - Required libraries: Adafruit SSD1306, Adafruit GFX, Adafruit BusIO

---

### Step 3: Install Dependencies

PlatformIO installs libraries automatically on first build. To force it manually, open the PlatformIO terminal and run:

```
pio pkg install
```

---

### Step 4: Connect Your ESP32

1. Plug your ESP32 into your computer via USB.
2. Confirm the driver is installed:
   - **Windows:** Check Device Manager for a **CP2102** or **CH340** COM port.
   - **Mac:** Run `ls /dev/tty.*` in Terminal and look for a new entry.
   - **Linux:** Run `ls /dev/ttyUSB*` and look for a new entry.
3. If no port appears, install the USB-Serial driver for your board:
   - [CP2102 driver (SiLabs)](https://www.silabs.com/developers/usb-to-uart-bridge-vcp-drivers)
   - [CH340 driver](https://www.wch-ic.com/downloads/CH341SER_EXE.html)

---

### Step 5: Build and Flash the Firmware

1. In VS Code, click the **PlatformIO icon** → **Upload** (the right-arrow icon), OR run in the terminal:
   ```
   pio run --target upload
   ```
2. PlatformIO compiles the code and flashes it at 921,600 baud.
3. If the upload times out or fails, **hold the BOOT button** on the ESP32 while the upload begins, then release it once it starts transferring.

---

### Step 6: Upload the Filesystem (LittleFS)

The firmware uses **LittleFS** to store `.wav` sample files on the ESP32's internal flash. Flash the filesystem image separately:

```
pio run --target uploadfs
```

Or in VS Code: PlatformIO sidebar → **Platform** → **Upload Filesystem Image**.

> Do this even if the `data/` folder is currently empty — it initializes the LittleFS partition so the web UI can store and manage samples.

---

### Step 7: Verify It's Working

1. Open the **Serial Monitor** at **115,200 baud**:
   ```
   pio device monitor
   ```
2. Press the **Reset (EN)** button on your ESP32 and watch for boot messages.
3. On your phone or laptop, look for a Wi-Fi network named **`Funbox-Modular`** — this confirms the firmware is running correctly.
4. Connect to that network and open **`http://192.168.4.1`** in a browser to access the drag-and-drop sample UI.

---

### Quick Reference

| Task | Command |
|------|---------|
| Install libraries | `pio pkg install` |
| Build only | `pio run` |
| Flash firmware | `pio run --target upload` |
| Flash filesystem | `pio run --target uploadfs` |
| Open serial monitor | `pio device monitor` |

---

### Troubleshooting

| Problem | Fix |
|---------|-----|
| "No device found" | Install CP2102/CH340 driver; try a different USB cable |
| Upload fails / times out | Hold **BOOT** on the ESP32 while the upload starts |
| LittleFS errors in Serial | Run `pio run --target uploadfs` to initialize the partition |
| Libraries not found | Run `pio pkg install` or let PlatformIO auto-resolve on first build |
