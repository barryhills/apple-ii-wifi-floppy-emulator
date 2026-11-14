#pragma once
#include <stdbool.h>

void oled_init(void);               // probes the SSD1306; harmless if none is fitted
void oled_poll(void);               // main loop: buttons, picker, redraw when changed
bool oled_setup_keys_held(void);    // after oled_init(): UP and DOWN both pressed now
