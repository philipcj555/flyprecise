#pragma once
// Compile-time configuration for the FlyPrecise ESP32 dashboard.

#ifndef FAKE_UNO
#define FAKE_UNO 0
#endif

// Wi-Fi access point. Password is overridable from settings.json ("ap_pass").
#define AP_SSID            "FlyPrecise"
#define AP_PASS_DEFAULT    "flyprecise"    // TODO team: pick the real one (open question 2)
#define AP_IP              IPAddress(192, 168, 4, 1)
#define AP_NETMASK         IPAddress(255, 255, 255, 0)
#define MDNS_NAME          "flyprecise"    // http://flyprecise.local

// UART1 to the Uno. ESP32-S3 pin matrix: any GPIO works; 17/18 are free on the DevKitM-1.
#define UNO_UART_BAUD      9600
#define UNO_UART_TX_PIN    17
#define UNO_UART_RX_PIN    18

// Link is considered lost if no $T line arrives for this long.
#define UNO_LINK_TIMEOUT_MS 3000

// Storage
#define SESSIONS_PATH      "/sessions.csv"
#define SESSIONS_ROTATED   "/sessions.1.csv"
#define SETTINGS_PATH      "/settings.json"
#define SESSIONS_CSV_HEADER "id,epoch,target_g,delivered_ml,delivered_g,density_gml,reason\n"
// ~60 bytes/row * 5000 rows. When exceeded, sessions.csv rotates to sessions.1.csv.
#define SESSIONS_MAX_BYTES (300UL * 1024UL)
#define HISTORY_ROWS       50

// Target validation (kg)
#define TARGET_MIN_KG      0.1f
#define TARGET_MAX_KG      50.0f

// Unit conversions
#define GML_TO_LBGAL       8.345404f
#define G_TO_LB            0.00220462f
#define ML_TO_GAL          (1.0f / 3785.41f)
