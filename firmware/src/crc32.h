#pragma once
#include <stddef.h>
#include <stdint.h>

// CRC-32 (reflected polynomial 0xEDB88320), zlib-style chaining: start with
// crc = 0 and pass the previous result to continue. This is the CRC the WOZ
// spec (Appendix A) uses, and the one used for the flash directory.
uint32_t crc32_update(uint32_t crc, const void *data, size_t n);
