/*
 * ============================================================
 *  Funbox OS v1.0
 *  Target : ESP-WROOM-32S v1.1
 * ============================================================
 *
 *  PIN ASSIGNMENTS
 *  ---------------
 *  Audio L   GPIO 25  (DAC channel 1 — tip of jack)
 *  Audio R   GPIO 26  (DAC channel 2 — ring of jack)
 *  OLED SDA  GPIO 21
 *  OLED SCL  GPIO 22
 *
 *  Pad 1  GPIO 13      Pad 7  GPIO 33
 *  Pad 2  GPIO 12      Pad 8  GPIO 32
 *  Pad 3  GPIO 14      Pad 9  GPIO 19
 *  Pad 4  GPIO 27      Pad 10 GPIO 18
 *  Pad 5  GPIO 15      Pad 11 GPIO 5   (crash / game-over sound)
 *  Pad 6  GPIO 4       Pad 12 GPIO 17  (shift / hit sound)
 *
 *  Bank button  GPIO 0
 *
 *  All pads wired: ESP32 pin → button → GND  (INPUT_PULLUP, active LOW)
 *
 *  FEATURES
 *  --------
 *  • 10-voice polyphonic sample playback
 *  • Hardware-timer DAC audio @ ~22 kHz (Core 0)
 *  • LittleFS WAV sample storage
 *  • WiFi AP "Funbox-Modular" → web UI at 192.168.4.1
 *    – Drag-and-drop sample assignment
 *    – File upload / delete
 *  • BLE MIDI "Funbox MIDI"  (MIDI notes 60–71 → pads 1–12)
 *  • SSD1306 OLED 128×64 via I²C @ 400 kHz
 *  • Secret Guitar Hero mode
 *    – Toggle: hold Pad 12 + tap Bank Button
 *    – 5-lane note highway on OLED
 *    – Pad 12 sample plays on successful hit
 *    – Pad 11 sample plays on miss / game-over
 * ============================================================
 */

// ── Libraries ────────────────────────────────────────────────
#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <LittleFS.h>
#include <WiFi.h>
#include <WebServer.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <driver/dac.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>


// ── Configuration ─────────────────────────────────────────────
#define WIFI_SSID          "Funbox-Modular"
#define BLE_DEVICE_NAME    "Funbox MIDI"
#define SAMPLE_RATE_HZ     22050          // target playback rate
#define MAX_VOICES         10
#define MAX_SAMPLE_BYTES   (48 * 1024)    // 48 KB ceiling per pad (~2 s)
#define DISPLAY_FPS        30


// ── Pin map ───────────────────────────────────────────────────
static const uint8_t PAD_PINS[12] = {
    13, 12, 14, 27, 15, 4, 33, 32, 19, 18, 5, 17
};
#define BANK_PIN    0


// ── OLED ──────────────────────────────────────────────────────
#define SCREEN_W    128
#define SCREEN_H    64
#define OLED_ADDR   0x3C
Adafruit_SSD1306 oled(SCREEN_W, SCREEN_H, &Wire, -1);


// ╔══════════════════════════════════════════════════════════════╗
// ║  AUDIO ENGINE                                               ║
// ╚══════════════════════════════════════════════════════════════╝

struct Voice {
    volatile bool     active;
    const uint8_t    *data;
    volatile uint32_t pos;
    uint32_t          length;
};

struct Sample {
    uint8_t *data;       // heap-allocated, internal DRAM
    uint32_t length;     // total 8-bit mono samples stored
    bool     loaded;
    char     filename[64];
};

static Voice   voices[MAX_VOICES];
static Sample  samples[12];
static char    padMap[12][64];   // filename currently mapped to each pad

static hw_timer_t    *audioTimer = nullptr;
static portMUX_TYPE   audioMux   = portMUX_INITIALIZER_UNLOCKED;

// Hardware-timer ISR — runs on Core 0 (pinned via timer group 0)
void IRAM_ATTR onAudioTimer()
{
    portENTER_CRITICAL_ISR(&audioMux);

    int32_t mix   = 0;
    int     count = 0;

    for (int i = 0; i < MAX_VOICES; i++) {
        if (!voices[i].active) continue;
        mix += (int32_t)(voices[i].data[voices[i].pos]) - 128;
        voices[i].pos++;
        if (voices[i].pos >= voices[i].length) {
            voices[i].active = false;
        }
        count++;
    }

    // Soft-clip normalisation: avoids harsh square-wave clipping
    int32_t out;
    if (count > 1) {
        // tanh approximation: y = x / (1 + |x|/128), centred at 128
        out = mix * 128 / (128 + (mix < 0 ? -mix : mix)) + 128;
    } else {
        out = mix + 128;
    }
    if (out > 255) out = 255;
    if (out < 0)   out = 0;

    dac_output_voltage(DAC_CHANNEL_1, (uint8_t)out);
    dac_output_voltage(DAC_CHANNEL_2, (uint8_t)out);

    portEXIT_CRITICAL_ISR(&audioMux);
}

