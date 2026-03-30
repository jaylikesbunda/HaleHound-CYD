#pragma once

// ═══════════════════════════════════════════════════════════════════════════
// AIROHA RACE - CVE-2025-20700/20701/20702 Exploit Chain
// Unauthenticated BLE GATT access to Airoha-based BT headphones
// Targets: Sony XM4/XM5/XM6, Marshall, JBL, Jabra, Beyerdynamic, etc.
//
// Capabilities:
//   - Link key extraction (steal BT pairing keys)
//   - BR/EDR BD_ADDR extraction (Classic BT address)
//   - SDK/firmware version reads
//   - Flash memory dumping (256 bytes per RACE command)
//
// No pairing required. Pure BLE GATT client operations.
// ═══════════════════════════════════════════════════════════════════════════

namespace AirohaRace {

// Initialize scanner, start BLE, run two-phase discovery
void setup();

// Main event loop — touch/button handling, state machine
void loop();

// Check if user requested exit
bool isExitRequested();

// Cleanup — disconnect, deinit BLE, release resources
void cleanup();

}  // namespace AirohaRace
