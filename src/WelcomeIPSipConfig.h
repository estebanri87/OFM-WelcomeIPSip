#pragma once

// OFM-WelcomeIPSip is ESP32-only.
//
// Pairing against the MyBuildings cloud portal needs TLS/mTLS (mbedTLS) and an RSA-2048
// keypair/CSR plus RSA decrypt of the SIP password; the runtime SIP link needs TLS to the
// panel. That needs mbedTLS with hardware SHA and PSRAM for the TLS buffers. On RP2040
// BearSSL's 4 KB input buffer cannot take the TLS records involved here.
//
// Every header and source file of this module is wrapped in OPENKNX_WELCOMEIP_SIP, and the
// parent project guards its addModule() call with it. This header itself stays unguarded so
// that guard can be evaluated.

#if defined(ARDUINO_ARCH_ESP32) && (defined(KNX_IP_WIFI) || defined(KNX_IP_LAN))
#define OPENKNX_WELCOMEIP_SIP 1
#endif
