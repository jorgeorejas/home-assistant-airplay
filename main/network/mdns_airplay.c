#include "esp_log.h"
#include "esp_mac.h"
#include "mdns.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "airplay_pair.h"
#include "mdns_airplay.h"
#include "rtsp_handlers.h"
#include "wifi.h"
#include "settings.h"

static const char *TAG = "mdns_airplay";

/* RFC-1123 hostname slug from a possibly-UTF-8 friendly device name.
   Lowercases ASCII, folds common Spanish/Latin diacritics to their bare
   form, replaces runs of separators with a single '-', strips edges, and
   caps at 63 chars. Falls back to "ha-airplay" if nothing usable remains. */
static void make_hostname_slug(const char *name, char *out, size_t out_size) {
  if (out_size < 2) {
    return;
  }

  size_t w = 0;
  bool last_was_sep = true; /* leading separators get squashed */

  for (size_t i = 0; name[i] && w < out_size - 1;) {
    unsigned char b0 = (unsigned char)name[i];
    char c = 0;

    if (b0 < 0x80) {
      /* plain ASCII */
      if (isalnum(b0)) {
        c = (char)tolower(b0);
      } else {
        c = '-';
      }
      i += 1;
    } else if ((b0 & 0xE0) == 0xC0 && (unsigned char)name[i + 1]) {
      /* 2-byte UTF-8 — fold a few common Latin-1 letters */
      uint32_t cp = ((b0 & 0x1F) << 6) | ((unsigned char)name[i + 1] & 0x3F);
      switch (cp) {
      case 0xE0: case 0xE1: case 0xE2: case 0xE3: case 0xE4: case 0xE5:
      case 0xC0: case 0xC1: case 0xC2: case 0xC3: case 0xC4: case 0xC5:
        c = 'a'; break;
      case 0xE8: case 0xE9: case 0xEA: case 0xEB:
      case 0xC8: case 0xC9: case 0xCA: case 0xCB:
        c = 'e'; break;
      case 0xEC: case 0xED: case 0xEE: case 0xEF:
      case 0xCC: case 0xCD: case 0xCE: case 0xCF:
        c = 'i'; break;
      case 0xF2: case 0xF3: case 0xF4: case 0xF5: case 0xF6:
      case 0xD2: case 0xD3: case 0xD4: case 0xD5: case 0xD6:
        c = 'o'; break;
      case 0xF9: case 0xFA: case 0xFB: case 0xFC:
      case 0xD9: case 0xDA: case 0xDB: case 0xDC:
        c = 'u'; break;
      case 0xF1: case 0xD1: /* ñ / Ñ */
        c = 'n'; break;
      case 0xE7: case 0xC7: /* ç / Ç */
        c = 'c'; break;
      default:
        c = '-';
        break;
      }
      i += 2;
    } else {
      /* 3- or 4-byte UTF-8 — drop and treat as separator */
      i += 1;
      while (name[i] && ((unsigned char)name[i] & 0xC0) == 0x80) {
        i++;
      }
      c = '-';
    }

    if (c == '-') {
      if (!last_was_sep) {
        out[w++] = '-';
        last_was_sep = true;
      }
    } else {
      out[w++] = c;
      last_was_sep = false;
    }
  }

  /* trim trailing '-' */
  while (w > 0 && out[w - 1] == '-') {
    w--;
  }
  out[w] = '\0';

  if (w == 0) {
    strncpy(out, "ha-airplay", out_size - 1);
    out[out_size - 1] = '\0';
  }
}

// Feature flags are defined in rtsp_handlers.h (shared with /info handler)

// Protocol version
#ifdef CONFIG_AIRPLAY_FORCE_V1
#define AIRPLAY_PROTOCOL_VERSION "1"
#else
#define AIRPLAY_PROTOCOL_VERSION "2"
#endif
#define AIRPLAY_SOURCE_VERSION "377.40.00"

// Flags: 0x4 = audio receiver
#define AIRPLAY_FLAGS "0x4"

// Model identifier - AudioAccessory for speaker appearance
// AppleTV3,2 = Apple TV, AudioAccessory5,1 = HomePod mini (speaker)
#define AIRPLAY_MODEL "AudioAccessory5,1"

