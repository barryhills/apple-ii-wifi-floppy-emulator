#pragma once
#include <stdbool.h>
#include <stddef.h>

// Network settings: hostname, WiFi SSID and password.
//
// Stored in the reserved flash sector (STORE_CONFIG_OFFSET) with a CRC. The
// firmware carries no WiFi network or password (Barry, 2026-11-19: nothing
// secret in the image): with nothing stored, the SSID is empty, which sends
// the drive into setup mode, and the hostname is CFG_DEFAULT_HOSTNAME.

#define CFG_SSID_MAX      32
#define CFG_PASSWORD_MAX  63
#define CFG_HOSTNAME_MAX  63

#define CFG_DEFAULT_HOSTNAME "a2drive"

void         config_load(void);
const char  *config_ssid(void);
const char  *config_password(void);
const char  *config_hostname(void);

// Check new values; returns NULL if fine, else a message. password NULL or
// "" keeps the current password.
const char  *config_check(const char *ssid, const char *password, const char *hostname);

// Write to flash (main loop only; interrupts off briefly). 0 or -1 + err.
int          config_save(const char *ssid, const char *password, const char *hostname,
                         char *err, size_t errn);
