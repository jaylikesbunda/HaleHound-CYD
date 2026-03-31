#ifndef SUBREAD_H
#define SUBREAD_H

// ═══════════════════════════════════════════════════════════════════════════
// HaleHound-CYD — .Sub Read
// Flipper Zero .sub file browser and transmitter for CC1101
// Reads .sub files from SD card, transmits via CC1101 SubGHz radio
// Supported: RAW, Princeton, CAME, Nice FLO protocols
// Full CC1101 bands: 300-348, 387-464, 779-928 MHz
// Created: 2026-03-31
// ═══════════════════════════════════════════════════════════════════════════
//
// Place .sub files in /subghz/ on the SD card.
// Organize into subfolders as needed — nested browsing supported.
//
// ═══════════════════════════════════════════════════════════════════════════

#include <Arduino.h>
#include "cyd_config.h"

namespace SubRead {
    void setup();
    void loop();
    bool isExitRequested();
    void cleanup();
}

#endif // SUBREAD_H