// Trigger playback of the sample assigned to padIndex (0-based)
static void triggerPad(int padIndex)
{
    if (padIndex < 0 || padIndex >= 12) return;
    if (!samples[padIndex].loaded)      return;

    // Find a free voice; if none, steal the most-advanced one
    int freeVoice  = -1;
    uint32_t oldest = 0;
    int  oldestIdx  = 0;

    portENTER_CRITICAL(&audioMux);
    for (int i = 0; i < MAX_VOICES; i++) {
        if (!voices[i].active) { freeVoice = i; break; }
        if (voices[i].pos > oldest) { oldest = voices[i].pos; oldestIdx = i; }
    }
    if (freeVoice == -1) freeVoice = oldestIdx;

    voices[freeVoice].data   = samples[padIndex].data;
    voices[freeVoice].length = samples[padIndex].length;
    voices[freeVoice].pos    = 0;
    voices[freeVoice].active = true;
    portEXIT_CRITICAL(&audioMux);
}

// Load a .wav file from LittleFS into RAM, convert to 8-bit mono PCM
static bool loadWav(int padIndex, const char *filename)
{
    char path[80];
    snprintf(path, sizeof(path), "/%s", filename);

    File f = LittleFS.open(path, "r");
    if (!f) return false;

    // ── Parse RIFF/WAVE header ────────────────────────────────
    uint8_t hdr[12];
    if (f.read(hdr, 12) != 12) { f.close(); return false; }
    if (memcmp(hdr, "RIFF", 4) != 0 || memcmp(hdr + 8, "WAVE", 4) != 0) {
        f.close(); return false;
    }

    // Scan chunks for "fmt " and "data"
    uint16_t channels      = 1;
    uint32_t fileSampleRate = SAMPLE_RATE_HZ;
    uint16_t bitsPerSample = 8;
    uint32_t dataOffset    = 0;
    uint32_t dataSize      = 0;

    while (f.available() > 8) {
        char      chunkId[4];
        uint32_t  chunkSize;
        f.read((uint8_t *)chunkId,    4);
        f.read((uint8_t *)&chunkSize, 4);

        if (memcmp(chunkId, "fmt ", 4) == 0) {
            uint8_t fmt[16];
            uint32_t toRead = chunkSize < 16 ? chunkSize : 16;
            f.read(fmt, toRead);
            if (chunkSize > 16) f.seek(f.position() + (chunkSize - 16));

            uint16_t audioFmt;
            memcpy(&audioFmt, fmt,      2);
            memcpy(&channels, fmt + 2,  2);
            memcpy(&fileSampleRate, fmt + 4, 4);
            memcpy(&bitsPerSample, fmt + 14, 2);

            if (audioFmt != 1) { f.close(); return false; } // PCM only

        } else if (memcmp(chunkId, "data", 4) == 0) {
            dataOffset = f.position();
            dataSize   = chunkSize;
            break;

        } else {
            // Skip unknown chunk (word-align)
            f.seek(f.position() + chunkSize + (chunkSize & 1));
        }
    }

    if (dataSize == 0) { f.close(); return false; }

    uint32_t bytesPerFrame = channels * (bitsPerSample / 8);
    uint32_t totalFrames   = dataSize / bytesPerFrame;
    uint32_t storeFrames   = totalFrames < MAX_SAMPLE_BYTES
                             ? totalFrames : MAX_SAMPLE_BYTES;

    // Free previous allocation
    if (samples[padIndex].data) {
        portENTER_CRITICAL(&audioMux);
        for (int v = 0; v < MAX_VOICES; v++) {
            if (voices[v].active && voices[v].data == samples[padIndex].data)
                voices[v].active = false;
        }
        portEXIT_CRITICAL(&audioMux);
        heap_caps_free(samples[padIndex].data);
        samples[padIndex].data = nullptr;
    }

    // Allocate from internal DRAM (ISR-accessible, no cache penalty)
    samples[padIndex].data =
        (uint8_t *)heap_caps_malloc(storeFrames,
                                    MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    if (!samples[padIndex].data) { f.close(); return false; }

    f.seek(dataOffset);

    for (uint32_t i = 0; i < storeFrames; i++) {
        if (bitsPerSample == 8 && channels == 1) {
            samples[padIndex].data[i] = (uint8_t)f.read();

        } else if (bitsPerSample == 16 && channels == 1) {
            uint8_t raw[2]; f.read(raw, 2);
            int16_t s16; memcpy(&s16, raw, 2);
            samples[padIndex].data[i] = (uint8_t)(s16 / 256 + 128);

        } else if (bitsPerSample == 16 && channels == 2) {
            uint8_t raw[4]; f.read(raw, 4);
            int16_t l, r; memcpy(&l, raw, 2); memcpy(&r, raw + 2, 2);
            samples[padIndex].data[i] = (uint8_t)(((int32_t)l + r) / 512 + 128);

        } else if (bitsPerSample == 8 && channels == 2) {
            uint8_t l = (uint8_t)f.read();
            uint8_t r = (uint8_t)f.read();
            samples[padIndex].data[i] = (uint8_t)(((int)l + r) / 2);

        } else {
            samples[padIndex].data[i] = 128; // silence for unsupported format
        }
    }

    f.close();
    samples[padIndex].length = storeFrames;
    samples[padIndex].loaded = true;
    strncpy(samples[padIndex].filename, filename,
            sizeof(samples[padIndex].filename) - 1);
    strncpy(padMap[padIndex], filename, sizeof(padMap[padIndex]) - 1);
    return true;
}

static void audioSetup()
{
    dac_output_enable(DAC_CHANNEL_1);
    dac_output_enable(DAC_CHANNEL_2);
    dac_output_voltage(DAC_CHANNEL_1, 128);
    dac_output_voltage(DAC_CHANNEL_2, 128);

    // Timer 0, prescaler 80 → 1 MHz tick; alarm 45 → ~22 222 Hz
    audioTimer = timerBegin(0, 80, true);
    timerAttachInterrupt(audioTimer, &onAudioTimer, true);
    timerAlarmWrite(audioTimer, 45, true);
    timerAlarmEnable(audioTimer);
}


// ╔══════════════════════════════════════════════════════════════╗
// ║  PAD CONFIG PERSISTENCE                                     ║
// ╚══════════════════════════════════════════════════════════════╝

#define PAD_CONFIG_PATH "/padconfig.txt"

static void savePadConfig()
{
    File f = LittleFS.open(PAD_CONFIG_PATH, "w");
    if (!f) return;
    for (int i = 0; i < 12; i++) {
        if (padMap[i][0] != '\0')
            f.printf("%d=%s\n", i, padMap[i]);
    }
    f.close();
}

static void loadPadConfig()
{
    File f = LittleFS.open(PAD_CONFIG_PATH, "r");
    if (!f) return;
    while (f.available()) {
        String line = f.readStringUntil('\n');
        line.trim();
        int sep = line.indexOf('=');
        if (sep < 1) continue;
        int padIdx = line.substring(0, sep).toInt();
        String fname = line.substring(sep + 1);
        if (padIdx >= 0 && padIdx < 12 && fname.length() > 0)
            loadWav(padIdx, fname.c_str());
    }
    f.close();
}


// ╔══════════════════════════════════════════════════════════════╗
// ║  GUITAR HERO — SECRET MODE                                  ║
// ╚══════════════════════════════════════════════════════════════╝
//
//  Layout on 128×64 OLED
//  ┌────────────────────────────────┐
//  │ Score: 1250        x7 streak  │  ← 8 px header
//  ├──┬──┬──┬──┬──┬────────────────┤
//  │1 │2 │3 │4 │5 │                │  ← 47 px highway (notes fall ↓)
//  │  │██│  │  │██│                │
//  │  │  │  │██│  │                │
//  ├──HIT LINE (y=56)───────────────┤
//  │[1][2][3][4][5]                │  ← 8 px fret row (lit when pressed)
//  └────────────────────────────────┘
//
//  Lane 0-4 map to Pads 1-5 (GPIO 13,12,14,27,15)
//  Pad 12 (index 11) sample = hit sound
//  Pad 11 (index 10) sample = miss / game-over sound

#define GH_LANES       5
#define GH_LANE_W      24
#define GH_LANE_GAP    1
#define GH_NOTE_H      4
#define GH_HIGHWAY_TOP 9
#define GH_HIT_Y       54
#define GH_HIT_WINDOW  7    // ±px around hit line that counts
#define GH_NOTE_SPEED  2    // px per frame
#define GH_MAX_NOTES   24

enum GHState { GHS_IDLE, GHS_PLAYING, GHS_GAMEOVER };

struct GHNote {
    int8_t  lane;
    int16_t y;
    bool    hit;
    bool    missed;
};

static GHNote   ghNotes[GH_MAX_NOTES];
static GHState  ghState        = GHS_IDLE;
static int32_t  ghScore        = 0;
static int16_t  ghStreak       = 0;
static int16_t  ghBestStreak   = 0;
static uint32_t ghLastFrameMs  = 0;
static uint32_t ghNextNoteMs   = 0;
static uint32_t ghNoteInterval = 800;   // ms between spawns (decreases over time)
static int16_t  ghLives        = 3;

// Pad state for the current frame (updated in readButtons)
static bool padPressed[12];
static bool padJustPressed[12];

static inline int ghLaneX(int lane)
{
    // 4 px left margin; each lane is GH_LANE_W + GH_LANE_GAP wide
    return 4 + lane * (GH_LANE_W + GH_LANE_GAP);
}

static void ghResetNotes()
{
    for (int i = 0; i < GH_MAX_NOTES; i++) {
        ghNotes[i].lane   = 0;
        ghNotes[i].y      = 200;  // off-screen
        ghNotes[i].hit    = true;
        ghNotes[i].missed = true;
    }
}

static void ghSpawnNote()
{
    for (int i = 0; i < GH_MAX_NOTES; i++) {
        if (ghNotes[i].hit || ghNotes[i].missed) {
            ghNotes[i].lane   = (int8_t)random(0, GH_LANES);
            ghNotes[i].y      = GH_HIGHWAY_TOP;
            ghNotes[i].hit    = false;
            ghNotes[i].missed = false;
            return;
        }
    }
}

static void ghStart()
{
    ghResetNotes();
    ghScore        = 0;
    ghStreak       = 0;
    ghLives        = 3;
    ghNoteInterval = 800;
    ghState        = GHS_PLAYING;
    ghLastFrameMs  = millis();
    ghNextNoteMs   = millis() + 1000; // brief pause before first note
}

static void ghUpdate()
{
    uint32_t now = millis();
    if (now - ghLastFrameMs < (1000 / DISPLAY_FPS)) return;
    ghLastFrameMs = now;

    if (ghState != GHS_PLAYING) return;

    // Spawn notes on a timer
    if (now >= ghNextNoteMs) {
        ghNextNoteMs = now + ghNoteInterval;
        ghSpawnNote();
        // Occasional chord (15 % chance of a second simultaneous note)
        if (random(100) < 15) ghSpawnNote();
        // Gradually speed up
        if (ghNoteInterval > 300) ghNoteInterval -= 3;
    }

    // Advance all active notes
    for (int i = 0; i < GH_MAX_NOTES; i++) {
        if (ghNotes[i].hit || ghNotes[i].missed) continue;
        ghNotes[i].y += GH_NOTE_SPEED;

        // Past the hit zone without being hit → miss
        if (ghNotes[i].y > GH_HIT_Y + GH_HIT_WINDOW + GH_NOTE_H) {
            ghNotes[i].missed = true;
            ghStreak = 0;
            ghLives--;
            triggerPad(10);  // Pad 11 = crash sound
            if (ghLives <= 0) {
                ghState = GHS_GAMEOVER;
                if (ghStreak > ghBestStreak) ghBestStreak = ghStreak;
            }
        }
    }

    // Hit detection — check each lane pressed this frame
    for (int lane = 0; lane < GH_LANES; lane++) {
        if (!padJustPressed[lane]) continue;

        int best     = -1;
        int bestDist = GH_HIT_WINDOW + 1;

        for (int i = 0; i < GH_MAX_NOTES; i++) {
            if (ghNotes[i].hit || ghNotes[i].missed) continue;
            if (ghNotes[i].lane != lane)             continue;
            int dist = abs(ghNotes[i].y - GH_HIT_Y);
            if (dist <= GH_HIT_WINDOW && dist < bestDist) {
                bestDist = dist;
                best     = i;
            }
        }

        if (best >= 0) {
            ghNotes[best].hit = true;
            ghStreak++;
            if (ghStreak > ghBestStreak) ghBestStreak = ghStreak;
            ghScore += 100 + ghStreak * 10;  // combo multiplier
            triggerPad(11);  // Pad 12 = hit sound
        }
    }
}

static void ghDraw()
{
    oled.clearDisplay();
    char buf[32];

    // ── IDLE screen ───────────────────────────────────────────
    if (ghState == GHS_IDLE) {
        oled.setTextSize(1);
        oled.setTextColor(SSD1306_WHITE);
        oled.setCursor(14, 4);  oled.print("GUITAR  BOX");
        oled.setCursor(0,  20); oled.print("Bank = Start");
        oled.setCursor(0,  32); oled.print("Pads 1-5 = Frets");
        oled.setCursor(0,  44); oled.print("PAD12+Bank = Exit");
        if (ghBestStreak > 0) {
            snprintf(buf, sizeof(buf), "Best streak: %d", ghBestStreak);
            oled.setCursor(0, 56);
            oled.print(buf);
        }
        oled.display();
        return;
    }

    // ── GAME OVER screen ──────────────────────────────────────
    if (ghState == GHS_GAMEOVER) {
        oled.setTextSize(1);
        oled.setTextColor(SSD1306_WHITE);
        oled.setCursor(25, 4);  oled.print("GAME  OVER");
        snprintf(buf, sizeof(buf), "Score:  %ld", (long)ghScore);
        oled.setCursor(10, 20); oled.print(buf);
        snprintf(buf, sizeof(buf), "Best x: %d", ghBestStreak);
        oled.setCursor(10, 32); oled.print(buf);
        oled.setCursor(5,  48); oled.print("Bank = Play Again");
        oled.display();
        return;
    }

    // ── PLAYING screen ────────────────────────────────────────

    // Header: score + streak + lives
    oled.setTextSize(1);
    oled.setTextColor(SSD1306_WHITE);
    snprintf(buf, sizeof(buf), "%ld", (long)ghScore);
    oled.setCursor(0, 0);
    oled.print(buf);

    if (ghStreak > 1) {
        snprintf(buf, sizeof(buf), "x%d", ghStreak);
        oled.setCursor(60, 0);
        oled.print(buf);
    }

    // Lives (hearts)
    for (int l = 0; l < ghLives; l++) {
        oled.setCursor(104 + l * 8, 0);
        oled.print("\x03"); // heart char in some font tables; fallback to 'v'
    }

    // Lane separator lines (vertical)
    for (int l = 0; l <= GH_LANES; l++) {
        int x = 4 + l * (GH_LANE_W + GH_LANE_GAP);
        oled.drawFastVLine(x - 1, GH_HIGHWAY_TOP,
                           GH_HIT_Y - GH_HIGHWAY_TOP, SSD1306_WHITE);
    }

    // Hit line
    oled.drawFastHLine(4, GH_HIT_Y,
                       GH_LANES * (GH_LANE_W + GH_LANE_GAP), SSD1306_WHITE);

    // Falling notes
    for (int i = 0; i < GH_MAX_NOTES; i++) {
        if (ghNotes[i].hit || ghNotes[i].missed) continue;
        int16_t y = ghNotes[i].y;
        if (y + GH_NOTE_H < GH_HIGHWAY_TOP || y > GH_HIT_Y + GH_NOTE_H) continue;
        int x = ghLaneX(ghNotes[i].lane);
        oled.fillRect(x, y, GH_LANE_W, GH_NOTE_H, SSD1306_WHITE);
    }

    // Fret buttons at bottom (filled = pressed)
    for (int l = 0; l < GH_LANES; l++) {
        int x = ghLaneX(l);
        if (padPressed[l])
            oled.fillRect(x, SCREEN_H - 8, GH_LANE_W, 7, SSD1306_WHITE);
        else
            oled.drawRect(x, SCREEN_H - 8, GH_LANE_W, 7, SSD1306_WHITE);
    }

    oled.display();
}


// ╔══════════════════════════════════════════════════════════════╗
// ║  BLE MIDI                                                   ║
// ╚══════════════════════════════════════════════════════════════╝

// Standard MIDI-over-BLE service and characteristic UUIDs
#define MIDI_SVC_UUID  "03B80E5A-EDE8-4B33-A751-6CE34EC4C700"
#define MIDI_CHAR_UUID "7772E5DB-3868-4112-A1A9-F2669D106BF3"

static BLEServer         *bleServer  = nullptr;
static BLECharacteristic *midiCharac = nullptr;
static bool               bleConnected = false;

class BLEServerCB : public BLEServerCallbacks {
    void onConnect(BLEServer *)    override { bleConnected = true; }
    void onDisconnect(BLEServer *s) override {
        bleConnected = false;
        s->getAdvertising()->start();
    }
};

class MIDICharCB : public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic *c) override {
        std::string val = c->getValue();
        // BLE MIDI packet: [header][timestamp][status][d1][d2]
        if (val.length() < 3) return;
        // Skip the 1-byte header and 1-byte timestamp
        size_t off = (val.length() >= 5) ? 2 : 0;
        uint8_t status = (uint8_t)val[off];
        if ((status & 0xF0) == 0x90 && val.length() >= off + 3) {
            uint8_t note = (uint8_t)val[off + 1];
            uint8_t vel  = (uint8_t)val[off + 2];
            if (vel > 0 && note >= 60 && note <= 71)
                triggerPad(note - 60);
        }
    }
};

