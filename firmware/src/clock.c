#include <stdio.h>
#include "pico/stdlib.h"
#include "lwip/apps/sntp.h"

#include "clock.h"

static volatile int64_t offset_us;      // Unix time minus time_us_64()
static volatile bool synced;

void clock_start(void)
{
    sntp_setoperatingmode(SNTP_OPMODE_POLL);
    sntp_setservername(0, "pool.ntp.org");
    sntp_init();
}

void clock_set_unix(uint32_t sec)
{
    offset_us = (int64_t)sec * 1000000 - (int64_t)time_us_64();
    if (!synced)
        printf("clock: set from SNTP, unix %lu\n", (unsigned long)sec);
    synced = true;
}

int64_t clock_unix(void)
{
    if (!synced)
        return 0;
    return ((int64_t)time_us_64() + offset_us) / 1000000;
}
