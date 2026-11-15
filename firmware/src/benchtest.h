#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Bench self-tests, no wiring needed. A PIO state machine samples GP10
// (RDDATA_3V) itself at 2 MHz: a pin driven by one state machine reads back
// through its pad input. The capture is decoded independently of the player
// and compared bit for bit with the track.
//
// stream (milestone 4, extended): core 1's drive is suspended and core 0
//   plays one track of the mounted image from cell `start`, optionally only
//   its first `cells` cells as one revolution. Checks every cell across a
//   revolution boundary, and that playing began exactly at `start`.
// seek (milestone 5): with the drive running on core 1, core 0 overrides the
//   phase and ENABLE* inputs (GPIO input override: the pins themselves are
//   untouched) to step the head like DOS 3.3 does, to quarter track q, then
//   releases them. Checks the head position, that GP10 carries that track
//   (or nothing, for an empty position), and that it is at the angle core 1
//   reckons the disk is at. flash: during the capture, core 0 writes flash
//   (the mounted row's protect flag toggled and restored: two directory
//   erase/program cycles, net no change), which must not disturb core 1.
//
// Non-blocking after the start: poll from the main loop until it reports.

int  benchtest_stream(unsigned track, uint32_t cells, uint32_t start, char *err, size_t errn);
int  benchtest_seek(int q, bool flash, char *err, size_t errn);
bool benchtest_poll(char *report, size_t n);    // true when the report is ready
