# WiFi Disk II drive emulator — project memory

Read this first in every session. Keep it current: when a decision is made or an
open item closes, edit this file in the same session.

## What this is

A small board that plugs into the 20-pin drive connector of a stock Apple Disk II
controller card in Barry's Apple //e and behaves as a real 5.25" drive. Disk
images are pushed to it over WiFi from the Mac (`curl`), so there is no SD card
and no sneakernet. The //e runs stock DOS 3.3; nothing is patched on the Apple side.

**Mental model: the vdisk is just a drive.** It can be plugged in as **drive 1
or drive 2**; the design does not care which (Barry, 2026-11-13). It answers
ENABLE, follows the stepper phase lines, serves read bits and accepts write
bits: if an I/O comes its way, it processes it. It does not know or care whether
DOS reached it through a boot, `CATALOG,D1`/`D2` or a direct RWTS call. Nothing
in the design assumes either booting from the vdisk or not booting from it.

Dev loop the project exists for: build on the Mac → `curl` push image →
`BRUN PROG` from the vdisk (D1 or D2) on the //e. No reboot.

## Working agreements (how Barry wants to work)

- **Staged milestones, each ending in a test Barry runs.** Barry sets the stages
  and tests; Claude owns internals within a stage. Do off-target testing (Mac,
  bare Pico) wherever possible before needing the //e.
- **Verify against sources, not recall.** Primary sources preferred (Apple
  drawings, *Beneath Apple DOS*, *Understanding the Apple II*, RP2350
  datasheet, Pico SDK docs, WOZ spec, working source code). Since 2026-11-13
  Barry also allows secondary sources (articles, forum posts, emulator source,
  scans). Cite the source in code comments, and flag anything unverified or
  resting on a single secondary source. Never rely on recall for format or
  timing details.
- **New code is the prime suspect.** The //e, DOS 3.3 and the controller are 40
  years proven. When something fails, test our code's own logic first.
- Straight talk, no padding. Barry is a retired OS-kernel engineer; he reads C.
- Ask before git commits/pushes; branch before committing to main.

## Status (as of 2026-11-19)

All work through v0.15 is on `main` (fast-forwarded 2026-11-19); the
milestone branches named below (m1-...m6, gui-carousel, m3d/m3e) were
deleted after merging. Their commits remain in `main`; tags v0.5-list,
v0.7-settings, v0.7.1, v0.13.1-boot, v0.13.2-dirring mark known-good points.


- **Board rev B ordered** from JLCPCB (5 bare boards, 2-layer, 1.6 mm, invoice
  2026-11-14, ~$40 all in). In the mail as of 2026-11-15. Barry is
  breadboarding U1/U2 meanwhile.
- **Parts on order (Amazon):** SN74LVC245AN (DIP-20), 2x10 shrouded box header,
  1N5817.
- **Barry already has:** Raspberry Pi Pico 2 W (x2+, headers pre-soldered),
  20-pin female/female ribbon cables, 74LS125, all passives, ELEGOO 0.96" SSD1306
  I2C OLED (pin order GND VCC SCL SDA), logic analyzer.
- **Not yet done:** 1:1 paper fit check of the real parts against
  `docs/fit-check-revB-1to1.pdf`; Gerber check in a third-party viewer was done
  only via JLCPCB's upload preview.
- **Firmware:** milestone 0 **passed** 2026-11-13 (`firmware/blink/`). Full-flash
  pattern test deliberately skipped.
- **Milestone 1 passed** 2026-11-13 (Barry: works after reboot). Committed
  `8f45046` on branch `m1-wifi-http` (local only, no remote).
- **Milestone 2 passed** 2026-11-13 (Barry: upload, download, power-cycle
  persistence, image integrity). v0.2-m2, branch `m2-flash-storage`. Claude's tests:
  web page upload/download of one .dsk; SHA-256 (RP2350 hardware) matches
  `shasum`; byte-identical round trip; survives reboot (re-hashed at boot);
  wrong size 400, no length 411, concurrent upload 409, aborted upload leaves
  the stored image intact; 14 uploads rotate through all image spaces.
  Next: milestone 3 (GCR encoder) and/or stored config + config page.
- **Milestone 3 passed** 2026-11-13 (Barry: BOOT!!.woz from the Pico works in
  Virtual ][). Branch `m3-gcr-encoder`. Details:
  `GET /image.woz` streams a WOZ 2 (INFO v2) built by `src/gcr.c`/`src/woz.c`;
  `tools/woz_check.py` (independent latch-model decoder) decodes all 560
  sectors identical to the .dsk; Pico output bit-identical to the host build
  (`tools/host/mkwoz.c`). 50,144 bits/track, gaps 16/6/16. The WOZ download
  was a test harness for the encoder, not a product feature.
- **Direction confirmed 2026-11-13:** the drive stores .dsk and .woz natively
  and serves either; no conversion tool for the user. Both feed one per-track
  bit-stream player (DSK: encode the track on head move; WOZ: look the track
  up).
- **Native .woz storage passed** 2026-11-13 (Barry: both file types, persistence).
  v0.4-woz, branch `m3b-woz-storage`. Details:
  PUT /image accepts .dsk (143,360 bytes) or WOZ 1/2 (up to 256 KB),
  validated by `woz_parse()` (signature, CRC, chunks, 5.25" only, every
  referenced track); format shown on the page; WOZ button removed
  (`/image.woz` kept as a hidden test endpoint for .dsk only). Uploads now
  stream straight to a free flash image space (16 KB RAM ring, TCP ack
  deferred until written; ~2 s per WOZ); the 140 KB upload RAM buffer is
  gone (RAM in use 114 KB).
- **Image list GUI passed** 2026-11-13 (Barry; superseded by the carousel GUI
  below). v0.5-list, branch `m3c-image-list`. Details: 10 fixed rows (Barry's design: name without extension, DSK/WOZ,
  bytes, M and W text tags, arrow cursor; buttons Mount/Download/Upload/
  Delete/Write/Mount, last two side by side; arrow keys + Enter; detail line for the selected row). Directory
  v2 (10 rows + mounted row; v1 migrates in place). Mount of an empty row =
  refused; eject is its own request. GUI rule: every button acts only on
  the selected row (Mount/Unmount, Write enable/protect). Mount/delete run in
  the main loop like uploads. Sticky mount across
  power cycles. Next: PIO player (milestones 4-5).
