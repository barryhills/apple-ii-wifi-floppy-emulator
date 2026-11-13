#pragma once
#include <stddef.h>

#define STATUS_JSON_MAX 768   // worst-case SSID escapes to 192 chars

// Write the /status JSON object into buf (NUL-terminated); returns its length.
size_t status_json(char *buf, size_t n);
