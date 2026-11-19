#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/flash.h"

#include "config.h"
#include "crc32.h"
#include "store.h"

#define CFG_MAGIC    0x46433241u     // "A2CF"
#define CFG_VERSION  1

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t size;
    char     ssid[CFG_SSID_MAX + 1];
    char     password[CFG_PASSWORD_MAX + 1];
    char     hostname[CFG_HOSTNAME_MAX + 1];
    uint32_t crc;                       // CRC32 of all bytes before it
} cfg_rec_t;

// Live values. Stable storage: lwIP keeps a pointer to the hostname.
static char ssid[CFG_SSID_MAX + 1];
static char password[CFG_PASSWORD_MAX + 1];
static char hostname[CFG_HOSTNAME_MAX + 1];

static void copy(char *dst, size_t n, const char *src)
{
    size_t l = strnlen(src, n - 1);
    memcpy(dst, src, l);
    dst[l] = 0;
}

void config_load(void)
{
    const cfg_rec_t *r = (const cfg_rec_t *)(XIP_BASE + STORE_CONFIG_OFFSET);
    if (r->magic == CFG_MAGIC && r->version == CFG_VERSION && r->size == sizeof *r &&
        r->crc == crc32_update(0, r, offsetof(cfg_rec_t, crc)) &&
        memchr(r->ssid, 0, sizeof r->ssid) && memchr(r->password, 0, sizeof r->password) &&
        memchr(r->hostname, 0, sizeof r->hostname)) {
        copy(ssid, sizeof ssid, r->ssid);
        copy(password, sizeof password, r->password);
        copy(hostname, sizeof hostname, r->hostname);
        printf("config: WiFi \"%s\", hostname %s\n", ssid, hostname);
    } else {
        ssid[0] = password[0] = 0;              // not configured: setup mode
        copy(hostname, sizeof hostname, CFG_DEFAULT_HOSTNAME);
        printf("config: no WiFi settings, hostname %s\n", hostname);
    }
}

const char *config_ssid(void)     { return ssid; }
const char *config_password(void) { return password; }
const char *config_hostname(void) { return hostname; }

const char *config_check(const char *s, const char *pw, const char *h)
{
    size_t n = strlen(s);
    if (n < 1 || n > CFG_SSID_MAX)
        return "WiFi network name must be 1 to 32 characters";
    if (pw && *pw) {
        n = strlen(pw);
        if (n < 8 || n > CFG_PASSWORD_MAX)
            return "WPA2 password must be 8 to 63 characters";
        for (const char *p = pw; *p; p++)
            if (*p < 0x20 || *p > 0x7e)
                return "password must be printable ASCII";
    }
    n = strlen(h);
    if (n < 1 || n > CFG_HOSTNAME_MAX)
        return "hostname must be 1 to 63 characters";
    if (h[0] == '-' || h[n - 1] == '-')
        return "hostname cannot start or end with '-'";
    for (const char *p = h; *p; p++)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '-'))
            return "hostname may use only a-z, 0-9 and '-'";
    return NULL;
}

int config_save(const char *s, const char *pw, const char *h, char *err, size_t errn)
{
    const char *why = config_check(s, pw, h);
    if (why) {
        snprintf(err, errn, "%s", why);
        return -1;
    }
    static cfg_rec_t r;                 // static: off the shared stack
    memset(&r, 0, sizeof r);
    r.magic = CFG_MAGIC;
    r.version = CFG_VERSION;
    r.size = sizeof r;
    copy(r.ssid, sizeof r.ssid, s);
    copy(r.password, sizeof r.password, (pw && *pw) ? pw : password);
    copy(r.hostname, sizeof r.hostname, h);
    r.crc = crc32_update(0, &r, offsetof(cfg_rec_t, crc));
    if (store_write_raw_sector(STORE_CONFIG_OFFSET, &r, sizeof r, err, errn))
        return -1;
    printf("config: saved (hostname %s)\n", r.hostname);
    return 0;
}
