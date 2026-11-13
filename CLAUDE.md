# WiFi Disk II drive emulator — project memory

Read this first in every session. Keep it current: when a decision is made or an
open item closes, edit this file in the same session.

## What this is

A small board that plugs into the 20-pin drive connector of a stock Apple Disk II
controller card in Barry's Apple //e and behaves as a real 5.25" drive. Disk
images are pushed to it over WiFi from the Mac (`curl`), so there is no SD card
and no sneakernet. The //e runs stock DOS 3.3; nothing is patched on the Apple side.

**Mental model: the vdisk is just a drive.** It is normally plugged in as
**drive 2**. It answers ENABLE, follows the stepper phase lines, serves read bits
and accepts write bits. It does not know or care whether DOS reached it through a
boot, `CATALOG,D2` or a direct RWTS call. Avoid "PR#6 thinking": nothing about
the design assumes booting from the vdisk.

Dev loop the project exists for: build on the Mac → `curl` push image →
`BRUN PROG,D2` on the //e. No reboot.

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

## Status (as of 2026-11-14)

- **Board rev B ordered** from JLCPCB (5 bare boards, 2-layer, 1.6 mm, invoice
  2026-11-14, ~$40 all in). Expect them roughly a week later.
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
  gone (RAM in use 114 KB). Next: PIO player (milestones 4-5).
- **Unexplained hang (2026-11-13):** v0.2-m2 froze while idle (no USB serial,
  no picotool reset, no network) after Barry's M2 tests; cured by replug.
  Cause unknown. Since v0.3-m3 the LED is a heartbeat (5 Hz connecting, 1 Hz
  up; steady = main loop stopped) and boot waits 2 s for a USB console.
  Next time: capture the console before replugging.

## Toolchain (Mac, Intel, native x86_64)

- Arm GNU Toolchain 14.2.Rel1 (`brew install --cask gcc-arm-embedded`, needs
  sudo, run in Terminal). Not the Homebrew `arm-none-eabi-gcc` formula.
- Pico SDK 2.3.1 at `~/pico/pico-sdk` (submodules: cyw43-driver, lwip, tinyusb,
  btstack, mbedtls). CMakeLists defaults to that path if `PICO_SDK_PATH` unset.
- ninja 1.13.2, picotool 2.3.1 (Homebrew), cmake 4.4.3 (no issues seen).
- Homebrew has dropped Intel bottles (Tier 3); MacPorts is the fallback.
- Build: `cmake -S . -B build -G Ninja && ninja -C build`.
- Flash: BOOTSEL, then `picotool load -v -x build/X.uf2`. Don't touch the
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
| RUN (30) | RESTART* | — | SW4 hard-resets the Pico |

Reflash in place: hold BOOTSEL, tap SW4 (RESTART), release BOOTSEL.
D2 is a plain power LED on 3.3V, not a GPIO. The Pico W's onboard LED is driven
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
| RDDATA pulse sense | **conflict** | tinyDiskII: idle low, 1 µs high pulse at end of cell. RP2350 project: idle high, 1 µs low pulse, "as a Disk II's read amplifier gives". Both reportedly work. Start with idle-high; it's a firmware constant. |
| Write decode: a WRDATA transition in a cell = 1, none = 0; sync on D5 | from working code | tinyDiskII |
| Apple write cell is ~3.958 µs; resync on each rising edge | from working code's README | vibr77 STM32 project |
| Each track revolution must be exactly the track's bit count (no word padding) or nibble-counting loaders fail | from working code | RP2350 `src/disk2.pio` comments |
| Head tracking: phases treated one-hot; step to next phase = +1 half-track (0–69), previous = −1; track = half-track/2 | from working code | tinyDiskII |
| Ignore phase changes while ENABLE* is high | design rule | real drive's stepper is powered only when enabled (Apple dwg, Q1) |

Still to verify against *Beneath Apple DOS*: 6-and-2 GCR tables, address/data
field layout, sync gap lengths, DOS 3.3 logical-to-physical sector interleave
(.dsk is DOS order; .po is ProDOS order).

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
- **Network:** DHCP, mDNS name `a2drive.local` (DHCP reservation as fallback),
  plain HTTP, no auth on the home LAN. WiFi is 2.4 GHz only (fine for Barry).
  A failed upload must leave the mounted image untouched (swap after complete).
