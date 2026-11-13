#pragma once

// HTTP routes:
//   GET /         configurator page
//   GET /status   JSON status
//   GET /image    download the stored image
//   GET /image.woz  the stored image GCR-encoded, as a WOZ 2 file
//   PUT /image    upload a 143,360-byte .dsk (?name=file.dsk)
void api_init(void);    // registers with the HTTP engine; lwIP lock held
void api_poll(void);    // main loop: commits a completed upload to flash
