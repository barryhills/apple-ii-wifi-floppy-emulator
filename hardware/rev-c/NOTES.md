# Board rev C: working notes

Branch `pcb-rev-c`. Placement agreed with Barry 2026-11-20 (`placement-revC.png`,
`placement_c.py`); trial route `trial-route-revC.png`.

## Build

    cd hardware/rev-c/source/pcb
    NEAR_COST=$(cat near_cost.txt) ORDER=$(cat order_c.txt) python3 board_c.py ../../out
    python3 drc_c.py ../../out ../../../source/schematic_rev0.4.py   # rev 0.3 until 0.4 exists
    python3 hand_check.py ../../out/model.json                        # track-to-pad gaps < 0.5 mm
    python3 ../../trial_view.py ../../out/model.json ../../trial-route-revC.png

`board_c.py` is rev B's `board_b.py` with the rev C placement and netlist, plus a
router cost for nodes one grid step from another net's pad (hand soldering,
Barry): tight track-to-pad spots 169 -> 8 (all single passes between adjacent
pins), 27 vias. Power nets route first.

## Changes from rev B (Barry's list)

1. J2 test header removed (it was never on the schematic).
2. Silkscreen title "APPLE II WIFI FLOPPY EMULATOR  REV C  2026-11  BY BARRY HILLS".
3-7, 12-15. Parts moved (see placement drawing); 1.54in OLED (44.45 x 38.1 mm),
   lower-left corner tangent to the bottom-left mounting hole.
8. R5 fed from Pico pin 21 (GP16, net LED_ACT) instead of +3V3.
9. R5 = 180 ohm.
10. D2 square pads (1.8 mm, 0.8 mm hole kept).
11. D2 labelled ACT.

## Schematic rev 0.4 (only these)

R5 top to Pico pin 21 (LED_ACT); R5 180; D2 note "activity"; J3 label covers the
0.96in SSD1306 and 1.54in SH1106-class modules (same pins: GND VCC SCL SDA).

## Silkscreen requirements (Barry)

- UP, DOWN, ENTER, RESTART next to their switches, outside the switch bodies.
- ACT above D2 (below would run into the mounting hole).
- J1 pin 1: a bold "1" with a pointer about 2.5 mm beyond the real connector
  outline, so neither the box header nor the IDC plug covers it (rev B's sat
  0.7 mm out and got covered).
- R5 marked 180.

## Open

- OLED header position on the module is ASSUMED: centred, pin centres 2.0 mm
  below the module's top edge. Barry to measure before ordering.
- A low pan-head or nylon screw at the bottom-left hole (under the OLED corner).
