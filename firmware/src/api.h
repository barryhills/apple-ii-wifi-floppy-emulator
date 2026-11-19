#pragma once
#include <stdbool.h>

// HTTP routes:
//   GET /         configurator page
//   GET /status   JSON status
//   GET /image    download the stored image
//   GET /image.woz  test only: a stored .dsk GCR-encoded as WOZ 2
//   PUT /image    upload a .dsk (143,360 bytes, up to 255 trailing tolerated) or .woz (?name=file)
void api_init(void);    // registers with the HTTP engine; lwIP lock held
bool api_busy(void);    // upload, mount/delete/rename, settings or restart in progress
bool api_store_busy(void);  // the same, less bench tests: the store must not be touched
bool api_poll(void);    // main loop: moves upload data to flash; true while busy
// Image spaces that downloads are still reading (a replaced or deleted
// image keeps its space until they finish): never write into these.
#include <stdint.h>
uint32_t api_reader_mask(void);
