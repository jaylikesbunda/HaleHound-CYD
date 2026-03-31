// ═══════════════════════════════════════════════════════════════════════════
// HaleHound-CYD Battery Monitor Implementation
// ADC-based LiPo battery voltage reading + UI indicator
// GPIO 34 (ADC1 CH6) — works with WiFi active
// Created: 2026-03-20
// ═══════════════════════════════════════════════════════════════════════════

#include "battery_monitor.h"
#include "shared.h"
#include <esp_adc_cal.h>

#if CYD_HAS_BATTERY

// ═══════════════════════════════════════════════════════════════════════════
// INTERNAL STATE
// ═══════════════════════════════════════════════════════════════════════════

static float    _smoothedMv   = 0.0f;   // EMA-smoothed voltage (mV at battery)
static int      _cachedPct    = -1;      // Cached percentage (0-100, -1 = not read yet)
static int      _cachedMv     = 0;       // Cached voltage in mV
static uint32_t _lastUpdateMs = 0;       // Last ADC sample time
static bool     _initialized  = false;

// ADC calibration characteristics
static esp_adc_cal_characteristics_t _adcChars;

// ═══════════════════════════════════════════════════════════════════════════
// LIPO DISCHARGE CURVE — Lookup table for voltage → percentage
// Based on real LiPo discharge measurements (1C load, 3.7V nominal cell)
// Interpolated linearly between points.
// ═══════════════════════════════════════════════════════════════════════════

struct VoltPct {
    uint16_t mv;     // Battery voltage in mV
    uint8_t  pct;    // Percentage at this voltage
};

// 11-point discharge curve (descending voltage order)
static const VoltPct _lipoCurve[] = {
    { 4200, 100 },
    { 4150,  95 },
    { 4110,  90 },
    { 4080,  85 },
    { 4020,  80 },
    { 3980,  70 },
    { 3950,  60 },
    { 3910,  50 },
    { 3870,  40 },
    { 3830,  30 },
    { 3790,  20 },
    { 3740,  15 },
    { 3680,  10 },
    { 3580,   5 },
    { 3400,   1 },
    { 3300,   0 },
};
static const int _lipoCurveLen = sizeof(_lipoCurve) / sizeof(_lipoCurve[0]);

// ═══════════════════════════════════════════════════════════════════════════
// VOLTAGE TO PERCENTAGE (linear interpolation on discharge curve)
// ═══════════════════════════════════════════════════════════════════════════

static int voltageToPercent(int mv) {
    // Above max — clamp to 100
    if (mv >= _lipoCurve[0].mv) return 100;
    // Below min — clamp to 0
    if (mv <= _lipoCurve[_lipoCurveLen - 1].mv) return 0;

    // Find the two curve points that bracket this voltage
    for (int i = 0; i < _lipoCurveLen - 1; i++) {
        int mvHigh = _lipoCurve[i].mv;
        int mvLow  = _lipoCurve[i + 1].mv;
        if (mv <= mvHigh && mv >= mvLow) {
            // Linear interpolation between the two points
            int pctHigh = _lipoCurve[i].pct;
            int pctLow  = _lipoCurve[i + 1].pct;
            float ratio = (float)(mv - mvLow) / (float)(mvHigh - mvLow);
            return pctLow + (int)(ratio * (pctHigh - pctLow));
        }
    }
    return 0;
}

// ═══════════════════════════════════════════════════════════════════════════
// RAW ADC READING — Multi-sample average with calibration
// ═══════════════════════════════════════════════════════════════════════════

static int readBatteryMv() {
    uint32_t sum = 0;
    for (int i = 0; i < BATT_SAMPLE_COUNT; i++) {
        sum += esp_adc_cal_raw_to_voltage(analogRead(CYD_BATTERY_ADC), &_adcChars);
    }
    int adcMv = sum / BATT_SAMPLE_COUNT;

    // Apply voltage divider ratio to get actual battery voltage
    int battMv = (int)(adcMv * CYD_BATTERY_DIVIDER_RATIO);
    return battMv;
}

// ═══════════════════════════════════════════════════════════════════════════
// PUBLIC API
// ═══════════════════════════════════════════════════════════════════════════

void batteryInit() {
    // Configure ADC — GPIO 34 is ADC1 channel 6
    analogSetAttenuation(ADC_11db);    // Full range 0-3.3V
    analogSetWidth(12);                 // 12-bit resolution (0-4095)
    pinMode(CYD_BATTERY_ADC, INPUT);

    // Characterize ADC for accurate mV conversion using factory calibration
    esp_adc_cal_characterize(ADC_UNIT_1, ADC_ATTEN_DB_11, ADC_WIDTH_BIT_12, 1100, &_adcChars);

    // Take initial reading (fill the smoother)
    int firstMv = readBatteryMv();
    _smoothedMv = (float)firstMv;
    _cachedMv   = firstMv;
    _cachedPct  = voltageToPercent(firstMv);
    _lastUpdateMs = millis();
    _initialized  = true;

    Serial.printf("[BATT] Init: %dmV (%d%%)\n", _cachedMv, _cachedPct);
}