- **Carousel GUI (v0.8-carousel, branch `gui-carousel`, 2026-11-14; awaiting
  Barry's test):** simplification pass. User-facing name for the image
  collection is the **carousel** (code/API keep "row"). Rows show the full
  filename incl. extension; size and type columns gone (format stays on the
  detail line). M/P tags and the Mount/Protect buttons replaced by per-row
  controls that are both state and switch: ●/○ (click ○ = mount, click ● =
  eject; not a native radio, whose arrow keys would mount on every press) and
  🔒/🔓 (click toggles protect). Download/Upload/Rename/Delete act on the
  highlighted row; ▶ cursor and Enter-to-mount dropped, arrow keys still
  select. **LED:** solid on from power-on until joined to a WiFi network
  (and again if the link drops), 1 Hz blink while joined. Lost on purpose:
  a hang while connecting now looks like "connecting" (watchdog still
  reboots). LED can't light before `cyw43_arch_init()`, i.e. dark for the
  first ~2 s (USB console wait).
- **OLED + buttons passed** 2026-11-14 (Barry, breadboard; v0.9.1-buttons).
  `src/oled.c`: SSD1306 on I2C0 at 0x3C, init from pico-examples; 5x7 font
  from Adafruit-GFX (`src/font5x7.h`, BSD) drawn **double height** (5x14):
  4 lines x 21 chars (single height was too small for Barry). Home screen:
  hostname.local / IP (or "connecting...") / brief message / mounted image
  name. Any button opens the picker: header "Mount N of M" + 3-line
  scrolling window of filled rows, cursor in inverse video, '*' = mounted;
  **UP (pin 19) = previous image, DOWN (pin 20) = next** (flipped
  2026-11-19 on the rev B board; the breadboard had the buttons swapped);
  hold repeats; SELECT mounts; 15 s idle -> home. Mount refused while the
  web API is busy (`api_busy()`). Breadboard: timing jumper GP10 (pin 14)
  -> GP8 (pin 11) for milestone 4.
- **WiFi setup mode passed** 2026-11-14 (Barry; v0.10-setup). The drive
  becomes an open access point, SSID `a2drive-setup`, at 192.168.4.1
  (cyw43 default AP address, channel 3), with the pico-examples DHCP server
  (`src/third_party/dhcpserver/`, MIT, unmodified); same web page, which
  shows a setup note and focuses the WiFi field. Entered at boot only:
  UP+DOWN held at power-on (read ~2 s in, after the USB console wait), no
  SSID configured. (Until v0.15.3 also an automatic fallback: saved
  network not joined in 1 min -> reboot into setup, which rebooted again
  after 5 min idle; removed, see v0.15.3 below.) Setup mode never times
  out. Nothing is erased: a plain restart returns to normal. OLED (since
  v0.15.2): "WiFi setup mode / Join a2drive-setup / http://192.168.4.1 /
  No password needed". No DNS
  server (type the IP). macOS lists the SSID under "Other Networks", and
  can take a few seconds to show it.
  v0.10.1 (Barry passed 2026-11-14): keys held at boot are ignored until
  released (no picker on entering setup); settings form has a Show
  checkbox for the password; Return moves to the next field, only the Save
  button saves (after a confirm); after Save an alert says what to do next
  (switch the Mac to the new network / open the new hostname).
- **Milestone 4 passed** 2026-11-14 (v0.11-m4, branch `m4-bit-timing`).
  Player: `src/disk2.pio` (program body unchanged from
  RP2350-Floppy-Emulator, MIT; 16 PIO cycles/cell at 4 MHz, idle high, a 1
  = 1 us low) + 2 DMA channels (`src/player.c`): track buffer = [cells-1,
  cells/32+1 words MSB first], data channel -> TX FIFO, chains to a control
  channel that rewrites the data channel's READ_ADDR_TRIG (TRANS_COUNT
  reloads), so revolutions loop at exactly the track's cell count with no
  CPU. Test `POST /test/stream?track=N[&cells=N]` (`src/streamtest.c`):
  samples GP8 at 2 MHz into 64 KB by DMA (~1.3 rev), decodes edges, aligns
  (sliding 1024-bit window; real-disk WOZ sync gaps are ambiguous) and
  compares bit for bit across the wrap. Claude: DSK 35/35, WOZ (Boot!!.woz)
  35/35, 14 revolution lengths not multiples of 32 (1,001-51,199): all
  bit-exact, every gap an exact multiple of 4 us, every pulse 1 us. Barry:
  Rigol scope on GP10 shows 1 us low pulses 4 us apart, 3.3 V. Bench jumper
  is **physical pin 14 (GP10) -> pin 11 (GP8)**. Barry speaks physical pin
  numbers: give pin numbers first.
