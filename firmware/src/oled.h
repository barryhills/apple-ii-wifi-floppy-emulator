#pragma once

void oled_init(void);   // probes the SSD1306; harmless if none is fitted
void oled_poll(void);   // main loop: redraws the status screen when it changes
