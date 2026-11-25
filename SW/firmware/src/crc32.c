#include "crc32.h"

static uint32_t table[256];
static int ready;

static void init(void)
{
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++)
            c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
        table[i] = c;
    }
    ready = 1;
}

uint32_t crc32_update(uint32_t crc, const void *data, size_t n)
{
    if (!ready)
        init();
    const uint8_t *p = data;
    crc = ~crc;
    while (n--)
        crc = table[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return ~crc;
}
