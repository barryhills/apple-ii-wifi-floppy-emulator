#pragma once
#include <stdbool.h>
#include <stddef.h>

// Network settings: hostname, WiFi SSID and password.
//
// Stored in the reserved flash sector (STORE_CONFIG_OFFSET) with a CRC; if
// none is stored, the build-time defaults from firmware/config.json apply.
// If stored WiFi settings cannot join, net.c falls back to the build-time
// ones for the rest of that boot (config_use_builtin_wifi()).

#define CFG_SSID_MAX      32
#define CFG_PASSWORD_MAX  63
#define CFG_HOSTNAME_MAX  63

typedef enum { CFG_BUILTIN, CFG_STORED, CFG_STORED_FAILED } cfg_source_t;

void         config_load(void);
const char  *config_ssid(void);
const char  *config_password(void);
const char  *config_hostname(void);
cfg_source_t config_source(void);
const char  *config_source_text(void);
bool         config_builtin_wifi_differs(void);
void         config_use_builtin_wifi(void);     // fallback for this boot

// Check new values; returns NULL if fine, else a message. password NULL or
// "" keeps the current password.
const char  *config_check(const char *ssid, const char *password, const char *hostname);

// Write to flash (main loop only; interrupts off briefly). 0 or -1 + err.
int          config_save(const char *ssid, const char *password, const char *hostname,
                         char *err, size_t errn);
