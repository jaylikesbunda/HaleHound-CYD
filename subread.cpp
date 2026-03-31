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
// Organize into subfolders — nested browsing supported.
//
// NOTE: Does NOT call SD.end() — just deselects CS on cleanup.
//       SD.end() destabilizes shared SPI bus (CC1101, NRF24 share VSPI).
// ═══════════════════════════════════════════════════════════════════════════

#include "subread.h"
#include "shared.h"
#include "utils.h"
#include "touch_buttons.h"
#include "spi_manager.h"
#include "icon.h"
#include "skull_bg.h"
#include "nosifer_font.h"
#include <TFT_eSPI.h>
#include <SD.h>
#include <SPI.h>
#include <RCSwitch.h>
#include <ELECHOUSE_CC1101_SRC_DRV.h>

// ═══════════════════════════════════════════════════════════════════════════
// EXTERNAL OBJECTS
// ═══════════════════════════════════════════════════════════════════════════

extern TFT_eSPI tft;

namespace SubRead {

// ═══════════════════════════════════════════════════════════════════════════
// CONFIGURATION
// ═══════════════════════════════════════════════════════════════════════════

#define SR_ROOT          "/subghz"
#define SR_MAX_ENTRIES   40           // folders + files per directory (heap-allocated)
#define SR_LIST_Y        72
#define SR_LIST_BOTTOM   (SCREEN_HEIGHT - 28)
#define SR_ROW_HEIGHT    24
#define SR_VISIBLE_ROWS  ((SR_LIST_BOTTOM - SR_LIST_Y) / SR_ROW_HEIGHT)
#define SR_MAX_PATH      80
#define SR_MAX_FILE_SIZE 72000        // 72 KB cap — covers large deBruijn/RAW captures

// ═══════════════════════════════════════════════════════════════════════════
// TYPES
// ═══════════════════════════════════════════════════════════════════════════

enum Phase {
    PHASE_NO_SD,
    PHASE_NO_FILES,
    PHASE_BROWSE,
    PHASE_TRANSMITTING
};

enum SubProtocol {
    PROTO_RAW       = 0,
    PROTO_PRINCETON = 1,
    PROTO_CAME      = 2,
    PROTO_NICEFLO   = 3,
    PROTO_UNKNOWN   = 4
};

struct DirEntry {
    char        displayName[24];       // shown in the list (.sub stripped)
    char        fullPath[SR_MAX_PATH]; // absolute path for open/navigate
    bool        isDir;                 // true = folder, false = .sub file
    uint32_t    freqHz;
    SubProtocol proto;
    bool        freqValid;
};

// ═══════════════════════════════════════════════════════════════════════════
// STATE
// ═══════════════════════════════════════════════════════════════════════════

static Phase       phase         = PHASE_NO_SD;
static bool        initialized   = false;
static bool        exitRequested = false;
static bool        sdMounted     = false;
static bool        cc1101Ready   = false;
static bool        abortTx       = false;

static DirEntry*   entries       = nullptr;  // heap-allocated in setup(), freed in cleanup()
static int         entryCount    = 0;
static int         scrollOffset  = 0;
static int         selectedIndex = -1;

static char        currentPath[SR_MAX_PATH];

// ═══════════════════════════════════════════════════════════════════════════
// CC1101 PA MODULE CONTROL
// ═══════════════════════════════════════════════════════════════════════════

static void srPaSetTx() {
    #if defined(CC1101_TX_EN) && defined(CC1101_RX_EN)
    if (cc1101_pa_module) {
        digitalWrite(CC1101_RX_EN, LOW);
        delayMicroseconds(2);
        digitalWrite(CC1101_TX_EN, HIGH);
    }
    #endif
    ELECHOUSE_cc1101.SetTx();
}

static void srPaSetIdle() {
    ELECHOUSE_cc1101.setSidle();
    #if defined(CC1101_TX_EN) && defined(CC1101_RX_EN)
    if (cc1101_pa_module) {
        digitalWrite(CC1101_TX_EN, LOW);
        digitalWrite(CC1101_RX_EN, LOW);
    }
    #endif
}

// ═══════════════════════════════════════════════════════════════════════════
// FREQUENCY HELPERS — Full CC1101 supported bands
// ═══════════════════════════════════════════════════════════════════════════

static bool validateFrequency(uint32_t hz) {
    if (hz >= 300000000UL && hz <= 348000000UL) return true;  // 300-348 MHz
    if (hz >= 387000000UL && hz <= 464000000UL) return true;  // 387-464 MHz
    if (hz >= 779000000UL && hz <= 928000000UL) return true;  // 779-928 MHz
    return false;
}

static const char* protoName(SubProtocol p) {
    switch (p) {
        case PROTO_RAW:       return "RAW";
        case PROTO_PRINCETON: return "PRINCETON";
        case PROTO_CAME:      return "CAME";
        case PROTO_NICEFLO:   return "NICE FLO";
        default:              return "???";
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// SPI BUS HELPERS
// ═══════════════════════════════════════════════════════════════════════════

static void deselectAllCS() {
    pinMode(NRF24_CSN, OUTPUT); digitalWrite(NRF24_CSN, HIGH);
    pinMode(CC1101_CS, OUTPUT); digitalWrite(CC1101_CS, HIGH);
    pinMode(SD_CS, OUTPUT);     digitalWrite(SD_CS, HIGH);
}

// ═══════════════════════════════════════════════════════════════════════════
// SD CARD
// ═══════════════════════════════════════════════════════════════════════════

static bool mountSD() {
    deselectAllCS();
    SPI.end();
    delay(5);
    SPI.begin(18, 19, 23, SD_CS);
    delay(5);
    if (SD.begin(SD_CS, SPI, 4000000)) return true;
    // One retry
    SD.end();
    SPI.end();
    delay(10);
    SPI.begin(18, 19, 23, SD_CS);
    if (SD.begin(SD_CS, SPI, 4000000)) return true;
    return false;
}

static void rearmSD() {
    deselectAllCS();
    SPI.end();
    delay(5);
    SPI.begin(18, 19, 23, SD_CS);
    delay(5);
    SD.begin(SD_CS, SPI, 4000000);
}

// ═══════════════════════════════════════════════════════════════════════════
// FILE HEADER PARSING — Quick-parse first 12 lines for Frequency/Protocol
// ═══════════════════════════════════════════════════════════════════════════

static void quickParseHeader(const char* fullPath, DirEntry& out) {
    out.freqHz    = 0;
    out.proto     = PROTO_UNKNOWN;
    out.freqValid = false;

    File f = SD.open(fullPath, FILE_READ);
    if (!f) return;

    char line[96];
    int linesRead = 0;

    while (f.available() && linesRead < 12) {
        int i = 0;
        while (f.available() && i < (int)sizeof(line) - 1) {
            char c = (char)f.read();
            if (c == '\n') break;
            if (c == '\r') continue;
            line[i++] = c;
        }
        line[i] = '\0';
        linesRead++;

        if (strncmp(line, "Frequency:", 10) == 0) {
            out.freqHz    = (uint32_t)atol(line + 10);
            out.freqValid = validateFrequency(out.freqHz);
        } else if (strncmp(line, "Protocol:", 9) == 0) {
            const char* p = line + 9;
            while (*p == ' ') p++;
            if      (strcasecmp(p, "RAW")       == 0) out.proto = PROTO_RAW;
            else if (strcasecmp(p, "Princeton")  == 0) out.proto = PROTO_PRINCETON;
            else if (strcasecmp(p, "CAME")       == 0) out.proto = PROTO_CAME;
            else if (strcasecmp(p, "Nice_Flo")   == 0) out.proto = PROTO_NICEFLO;
            else if (strcasecmp(p, "Nice FLO")   == 0) out.proto = PROTO_NICEFLO;
            else                                        out.proto = PROTO_UNKNOWN;
        } else if (strncmp(line, "RAW_Data:", 9) == 0) {
            break;  // reached data section — stop reading header
        }
    }

    f.close();
}

// ═══════════════════════════════════════════════════════════════════════════
// DISPLAY NAME FORMATTING
// ═══════════════════════════════════════════════════════════════════════════

static void makeDisplayName(const char* baseName, bool isDir, char* dst, int dstLen) {
    strncpy(dst, baseName, dstLen - 1);
    dst[dstLen - 1] = '\0';
    if (!isDir) {
        int len = strlen(dst);
        if (len > 4 && strcasecmp(dst + len - 4, ".sub") == 0) {
            dst[len - 4] = '\0';
        }
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// DIRECTORY SCANNING — Two-pass: folders first, then .sub files
// ═══════════════════════════════════════════════════════════════════════════

static int scanDirectory(const char* path) {
    entryCount    = 0;
    scrollOffset  = 0;
    selectedIndex = -1;

    if (!entries) return 0;
    if (!SD.exists(path)) return 0;

    File dir = SD.open(path);
    if (!dir || !dir.isDirectory()) {
        if (dir) dir.close();
        return 0;
    }

    // Pass 1 — folders
    File entry;
    while ((entry = dir.openNextFile()) && entryCount < SR_MAX_ENTRIES) {
        if (!entry.isDirectory()) { entry.close(); continue; }

        const char* name = entry.name();
        const char* base = strrchr(name, '/');
        base = base ? base + 1 : name;

        entries[entryCount].isDir = true;
        makeDisplayName(base, true, entries[entryCount].displayName,
                        sizeof(entries[entryCount].displayName));
        snprintf(entries[entryCount].fullPath, sizeof(entries[entryCount].fullPath),
                 "%s/%s", path, base);
        entries[entryCount].freqHz    = 0;
        entries[entryCount].proto     = PROTO_UNKNOWN;
        entries[entryCount].freqValid = false;
        entryCount++;

        entry.close();
    }
    dir.close();

    // Pass 2 — .sub files
    dir = SD.open(path);
    if (!dir) return entryCount;

    while ((entry = dir.openNextFile()) && entryCount < SR_MAX_ENTRIES) {
        if (entry.isDirectory()) { entry.close(); continue; }

        const char* name = entry.name();
        int len = strlen(name);
        if (len <= 4 || strcasecmp(name + len - 4, ".sub") != 0) {
            entry.close();
            continue;
        }

        const char* base = strrchr(name, '/');
        base = base ? base + 1 : name;

        entries[entryCount].isDir = false;
        makeDisplayName(base, false, entries[entryCount].displayName,
                        sizeof(entries[entryCount].displayName));
        snprintf(entries[entryCount].fullPath, sizeof(entries[entryCount].fullPath),
                 "%s/%s", path, base);

        entry.close();

        // Quick-parse header for freq/protocol
        quickParseHeader(entries[entryCount].fullPath, entries[entryCount]);
        entryCount++;
    }
    dir.close();
    return entryCount;
}

// ═══════════════════════════════════════════════════════════════════════════
// NAVIGATION HELPERS
// ═══════════════════════════════════════════════════════════════════════════

static bool isAtRoot() {
    return strcmp(currentPath, SR_ROOT) == 0;
}

static void navigateUp() {
    if (isAtRoot()) return;
    char* slash = strrchr(currentPath, '/');
    if (slash && slash > currentPath) {
        *slash = '\0';
    } else {
        strncpy(currentPath, SR_ROOT, sizeof(currentPath) - 1);
    }
    if (strlen(currentPath) < strlen(SR_ROOT)) {
        strncpy(currentPath, SR_ROOT, sizeof(currentPath) - 1);
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// CC1101 INIT FOR TX — Deferred until first transmit
// ═══════════════════════════════════════════════════════════════════════════

static bool initCC1101forTX(float freqMHz) {
    deselectAllCS();
    SPI.end();
    delay(5);

    if (!cc1101SafeCheck()) {
        #if CYD_DEBUG
        Serial.println("[SUBREAD] CC1101 safe check failed");
        #endif
        return false;
    }

    ELECHOUSE_cc1101.setSpiPin(RADIO_SPI_SCK, RADIO_SPI_MISO, RADIO_SPI_MOSI, CC1101_CS);
    ELECHOUSE_cc1101.setGDO(CC1101_GDO0, CC1101_GDO2);

    if (!ELECHOUSE_cc1101.getCC1101()) {
        #if CYD_DEBUG
        Serial.println("[SUBREAD] CC1101 not found");
        #endif
        return false;
    }

    ELECHOUSE_cc1101.Init();
    ELECHOUSE_cc1101.setCCMode(0);              // Raw/async mode
    ELECHOUSE_cc1101.setModulation(2);          // ASK/OOK
    ELECHOUSE_cc1101.setRxBW(270);              // 270kHz BW (Flipper OOK preset)
    ELECHOUSE_cc1101.setDRate(3.79);            // 3.794 kBaud (Flipper OOK preset)
    ELECHOUSE_cc1101.SpiWriteReg(0x12, 0x30);  // MDMCFG2: no sync, no carrier sense
    ELECHOUSE_cc1101.SpiWriteReg(0x02, 0x0D);  // IOCFG0: async serial on GDO0
    ELECHOUSE_cc1101.SpiWriteReg(0x22, 0x11);  // FREND0: OOK PA from PATABLE[1]
    ELECHOUSE_cc1101.setMHZ(freqMHz);
    ELECHOUSE_cc1101.setPA(12);                 // Max power

    spiDeselect();

    #if CYD_DEBUG
    Serial.printf("[SUBREAD] CC1101 ready @ %.3f MHz\n", freqMHz);
    #endif
    return true;
}

// ═══════════════════════════════════════════════════════════════════════════
// UI DRAWING — HaleHound theme: skull watermark, Nosifer glitch, VALHALLA
// ═══════════════════════════════════════════════════════════════════════════

static void drawSRIconBar(bool showFlash) {
    tft.drawLine(0, ICON_BAR_TOP, SCREEN_WIDTH, ICON_BAR_TOP, HALEHOUND_MAGENTA);
    tft.fillRect(0, ICON_BAR_Y, SCREEN_WIDTH, ICON_BAR_H, HALEHOUND_GUNMETAL);
    tft.drawBitmap(10, ICON_BAR_Y, bitmap_icon_go_back, 16, 16, HALEHOUND_MAGENTA);
    if (showFlash) {
        tft.drawBitmap(SCALE_X(215), ICON_BAR_Y, bitmap_icon_flash, 16, 16, HALEHOUND_HOTPINK);
    }
    tft.drawLine(0, ICON_BAR_BOTTOM, SCREEN_WIDTH, ICON_BAR_BOTTOM, HALEHOUND_HOTPINK);
}

static void getBreadcrumb(char* dst, int dstLen) {
    strncpy(dst, currentPath, dstLen - 1);
    dst[dstLen - 1] = '\0';
    int len = strlen(dst);
    if (len > 22) {
        char tmp[24];
        snprintf(tmp, sizeof(tmp), "...%s", dst + len - 19);
        strncpy(dst, tmp, dstLen - 1);
    }
}

static void drawBrowseScreen() {
    tft.fillScreen(HALEHOUND_BLACK);

    drawStatusBar();

    bool fileSelected = selectedIndex >= 0 && selectedIndex < entryCount &&
                        !entries[selectedIndex].isDir &&
                        entries[selectedIndex].freqValid;
    drawSRIconBar(fileSelected);

    // Breadcrumb path — right after icon bar
    char crumb[28];
    getBreadcrumb(crumb, sizeof(crumb));
    tft.setTextFont(2);
    tft.setTextSize(1);
    tft.setTextColor(HALEHOUND_HOTPINK, HALEHOUND_BLACK);
    tft.setCursor(10, ICON_BAR_BOTTOM + 3);
    tft.print(".SUB");
    tft.setTextColor(HALEHOUND_VIOLET, HALEHOUND_BLACK);
    tft.setCursor(50, ICON_BAR_BOTTOM + 3);
    tft.print(crumb);

    // Separator
    tft.drawLine(0, ICON_BAR_BOTTOM + 17, SCREEN_WIDTH, ICON_BAR_BOTTOM + 17, HALEHOUND_HOTPINK);

    // Column headers
    tft.setTextColor(HALEHOUND_VIOLET, HALEHOUND_BLACK);
    tft.setCursor(10,           ICON_BAR_BOTTOM + 20);
    tft.print("NAME");
    tft.setCursor(SCALE_X(150), ICON_BAR_BOTTOM + 20);
    tft.print("FREQ");
    tft.setCursor(SCALE_X(200), ICON_BAR_BOTTOM + 20);
    tft.print("PROTO");

    tft.drawLine(0, SR_LIST_Y - 2, SCREEN_WIDTH, SR_LIST_Y - 2, HALEHOUND_VIOLET);

    // Bottom bar
    tft.fillRect(0, SR_LIST_BOTTOM, SCREEN_WIDTH, SCREEN_HEIGHT - SR_LIST_BOTTOM, HALEHOUND_BLACK);
    tft.drawLine(0, SR_LIST_BOTTOM, SCREEN_WIDTH, SR_LIST_BOTTOM, HALEHOUND_HOTPINK);
    tft.setTextFont(2);
    tft.setTextSize(1);
    if (!isAtRoot()) {
        drawCenteredText(SR_LIST_BOTTOM + 4, "BACK: UP DIR", HALEHOUND_GUNMETAL, 1);
    } else {
        drawCenteredText(SR_LIST_BOTTOM + 4, "TAP FILE TO TX", HALEHOUND_GUNMETAL, 1);
    }
}

static void drawEntryRow(int idx, bool highlight) {
    int row = idx - scrollOffset;
    if (row < 0 || row >= SR_VISIBLE_ROWS) return;

    int y = SR_LIST_Y + row * SR_ROW_HEIGHT;
    const DirEntry& e = entries[idx];

    tft.fillRect(0, y, SCREEN_WIDTH, SR_ROW_HEIGHT - 1,
                 highlight ? HALEHOUND_VIOLET : HALEHOUND_BLACK);

    tft.setTextFont(2);
    tft.setTextSize(1);

    if (e.isDir) {
        uint16_t col = highlight ? HALEHOUND_BRIGHT : HALEHOUND_MAGENTA;
        tft.setTextColor(col, highlight ? HALEHOUND_VIOLET : HALEHOUND_BLACK);
        tft.setCursor(8, y + 4);
        tft.print("> ");
        tft.print(e.displayName);
    } else {
        bool valid = e.freqValid;
        uint16_t nameCol  = highlight ? HALEHOUND_BRIGHT : (valid ? HALEHOUND_MAGENTA : HALEHOUND_GUNMETAL);
        uint16_t freqCol  = highlight ? HALEHOUND_BRIGHT : (valid ? HALEHOUND_VIOLET  : HALEHOUND_GUNMETAL);
        uint16_t protoCol = highlight ? HALEHOUND_BRIGHT : (e.proto != PROTO_UNKNOWN ? HALEHOUND_HOTPINK : HALEHOUND_GUNMETAL);
        uint16_t bg       = highlight ? HALEHOUND_VIOLET : HALEHOUND_BLACK;

        // Name (prefix [!] for unsupported freq)
        char nameStr[20];
        strncpy(nameStr, e.displayName, sizeof(nameStr) - 1);
        nameStr[sizeof(nameStr) - 1] = '\0';
        if (!valid) {
            char tmp[22];
            snprintf(tmp, sizeof(tmp), "[!]%.15s", nameStr);
            strncpy(nameStr, tmp, sizeof(nameStr) - 1);
        }
        tft.setTextColor(nameCol, bg);
        tft.setCursor(8, y + 4);
        tft.print(nameStr);

        // Frequency in MHz
        char freqStr[10];
        if (e.freqHz > 0) snprintf(freqStr, sizeof(freqStr), "%.0f", e.freqHz / 1000000.0f);
        else               strcpy(freqStr, "???");
        tft.setTextColor(freqCol, bg);
        tft.setCursor(SCALE_X(148), y + 4);
        tft.print(freqStr);

        // Protocol badge
        tft.setTextColor(protoCol, bg);
        tft.setCursor(SCALE_X(196), y + 4);
        tft.print(protoName(e.proto));
    }
}

static void drawEntryList() {
    for (int i = 0; i < SR_VISIBLE_ROWS; i++) {
        int idx = scrollOffset + i;
        if (idx >= entryCount) {
            int y = SR_LIST_Y + i * SR_ROW_HEIGHT;
            tft.fillRect(0, y, SCREEN_WIDTH, SR_ROW_HEIGHT - 1, HALEHOUND_BLACK);
        } else {
            drawEntryRow(idx, idx == selectedIndex);
        }
    }

    // UP button — left side of bottom bar
    tft.setTextFont(2);
    tft.setTextSize(1);
    if (scrollOffset > 0) {
        tft.fillRoundRect(4, SR_LIST_BOTTOM + 2, 36, 18, 3, HALEHOUND_GUNMETAL);
        tft.drawRoundRect(4, SR_LIST_BOTTOM + 2, 36, 18, 3, HALEHOUND_HOTPINK);
        tft.setTextColor(HALEHOUND_MAGENTA, HALEHOUND_GUNMETAL);
        tft.setCursor(10, SR_LIST_BOTTOM + 5);
        tft.print("UP");
    } else {
        tft.fillRect(4, SR_LIST_BOTTOM + 2, 36, 18, HALEHOUND_BLACK);
    }

    // NEXT button — right side of bottom bar
    if (scrollOffset + SR_VISIBLE_ROWS < entryCount) {
        tft.fillRoundRect(SCREEN_WIDTH - 46, SR_LIST_BOTTOM + 2, 42, 18, 3, HALEHOUND_GUNMETAL);
        tft.drawRoundRect(SCREEN_WIDTH - 46, SR_LIST_BOTTOM + 2, 42, 18, 3, HALEHOUND_HOTPINK);
        tft.setTextColor(HALEHOUND_MAGENTA, HALEHOUND_GUNMETAL);
        tft.setCursor(SCREEN_WIDTH - 40, SR_LIST_BOTTOM + 5);
        tft.print("NEXT");
    } else {
        tft.fillRect(SCREEN_WIDTH - 46, SR_LIST_BOTTOM + 2, 42, 18, HALEHOUND_BLACK);
    }
}

static void drawEmptyStateScreen(const char* title, const char* line1, const char* line2) {
    tft.fillScreen(HALEHOUND_BLACK);

    // Skull watermark
    tft.drawBitmap(0, 0, skull_bg_bitmap, SKULL_BG_WIDTH, SKULL_BG_HEIGHT, 0x0041);

    drawStatusBar();
    drawSRIconBar(false);

    // Nosifer glitch title
    drawGlitchTitle(ICON_BAR_BOTTOM + 6, ".SUB READ");

    // Centered content frame — double border
    int frameX = SCALE_X(12);
    int frameY = SCALE_Y(100);
    int frameW = SCALE_W(216);
    int frameH = SCALE_Y(120);
    tft.fillRoundRect(frameX, frameY, frameW, frameH, 6, HALEHOUND_BLACK);
    tft.drawRoundRect(frameX, frameY, frameW, frameH, 6, HALEHOUND_HOTPINK);
    tft.drawRoundRect(frameX - 2, frameY - 2, frameW + 4, frameH + 4, 8, HALEHOUND_MAGENTA);

    // Title inside frame
    drawCenteredText(frameY + SCALE_Y(12), title, HALEHOUND_HOTPINK, 1);

    // Separator inside frame
    tft.drawLine(frameX + 10, frameY + SCALE_Y(30), frameX + frameW - 10, frameY + SCALE_Y(30), HALEHOUND_VIOLET);

    // Info text
    tft.setTextFont(2);
    tft.setTextSize(1);
    tft.setTextColor(HALEHOUND_MAGENTA, HALEHOUND_BLACK);
    tft.setCursor(frameX + 10, frameY + SCALE_Y(38));
    tft.print(line1);
    tft.setTextColor(HALEHOUND_VIOLET, HALEHOUND_BLACK);
    tft.setCursor(frameX + 10, frameY + SCALE_Y(55));
    tft.print(line2);

    // Hint text at frame bottom
    drawCenteredText(frameY + frameH - SCALE_Y(18), "BACK TO EXIT", HALEHOUND_GUNMETAL, 1);

    // Bottom accent line
    tft.drawLine(0, SCREEN_HEIGHT - 2, SCREEN_WIDTH, SCREEN_HEIGHT - 2, HALEHOUND_HOTPINK);
}

static void drawNoSDScreen() {
    drawEmptyStateScreen(
        "NO SD CARD",
        "Insert SD card with",
        "/subghz/ folder"
    );
}

static void drawNoFilesScreen() {
    drawEmptyStateScreen(
        "NO .SUB FILES",
        "No files found in:",
        currentPath
    );
}

static void drawFreqError(uint32_t hz) {
    int boxY = SCALE_Y(130);
    int boxH = SCALE_Y(70);
    tft.fillRoundRect(SCALE_X(10), boxY, SCALE_W(220), boxH, 4, HALEHOUND_GUNMETAL);
    tft.drawRoundRect(SCALE_X(10), boxY, SCALE_W(220), boxH, 4, HALEHOUND_HOTPINK);
    tft.setTextFont(2);
    tft.setTextSize(1);
    drawCenteredText(boxY + 6,  "FREQ NOT SUPPORTED", HALEHOUND_HOTPINK, 1);
    char buf[28];
    snprintf(buf, sizeof(buf), "File: %lu Hz", (unsigned long)hz);
    drawCenteredText(boxY + 22, buf, HALEHOUND_MAGENTA, 1);
    drawCenteredText(boxY + 38, "300-348 / 387-464 / 779-928", HALEHOUND_VIOLET, 1);
}

static void drawTxScreen(const char* displayName, float freqMHz, SubProtocol proto) {
    tft.fillScreen(HALEHOUND_BLACK);
    tft.drawBitmap(0, 0, skull_bg_bitmap, SKULL_BG_WIDTH, SKULL_BG_HEIGHT, 0x0041);
    drawStatusBar();
    drawSRIconBar(false);

    // Glitch title
    drawGlitchTitle(ICON_BAR_BOTTOM + 4, ".SUB TX");

    // File name
    char shortName[22];
    strncpy(shortName, displayName, sizeof(shortName) - 1);
    shortName[sizeof(shortName) - 1] = '\0';
    drawCenteredText(SCALE_Y(66), shortName, HALEHOUND_BRIGHT, 1);

    // Frequency + protocol
    char freqStr[28];
    snprintf(freqStr, sizeof(freqStr), "FREQ: %.3f MHz", freqMHz);
    drawCenteredText(SCALE_Y(82), freqStr, HALEHOUND_MAGENTA, 1);

    char protoStr[24];
    snprintf(protoStr, sizeof(protoStr), "PROTO: %s", protoName(proto));
    drawCenteredText(SCALE_Y(96), protoStr, HALEHOUND_VIOLET, 1);

    // Double-border frame around progress area
    tft.drawRoundRect(SCALE_X(8), SCALE_Y(115), SCALE_W(224), SCALE_Y(55), 4, HALEHOUND_HOTPINK);
    tft.drawRoundRect(SCALE_X(6), SCALE_Y(113), SCALE_W(228), SCALE_Y(59), 6, HALEHOUND_MAGENTA);

    drawCenteredText(SCALE_Y(120), "TRANSMITTING...", HALEHOUND_HOTPINK, 1);

    tft.setTextColor(HALEHOUND_GUNMETAL);
    tft.setTextFont(2);
    tft.setTextSize(1);
    tft.setCursor(10, SCREEN_HEIGHT - 20);
    tft.print("BACK: ABORT");
}

static void drawTxProgress(int done, int total) {
    int pct = (total > 0) ? (done * 100 / total) : 0;
    drawProgressBar(SCALE_X(14), SCALE_Y(138), SCALE_W(212), SCALE_Y(10), pct, HALEHOUND_HOTPINK);
    char buf[20];
    snprintf(buf, sizeof(buf), "REP %d / %d", done + 1, total);
    tft.fillRect(SCALE_X(14), SCALE_Y(152), SCALE_W(212), SCALE_Y(12), HALEHOUND_BLACK);
    drawCenteredText(SCALE_Y(152), buf, HALEHOUND_MAGENTA, 1);
}

static void drawTxDone() {
    tft.fillRect(SCALE_X(14), SCALE_Y(120), SCALE_W(212), SCALE_Y(12), HALEHOUND_BLACK);
    drawCenteredText(SCALE_Y(120), "TX COMPLETE", HALEHOUND_BRIGHT, 1);
    delay(800);
}

static void drawTxError(const char* msg) {
    int boxY = SCALE_Y(180);
    tft.fillRoundRect(SCALE_X(10), boxY, SCALE_W(220), SCALE_Y(40), 4, HALEHOUND_GUNMETAL);
    tft.drawRoundRect(SCALE_X(10), boxY, SCALE_W(220), SCALE_Y(40), 4, HALEHOUND_HOTPINK);
    drawCenteredText(boxY + 6,  "TX ERROR", HALEHOUND_HOTPINK, 1);
    drawCenteredText(boxY + 20, msg,        HALEHOUND_MAGENTA, 1);
}

// ═══════════════════════════════════════════════════════════════════════════
// HEX DIGIT HELPER
// ═══════════════════════════════════════════════════════════════════════════

static uint8_t hexDigit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return 0;
}

// ═══════════════════════════════════════════════════════════════════════════
// FILE LOADING — Read entire .sub file into heap before CC1101 init
// Must be called BEFORE SPI.end() for CC1101 — SD still needs the bus
// ═══════════════════════════════════════════════════════════════════════════

static uint8_t* loadFileToRAM(const char* fullPath, size_t* outLen) {
    deselectAllCS();
    SPI.end();
    delay(5);
    SPI.begin(18, 19, 23, SD_CS);
    delay(5);
    if (!SD.begin(SD_CS, SPI, 4000000)) {
        #if CYD_DEBUG
        Serial.printf("[SUBREAD] loadFileToRAM: SD remount failed for %s\n", fullPath);
        #endif
        return nullptr;
    }
    File f = SD.open(fullPath, FILE_READ);
    if (!f) {
        #if CYD_DEBUG
        Serial.printf("[SUBREAD] loadFileToRAM: open failed for %s\n", fullPath);
        #endif
        return nullptr;
    }
    size_t sz = f.size();
    if (sz == 0 || sz > SR_MAX_FILE_SIZE) {
        f.close();
        return nullptr;
    }
    uint8_t* buf = (uint8_t*)malloc(sz + 1);
    if (!buf) { f.close(); return nullptr; }
    size_t got = f.read(buf, sz);
    buf[got] = '\0';
    f.close();
    *outLen = got;
    return buf;
}

// ═══════════════════════════════════════════════════════════════════════════
// RAW DATA PARSING — Walk buffer for RAW_Data lines
// ═══════════════════════════════════════════════════════════════════════════

static bool nextRawLine(const char* buf, size_t len, size_t* pos,
                        const char** dataStart, size_t* dataLen) {
    while (*pos < len) {
        const char* lineStart = buf + *pos;
        size_t lineBegin = *pos;
        while (*pos < len && buf[*pos] != '\n') (*pos)++;
        size_t lineEnd = *pos;
        if (*pos < len) (*pos)++;  // skip newline

        if (strncmp(lineStart, "RAW_Data:", 9) == 0) {
            *dataStart = lineStart + 9;
            *dataLen   = lineEnd - lineBegin - 9;
            return true;
        }
    }
    return false;
}

static int32_t parseNextInt(const char** pp, const char* end) {
    while (*pp < end && (**pp == ' ' || **pp == '\t')) (*pp)++;
    if (*pp >= end || **pp == '\n' || **pp == '\r') return 0;
    bool neg = false;
    if (**pp == '-') { neg = true; (*pp)++; }
    else if (**pp == '+') { (*pp)++; }
    if (*pp >= end || **pp < '0' || **pp > '9') return 0;
    int32_t v = 0;
    while (*pp < end && **pp >= '0' && **pp <= '9') { v = v * 10 + (**pp - '0'); (*pp)++; }
    return neg ? -v : v;
}

// ═══════════════════════════════════════════════════════════════════════════
// TRANSMIT — RAW PROTOCOL
// Bit-bang GDO0: positive value = HIGH for N µs, negative = LOW for N µs
// Same approach as the working SubGHz brute force TX path
// ═══════════════════════════════════════════════════════════════════════════

static void transmitRAW(const uint8_t* buf, size_t len) {
    const int repeats = 3;

    for (int rep = 0; rep < repeats && !abortTx; rep++) {
        drawTxProgress(rep, repeats);

        size_t pos = 0;
        const char* dataStart;
        size_t dataLen;

        while (nextRawLine((const char*)buf, len, &pos, &dataStart, &dataLen)) {
            const char* dp  = dataStart;
            const char* end = dataStart + dataLen;

            while (dp < end) {
                int32_t val = parseNextInt(&dp, end);
                if (val == 0) break;
                if (val > 0) {
                    digitalWrite(CC1101_GDO0, HIGH);
                    delayMicroseconds((uint32_t)val);
                } else {
                    digitalWrite(CC1101_GDO0, LOW);
                    delayMicroseconds((uint32_t)(-val));
                }
            }
            digitalWrite(CC1101_GDO0, LOW);  // idle between bursts
            delayMicroseconds(500);
        }

        delay(50);
        touchButtonsUpdate();
        if (isBackButtonTapped() || buttonPressed(BTN_BACK) || buttonPressed(BTN_BOOT))
            abortTx = true;
    }
    digitalWrite(CC1101_GDO0, LOW);
}

// ═══════════════════════════════════════════════════════════════════════════
// TRANSMIT — STATIC KEY (Princeton / CAME / Nice FLO)
// Uses RCSwitch bit-bang — protocol 1 with TE pulse length override
// Princeton + CAME share same bit encoding (1:3 ratio), different default TE
// Nice FLO has different sync ratio — close enough for most receivers
// ═══════════════════════════════════════════════════════════════════════════

static void transmitStaticKey(const uint8_t* buf, size_t len, SubProtocol proto) {
    uint32_t te       = 0;
    int      bitCount = 0;
    uint64_t keyVal   = 0;

    const char* p   = (const char*)buf;
    const char* end = p + len;
    char line[96];

    while (p < end) {
        int i = 0;
        while (p < end && i < (int)sizeof(line) - 1) {
            char c = *p++;
            if (c == '\n') break;
            if (c == '\r') continue;
            line[i++] = c;
        }
        line[i] = '\0';

        if      (strncmp(line, "TE:",  3) == 0) te       = (uint32_t)atol(line + 3);
        else if (strncmp(line, "Bit:", 4) == 0) bitCount = atoi(line + 4);
        else if (strncmp(line, "Key:", 4) == 0) {
            const char* kp = line + 4;
            while (*kp == ' ') kp++;
            keyVal = 0;
            while (*kp && *(kp+1)) {
                if (*kp == ' ') { kp++; continue; }
                keyVal = (keyVal << 8) | ((uint64_t)((hexDigit(kp[0]) << 4) | hexDigit(kp[1])));
                kp += 2;
            }
        }
    }

    if (bitCount == 0 || bitCount > 64) {
        drawTxError("BAD KEY/BIT");
        delay(1500);
        return;
    }

    // Protocol-default TE when file omits it
    if (te == 0) {
        if      (proto == PROTO_CAME)    te = 320;
        else if (proto == PROTO_NICEFLO) te = 500;
        else                             te = 350;  // Princeton default
    }

    RCSwitch txSwitch;
    txSwitch.enableTransmit(CC1101_GDO0);
    txSwitch.setProtocol(1);          // Protocol 1: sync + OOK 1:3 bits
    txSwitch.setPulseLength(te);
    txSwitch.setRepeatTransmit(5);

    unsigned long code = (unsigned long)(keyVal & ((bitCount < 64) ? ((1ULL << bitCount) - 1) : 0xFFFFFFFFFFFFFFFFULL));

    const int repeats = 5;
    for (int rep = 0; rep < repeats && !abortTx; rep++) {
        drawTxProgress(rep, repeats);
        txSwitch.send(code, bitCount);
        delay(30);
        touchButtonsUpdate();
        if (isBackButtonTapped() || buttonPressed(BTN_BACK) || buttonPressed(BTN_BOOT))
            abortTx = true;
    }

    txSwitch.disableTransmit();
}

// ═══════════════════════════════════════════════════════════════════════════
// START TRANSMIT — Load file → init CC1101 → TX → return to browser
// File loaded to RAM first, then SD closed, then CC1101 takes the SPI bus
// ═══════════════════════════════════════════════════════════════════════════

static void startTransmit(int idx) {
    if (idx < 0 || idx >= entryCount || entries[idx].isDir) return;
    const DirEntry& e = entries[idx];

    if (!e.freqValid) {
        drawFreqError(e.freqHz);
        delay(2000);
        drawBrowseScreen();
        drawEntryList();
        return;
    }

    float freqMHz = e.freqHz / 1000000.0f;  // Exact frequency — no normalization
    abortTx = false;

    phase = PHASE_TRANSMITTING;
    drawTxScreen(e.displayName, freqMHz, e.proto);

    // Step 1: Load file into RAM while SD bus is still up
    size_t fileLen = 0;
    uint8_t* fileBuf = loadFileToRAM(e.fullPath, &fileLen);
    if (!fileBuf) {
        drawTxError("OPEN FAILED");
        delay(2000);
        phase = PHASE_BROWSE;
        rearmSD();
        drawBrowseScreen();
        drawEntryList();
        return;
    }

    // Step 2: Init CC1101 (calls SPI.end — SD no longer accessible)
    if (!cc1101Ready) {
        if (!initCC1101forTX(freqMHz)) {
            free(fileBuf);
            drawTxError("CC1101 NOT FOUND");
            delay(2000);
            phase = PHASE_BROWSE;
            rearmSD();
            drawBrowseScreen();
            drawEntryList();
            return;
        }
        cc1101Ready = true;
    } else {
        srPaSetIdle();
        ELECHOUSE_cc1101.setMHZ(freqMHz);
    }

    // Step 3: TX from RAM
    pinMode(CC1101_GDO0, OUTPUT);
    digitalWrite(CC1101_GDO0, LOW);
    delay(5);
    srPaSetTx();
    delay(10);

    if (e.proto == PROTO_RAW || e.proto == PROTO_UNKNOWN) {
        if (e.proto == PROTO_UNKNOWN) {
            tft.setTextFont(2);
            tft.setTextSize(1);
            drawCenteredText(SCALE_Y(175), "UNKNOWN PROTO - TRYING RAW", HALEHOUND_VIOLET, 1);
            delay(600);
        }
        transmitRAW(fileBuf, fileLen);
    } else {
        transmitStaticKey(fileBuf, fileLen, e.proto);
    }

    free(fileBuf);

    if (!abortTx) drawTxDone();

    srPaSetIdle();
    spiDeselect();

    // Return to browser
    phase = PHASE_BROWSE;
    rearmSD();
    drawBrowseScreen();
    drawEntryList();
}

// ═══════════════════════════════════════════════════════════════════════════
// FOLDER NAVIGATION
// ═══════════════════════════════════════════════════════════════════════════

static void enterDir(int idx) {
    if (idx < 0 || idx >= entryCount || !entries[idx].isDir) return;

    strncpy(currentPath, entries[idx].fullPath, sizeof(currentPath) - 1);
    currentPath[sizeof(currentPath) - 1] = '\0';

    rearmSD();
    scanDirectory(currentPath);

    if (entryCount == 0) {
        phase = PHASE_NO_FILES;
        drawNoFilesScreen();
    } else {
        phase = PHASE_BROWSE;
        drawBrowseScreen();
        drawEntryList();
    }
}

static void goUp() {
    if (isAtRoot()) {
        exitRequested = true;
        return;
    }
    navigateUp();
    rearmSD();
    scanDirectory(currentPath);
    phase = PHASE_BROWSE;
    drawBrowseScreen();
    drawEntryList();
}

// ═══════════════════════════════════════════════════════════════════════════
// TOUCH HANDLING
// ═══════════════════════════════════════════════════════════════════════════

static void handleBrowseTouch() {
    static unsigned long lastTap = 0;
    if (millis() - lastTap < 200) return;

    uint16_t tx, ty;
    if (!getTouchPoint(&tx, &ty)) return;
    lastTap = millis();

    // Icon bar — back handled by isBackButtonTapped() in loop()
    if (ty <= ICON_BAR_TOUCH_BOTTOM) return;

    // Bottom bar — UP (left half) or NEXT (right half)
    if (ty > SR_LIST_BOTTOM) {
        if (tx < SCREEN_WIDTH / 2) {
            if (scrollOffset > 0) {
                scrollOffset--;
                drawEntryList();
            }
        } else {
            if (scrollOffset + SR_VISIBLE_ROWS < entryCount) {
                scrollOffset++;
                drawEntryList();
            }
        }
        return;
    }

    // Row tap in list area
    int row = (ty - SR_LIST_Y) / SR_ROW_HEIGHT;
    int idx = scrollOffset + row;
    if (idx < 0 || idx >= entryCount) return;

    int prevSel = selectedIndex;
    selectedIndex = idx;

    if (entries[idx].isDir) {
        selectedIndex = -1;
        enterDir(idx);
    } else {
        // Highlight selected row, update flash icon
        if (prevSel != idx) {
            drawSRIconBar(!entries[idx].isDir && entries[idx].freqValid);
            drawEntryRow(idx, true);
            if (prevSel >= 0 && prevSel != idx) drawEntryRow(prevSel, false);
        }
        delay(120);
        startTransmit(idx);
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// PUBLIC API
// ═══════════════════════════════════════════════════════════════════════════

void setup() {
    if (initialized) return;

    exitRequested = false;
    sdMounted     = false;
    cc1101Ready   = false;
    abortTx       = false;
    entryCount    = 0;
    scrollOffset  = 0;
    selectedIndex = -1;
    phase         = PHASE_NO_SD;

    // Heap-allocate entry list — freed in cleanup(), no permanent DRAM cost
    entries = (DirEntry*)malloc(SR_MAX_ENTRIES * sizeof(DirEntry));
    if (!entries) {
        #if CYD_DEBUG
        Serial.println("[SUBREAD] malloc failed for entries");
        #endif
        exitRequested = true;
        initialized = true;
        return;
    }

    strncpy(currentPath, SR_ROOT, sizeof(currentPath) - 1);
    currentPath[sizeof(currentPath) - 1] = '\0';

    tft.fillScreen(HALEHOUND_BLACK);
    drawStatusBar();
    drawSRIconBar(false);

    sdMounted = mountSD();

    if (!sdMounted) {
        drawNoSDScreen();
        initialized = true;
        return;
    }

    scanDirectory(currentPath);

    if (entryCount == 0) {
        phase = PHASE_NO_FILES;
        drawNoFilesScreen();
    } else {
        phase = PHASE_BROWSE;
        drawBrowseScreen();
        drawEntryList();
    }

    initialized = true;
}

void loop() {
    if (!initialized) return;

    touchButtonsUpdate();

    // Back detection — subfolder = go up, root = exit
    if (isBackButtonTapped() || buttonPressed(BTN_BACK) || buttonPressed(BTN_BOOT)) {

        if (phase == PHASE_NO_FILES && !isAtRoot()) {
            goUp();
            return;
        }

        if (phase == PHASE_BROWSE && !isAtRoot()) {
            goUp();
            return;
        }

        exitRequested = true;
        return;
    }

    switch (phase) {
        case PHASE_NO_SD:
        case PHASE_NO_FILES:
            break;

        case PHASE_BROWSE:
            handleBrowseTouch();
            break;

        case PHASE_TRANSMITTING:
            phase = PHASE_BROWSE;
            drawBrowseScreen();
            drawEntryList();
            break;
    }
}

bool isExitRequested() {
    return exitRequested;
}

void cleanup() {
    if (!initialized) return;

    if (cc1101Ready) {
        srPaSetIdle();
        cc1101Ready = false;
    }

    digitalWrite(CC1101_GDO0, LOW);
    spiDeselect();

    if (entries) {
        free(entries);
        entries = nullptr;
    }

    strncpy(currentPath, SR_ROOT, sizeof(currentPath) - 1);
    phase         = PHASE_NO_SD;
    initialized   = false;
    exitRequested = false;
    sdMounted     = false;
    entryCount    = 0;
    scrollOffset  = 0;
    selectedIndex = -1;
    abortTx       = false;
}

}  // namespace SubRead