void batteryUpdate() {
    if (!_initialized) return;

    // Self-throttle — only sample every BATT_UPDATE_MS
    uint32_t now = millis();
    if (now - _lastUpdateMs < BATT_UPDATE_MS) return;
    _lastUpdateMs = now;

    // Read raw and apply EMA smoothing
    int rawMv = readBatteryMv();
    _smoothedMv = (_smoothedMv * (1.0f - BATT_SMOOTH_ALPHA)) + (rawMv * BATT_SMOOTH_ALPHA);

    _cachedMv  = (int)_smoothedMv;
    _cachedPct = voltageToPercent(_cachedMv);
}

int batteryGetPercent() {
    return (_cachedPct >= 0) ? _cachedPct : 0;
}

int batteryGetVoltage() {
    return _cachedMv;
}

bool batteryIsLow() {
    return _cachedMv > 0 && _cachedMv < BATT_MV_LOW_WARN;
}

bool batteryIsCritical() {
    return _cachedMv > 0 && _cachedMv < BATT_MV_CRITICAL;
}

// ═══════════════════════════════════════════════════════════════════════════
// BATTERY UI INDICATOR — Programmatic icon (no bitmap)
// Draws a 24x10 battery outline with fill level + percentage text
// ═══════════════════════════════════════════════════════════════════════════

void drawBatteryIndicator(int x, int y) {
    extern TFT_eSPI tft;

    int pct = batteryGetPercent();
    int mv  = batteryGetVoltage();

    // If no valid reading yet, show placeholder
    if (_cachedPct < 0 || mv == 0) {
        tft.setTextColor(HALEHOUND_GUNMETAL);
        tft.setTextSize(1);
        tft.setCursor(x, y + 1);
        tft.print("BAT:--");
        return;
    }

    // ── Color based on charge level ──
    uint16_t fillColor;
    if (pct > 50) {
        fillColor = 0x07E0;              // Green
    } else if (pct > 20) {
        fillColor = 0xFDE0;              // Yellow
    } else if (pct > 10) {
        fillColor = 0xFD00;              // Orange
    } else {
        fillColor = 0xF800;              // Red
    }

    uint16_t outlineColor = HALEHOUND_GUNMETAL;

    // ── Battery icon: 22x10 body + 2x4 nub ──
    //  ┌──────────────────────┐┐
    //  │██████████░░░░░░░░░░░░│█  ← positive terminal nub
    //  └──────────────────────┘┘
    int bodyW = 22;
    int bodyH = 10;
    int nubW  = 2;
    int nubH  = 4;

    // Clear area (body + nub + text)
    tft.fillRect(x, y, bodyW + nubW + 30, bodyH, HALEHOUND_BLACK);

    // Battery body outline
    tft.drawRect(x, y, bodyW, bodyH, outlineColor);

    // Positive terminal nub (right side, centered vertically)
    tft.fillRect(x + bodyW, y + (bodyH - nubH) / 2, nubW, nubH, outlineColor);

    // Fill level inside body (1px inset from outline)
    int fillMaxW = bodyW - 4;  // Inner fill area width (2px padding each side)
    int fillW = (pct * fillMaxW) / 100;
    if (fillW < 1 && pct > 0) fillW = 1;  // At least 1px if not dead

    if (fillW > 0) {
        tft.fillRect(x + 2, y + 2, fillW, bodyH - 4, fillColor);
    }

    // ── Low battery blink effect ──
    if (pct <= 10 && (millis() / 500) % 2 == 0) {
        // Blink the fill red on/off every 500ms
        tft.fillRect(x + 2, y + 2, fillMaxW, bodyH - 4, HALEHOUND_BLACK);
    }

    // ── Percentage text right of icon ──
    tft.setTextColor(fillColor);
    tft.setTextSize(1);
    tft.setCursor(x + bodyW + nubW + 3, y + 1);

    char buf[8];
    snprintf(buf, sizeof(buf), "%d%%", pct);
    tft.print(buf);
}

#else  // CYD_HAS_BATTERY == 0 — stub everything out

void batteryInit() {}
void batteryUpdate() {}
int  batteryGetPercent() { return -1; }
int  batteryGetVoltage() { return 0; }
bool batteryIsLow() { return false; }
bool batteryIsCritical() { return false; }
void drawBatteryIndicator(int x, int y) { (void)x; (void)y; }

#endif // CYD_HAS_BATTERY