- **PAUSE POINT 2026-11-14 (after milestone 4).** Barry's direction: **no
  synthetic workloads** for milestone 5; the first test is the real //e
  through the buffered interface (U1/U2 make that safe). Waiting on the rev
  B boards and parts. Barry may breadboard U1/U2 first: male Dupont jumper
  pins push straight into his female ribbon-cable sockets, 11 wires (pins
  2/4/6/8 phases, 10 WRREQ*, 14 ENABLE*, 16 RDDATA, 18 WRDATA, 20 WRPROT,
  11 +5V, 1 GND); **leave pins 9, 13, 15, 17, 19 (+/-12 V) empty**; meter
  pin 1 = GND and pin 11 = +5 V before plugging into the //e.
  Milestone 5 firmware: built 2026-11-15, next item. Still open for
  milestone 6: writes need edge resync (//e cell ~3.958 us).
- **Milestone 5 firmware built 2026-11-15 (v0.12-m5, branch
  `m5-read-on-iie`); awaiting Barry's //e test.** Barry said proceed
  (2026-11-15). Core 1 runs the drive (`src/drive.c`), entirely from RAM:
  ENABLE*, phases -> head model (`src/stepper.h`), player repointed on a head
  move, WRPROT high (protected until milestone 6). Core 0 builds every track
  of the mounted image into a 256 KB RAM pool at mount/boot (DSK: GCR encode,
  ~165 ms; WOZ: copy, ~22 ms) and re-builds when the mounted row or its image
  space changes. **RDDATA is now idle low, 1 us high pulse** (see protocol
  table); `POST /test/rdpulse?idle=high` flips it until restart if the //e
  disagrees. Head model: vector sum of energized magnets, quarter tracks
  0-159, a phase state must hold 100 us to move the head (software releasing
  a quarter-track hold one phase at a time must not snap it; found by the
  bench test). The disk's angle carries across head moves (new track starts
  at the same fraction of a revolution; player starts mid-revolution).
  DSK: track t at quarter tracks 4t-1..4t+1, half-track positions silent;
  address-field volume = the VTOC's ($06) if the VTOC looks valid, else 254.
  Claude's tests (no wiring: a PIO samples GP10 itself; phases/ENABLE* driven
  by GPIO *input override*): stepper host test (`tools/host/stepper_test.c`:
  exhaustive vs float model, DOS seeks with/without overlap, recalibrate from
  every position, timing); stream test bit-exact incl. mid-revolution starts
  and odd lengths; seek test DSK + WOZ (Boot!!.woz, Virtual ][ 51,200-bit
  tracks), whole/half/quarter tracks, angle exact to the cell; flash writes
  during a capture leave core 1's longest loop pass at 2 us and the stream
  bit-exact; `tools/check_core1_ram.py` proves core-1 code is all in RAM.
  RAM: static 487 KB of 512; heap headroom 16 KB idle, 8 KB after 26
  concurrent HTTP requests; core-0 stack low-water ~1.5 KB under load (was
  880 B before `-fconserve-stack` and a streaming 6-and-2 encoder). Boot to
  serving ~3 s (2 s USB console wait; the //e boot ROM just retries).
  **Barry's milestone 5 test:** see the end of this file's milestone table.
- **Milestone 6 firmware built 2026-11-15 (v0.13-m6, branch `m6-writes`);
  awaiting the //e.** Write path: `src/wrcap.pio` times WRDATA transitions
  (150 MHz, 75 counts/us, +2 counts per edge for its own overhead); core 1
  drains it every pass and `src/wrdec.h` turns intervals into cells with an
  adaptive cell length (starts 3.93 us, follows 1-3-cell intervals; the //e
  writes ~3.91 us cells). Bits go into the RAM track under the head from
  the cell under the head at WRREQ*, one track cell per written cell (2%
  longer than a real head would lay them; harmless, ends well inside gap 3).
  Writes only while ENABLE*+WRREQ* low, the head on data and an image
  served: WRPROT high only while nothing is served (write-protect removed
  in v0.13.1, see below). **Gap resync:** Virtual ][ WOZ gaps are plain 8-bit FFs (gap 2 =
  72 cells, gap 3 = 120); a write ending mid-run misframed the next
  sector's address field (found by the bench test); core 1 now rewrites an
  all-1s run that runs into D5 AA as 10-bit self-syncs ending at the D5.
  Write-back (core 0, `drive.c`): 500 ms after the last write, or at once
  before a mount change, the whole image is rewritten into a free image
  space and the directory switched (power-cut safe); per-track generation
  counters (odd = being written) so writes during a save are never lost;
  .dsk takes the sectors `gcr_decode_track` finds (a sector that no longer
  decodes keeps its old bytes; counted as lost_sectors); WOZ takes the bits
  and a new CRC. Row keeps its name; "uploaded" becomes the save time.
  Idle time pre-erases the next free image space (one 4 KB sector per
  250 ms; writes skip erased sectors): a save then takes ~0.4 s, ~1.5-2.6 s
  if not yet erased. Tests (Claude): host `tools/host/gcrdec_test.c`
  (encode/decode all tracks, rotated, damaged, DOS-style rewrite),
  `tools/host/wrdec_test.c` (cells 3.85-4.05 us, +-0.5 us edge jitter: 0
  errors); device `/test/write` on .dsk and WOZ, cells 3.85-4.05 us, first
  and last sectors, adjacent/repeat writes: RAM track and stored image
  (whole-image CRC) correct, other sectors untouched. RAM: heap 19.7 KB
  static headroom (16 KB idle, 12 KB under load); core-0 stack low-water
  ~1.5 KB under load (status struct strings moved out, handler buffers made
  static, largest frame now 416 B, bench only).
  `/status` drive object adds `pins` (raw levels of every connector signal
  at the Pico: bring-up aid), writable, writing, writes, write_cell_ns,
  dirty_tracks, saves, save_failures, lost_sectors, gap_resyncs, save.
  OLED: (until v0.14.1) "Track 17  ON W" while writing, "*" while unsaved;
  now the activity LED does that and line 3 is the track only.
- **Write-protect removed 2026-11-19 (v0.13.1-nowp, Barry: "all images
  read/write"; saves ~1.3 KB flash).** Gone: lock column on the page,
  `POST /protect/N`, `protected` in `/images`, `store_set_writable`,
  `store_write_finish_flags`, honouring a WOZ's INFO write-protect byte
  (the byte is kept in the file as is). The directory entry's `flags`
  byte stays (layout unchanged, written 0, ignored). WRPROT stays high
  while nothing is served. Bench `/test/seek?flash=1` now renames the row
  and back for its two directory writes. Flashing via `picotool -f`.
- **Rev B board built 2026-11-19** (Barry: breadboard abandoned as too
  tedious). OLED and buttons work (UP/DOWN flipped in firmware, above).
  J1 ohmed out OK. **First //e boot from the vdisk worked first time**
  (2026-11-19, v0.13.1-nowp, as D1, on a test //e without a keyboard):
  RDDATA idle-low sense confirmed. Booting exercised plenty of reads:
  VTOC/catalog navigation, binary file loads. Barry: boot "seemed quick" (not
  measured). Not yet tried: writes, both image types, D2. Next: a
  production //e with a keyboard.
- **Resource budget (v0.13.1, 2026-11-19; production binary since v0.15
  frees `cap` and the other bench buffers, ~85 KB, see below):** SRAM static ~506 of 512 KB
  main: track pool 256, bench capture `cap` 64 (bench-only: first thing to
  reclaim if RAM gets tight), lwIP pbufs 36, lwIP heap 24, upload ring 16,
  enc/dec/track_buf 21; stacks in scratch banks. Firmware ~463 KB of the
  1 MB region. Bigger boards don't help: every RP2350 has 520 KB SRAM;
  PSRAM shares QMI with flash, so core 1 can't use it. Pimoroni Pico Plus
  2 W (16 MB flash) considered and not adopted (pinout unverified against
  Pimoroni's diagram; needs its own build); stay on Pico 2 W.
- **//e test passed 2026-11-19 (Barry, v0.13.1):** read and write, DSK and
  WOZ; Copy II+ verified all files; LOCK/UNLOCK; DELETE; integrity after
  reboot. Nothing found wrong. Open concerns (Claude's ranking): power-off
  inside the ~1-3 s save window loses that save (old image safe); flash
  directory wear (fixed in v0.13.2); crash/reboot loses unsaved writes;
  write cell on Barry's other //e's (check `drive.write_cell_ns`); INIT
  untested.
- **Directory ring (v0.13.2-dirring, 2026-11-19):** wear spread over 14
  sectors instead of 2 (~7x life). No RAM cost (bss unchanged). No new
  concurrency: dir_commit is still main-loop only, same callers, `cur`
  still swapped under the lwIP lock; only the target sector index changed;
  the live sector is never the one erased. `store_write_raw_sector` now
  accepts only the settings sector. Tested on Barry's board: before
  flashing, ring sectors 2-13 read erased (A seq 211, B 212); upgrade found
  B; 30 commits wrapped the ring twice (seq 213-242 in ring order, settings
  sector byte-identical); after reboot newest won, all 10 rows identical
  and verified. **Downgrade hazard:** firmware before v0.13.2 reads only
  A/B and would boot a stale directory; restore from backups if ever
  going back. Backups: a local folder outside the repo.
- **Activity LED (v0.13.3-led, 2026-11-19, Barry's call):** D2 becomes a
  "don't switch off now" light instead of a power LED (heartbeat + OLED are
  proof of life). Bodge (Barry: on the back): fit R5 and D2 normally; on
  the bottom layer cut the +3V3 trace at R5's +3V3 pad (the pad farther
  from D2): it is a leaf stub, 2.54 mm toward the J1 edge then a turn
  (from the ordered Gerbers, GBL); wire that pad to Pico pin 21 (GP16),
  whose pad has no trace. Lit while selected (ENABLE* low), while
  writing, or while writes are unsaved or being saved; off = safe to power
  off. Set by core 0 at the top of `drive_poll()` (atomic SIO set/clr;
  core 1 untouched), so it lags by one main-loop pass (longer while core 0
  is blocked, e.g. a bench test or one upload sector write). `/status`
  pins shows `LED=`. Bench test (no LED fitted): /test/write on a scratch
  row showed LED=1 while dirty, LED=0 on the pass after the save.
  This is also the answer to concern #1 (power-off in the save window).
  Barry fitted the bodge and tested it 2026-11-19: working.
- **No WiFi credentials in firmware (v0.14-nocreds, 2026-11-19, Barry:
  simplicity; careful with passwords before publishing):** config.json,
  config.example.json and config_defaults.h.in removed; the build needs no
  config. Nothing stored -> empty SSID -> setup mode; default hostname
  `a2drive` (CFG_DEFAULT_HOSTNAME in config.h). (Fallback to setup mode
  removed in v0.15.3: the drive just keeps retrying.) Page: "Network config" row removed
  (with Signal and Last reset, still in /status except `settings`);
  /settings drops `source`. Virgin flow: OLED shows setup screen -> join
  `a2drive-setup` -> open 192.168.4.1 -> enter WiFi + hostname -> Save ->
  drive restarts on the home network. Checked: UF2 does not contain the
  password; Barry's board kept its stored settings across the flash.
  **Virgin setup passed 2026-11-19 (Barry):** whole flash erased
  (`picotool erase -r 0x10000000 0x10400000 -f`), firmware loaded, setup
  mode came up, settings saved, drive joined; password persisted across a
  reset. Barry reloads images from his own golden copies (no restore).
  Follow-up: user-facing text said "your Mac"; now "this computer or
  phone", plus "or the IP address on the drive's display" (phones may not
  resolve .local). firmware/config.json deleted. Setup at 192.168.4.1
  failed in Vivaldi and Edge (both Chromium), worked in Safari: likely
  Chromium trying https first for a bare IP and/or macOS 15 Local Network
  permission for third-party browsers. OLED setup line 3 is now
  "http://192.168.4.1" (v0.14.2) instead of "Open 192.168.4.1".
- **GUI + OLED trim (v0.14.1-oled, 2026-11-19, Barry passed):** Page:
  title/h1 "Apple II WiFi Disk Manager"; sections IMAGE CAROUSEL and
  DEVICE; detail line drops the SHA-256 (still in /images, still checked
  at boot: "flash check OK"); no success message on mount/eject (the
  ●/○ says it; failures still show); Device list drops Signal, Last reset,
  Network config and the Drive/track row (a Drive row appears only when
  the image can't be served); `/images` polled every 3 s, `/status` only
  on load and after an operation. OLED: line 3 "Track N" only (no ON/W/*;
  the activity LED covers it); image names without file type on the home
  screen and picker (web page keeps it); setup screen rewritten as steps.
- **.dsk trailing junk tolerated (v0.14.3, 2026-11-19):** Barry's "Copy
  II Plus 5.5.dsk" is 143,363 bytes (31 identical copies on his Mac): a
  valid DOS 3.3 disk (boot sector at 0, VTOC at $11000) plus 3 bytes
  `af ec ee` at the end. A .dsk may now be 143,360 + 0..255 bytes
  (`DSK_SIZE_OK`, store.h): only the first 143,360 are the disk; the tail
  is stored and written back unchanged, so a download returns the upload.
  Checked in upload (api.c), format detection (store.c), write-back
  (drive.c appends the tail) and the page. Tested: 143,359 and 143,616
  refused (400); 143,615 accepted; Copy II Plus uploads, downloads
  byte-identical, reads (seek t17), and a sector write changes exactly
  256 bytes (T17 logical sector 6) with the tail intact.
  Mac gotcha: curl -T treats `[]` in a path as a glob; Barry's folder is
  `APPLE ][`, so use `curl -g`.
- **Rotation survives reboots (v0.14.4, 2026-11-19):** free image spaces
  are picked round-robin from the last one written (`pick_slot`); until
  v0.14.4 that position reset at boot, biasing wear to low spaces. Now
  `recover_last_slot()` derives it at boot from the directory ring: the
  newest directory using a space its predecessor (seq-1) did not. No new
  stored state, no format change; history gone -> old behaviour. The idle
  pre-erase needs nothing either (it checks for erased sectors). Not an
  "80% full" issue: raw NOR, no FTL; one spare space is always reserved.
  But a full carousel narrows wear to the mounted space + the spare.
  Tested: last write before flashing was space 8; after the flash reboot
  the next upload went to 9 (old code: 5), then 10; reboot; then 5, 6
  (wrapping past 0-4 in use). Scratch row 9 deleted afterwards.
- **Production vs bench binary (v0.15, 2026-11-19, Barry: one source,
  one toggle, two binaries):** CMake option `A2DRIVE_BENCH` (default OFF).
  OFF drops benchtest.c (capture 64 KB, decode 8 KB, track 6.5 KB),
  the test report buffer, all `/test/*` routes and `/image.woz`. drive.c's
  small test hooks stay in both, so the drive code is identical. Measured:
  bss 497 -> 412 KB, text -14 KB; heap headroom at idle 12 KB -> 96 KB.
  Production on Barry's board: all /test/* and /image.woz 404; page,
  images, download, serving OK; core 1 still RAM-only, loop max 2 us.
  Barry: production v0.15 boots the //e (2026-11-19); writes not re-run
  on v0.15 (drive code identical to v0.14.4).
- **Boot lamp (v0.15.1-2, 2026-11-19, branch `led-boot-lamp`):** GP16 LED
  on from the first line of main() until the first WiFi join (onboard
  heartbeat starts), then activity only; in setup mode it goes off once
  setup is decided (~2 s).
- **No network healing (v0.15.3, 2026-11-19, Barry: "network failures are
  transient... lets not try to heal the network from the pico"):** the
  automatic fallback to setup mode is gone (1-min reboot into setup, the
  5-min idle reboot out of it, the watchdog-scratch request, and the
  short-lived `drive_used()` guard of v0.15.2). A saved network that
  won't join is retried every 5 s forever; nothing network-related ever
  reboots the drive under the //e. Setup mode = no network saved, or
  UP+DOWN at power-on (RESTART button). Wrong password: OLED keeps saying
  "connecting..."; the after-save alert on the page now says to use
  UP+DOWN + RESTART. Flashed: joins normally.
- **Two OLED kinds, one code (v0.15.4, 2026-11-20, Barry tested both):**
  the 0.96" SSD1306 and a 1.54" module with a CH1116 (SH1106-class,
  132x64 RAM, 128x64 panel; needs a rev C footprint, later). The CH1116
  has no horizontal addressing, so oled.c now sends the frame a page at a
  time (0xB0+page, column 2, the 126 text columns) and clears the whole
  RAM once at init; no 0x20 memory-mode command. Same init otherwise
  (0x8D 0x14 charge pump works on the CH1116 module). On the SSD1306 the
  text sits 2 px further right; the inverse bar is 126 px. Cost: +260 B
  RAM, ~+1.2 ms per redraw (~24.7 ms), ~26 ms once at boot; core 1
  untouched. Lesson: the first "same problem" report was the old
  firmware: the test build had not been flashed onto that Pico; verify
  the running version (console or /status) before judging a test.
  Reference: Zephyr display_ssd1306.c treats CH1116 like CH1115/SH1106
  (0xAD 0x8B DC-DC, VCOM 0x35, 0x00 command framing); not needed here.
  CH1116 datasheet (baud-dance.com) could not be fetched.
- **Several boards (Barry has 6 //e's):** every board boots as
  `a2drive.local` (since v0.14 via setup mode, where the hostname is set) and our mDNS has no name-conflict handling;
  bring each new board up alone and rename it in Settings. A unique default
  name (board-ID suffix) was offered and declined (Barry, 2026-11-19).
- **Hang diagnostics (v0.6.1+, permanent):** HardFault handler records PC/LR/CFSR
  in watchdog scratch 0-3 and reboots; 4 s watchdog; MSPLIM stack guard
  (`PICO_USE_STACK_GUARDS`); stack high-water in /status. Page shows "Stack
  headroom" ("Last reset" moved to /status only in v0.14.1) (lowest seen ~1.9 KB of 4 KB under stress).
  Directory copies (1.1 KB) moved off the shared stack: likely cause of the
  two freezes (stack overflow during uploads), not proven.
- **Unexplained hang (2026-11-13):** v0.2-m2 froze while idle (no USB serial,
  no picotool reset, no network) after Barry's M2 tests; cured by replug.
  Cause unknown. Since v0.3-m3 the LED is a heartbeat (since v0.8: solid until
  joined, 1 Hz up; steady while joined = main loop stopped) and boot waits 2 s for a USB console.
  Next time: capture the console before replugging.

## Toolchain (Mac, Intel, native x86_64)

- Arm GNU Toolchain 14.2.Rel1 (`brew install --cask gcc-arm-embedded`, needs
  sudo, run in Terminal). Not the Homebrew `arm-none-eabi-gcc` formula.
- Pico SDK 2.3.1 at `~/pico/pico-sdk` (submodules: cyw43-driver, lwip, tinyusb,
  btstack, mbedtls). CMakeLists defaults to that path if `PICO_SDK_PATH` unset.
- ninja 1.13.2, picotool 2.3.1 (Homebrew), cmake 4.4.3 (no issues seen).
- Homebrew has dropped Intel bottles (Tier 3); MacPorts is the fallback.
- Build: `cmake -S . -B build -G Ninja && ninja -C build` (production
  binary). Bench/debug binary, same source: `cmake -S . -B build-bench -G
  Ninja -DA2DRIVE_BENCH=ON && ninja -C build-bench` (adds /test/*,
  /image.woz; version "-bench"). Flash `build-bench/a2drive.uf2` only for
  bench tests, then put the production binary back.
- Flash: `picotool load -f -v -x build/X.uf2` (`-f` reboots running firmware
  into BOOTSEL over USB; no buttons). Hung Pico: BOOTSEL + RESTART, then
  the same command without `-f`. Don't touch the
  RP2350 drive in Finder (crashed Finder/Safari once).
- Serial: use `/dev/cu.usbmodem*`, not `tty.*`. `screen /dev/cu.usbmodemNNNN 115200`.
- `picotool erase` without `--range` fails: it can't determine the flash size
  (OTP flash devinfo reads 0x0c00).

## Hardware summary (schematic rev 0.3, board rev B)

Source of truth: `docs/wifi-disk-ii-schematic-r0.3.svg`. The board was generated
by the custom Python in `hardware/source/` (KiCad was unavailable), with its own
DRC and a pin-by-pin cross-check against the schematic (84 pins, 0 mismatches).

### J1 — Disk II 20-pin connector (verified)

Verified against Apple drawing 050-0007-01, Disk II analog board
(`docs/reference/disk-ii-analog-board-schematic.pdf`), and the //c external
drive table (`docs/reference/iic-db19-to-20pin-table.jpg`).

| Pin | Signal | Pin | Signal |
|---|---|---|---|
| 1 | GND | 2 | PH0 |
| 3 | GND | 4 | PH1 |
| 5 | GND | 6 | PH2 |
| 7 | GND | 8 | PH3 |
| 9 | -12V (open) | 10 | WRREQ* |
| 11 | +5V | 12 | +5V |
| 13 | +12V (open) | 14 | ENABLE* |
| 15 | +12V (open) | 16 | RDDATA |
| 17 | +12V (open) | 18 | WRDATA |
| 19 | +12V (open) | 20 | WRPROT |

All signals are 5V TTL. The ±12V pins are not connected on our board. Pin 14 is
the only per-drive signal: **which controller connector the ribbon is on decides
drive 1 vs drive 2**; the firmware never needs to know. The controller card's
header is unkeyed — a reversed ribbon puts ±12V on logic and kills U1/U2.

### Signal path

- **U1 74LVC245 (Vcc 3.3V), inputs 5V → 3.3V.** DIR tied high (A→B), OE* to GND.
  A1–A7 = PH0, PH1, PH2, PH3, ENABLE*, WRREQ*, WRDATA. A8 grounded, B8 open.
- **U2 74HCT125 or 74LS125 (Vcc 5V), outputs 3.3V → 5V.** Section 1: RDDATA,
  section 2: WRPROT. Both output-enables tied to ENABLE*, so the board releases
  the bus whenever the drive isn't selected (same as a real drive, which also
  uses 74LS125s here). Sections 3–4 unused (inputs to GND, OE* high).
- **R1, R2 470 Ω** in series from the Pico into U2 inputs (suits LS and HCT).
- **R3, R4 10k** pull ENABLE* and WRREQ* up to 3.3V so the Pico sees
  "deselected, not writing" with the ribbon unplugged.
- **Power:** Apple +5V → D1 (1N5817; any 1 A rectifier like 1N400x is fine) →
  Pico VSYS. USB may be connected at the same time. C3 10 µF bulk at J1.

### Pico 2 W pin map

| GPIO (pin) | Net | Dir | Notes |
|---|---|---|---|
| GP2 (4) | PH0_3V | in | Phases on consecutive GPIOs: one PIO/GPIO read gets all four |
| GP3 (5) | PH1_3V | in | |
| GP4 (6) | PH2_3V | in | |
| GP5 (7) | PH3_3V | in | |
| GP6 (9) | ENABLE_3V* | in | active low |
| GP7 (10) | WRREQ_3V* | in | active low |
| GP8 (11) | WRDATA_3V | in | |
| GP9 (12) | ENTER* | in | button, needs internal pull-up |
| GP10 (14) | RDDATA_3V | out | via R1 → U2 |
| GP11 (15) | WRPROT_3V | out | via R2 → U2 |
| GP12 (16) | SDA | I2C0 | OLED; module has its own pull-ups |
| GP13 (17) | SCL | I2C0 | |
| GP14 (19) | UP* | in | button, internal pull-up |
| GP15 (20) | DOWN* | in | button, internal pull-up |
| GP16 (21) | ACT LED | out | rev B bodge on the back: cut R5's +3V3 stub, wire R5's +3V3 pad to pin 21 (v0.13.3) |
| RUN (30) | RESTART* | — | SW4 hard-resets the Pico (RP2350 reset input, not a GPIO; no firmware logic). Barry fitting it 2026-11-19: main use now is UP+DOWN + tap RESTART = setup mode without power-cycling the //e |

Reflash in place: hold BOOTSEL, tap SW4 (RESTART), release BOOTSEL.
D2 was a plain power LED on 3.3V; since v0.13.3 it is the activity LED on
GP16 via a bodge on the back of the board (one cut, one wire). The Pico W's onboard LED is driven
through the CYW43 WiFi chip, not a normal GPIO.

### Board rev B layout

99.06 × 68.58 mm. J1 along the back edge (pin 1 at the right); OLED at front
left; UP above DOWN just right of the OLED, ENTER right of that pair; RESTART at
front right. J2 is an 11-pin test header at connector levels: PH0, PH1, PH2, PH3,
WRREQ*, +5V, ENABLE*, RDDATA, WRDATA, WRPROT, GND — for the logic analyzer.
Known limits: no ground pour (0.4 mm traces); OLED outline (27.3 × 27.8 mm,
header 2 mm below top edge) and button/cap footprints are typical dimensions,
not measured from Barry's parts.

## Disk II protocol facts we rely on

| Fact | Status | Source |
|---|---|---|
| ENABLE*, WRREQ* active low | verified | Apple dwg 050-0007-01 (overbars) |
| WRPROT high = write-protected | from working code | tinyDiskII `floppy_main.cpp` |
| Bit cell 4 µs | from working code | tinyDiskII, RP2350 project |
| RDDATA pulse sense: **idle low, 1 µs high pulse** (since v0.12) | primary source | MC3470 data sheet (Motorola, bitsavers `components/motorola/_dataSheets/MC3470.pdf`) Fig. 28 "Digital Output": low with a positive pulse per flux peak; Apple dwg 050-0007-01: MC3470 pin 10 → 74LS125 (non-inverting) → pin 16. Agrees with tinyDiskII and SDISK II. RP2350 project uses idle high (both reportedly work: the card likely reacts to one edge). Done as a GPIO output invert on GP10; `/test/rdpulse` flips it. **Confirmed on the //e 2026-11-19: boots with idle low.** |
| Write decode: a WRDATA transition in a cell = 1, none = 0; sync on D5 | from working code | tinyDiskII |
| Our write decode: interval between transitions → n cells (n−1 zeros + 1), adaptive cell length | design | `src/wrdec.h`, host-tested 3.85–4.05 µs |
| Apple write cell is ~3.958 µs; resync on each rising edge | from working code's README | vibr77 STM32 project |
| Each track revolution must be exactly the track's bit count (no word padding) or nibble-counting loaders fail | from working code | RP2350 `src/disk2.pio` comments |
| Head tracking: one-hot next phase = +1 half track, previous = −1, opposite/same = none; track = half-track/2 | from working code | tinyDiskII, SDISK II (quarter-track counter 0–139) |
| Our model: vector sum of energized phases → one of 8 rotor positions (2 tracks per turn), short way, quarter tracks 0–159; reduces to the one-hot rules; a phase state must hold 100 µs | design | `src/stepper.h`, host-tested |
| Ignore phase changes while ENABLE* is high | design rule | real drive's stepper is powered only when enabled (Apple dwg, Q1) |

Verified against *Beneath Apple DOS*: 6-and-2 GCR table, address/data field
layout, sync gaps, DOS 3.3 interleave (2026-11-13, see `src/gcr.c`); VTOC
layout, ch. 4 (2026-11-15). Unverified: the P5 boot ROM's recalibrate phase
order (only a host-test scenario depends on it).

## Firmware plan

- **Language/toolchain:** C with the Raspberry Pi Pico SDK (RP2350, `pico2_w`
  board), lwIP and the CYW43 driver from the SDK. Builds on Barry's Mac.
- **Read path:** whole 143,360-byte .dsk held in RAM. Encode the current track to
  6-and-2 GCR on head movement; PIO state machine + DMA clocks it out on RDDATA.
  Internal format = per-track bitstream, so WOZ input later is a second loader
  feeding the same player.
- **Write path:** PIO captures WRDATA transitions; decode to sectors; update RAM
  image; save to flash only while the drive is deselected (flash erase stalls the
  chip). Until writes work, report write-protected.
- **Storage:** 4 MB flash; firmware < 1 MB. A .dsk is exactly 35 × 4 KB sectors.
  Slot table + directory sector designed in from day one (WOZ would need
  ~256 KB slots).
- **Interim persistence rule:** one image, stored in flash, served at every
  power-on. Full persistence behaviour is deferred (see below).
- **Network:** DHCP, mDNS name `a2drive.local` by default. Barry's drive
  was `vdisk.local` (2026-11-13) until the 2026-11-19 full erase; he kept
  the default in setup, so it is **`a2drive.local`** again (IP on the home LAN).
  DHCP reservation as fallback;
  plain HTTP, no auth on the home LAN. WiFi is 2.4 GHz only (fine for Barry).
  A failed upload must leave the mounted image untouched (swap after complete).
- **IPv6 is enabled (link-local) on purpose:** lwIP's mDNS can't send the NSEC
  "no AAAA" reply, so with IPv4 only every macOS `.local` lookup stalled 5 s.
  With IPv6 on, lookups take ~2 ms; curl connects over fe80:: link-local.
- HTTP server is our own on lwIP raw TCP (`src/http.c`), not lwIP httpd, which
  can't do PUT/DELETE.

### HTTP API (as built, v0.7-settings; rows are 1-10)

| Request | Purpose |
|---|---|
| `GET /` | the page (`src/web/index.html`, embedded at build) |
| `GET /images` | JSON list: mounted row, busy, 10 rows (name, format, size, sha256, uploaded, verified) or null |
| `PUT /image/N?name=X` | upload .dsk (143,360 bytes) or .woz (up to 256 KB) into row N, replacing it |
| `GET /image/N` | download row N as uploaded |
| `DELETE /image/N` | clear row N (if mounted, the drive becomes empty) |
| `POST /mount/N` | mount row N (an empty row is refused, 409) |
| `POST /eject` | unmount: drive empty |
| `POST /rename/N?name=X` | rename row N; keeps the file type (a typed .dsk/.do/.woz is dropped) |
| `GET /image` | download the mounted image |
| `GET /status` | device JSON incl. mounted_row and mounted image |
| `GET /settings` | hostname, SSID, password_set, source (never the password) |
| `POST /settings` | form body `ssid=&password=&hostname=` (blank password = keep); saves to flash, restarts |
| `POST /restart` | restart (refused while busy) |
| `GET /image.woz` | bench binary only: mounted .dsk encoded to WOZ 2 |
| (bench binary only, `A2DRIVE_BENCH=ON`; 404 in production) `POST /test/stream?track=N[&start=C][&cells=N]` | bench: core 0 plays a track from cell C (drive suspended), captured on GP10 and checked bit for bit |
| `POST /test/seek?q=N\|track=N[&flash=1]` | bench: steps the running drive by GPIO input override, checks head, data, angle; flash=1 writes the directory twice during the capture |
| `POST /test/rdpulse?idle=low\|high` | RDDATA sense until restart |
| `POST /test/write?track=T&sector=S[&cell_ns=3910][&seed=N]` | bench: writes physical sector S like DOS via a test PIO on GP22 (unconnected); checks RAM track, then the saved image. CHANGES THE MOUNTED IMAGE (use a scratch row) |

`/status` has a `drive` object: serving, enabled, track (e.g. 17.25),
quarter_track, on_data, steps, selects, load_ms, loop_max_us, rddata_idle,
error; plus heap_headroom/heap_in_use. The OLED's third line shows the head
("Track 17"); the web page does not (v0.14.1).

**GUI vocabulary (decided 2026-11-13, revised 2026-11-14):** the collection
of images is the **carousel**. The image the drive serves is the **mounted**
image (● on its row; earlier tag M). No write-protect (removed
2026-11-19): every image is writable. **Users
never see the word "slot"** (it means Apple slot / drive to
them). Internally, a 256 KB flash area is an "image space".

### Flash layout (src/store.h)

0–1 MB firmware (checked at boot) · 1 MB: image directory ring of 14
sectors (CRC32 + sequence number, each change to the next sector, newest
valid wins; since v0.13.2, before only A/B): ring 0-1 = 0x100000-0x101FFF
(old A/B), ring 2-13 = 0x104000-0x10FFFF · 0x102000 stored config,
0x103000 reserved · 1 MB+64 KB to 4 MB: 11 image spaces of 256 KB. A new image
goes to a free space, is read back, then the directory switches. Flash writes
use `flash_safe_execute`, one 4 KB sector at a time. **Core 1 is never
locked out** (`PICO_FLASH_ASSUME_CORE1_SAFE=1`): flash writes only mask core
0's interrupts, so core 1 must run entirely from RAM and read no flash (no
`static const` tables, no out-of-line SDK calls). Run
`tools/check_core1_ram.py` after changing core-1 code.

### Firmware notes

- Main stack raised to 4 KB (`PICO_STACK_SIZE`): lwIP callbacks run in IRQ
  context on it. Large scratch buffers are static; `status_json()` needs the
  lwIP lock.
- SNTP (`pool.ntp.org`) timestamps uploads; "unknown" until synced.

### Milestones

| # | Block | Acceptance test (Barry runs it) | Needs board? |
|---|---|---|---|
| 0 | Toolchain on Mac | blink builds and flashes; serial console works | no |
| 1 | WiFi + HTTP | creds entered over USB serial; `curl a2drive.local/status` answers | no |
| 1a | Basic OLED | shows IP and status (can be done any time; useful early) | no (wire module to bare Pico) |
| 2 | Flash storage | push/pull/delete; SHA-256 matches on the Mac | no |
| 3 | GCR encoder | Pico encodes the image and serves it back as WOZ; it boots/catalogs in a Mac emulator | no |
| 4 | Bit timing | PIO stream on GP10 jumpered to a second Pico, captured bit-exact at 4 µs cells | no |
| 5 | Read on the //e | `CATALOG` lists the pushed image from the vdisk (as D1 or D2); `BLOAD`/`RUN` works; booting it as D1 works | yes |
| 5 (firmware built) | Barry's test: //e off, meter J1/ribbon (pin 1 GND, pin 11 +5 V), plug in as D2 first; power on; OLED should show "Track 0"; `CATALOG,D2` (head moves: OLED shows track and ON); `BRUN`/`RUN`; then as D1: boot. If nothing reads: `curl -X POST 'http://vdisk.local/test/rdpulse?idle=high'` and retry (then tell Claude) | yes |
| 6 | Writes | `SAVE`/`BSAVE` to the vdisk; pull image; file present and image consistent on the Mac | yes |
| 6 (firmware built) | Barry's test: mount a scratch row; `SAVE`/`BSAVE` a file; wait for the drive light to go off (~1 s) + ~1 s; `CATALOG`; download the image; open it in CiderPress2 / Virtual ][. `/status` drive.write_cell_ns shows the //e's real write cell | yes |
| 7+ | Knock, selector, button navigation, WOZ input | later | yes |

Write decode can also be tested pre-board by feeding the milestone-4 capture
into the decoder.

## Deferred / open items

- **Division of control (decided 2026-11-13, Barry; revised same day):**
  - *Web page:* everything: upload, download, delete, mount (M), rename,
    settings, restart.
  - *Drive (OLED + buttons):* setup-mode bootstrap (UP+DOWN at power-on);
    otherwise navigate the list and **change the mounted image (M) only**.
    Never delete, upload or rename.
  - *The //e via a disk copier (Copy II+):* writes into the **mounted**
    image only: ordinary drive writes (milestone 6).
  - *The //e filling an empty slot:* wanted, but the **absolute last feature**.
  - *A //e selector program* (M, P, rename): wanted, optional, after the
    core. Needs the knock analysis.
  - Last mounted image stays mounted across power-off.
  - OLED list: open question whether to mark the mounted image (likely yes,
    since the buttons change it).
- **Rename** (built in v0.6-rename; Barry tested): GUI-only button acting on the selected
  row; changes the display name, keeps the file type (download =
  newname + original extension); same character rules as uploads. Short
  names matter for the ~16-21 character OLED.
- **Network setup (agreed 2026-11-13; Settings/Restart BUILT in v0.7; setup-mode AP BUILT in v0.10, see Status):**
  Device window: info list, then always-visible fields (hostname, WiFi network,
  write-only password), then buttons Save (stores + restarts) · Restart · Cancel.
  If stored WiFi settings don't connect, the drive keeps retrying (since
  v0.15.3; earlier it fell back to setup mode after 60 s).
  Device window gets **Settings**
  (hostname, SSID, write-only password; Save & restart) and **Restart**
  (greyed during flash work). Settings stored in the reserved flash sector;
  nothing else (no build-time WiFi since v0.14). **Setup mode:** the Pico becomes
  its own access point "a2drive-setup" at a fixed address (e.g. 192.168.4.1)
  and serves the same page opened on Settings. Entered automatically only when
  no settings are stored, or by holding UP+DOWN at power-on. OLED in setup mode: "Join a2drive-setup / Open 192.168.4.1".
  Not built yet. Check pico-examples' AP DHCP/DNS servers before relying on
  them.
- **//e configurator: ON HOLD (Barry, 2026-11-15: "the OLED UI will be good
  for a long time"). Reference design (agreed 2026-11-13; replaces the
  "knock" idea), kept for later:**
  - Tracks beyond the image's last track (34 for DSK; up to 39 for a 40-track
    WOZ) return no readable data, so a 40-track DOS gets I/O ERROR as on a
    real 35-track drive.
  - One track beyond any image (e.g. track 41) is the **post office**: RWTS
    *writes* to its sectors deliver commands (mount row N, rename, protect,
    delete); RWTS *reads* return replies (image list, mounted row, P flags,
    last result). The drive synthesizes address fields for that track. No new
    low-level protocol: the //e program just calls RWTS.
  - The configurator is an ordinary image in a row, so it's optional. Load:
    mount it (web/OLED), `BRUN` it from the vdisk.
  - Guard: the post office only opens while the mounted row is flagged as
    the configurator (GUI tag, e.g. "C"); mounting another row closes it.
  - Barry (2026-11-13): DOS refuses I/O past track 34, so the configurator
    must **bypass DOS** (call RWTS directly, or its own read/write routine).
    No real floppy mechanism reaches track 41: it exists only in our virtual
    drive. Expect to learn DOS/controller rules out there by experiment.
  - To verify before building: what RWTS itself does above track 34; volume
    check (IOB volume 0 = any). Barry's 40-track drives/DOS are out of scope.
  - Barry thinks in hex at times ("track 23" = $23 = 35).
- **Mac emulator (answered 2026-11-13):** Virtual ][ 12.0
  (`nl.xs4all.gp.virtualii`). Its built-in help says "supports both version 1
  and version 2 of the Woz file format". Milestone 3 targets WOZ 2.
- **WOZ input** — quarter-track head tracking, larger slots. Read first; WOZ
  write and copy-protection fidelity (weak bits, cross-track sync) last.
- **Out of scope:** INIT/formatting (push a blank image instead), SD card,
  SmartPort / 3.5" / Mac drives, authentication, OTA firmware update (maybe later;
  USB stays the recovery path).

## Prior art (study; mind licences)

| Project | What it is | Licence | Use |
|---|---|---|---|
| [rezafouladian/RP2350-Floppy-Emulator](https://github.com/rezafouladian/RP2350-Floppy-Emulator) | Rust, RP2350, PIO+DMA Disk II read/write, many formats, host-side PIO simulation tests | MIT OR Apache-2.0 | Primary reference; may borrow with attribution. Note its "internal 20-pin" column is the IWM/SmartPort-host pinout, not the Disk II card's. Wires GPIO directly with no buffers. |
| [GLGPrograms/tinyDiskII](https://github.com/GLGPrograms/tinyDiskII) | ATxmega16, SD card, .NIC images, read/write; 74LVC244 in, 74AHCT125 out | GPL-3.0 | Source of polarity constants; copying code makes ours GPL |
| [vibr77/AppleIIDiskIIStm32F411](https://github.com/vibr77/AppleIIDiskIIStm32F411) | STM32F411, SD, OLED, Disk II + SmartPort, WOZ read/write | none found (all rights reserved) | Read for method only; do not copy |
| SDISK II (Koichi Nishida) | original ATmega328 drive emulator | — | ancestor of tinyDiskII |
| BMOW Floppy Emu | ATmega1284 + XC9572XL CPLD, SD card | closed | none available |
| FujiNet | network-as-disk via TNFS server | GPL-3.0 | Barry dislikes: needs a server, WiFi drops = disk errors. Our design keeps the disk local. Its `lib/bus/iwm/iwm_ll.cpp` has an ESP32 Disk II path (read for method only). |
| SDISK II firmware (Nishida), copies at ironsteel/sdisk2-firmware, rbanffy/sdisk2 | ATmega328, the original micro-based Disk II emulator | GPL-3.0 | Read for method only: RDDATA idle low + ~1 µs high pulse; WRPROT high = protected; quarter-track stepper table; phases ignored while ENABLE* high |

## Repository layout

- `docs/` — schematic, board renders, 1:1 fit-check PDF, `reference/` scans
- `hardware/source/` — Python generators (schematic, board, DRC, Gerbers);
  see its README for how to run
- `hardware/fab/` — the exact Gerber zip ordered from JLCPCB (rev B)
- `firmware/` — Pico SDK project (to be created at milestone 0)
- `tools/` — Mac-side scripts (push/pull, image checks)
