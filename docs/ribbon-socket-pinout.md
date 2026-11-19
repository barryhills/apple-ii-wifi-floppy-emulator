# 20-pin ribbon socket: pin map for breadboarding

Female IDC socket on the ribbon from the Disk II controller card. Viewed
**looking into the holes** (as when pushing in jumper pins), **key tab on
top, red stripe on the left**. Signal names from Apple dwg 050-0007-01 and
schematic rev 0.3 (see CLAUDE.md, J1 table).

```
                 ┌──── tab ────┐
   ┌─────────────┘             └─────────────┐
   │  o    o    o    o    o    o    o    o    o    o  │  row A (tab side)
   │  o    o    o    o    o    o    o    o    o    o  │  row B
   └─────────────────────────────────────────────┘
 col 1    2    3    4    5    6    7    8    9    10
 (red stripe end)
```

| col | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 |
|---|---|---|---|---|---|---|---|---|---|---|
| **pin-1 row** | 1 GND | 3 GND | 5 GND | 7 GND | 9 −12V ✗ | 11 +5V | 13 +12V ✗ | 15 +12V ✗ | 17 +12V ✗ | 19 +12V ✗ |
| **pin-2 row** | 2 PH0 | 4 PH1 | 6 PH2 | 8 PH3 | 10 WRREQ* | 12 +5V | 14 ENABLE* | 16 RDDATA | 18 WRDATA | 20 WRPROT |

✗ = leave empty (±12 V). Pin 11 (+5 V) sits between −12 V (9) and +12 V (13).

**Which row is the pin-1 row is not fixed by the tab** (connector makers
differ). Identify it:

1. The molded ▲ on the housing marks hole 1 (column 1).
2. Confirm with the meter, //e **off**, ribbon on the controller card:
   continuity among the four holes at the stripe end of one row = pins
   1/3/5/7 (all GND) = the pin-1 row. If the four GND holes are at the far
   end instead, the cable is reversed at the card (its header is unkeyed).
3. //e on: pins 11 and 12 read +5 V against pin 1.

## Where each wire goes (schematic rev 0.3)

| Socket pin | Signal | Breadboard |
|---|---|---|
| 2, 4, 6, 8 | PH0–PH3 | U1 pins 2, 3, 4, 5 |
| 14 | ENABLE* | U1 pin 6, U2 pins 1 and 4, R3 10k to 3.3 V |
| 10 | WRREQ* | U1 pin 7, R4 10k to 3.3 V |
| 18 | WRDATA | U1 pin 8 |
| 16 | RDDATA | from U2 pin 3 |
| 20 | WRPROT | from U2 pin 6 |
| 11 | +5 V | U2 pin 14 (and pins 10, 13), D1 → Pico VSYS (pin 39) |
| 1 | GND | common ground |

U1 outputs (pins 18–12) → Pico pins 4, 5, 6, 7, 9, 10, 11 (GP2–GP8).
Pico pin 14 (GP10) → 470 Ω → U2 pin 2; Pico pin 15 (GP11) → 470 Ω → U2 pin 5.

RDDATA idles **low** with a 1 µs **high** pulse per 1 bit (since firmware
v0.12).
