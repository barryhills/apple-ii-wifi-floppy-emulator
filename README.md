# WiFi Disk II drive emulator

A Raspberry Pi Pico 2 W board that plugs into an Apple Disk II controller card's
drive connector and acts as a 5.25" drive. Disk images are pushed over WiFi with
`curl`; the Apple side runs stock DOS 3.3.

    curl -T mydisk.dsk http://a2drive.local/slot/1
    ]CATALOG,D2

Status: board rev B ordered; firmware not started. See `CLAUDE.md` for the full
design record, decisions and milestones.
