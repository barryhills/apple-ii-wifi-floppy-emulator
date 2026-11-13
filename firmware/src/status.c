#include <stdio.h>
#include "pico/stdlib.h"
#include "pico/unique_id.h"

#include "net.h"
#include "status.h"

// Copy s into out as JSON string content (no quotes). SSIDs may hold any
// bytes: escape quote, backslash and control bytes (\u00XX); pass UTF-8 through.
static void json_escape(char *out, size_t n, const char *s)
{
    size_t o = 0;
    for (; *s && o + 6 < n; s++) {   // room for \u00XX + NUL
        unsigned char ch = (unsigned char)*s;
        if (ch == '"' || ch == '\\') {
            out[o++] = '\\';
            out[o++] = (char)ch;
        } else if (ch < 0x20 || ch == 0x7f) {
            o += (size_t)snprintf(out + o, n - o, "\\u%04x", ch);
        } else {
            out[o++] = (char)ch;
        }
    }
    out[o] = 0;
}

// Disk fields are placeholders until the drive emulation exists:
// mounted slot and head position are null, and the drive reports
// write-protected until writes are implemented.
size_t status_json(char *buf, size_t n)
{
    char ssid[32 * 6 + 1];      // worst case: 32 bytes, each \u00XX
    json_escape(ssid, sizeof ssid, net_ssid());

    char id[2 * PICO_UNIQUE_BOARD_ID_SIZE_BYTES + 1];
    pico_get_unique_board_id_string(id, sizeof id);

    char rssi[16] = "null";
    int32_t r;
    if (net_rssi(&r))
        snprintf(rssi, sizeof rssi, "%ld", (long)r);

    int len = snprintf(buf, n,
        "{\"firmware\":\"%s\",\"board_id\":\"%s\",\"hostname\":\"%s\","
        "\"ssid\":\"%s\",\"ip\":\"%s\",\"rssi_dbm\":%s,\"uptime_s\":%llu,"
        "\"mounted_slot\":null,\"halftrack\":null,\"write_protect\":true}\n",
        A2DRIVE_VERSION, id, net_hostname(), ssid, net_ip_str(), rssi,
        (unsigned long long)(time_us_64() / 1000000));
    if (len < 0)
        len = 0;
    if ((size_t)len >= n)
        len = (int)n - 1;
    return (size_t)len;
}
