# Board rev C: working notes

Branch `pcb-rev-c`. Placement agreed with Barry 2026-11-20 (`placement-revC.png`,
`placement_c.py`); trial route `trial-route-revC.png`.

## Build

    cd hardware/rev-c/source/pcb
    NEAR_COST=$(cat near_cost.txt) ORDER=$(cat order_c.txt) python3 board_c.py ../../out
    python3 drc_c.py ../../out ../../../source/schematic_rev0.4.py   # rev 0.3 until 0.4 exists
    python3 hand_check.py ../../out/model.json                        # track-to-pad gaps < 0.5 mm
    python3 ../../trial_view.py ../../out/model.json ../../trial-route-revC.png
    python3 gerb_c.py ../../out                 # Gerbers + drill + zip (out/apple2-wifi-floppy-revC-*)
    python3 view_c.py ../../out ../../revC 18   # top/bottom renders by PARSING the Gerbers
    python3 pdf_c.py ../../out                  # out/fit-check-revC-1to1.pdf (US Letter, print at 100%)
    python3 ../schematic_rev0.4.py ../../wifi-disk-ii-schematic-r0.4.svg

`board_c.py` is rev B's `board_b.py` with the rev C placement and netlist, plus a
router cost for nodes one grid step from another net's pad (hand soldering,
Barry). Current route: 32 nets, 22 vias, DRC 0 errors, matches schematic 0.4
(85 pins); 13 track-to-pad gaps under 0.5 mm, all single passes between two
adjacent pins (none along a pin row). Power nets route first (order_c.txt).

## Changes from rev B (Barry's list)

1. J2 test header removed (it was never on the schematic).
2. Silkscreen title "APPLE II WIFI FLOPPY EMULATOR  REV C  2026-11  BY BARRY HILLS".
3-7, 12-15. Parts moved (see placement drawing); 1.54in OLED (44.45 x 38.1 mm),
   lower-left corner tangent to the bottom-left mounting hole.
8. R5 fed from Pico pin 21 (GP16, net LED_ACT) instead of +3V3.
9. R5 = 180 ohm.
10. D2 square pads (1.8 mm, 0.8 mm hole kept).
11. D2 labelled ACT.

## Schematic rev 0.4 (done: board matches it, 85 pins, 0 mismatches)

R5 top to Pico pin 21 (LED_ACT); R5 180; D2 note "activity"; J3 label covers the
0.96in SSD1306 and 1.54in SH1106-class modules (same pins: GND VCC SCL SDA).

## Silkscreen requirements (Barry)

- UP, DOWN, ENTER, RESTART next to their switches, outside the switch bodies.
- ACT above D2 (below would run into the mounting hole).
- J1 pin 1: a bold "1" with a pointer about 2.5 mm beyond the real connector
  outline, so neither the box header nor the IDC plug covers it (rev B's sat
  0.7 mm out and got covered).
- R5 marked 180.

## OLED / J3 (settled 2026-11-20 with the 1:1 fit print)

- J3 header 2.2 mm left of where a centred header would be (Barry, fit check;
  the module board is 42.4 mm wide). Pin centres 2.0 mm below the module's top.
- Display and J3 lowered 3.41 mm (Barry: up to 4 mm fine; removable tabs may
  cover a sliver of the bottom-left hole), putting J3 on the grid (x 17..23,
  y 24), 3 steps below U1's bottom row: this cleared the crowded U1/J3 lane.
- No silkscreen outline for the display (Barry: distracting); just J3's header
  box, pin labels and "J3 OLED".
- Title back along the bottom edge, partly under the display (Barry: fine).
- RESTART label below SW4.

## Open

- (Done 2026-11-20: Barry's 1:1 fit check passed. Order files in `fab/`:
  `apple2-wifi-floppy-revC-gerbers.zip`, sha256 93705eb73d22b168..., identical
  to a fresh build from the committed source; plus the fit-check PDF.)
- Gerbers in a third-party viewer (new square-pad code), or JLCPCB's preview.
- A low pan-head or nylon screw at the bottom-left hole (under the OLED corner).