void mdns_airplay_init(void) {
  char mac_str[18];
  char device_id[18];
  char features_str[32];
  char service_name[80];
  char pk_str[65]; // 32 bytes = 64 hex chars + null
  char device_name[65];

  // Get device name from settings
  settings_get_device_name(device_name, sizeof(device_name));

  // Get MAC address
  wifi_get_mac_str(mac_str, sizeof(mac_str));
  strncpy(device_id, mac_str, sizeof(device_id));

  // Get real Ed25519 public key from HAP module
  const uint8_t *pk = hap_get_public_key();
  for (int i = 0; i < 32; i++) {
    snprintf(pk_str + (size_t)i * 2, 3, "%02x", pk[i]);
  }

  // Format features as "hi,lo" hex string
  snprintf(features_str, sizeof(features_str), "0x%X,0x%X", AIRPLAY_FEATURES_LO,
           AIRPLAY_FEATURES_HI);

  // Create service name for RAOP: <mac>@<name>
  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  snprintf(service_name, sizeof(service_name), "%02X%02X%02X%02X%02X%02X@%s",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], device_name);

  // Initialize mDNS
  ESP_ERROR_CHECK(mdns_init());

  // mDNS hostname must be RFC-1123 (ASCII, no spaces). Slug the friendly
  // device name so `<slug>.local` works in browsers, while service
  // instance names below keep the original UTF-8 friendly name iOS shows.
  char hostname[64];
  make_hostname_slug(device_name, hostname, sizeof(hostname));
  ESP_ERROR_CHECK(mdns_hostname_set(hostname));
  ESP_LOGI(TAG, "mDNS hostname: %s.local (friendly: %s)", hostname,
           device_name);

#ifndef CONFIG_AIRPLAY_FORCE_V1
  // ========================================
  // _airplay._tcp service (port 7000)
  // Only registered for AirPlay 2 mode
  // ========================================
  mdns_txt_item_t airplay_txt[] = {
      {"deviceid", device_id},
      {"features", features_str},
      {"flags", AIRPLAY_FLAGS},
      {"model", AIRPLAY_MODEL},
      {"pk", pk_str},
      {"pi", "00000000-0000-0000-0000-000000000000"}, // Pairing identity UUID
      {"srcvers", AIRPLAY_SOURCE_VERSION},
      {"vv", AIRPLAY_PROTOCOL_VERSION},
      {"acl", "0"},
  };

  esp_err_t err =
      mdns_service_add(device_name, "_airplay", "_tcp", 7000, airplay_txt,
                       sizeof(airplay_txt) / sizeof(airplay_txt[0]));
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to add _airplay._tcp service: %s",
             esp_err_to_name(err));
  }
#endif

  // ========================================
  // _raop._tcp service (port 7000)
  // RAOP = Remote Audio Output Protocol
  // Service name format: <MAC>@<DeviceName>
  // ========================================
#ifdef CONFIG_AIRPLAY_FORCE_V1
  // AirPlay v1 (classic RAOP): match squeezelite-esp32 txt record format.
  // No features, no pk, no HAP pairing — just classic RAOP fields.
  mdns_txt_item_t raop_txt[] = {
      {"am", AIRPLAY_MODEL}, {"tp", "UDP"}, // Transport protocol
      {"sm", "false"},                      // Sharing mode
      {"sv", "false"},                      // Server version (unused)
      {"ek", "1"},                          // Encryption key available
      {"et", "0,1"},                        // Encryption types: none, RSA
      {"md", "0,1,2"},                      // Metadata types
      {"cn", "0,1"},                        // Audio codecs: PCM, ALAC
      {"ch", "2"},                          // Channels
      {"ss", "16"},                         // Sample size (bits)
      {"sr", "44100"},                      // Sample rate
      {"vn", "3"},                          // Version number
      {"txtvers", "1"},                     // TXT record version
  };
#else
  mdns_txt_item_t raop_txt[] = {
      {"am", AIRPLAY_MODEL},
      {"cn", "0,1,2,3"},     // Audio codecs: PCM, ALAC, AAC, AAC-ELD
      {"da", "true"},        // Digest auth
      {"et", "0,3,5"},       // Encryption types
      {"ft", features_str},  // Features (same as airplay)
      {"md", "0,1,2"},       // Metadata types
      {"pk", pk_str},        // Public key
      {"sf", AIRPLAY_FLAGS}, // Status flags
      {"tp", "UDP"},         // Transport protocol
      {"vn", "65537"},       // Version number
      {"vs", AIRPLAY_SOURCE_VERSION},
      {"vv", AIRPLAY_PROTOCOL_VERSION},
  };
#endif

  esp_err_t err_raop =
      mdns_service_add(service_name, "_raop", "_tcp", 7000, raop_txt,
                       sizeof(raop_txt) / sizeof(raop_txt[0]));
  if (err_raop != ESP_OK) {
    ESP_LOGE(TAG, "Failed to add _raop._tcp service: %s",
             esp_err_to_name(err_raop));
  }
}
