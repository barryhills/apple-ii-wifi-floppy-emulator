# Apple II WiFi Floppy Emulator

A Raspberry Pi Pico 2 W board that plugs into an Apple Disk II controller
card's drive connector and acts as a 5.25" drive (drive 1 or 2). Disk images
(`.dsk`, `.woz`) are loaded over WiFi from a web page or with `curl`; the
Apple side runs stock DOS 3.3, unmodified, and reads and writes them like
floppies.

- **Using it:** [DOCS/user-guide.md](DOCS/user-guide.md): firmware loading,
  WiFi setup, the web page and buttons, the lights, the optional activity
  LED change.
- **Hardware:** one folder per board revision under `HW/` (`HW/REVB/`: schematic,
  Gerbers, fit check, generator source).
- **Firmware:** download the ready-made Pico image,
  [a2drive.uf2](https://github.com/barryhills/apple-ii-wifi-floppy-emulator/releases/latest/download/a2drive.uf2)
  (latest release; how to load it: [user guide](DOCS/user-guide.md), section 2).
  Source: C, Raspberry Pi Pico SDK (`SW/firmware/`). Build:
  `cmake -S SW/firmware -B SW/firmware/build -G Ninja && ninja -C SW/firmware/build`.
- **Design record:** `DESIGN.md`: decisions, sources, test history.
- **Licence:** MIT (`LICENSE`); borrowed code and its licences:
  `THIRD-PARTY-NOTICES.md`.