- **IPv6 is enabled (link-local) on purpose:** lwIP's mDNS can't send the NSEC
  "no AAAA" reply, so with IPv4 only every macOS `.local` lookup stalled 5 s.
  With IPv6 on, lookups take ~2 ms; curl connects over fe80:: link-local.
- HTTP server is our own on lwIP raw TCP (`src/http.c`), not lwIP httpd, which
  can't do PUT/DELETE.

### HTTP API (as built, v0.2-m2)

| Request | Purpose |
|---|---|
| `GET /` | configurator page (`src/web/index.html`, embedded at build) |
| `GET /status` | JSON: firmware, board, hostname, SSID, IP, RSSI, uptime, time, image {name, size, sha256, uploaded, verified} |
| `PUT /image?name=X` | upload; body must be exactly 143,360 bytes; reply after the flash write+verify |
| `GET /image` | download; `Content-Disposition` filename, `X-Image-SHA256` header |

Later: `PUT /config`, and a list of images once the multi-image design is
decided. **GUI vocabulary (decided 2026-11-13):** the image the drive serves
is the **mounted** image, tag **M**; a writable image gets tag **W**. **Users
never see the word "slot"** (it means Apple slot / drive to
them). Internally, a 256 KB flash area is an "image space".

### Flash layout (src/store.h)

0–1 MB firmware (checked at boot) · 1 MB: image directory copies A/B (CRC32 +
sequence number, alternate writes, newest valid wins) · +8 KB: reserved for
stored config · 1 MB+64 KB to 4 MB: 11 image spaces of 256 KB. A new image
goes to a free space, is read back, then the directory switches. Flash writes
use `flash_safe_execute`, one 4 KB sector at a time. **When core 1 starts
running the drive emulation it must call `flash_safe_execute_core_init()`**,
and flash writes stall XIP, so core-1 code that runs during them must be in RAM.

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
| 5 | Read on the //e | `CATALOG,D2` lists the pushed image; `BLOAD`/`RUN` from D2 works | yes |
| 6 | Writes | `SAVE`/`BSAVE ...,D2`; pull image; file present and image consistent on the Mac | yes |
| 7+ | Knock, selector, button navigation, WOZ input | later | yes |

Write decode can also be tested pre-board by feeding the milestone-4 capture
into the decoder.

## Deferred / open items

- **Persistence + multiple images + OLED navigation** — one discussion, later.
  Which image is active after power-on / reset, slot management from the //e and
  from the buttons. Earlier ideas (not decided): sticky mount; selector program
  that reads a directory sector and writes a mount command via RWTS to drive 2.
- **WiFi provisioning — decided 2026-11-13:** build-time defaults from
  `firmware/config.json` (gitignored; `config.example.json` committed; keys
  `ssid`, `password`, `hostname`; CMake validates and generates
  `config_defaults.h`). Next: config stored in a flash sector overrides the
  defaults, edited from the Pico-served HTML configurator (which later grows
  slot/image management). Optional later: soft-AP setup fallback.
- **Knock** — Apple selects drive 2, motor on (asserts ENABLE*), toggles a phase
  pattern a normal seek never produces, motor off. Needs analysis of the RWTS
  seek routine to pick a provably unused pattern. Knock steps must not move the
  virtual head.
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
| FujiNet | network-as-disk via TNFS server | — | Barry dislikes: needs a server, WiFi drops = disk errors. Our design keeps the disk local. |

## Repository layout

- `docs/` — schematic, board renders, 1:1 fit-check PDF, `reference/` scans
- `hardware/source/` — Python generators (schematic, board, DRC, Gerbers);
  see its README for how to run
- `hardware/fab/` — the exact Gerber zip ordered from JLCPCB (rev B)
- `firmware/` — Pico SDK project (to be created at milestone 0)
- `tools/` — Mac-side scripts (push/pull, image checks)
