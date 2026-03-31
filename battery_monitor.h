#ifndef BATTERY_MONITOR_H
#define BATTERY_MONITOR_H

// ═══════════════════════════════════════════════════════════════════════════
// HaleHound-CYD Battery Monitor
// ADC-based LiPo battery voltage reading + UI indicator
// GPIO 34 (ADC1 CH6) — works with WiFi active
// Created: 2026-03-20
// ═══════════════════════════════════════════════════════════════════════════

#include <Arduino.h>
#include <TFT_eSPI.h>
#include "cyd_config.h"

// ═══════════════════════════════════════════════════════════════════════════
// CONFIGURATION
// ═══════════════════════════════════════════════════════════════════════════

// Voltage divider ratio (board resistor divider: Vbat → R1 → ADC → R2 → GND)
// E32R28T/E32R35T: 2:1 divider (100K/100K) — ADC reads half battery voltage
// Standard CYD + external battery: adjust to match your divider
#ifndef CYD_BATTERY_DIVIDER_RATIO
  #define CYD_BATTERY_DIVIDER_RATIO  2.0f
#endif

// LiPo voltage thresholds (millivolts at battery terminals, BEFORE divider)
#define BATT_MV_FULL       4200    // 100% — fully charged
#define BATT_MV_EMPTY      3300    // 0%   — cutoff (protect cell)
#define BATT_MV_LOW_WARN   3500    // Low battery warning threshold
#define BATT_MV_CRITICAL   3400    // Critical — stop TX operations

// Sampling
#define BATT_SAMPLE_COUNT     16   // ADC samples to average per reading
#define BATT_UPDATE_MS      3000   // Update interval (3 seconds)
#define BATT_SMOOTH_ALPHA   0.15f  // EMA smoothing factor (0.0=slow, 1.0=instant)

// ═══════════════════════════════════════════════════════════════════════════
// PUBLIC API
// ═══════════════════════════════════════════════════════════════════════════

// Initialize ADC pin and take first reading. Call once in setup().
void batteryInit();

// Sample ADC and update cached values. Call from loop() — self-throttled.
void batteryUpdate();

// Get cached battery percentage (0-100)
int batteryGetPercent();

// Get cached battery voltage in millivolts (at battery, after divider correction)
int batteryGetVoltage();

// Returns true if battery is below BATT_MV_LOW_WARN
bool batteryIsLow();

// Returns true if battery is below BATT_MV_CRITICAL
bool batteryIsCritical();

// Draw battery icon + percentage at (x, y). Icon is 24x10px + text.
// Draws on the global tft object.
void drawBatteryIndicator(int x, int y);

#endif // BATTERY_MONITOR_H
