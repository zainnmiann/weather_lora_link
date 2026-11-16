// Placeholder secrets. Copy to include/secrets.h (git-ignored) and fill in real values.
// The station and the gateway MUST be built with the same WX_HMAC_KEY.
#pragma once

// Gateway only
#define WIFI_SSID     "your-ssid"
#define WIFI_PASSWORD "your-wifi-password"
#define MQTT_HOST     "192.168.1.10"
#define MQTT_PORT     1883
#define MQTT_USER     ""  // empty = anonymous
#define MQTT_PASSWORD ""

// 16 random bytes. Generate a fresh key with:
//   python3 -c "import os;print(', '.join(f'0x{b:02x}' for b in os.urandom(16)))"
#define WX_HMAC_KEY {0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, \
                     0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff}