static void bleSetup()
{
    BLEDevice::init(BLE_DEVICE_NAME);
    bleServer = BLEDevice::createServer();
    bleServer->setCallbacks(new BLEServerCB());

    BLEService *svc = bleServer->createService(MIDI_SVC_UUID);
    midiCharac = svc->createCharacteristic(
        MIDI_CHAR_UUID,
        BLECharacteristic::PROPERTY_READ    |
        BLECharacteristic::PROPERTY_WRITE   |
        BLECharacteristic::PROPERTY_NOTIFY  |
        BLECharacteristic::PROPERTY_WRITE_NR
    );
    midiCharac->addDescriptor(new BLE2902());
    midiCharac->setCallbacks(new MIDICharCB());
    svc->start();

    BLEAdvertising *adv = BLEDevice::getAdvertising();
    adv->addServiceUUID(MIDI_SVC_UUID);
    adv->setScanResponse(false);
    adv->setMinPreferred(0x06);
    BLEDevice::startAdvertising();
}


// ╔══════════════════════════════════════════════════════════════╗
// ║  WEB SERVER + WEB UI                                        ║
// ╚══════════════════════════════════════════════════════════════╝

static WebServer httpServer(80);

// ── Embedded HTML/JS ─────────────────────────────────────────
// Served directly from flash (PROGMEM) — no LittleFS needed
static const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Funbox Modular</title>
<style>
  *{box-sizing:border-box;margin:0;padding:0}
  body{background:#0d0d0d;color:#e0e0e0;font-family:monospace;padding:12px}
  h1{color:#00ff88;font-size:1.3em;margin-bottom:8px}
  .subtitle{color:#555;font-size:.75em;text-transform:uppercase;
            letter-spacing:2px;margin:12px 0 6px}
  #status{color:#ffcc00;min-height:1.4em;margin:6px 0;font-size:.85em}
  .pads{display:grid;grid-template-columns:repeat(4,1fr);gap:8px;
        margin-bottom:16px}
  .pad{background:#1a1a1a;border:2px solid #333;border-radius:6px;
       padding:8px 4px;text-align:center;min-height:58px;
       display:flex;flex-direction:column;justify-content:center;
       cursor:default;transition:border-color .15s}
  .pad.dragover{border-color:#00ff88;background:#0a1f14}
  .pad.loaded{border-color:#005522}
  .pad-num{font-size:1em;font-weight:bold;color:#555}
  .pad-file{color:#00cc66;font-size:.7em;margin-top:4px;
            word-break:break-all;min-height:1em}
  .files{display:flex;flex-direction:column;gap:4px;margin-bottom:12px}
  .fitem{background:#141428;border:1px solid #2a2a4a;border-radius:4px;
         padding:6px 10px;cursor:grab;display:flex;align-items:center;gap:8px}
  .fitem:hover{border-color:#444}
  .fitem:active{cursor:grabbing}
  .ficon{color:#4488ff}
  .fname{flex:1;font-size:.85em}
  .fdel{color:#ff4444;cursor:pointer;padding:0 4px;font-size:1.1em}
  .fdel:hover{color:#ff8888}
  .btn{background:#003311;border:1px solid #006622;color:#00ff88;
       padding:7px 14px;border-radius:4px;cursor:pointer;
       font-family:monospace;font-size:.85em;margin:3px}
  .btn:hover{background:#004418}
  input[type=file]{display:none}
  .info{color:#444;font-size:.75em;margin-top:14px;line-height:1.6}
</style>
</head>
<body>
<h1>&#127911; Funbox Modular</h1>
<div id="status">Ready — connect to Funbox-Modular Wi-Fi</div>

<div class="subtitle">Drum Pads &mdash; drag a sample onto a pad to assign it</div>
<div class="pads" id="pads"></div>

<div class="subtitle">Samples on device</div>
<div class="files" id="files"></div>

<button class="btn" onclick="document.getElementById('fu').click()">
  &#128194; Upload .wav
</button>
<input type="file" id="fu" accept=".wav,.WAV" multiple onchange="uploadFiles(this.files)">
<button class="btn" onclick="refresh()">&#8635; Refresh</button>

<div class="info">
  &#8226; Drag a file from the list onto any pad number to map it.<br>
  &#8226; BLE MIDI: notes C4&ndash;B4 (60&ndash;71) trigger pads 1&ndash;12.<br>
  &#8226; Guitar Hero mode: hold Pad&nbsp;12 + tap Bank button.
</div>

<script>
let padData={}, fileList=[];
function status(m){document.getElementById('status').textContent=m}

async function loadPads(){
  const r=await fetch('/padmap'); padData=await r.json(); renderPads();
}
async function loadFiles(){
  const r=await fetch('/files'); fileList=await r.json(); renderFiles();
}
function refresh(){loadPads();loadFiles();}

function renderPads(){
  const el=document.getElementById('pads'); el.innerHTML='';
  for(let i=1;i<=12;i++){
    const fname=padData['pad'+i]||'';
    const d=document.createElement('div');
    d.className='pad'+(fname?' loaded':'');
    d.dataset.pad=i;
    d.innerHTML=`<span class="pad-num">PAD ${i}</span>`+
                `<span class="pad-file">${fname||'&mdash;'}</span>`;
    d.addEventListener('dragover', e=>{e.preventDefault();d.classList.add('dragover')});
    d.addEventListener('dragleave',()=>d.classList.remove('dragover'));
    d.addEventListener('drop', async e=>{
      e.preventDefault(); d.classList.remove('dragover');
      const fn=e.dataTransfer.getData('text/plain'); if(!fn) return;
      status(`Assigning ${fn} to pad ${i}...`);
      const r=await fetch('/assign',{method:'POST',
        headers:{'Content-Type':'application/json'},
        body:JSON.stringify({pad:i,file:fn})});
      const j=await r.json();
      status(j.ok?`✓ Pad ${i} → ${fn}`:`✗ ${j.error||'Error'}`);
      refresh();
    });
    el.appendChild(d);
  }
}

function renderFiles(){
  const el=document.getElementById('files'); el.innerHTML='';
  if(!fileList.length){
    el.innerHTML='<div style="color:#444;font-size:.8em">No samples yet — upload a .wav file.</div>';
    return;
  }
  fileList.forEach(f=>{
    const d=document.createElement('div');
    d.className='fitem'; d.draggable=true; d.dataset.file=f;
    d.innerHTML=`<span class="ficon">&#127925;</span>`+
                `<span class="fname">${f}</span>`+
                `<span class="fdel" title="Delete" onclick="delFile('${f}')">&#10005;</span>`;
    d.addEventListener('dragstart',e=>e.dataTransfer.setData('text/plain',f));
    el.appendChild(d);
  });
}

async function delFile(f){
  if(!confirm(`Delete ${f}?`)) return;
  status('Deleting...');
  const r=await fetch('/delete',{method:'POST',
    headers:{'Content-Type':'application/json'},
    body:JSON.stringify({file:f})});
  const j=await r.json();
  status(j.ok?`✓ Deleted ${f}`:`✗ ${j.error||'Error'}`);
  refresh();
}

async function uploadFiles(files){
  for(const file of files){
    status(`Uploading ${file.name}...`);
    const fd=new FormData(); fd.append('file',file,file.name);
    const r=await fetch('/upload',{method:'POST',body:fd});
    const j=await r.json();
    status(j.ok?`✓ Uploaded ${file.name}`:`✗ ${j.error||'Upload failed'}`);
  }
  document.getElementById('fu').value='';
  refresh();
}

refresh();
</script>
</body>
</html>
)rawliteral";

// ── Route handlers ────────────────────────────────────────────

static void handleRoot()
{
    httpServer.send_P(200, "text/html", INDEX_HTML);
}

static void handleFiles()
{
    String json = "[";
    bool first = true;
    File root = LittleFS.open("/");
    File entry = root.openNextFile();
    while (entry) {
        if (!entry.isDirectory()) {
            String name = String(entry.name());
            if (name.startsWith("/")) name = name.substring(1);
            String lower = name;
            lower.toLowerCase();
            if (lower.endsWith(".wav")) {
                if (!first) json += ",";
                // Escape quotes in filename
                name.replace("\"", "\\\"");
                json += "\"" + name + "\"";
                first = false;
            }
        }
        entry = root.openNextFile();
    }
    json += "]";
    httpServer.send(200, "application/json", json);
}

static void handlePadMap()
{
    String json = "{";
    for (int i = 0; i < 12; i++) {
        if (i > 0) json += ",";
        String fname = String(padMap[i]);
        fname.replace("\"", "\\\"");
        json += "\"pad" + String(i + 1) + "\":\"" + fname + "\"";
    }
    json += "}";
    httpServer.send(200, "application/json", json);
}

// Simple JSON field extractor (avoids pulling in ArduinoJson)
static String extractJsonString(const String &body, const char *key)
{
    String search = String("\"") + key + "\":\"";
    int start = body.indexOf(search);
    if (start < 0) return "";
    start += search.length();
    int end = body.indexOf('"', start);
    if (end < 0) return "";
    return body.substring(start, end);
}

static int extractJsonInt(const String &body, const char *key)
{
    String search = String("\"") + key + "\":";
    int pos = body.indexOf(search);
    if (pos < 0) return -1;
    return body.substring(pos + search.length()).toInt();
}

static void handleAssign()
{
    if (!httpServer.hasArg("plain")) {
        httpServer.send(400, "application/json",
                        "{\"ok\":false,\"error\":\"missing body\"}");
        return;
    }
    String body = httpServer.arg("plain");
    int padNum   = extractJsonInt(body, "pad");
    String fname = extractJsonString(body, "file");

    if (padNum < 1 || padNum > 12 || fname.length() == 0) {
        httpServer.send(400, "application/json",
                        "{\"ok\":false,\"error\":\"invalid parameters\"}");
        return;
    }

    if (!loadWav(padNum - 1, fname.c_str())) {
        httpServer.send(500, "application/json",
                        "{\"ok\":false,\"error\":\"failed to load WAV\"}");
        return;
    }
    savePadConfig();
    httpServer.send(200, "application/json", "{\"ok\":true}");
}

static File uploadFile;

static void handleUploadBody()
{
    HTTPUpload &upload = httpServer.upload();
    if (upload.status == UPLOAD_FILE_START) {
        String path = "/" + upload.filename;
        // Strip any path prefix from browser
        int slash = path.lastIndexOf('/');
        if (slash > 0) path = "/" + path.substring(slash + 1);
        uploadFile = LittleFS.open(path, "w");
    } else if (upload.status == UPLOAD_FILE_WRITE) {
        if (uploadFile) uploadFile.write(upload.buf, upload.currentSize);
    } else if (upload.status == UPLOAD_FILE_END) {
        if (uploadFile) uploadFile.close();
    }
}

static void handleUploadComplete()
{
    if (uploadFile) uploadFile.close();  // safety
    httpServer.send(200, "application/json", "{\"ok\":true}");
}

static void handleDelete()
{
    if (!httpServer.hasArg("plain")) {
        httpServer.send(400, "application/json", "{\"ok\":false}");
        return;
    }
    String fname = extractJsonString(httpServer.arg("plain"), "file");
    if (fname.length() == 0) {
        httpServer.send(400, "application/json",
                        "{\"ok\":false,\"error\":\"missing file\"}");
        return;
    }
    String path = "/" + fname;
    if (!LittleFS.exists(path)) {
        httpServer.send(404, "application/json",
                        "{\"ok\":false,\"error\":\"not found\"}");
        return;
    }

    // Unload from any pad that references this file
    for (int i = 0; i < 12; i++) {
        if (strcmp(padMap[i], fname.c_str()) == 0) {
            portENTER_CRITICAL(&audioMux);
            for (int v = 0; v < MAX_VOICES; v++) {
                if (voices[v].active && voices[v].data == samples[i].data)
                    voices[v].active = false;
            }
            portEXIT_CRITICAL(&audioMux);
            heap_caps_free(samples[i].data);
            samples[i].data   = nullptr;
            samples[i].loaded = false;
            padMap[i][0]      = '\0';
        }
    }

    LittleFS.remove(path);
    savePadConfig();
    httpServer.send(200, "application/json", "{\"ok\":true}");
}

static void webSetup()
{
    WiFi.mode(WIFI_AP);
    WiFi.softAP(WIFI_SSID);

    httpServer.on("/",       HTTP_GET,  handleRoot);
    httpServer.on("/files",  HTTP_GET,  handleFiles);
    httpServer.on("/padmap", HTTP_GET,  handlePadMap);
    httpServer.on("/assign", HTTP_POST, handleAssign);
    httpServer.on("/delete", HTTP_POST, handleDelete);
    httpServer.on("/upload", HTTP_POST, handleUploadComplete, handleUploadBody);
    httpServer.begin();
}


// ╔══════════════════════════════════════════════════════════════╗
// ║  OLED — SAMPLER STATUS SCREEN                               ║
// ╚══════════════════════════════════════════════════════════════╝

static void drawSamplerScreen()
{
    oled.clearDisplay();
    oled.setTextColor(SSD1306_WHITE);
    oled.setTextSize(1);

    // Title bar
    oled.setCursor(24, 0);
    oled.print("FUNBOX  OS");
    oled.drawFastHLine(0, 9, SCREEN_W, SSD1306_WHITE);

    // Pad grid: 3 columns × 4 rows (pads 1-12)
    // Each cell: ~42 px wide, 12 px tall
    for (int i = 0; i < 12; i++) {
        int col = i % 3;
        int row = i / 3;
        int x   = col * 43;
        int y   = 11 + row * 12;

        char label[6];
        snprintf(label, sizeof(label), "P%02d", i + 1);
        oled.setCursor(x, y);
        oled.print(label);

        if (samples[i].loaded) {
            // Draw a small filled dot to indicate a loaded sample
            oled.fillCircle(x + 22, y + 3, 2, SSD1306_WHITE);
        }
    }

    // Footer
    oled.drawFastHLine(0, SCREEN_H - 10, SCREEN_W, SSD1306_WHITE);
    oled.setCursor(0, SCREEN_H - 8);
    oled.print(WIFI_SSID);
    oled.setCursor(96, SCREEN_H - 8);
    oled.print(bleConnected ? "BLE" : "   ");

    oled.display();
}


// ╔══════════════════════════════════════════════════════════════╗
// ║  BUTTON READING (debounced)                                 ║
// ╚══════════════════════════════════════════════════════════════╝

#define DEBOUNCE_MS 40

static bool     lastPadState[12]   = {};
static uint32_t padDebounceMs[12]  = {};
static bool     bankPressed        = false;
static bool     bankJustPressed    = false;
static bool     lastBankState      = true;  // HIGH = not pressed (PULLUP)
static uint32_t bankDebounceMs     = 0;

static void readButtons()
{
    uint32_t now = millis();

    for (int i = 0; i < 12; i++) {
        bool raw = (digitalRead(PAD_PINS[i]) == LOW);
        if (raw != lastPadState[i] && now - padDebounceMs[i] > DEBOUNCE_MS) {
            padDebounceMs[i]  = now;
            padJustPressed[i] = raw && !padPressed[i];
            padPressed[i]     = raw;
            lastPadState[i]   = raw;
        } else {
            padJustPressed[i] = false;
        }
    }

    bool rawBank = (digitalRead(BANK_PIN) == LOW);
    if (rawBank != lastBankState && now - bankDebounceMs > DEBOUNCE_MS) {
        bankDebounceMs = now;
        bankJustPressed = rawBank && !bankPressed;
        bankPressed     = rawBank;
        lastBankState   = rawBank;
    } else {
        bankJustPressed = false;
    }
}


// ╔══════════════════════════════════════════════════════════════╗
// ║  SETUP & LOOP                                               ║
// ╚══════════════════════════════════════════════════════════════╝

static bool     gameMode          = false;
static uint32_t lastDisplayMs     = 0;

void setup()
{
    Serial.begin(115200);
    Serial.println("\n\nFunbox OS v1.0 — booting...");

    // ── Pad & bank buttons
    for (int i = 0; i < 12; i++) {
        pinMode(PAD_PINS[i], INPUT_PULLUP);
        memset(&voices[i], 0, sizeof(Voice));
        samples[i] = { nullptr, 0, false, {} };
        padMap[i][0] = '\0';
    }
    // Initialise remaining voice slots
    for (int i = 12; i < MAX_VOICES; i++) memset(&voices[i], 0, sizeof(Voice));
    pinMode(BANK_PIN, INPUT_PULLUP);

    // ── LittleFS
    if (!LittleFS.begin(true)) {
        Serial.println("LittleFS: mount failed, reformatted.");
    } else {
        Serial.println("LittleFS: OK");
    }

    // ── OLED (I²C at 400 kHz)
    Wire.begin(21, 22);
    Wire.setClock(400000);
    if (!oled.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
        Serial.println("OLED: not found — check wiring");
    } else {
        oled.clearDisplay();
        oled.setTextSize(1);
        oled.setTextColor(SSD1306_WHITE);
        oled.setCursor(16, 22);
        oled.print("FUNBOX OS  v1.0");
        oled.setCursor(22, 36);
        oled.print("...booting...");
        oled.display();
        delay(1200);
        Serial.println("OLED: OK");
    }

    // ── Audio engine (hardware timer + DAC)
    audioSetup();
    Serial.println("Audio: timer started @ ~22 kHz");

    // ── Restore saved pad mappings
    loadPadConfig();

    // ── WiFi AP + web server
    webSetup();
    Serial.print("WiFi AP: "); Serial.println(WIFI_SSID);
    Serial.print("Web UI:  http://"); Serial.println(WiFi.softAPIP());

    // ── BLE MIDI
    bleSetup();
    Serial.println("BLE: advertising as \"" BLE_DEVICE_NAME "\"");

    Serial.println("Boot complete.\n");
    drawSamplerScreen();
}

void loop()
{
    readButtons();

    // ── Toggle game mode: hold Pad 12 + tap Bank
    if (bankJustPressed && padPressed[11]) {
        gameMode = !gameMode;
        if (gameMode) {
            ghState = GHS_IDLE;
        }
    }

    uint32_t now = millis();

    if (gameMode) {
        // Bank button (without Pad 12): start / restart game
        if (bankJustPressed && !padPressed[11]) {
            if (ghState == GHS_IDLE || ghState == GHS_GAMEOVER)
                ghStart();
        }

        ghUpdate();

        if (now - lastDisplayMs >= (1000 / DISPLAY_FPS)) {
            lastDisplayMs = now;
            ghDraw();
        }
    } else {
        // Sampler mode: pads trigger sounds
        for (int i = 0; i < 12; i++) {
            if (padJustPressed[i]) triggerPad(i);
        }

        // Refresh sampler screen at a moderate rate
        if (now - lastDisplayMs >= 200) {
            lastDisplayMs = now;
            drawSamplerScreen();
        }
    }

    // Serve HTTP requests
    httpServer.handleClient();

    // Yield to FreeRTOS scheduler
    vTaskDelay(1);
}
