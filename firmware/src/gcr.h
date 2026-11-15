#pragma once
#include <stddef.h>
#include <stdint.h>

// DOS 3.3 16-sector track encoder: one track of a .dsk (DOS logical sector
// order) -> the bit stream a Disk II head would read, MSB first, 4 us cells.
// Portable C: builds for the Pico and for host-side tests.

#define GCR_TRACKS          35
#define GCR_SECTORS         16
#define GCR_TRACK_DATA      (GCR_SECTORS * 256)     // 4096 .dsk bytes per track
#define GCR_TRACK_BUF       6656                    // 13 x 512 (a WOZ track slot)
#define GCR_DEFAULT_VOLUME  254                     // DOS 3.3 INIT default

// physical sector (order on the track) -> DOS logical sector (order in .dsk)
extern const uint8_t gcr_phys_to_logical[GCR_SECTORS];

// The volume number to put in a .dsk's address fields. DOS 3.3 INIT writes
// the same number into every address field and the VTOC (Beneath Apple DOS
// ch. 4: VTOC = track 17 sector 0; byte $06 volume 1-254, $35 sectors per
// track, $36-37 bytes per sector). If the VTOC looks like one, its volume;
// else GCR_DEFAULT_VOLUME. So CATALOG shows the disk's own volume number.
unsigned gcr_dsk_volume(const uint8_t *dsk);

// Encode one track into out (zero-padded to GCR_TRACK_BUF bytes).
// Returns the track length in bits: exactly one revolution, no padding.
size_t gcr_encode_track(uint8_t out[GCR_TRACK_BUF], const uint8_t *dsk_track,
                        unsigned track, unsigned volume);
