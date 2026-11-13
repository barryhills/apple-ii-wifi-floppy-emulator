#pragma once
#include <stdint.h>

// Wall-clock time from SNTP (pool.ntp.org). Used only to timestamp uploads.
void    clock_start(void);              // call with the lwIP lock held
void    clock_set_unix(uint32_t sec);   // SNTP callback (see lwipopts.h)
int64_t clock_unix(void);               // Unix seconds, 0 if not yet synced
