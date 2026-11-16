// Link configuration shared by station and gateway. Both must be built with identical RF values.
#pragma once

#if __has_include("secrets.h")
#include "secrets.h"
#else
#warning "include/secrets.h not found: using secrets.example.h (placeholder HMAC key, dummy WiFi)"
#include "secrets.example.h"
#endif

// ---- RF ---------------------------------------------------------------------------------------
// Use a frequency that is legal where the station is installed, e.g. "868.1" (EU868; used in Turkey),
// "915.0" (US915/AU915), "865.0625" (IN865). String, passed verbatim to AT+TEST=RFCFG.
#ifndef WX_FREQ_MHZ
#define WX_FREQ_MHZ "868.1"
#endif
#ifndef WX_SF
#define WX_SF "SF9"  // SF7 = fastest/shortest range ... SF12 = slowest/longest range
#endif
#define WX_BW_KHZ      125
#define WX_TX_PREAMBLE 12
#define WX_RX_PREAMBLE 15
#ifndef WX_TX_POWER_DBM
#define WX_TX_POWER_DBM 14  // Wio-E5-LE maximum (all our LoRa-E5 mini boards carry the LE module)
#endif

#define E5_BAUD 9600  // factory default of the Seeed AT firmware

// ---- Protocol timing --------------------------------------------------------------------------
#define WX_ACK_TIMEOUT_MS    2500
#define WX_MAX_TX_ATTEMPTS   3     // first try + 2 retries
#define WX_RETRY_BACKOFF_MIN 200   // ms, random backoff between retries
#define WX_RETRY_BACKOFF_MAX 900

// ---- Station schedule -------------------------------------------------------------------------
#ifndef WX_INTERVAL_S
#define WX_INTERVAL_S 300          // one reading every 5 minutes
#endif
#define WX_LOW_BATT_MV       3500  // below this the interval is stretched
#define WX_LOW_BATT_INTERVAL_FACTOR 4
