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
