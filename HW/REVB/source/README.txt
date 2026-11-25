WiFi Disk II - board source (rev B, schematic rev 0.3)

schematic_rev0.3.py   draws the schematic SVG (edit the output path at the bottom)
pcb/board_b.py        placement + netlist + router -> <outdir>/model.json
                      run:  ORDER=$(cat order_b.txt) python3 board_b.py outb
pcb/drc_b.py          design-rule, connectivity and schematic cross-check
                      run:  python3 drc_b.py outb ../schematic_rev0.3.py
pcb/gerb_b.py         writes Gerbers, drill files and the zip from model.json
pcb/view.py           renders top/bottom PNGs by parsing the Gerbers
pcb/pdf_b.py          1:1 fit-check PDF
order_b.txt           net routing order that gives a complete route
Needs: python3, pillow, matplotlib, reportlab, rsvg-convert (for the schematic PNG).
