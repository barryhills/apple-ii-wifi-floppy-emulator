#include <malloc.h>
#include <stdio.h>
#include "pico/stdlib.h"
#include "pico/unique_id.h"

#include "clock.h"
#include "config.h"
#include "diag.h"
#include "drive.h"
#include "json.h"
#include "net.h"
#include "status.h"
#include "store.h"

// "drive" is the emulated drive's state (drive.h). The drive reports
// write-protected until writes are implemented (milestone 6).
//
// Scratch buffers are static to keep this off the (small) interrupt-time
// stack, so callers must hold the lwIP lock (lwIP callbacks already do).
extern char end, __HeapLimit;       // linker: heap from end up to __HeapLimit

size_t status_json(char *buf, size_t n)
{
    // newlib's malloc never gives memory back to sbrk, so arena is the
    // heap's high-water mark; headroom is what it has never touched.
    struct mallinfo mi = mallinfo();
    unsigned long heap_headroom = (unsigned long)(&__HeapLimit - &end) - (unsigned long)mi.arena;

    static char ssid[32 * 6 + 1];
    json_escape(ssid, sizeof ssid, net_ssid());

    char id[2 * PICO_UNIQUE_BOARD_ID_SIZE_BYTES + 1];
    pico_get_unique_board_id_string(id, sizeof id);

    char rssi[16] = "null";
    int32_t r;
    if (net_rssi(&r))
        snprintf(rssi, sizeof rssi, "%ld", (long)r);

    char now[24] = "null";
    int64_t t = clock_unix();
    if (t)
        snprintf(now, sizeof now, "%lld", (long long)t);

    static char image[IMAGE_NAME_MAX * 6 + 200];
    snprintf(image, sizeof image, "null");
    image_info_t im;
    if (store_image(&im)) {
        static char name[IMAGE_NAME_MAX * 6];
        char hex[65], up[24] = "null";
        json_escape(name, sizeof name, im.name);
        sha256_hex(im.sha256, hex);
        if (im.uploaded)
            snprintf(up, sizeof up, "%lld", (long long)im.uploaded);
        snprintf(image, sizeof image,
                 "{\"name\":\"%s\",\"format\":\"%s\",\"size\":%lu,\"sha256\":\"%s\","
                 "\"uploaded\":%s,\"verified\":%s}",
                 name, store_format_name(im.format), (unsigned long)im.size, hex, up,
                 im.verified ? "true" : "false");
    }

    static char drive[1400];
    drive_status_json(drive, sizeof drive);

    int len = snprintf(buf, n,
        "{\"firmware\":\"%s\",\"board_id\":\"%s\",\"hostname\":\"%s\","
        "\"ssid\":\"%s\",\"ip\":\"%s\",\"rssi_dbm\":%s,\"uptime_s\":%llu,"
        "\"time\":%s,\"mounted_row\":%d,\"image\":%s,\"drive\":%s,\"write_protect\":true,"
        "\"last_reset\":\"%s\",\"stack_free\":%lu,\"heap_headroom\":%lu,\"heap_in_use\":%lu,\"settings\":\"%s\",\"setup_mode\":%s}\n",
        A2DRIVE_VERSION, id, net_hostname(), ssid, net_ip_str(), rssi,
        (unsigned long long)(time_us_64() / 1000000), now, store_mounted() + 1, image, drive,
        diag_last_reset(), (unsigned long)diag_stack_free(), heap_headroom, (unsigned long)mi.uordblks, config_source_text(),
        net_setup_mode() ? "true" : "false");
    if (len < 0)
        len = 0;
    if ((size_t)len >= n)
        len = (int)n - 1;
    return (size_t)len;
}
