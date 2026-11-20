# Apple II WiFi Floppy Emulator

A Raspberry Pi Pico 2 W board that plugs into an Apple Disk II controller
card's drive connector and acts as a 5.25" drive (drive 1 or 2). Disk images
(`.dsk`, `.woz`) are loaded over WiFi from a web page or with `curl`; the
Apple side runs stock DOS 3.3, unmodified, and reads and writes them like
floppies.

    curl -T mydisk.dsk "http://a2drive.local/image/1?name=mydisk.dsk"
    curl -X POST http://a2drive.local/mount/1
    ]CATALOG,D2

- **Using it:** [docs/user-guide.md](docs/user-guide.md): firmware loading,
  WiFi setup, the web page and buttons, the lights, the optional activity
  LED change.
- **Hardware:** board rev B (`hardware/`, schematic in `docs/`).
- **Firmware:** C, Raspberry Pi Pico SDK (`firmware/`). Build:
  `cmake -S firmware -B firmware/build -G Ninja && ninja -C firmware/build`.
- **Design record:** `CLAUDE.md`: decisions, sources, test history.
- **Licence:** MIT (`LICENSE`); borrowed code and its licences:
  `THIRD-PARTY-NOTICES.md`.
