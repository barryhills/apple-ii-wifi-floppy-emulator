W,H=1560,1325
o=[]
def t(x,y,s,size=13,anchor="start",fill="#111",weight="normal",fam="DejaVu Sans Mono, Menlo, monospace"):
    s=s.replace("&","&amp;").replace("<","&lt;")
    o.append(f'<text x="{x}" y="{y}" font-size="{size}" text-anchor="{anchor}" fill="{fill}" font-weight="{weight}" font-family="{fam}">{s}</text>')
def line(x1,y1,x2,y2,c="#111",w=1.5):
    o.append(f'<line x1="{x1}" y1="{y1}" x2="{x2}" y2="{y2}" stroke="{c}" stroke-width="{w}"/>')
def col(net):
    if net is None: return "#888"
    if net.startswith("GND"): return "#1a5fb4"
    if net.startswith("+") or net=="VSYS": return "#c01c28"
    return "#1b7a3a"
def ic(x,y,w,ref,name,left,right,pitch=30,stub=40):
    n=max(len(left),len(right)); h=n*pitch+20
    o.append(f'<rect x="{x}" y="{y}" width="{w}" height="{h}" fill="#fffbe6" stroke="#111" stroke-width="2"/>')
    t(x+w/2,y-26,ref,15,"middle",weight="bold"); t(x+w/2,y-9,name,13,"middle")
    for side,pins in (("L",left),("R",right)):
        for i,p in enumerate(pins):
            if p is None: continue
            num,pname,net=p; py=y+25+i*pitch
            if side=="L":
                line(x-stub,py,x,py); t(x-6,py-5,str(num),11,"end","#555"); t(x+7,py+4,pname,12)
                if net is None:
                    line(x-stub-5,py-5,x-stub+5,py+5,"#888"); line(x-stub-5,py+5,x-stub+5,py-5,"#888")
                else: t(x-stub-6,py+4,net,13,"end",col(net),"bold")
            else:
                line(x+w,py,x+w+stub,py); t(x+w+6,py-5,str(num),11,"start","#555"); t(x+w-7,py+4,pname,12,"end")
                if net is None:
                    line(x+w+stub-5,py-5,x+w+stub+5,py+5,"#888"); line(x+w+stub-5,py+5,x+w+stub+5,py-5,"#888")
                else: t(x+w+stub+6,py+4,net,13,"start",col(net),"bold")
    return h
def two(x,y,ref,val,a,b,kind="R"):
    # two-terminal part, horizontal
    line(x,y,x+35,y); line(x+95,y,x+130,y)
    if kind=="R":
        o.append(f'<rect x="{x+35}" y="{y-9}" width="60" height="18" fill="#fff" stroke="#111" stroke-width="2"/>')
    elif kind=="C":
        line(x+35,y,x+60,y); line(x+70,y,x+95,y); line(x+60,y-13,x+60,y+13,w=2.5); line(x+70,y-13,x+70,y+13,w=2.5)
    elif kind=="D":
        line(x+35,y,x+52,y); line(x+78,y,x+95,y)
        o.append(f'<polygon points="{x+52},{y-12} {x+52},{y+12} {x+78},{y}" fill="#fff" stroke="#111" stroke-width="2"/>')
        line(x+78,y-12,x+78,y+12,w=2.5)
    elif kind=="S":
        o.append(f'<circle cx="{x+40}" cy="{y}" r="4" fill="#fff" stroke="#111" stroke-width="2"/><circle cx="{x+90}" cy="{y}" r="4" fill="#fff" stroke="#111" stroke-width="2"/>')
        line(x+35,y,x+36,y); line(x+94,y,x+95,y); line(x+38,y-10,x+92,y-10,w=2.5); line(x+65,y-10,x+65,y-16,w=2)
    t(x+65,y-(24 if kind=="S" else 18),f"{ref}  {val}",12,"middle",weight="bold")
    t(x-6,y+4,a,13,"end",col(a),"bold"); t(x+136,y+4,b,13,"start",col(b),"bold")

o.append(f'<rect width="{W}" height="{H}" fill="#ffffff"/>')
o.append(f'<rect x="8" y="8" width="{W-16}" height="{H-16}" fill="none" stroke="#111" stroke-width="2"/>')
t(30,42,"Apple II WiFi Floppy Emulator - board rev C",22,weight="bold",fam="DejaVu Sans, Helvetica, sans-serif")
t(30,64,"Rev 0.4  2026-11-20   Nets with the same name are connected.  * = active low.  x = no connect.",13,fill="#444")

# J1
J=[(1,"GND"),(2,"PH0"),(3,"GND"),(4,"PH1"),(5,"GND"),(6,"PH2"),(7,"GND"),(8,"PH3"),(9,None),(10,"WRREQ*"),
   (11,"+5V_A2"),(12,"+5V_A2"),(13,None),(14,"ENABLE*"),(15,None),(16,"RDDATA"),(17,None),(18,"WRDATA"),(19,None),(20,"WRPROT")]
nm={9:"-12V",13:"+12V",15:"+12V",17:"+12V",19:"+12V"}
L=[(n,nm.get(n,""),net) for n,net in J if n%2]; R=[(n,nm.get(n,""),net) for n,net in J if n%2==0]
ic(170,130,110,"J1","2x10 header to Disk II card",L,R)
t(225,480,"-12V / +12V pins: leave OPEN.",12,"middle","#c01c28",weight="bold")
t(225,497,"Meter-check 9,11,12,13 vs GND",12,"middle","#c01c28")
t(225,514,"before connecting the Pico.",12,"middle","#c01c28")

# U1
ic(640,130,150,"U1","SN74LVC245AN  (Vcc = 3.3V)",
 [(1,"DIR","+3V3"),(2,"A1","PH0"),(3,"A2","PH1"),(4,"A3","PH2"),(5,"A4","PH3"),(6,"A5","ENABLE*"),(7,"A6","WRREQ*"),(8,"A7","WRDATA"),(9,"A8","GND"),(10,"GND","GND")],
 [(20,"VCC","+3V3"),(19,"OE*","GND"),(18,"B1","PH0_3V"),(17,"B2","PH1_3V"),(16,"B3","PH2_3V"),(15,"B4","PH3_3V"),(14,"B5","ENABLE_3V*"),(13,"B6","WRREQ_3V*"),(12,"B7","WRDATA_3V"),(11,"B8",None)])
t(715,470,"5V in (A) -> 3.3V out (B)",12,"middle","#444")

# U2
ic(640,580,150,"U2","74HCT125 or 74LS125  (Vcc = 5V)",
 [(1,"1OE*","ENABLE*"),(2,"1A","RDDATA_R"),(3,"1Y","RDDATA"),(4,"2OE*","ENABLE*"),(5,"2A","WRPROT_R"),(6,"2Y","WRPROT"),(7,"GND","GND")],
 [(14,"VCC","+5V_A2"),(13,"4OE*","+5V_A2"),(12,"4A","GND"),(11,"4Y",None),(10,"3OE*","+5V_A2"),(9,"3A","GND"),(8,"3Y",None)])
t(715,830,"3.3V in -> 5V out, Hi-Z unless ENABLE* low",12,"middle","#444")

# U3
ic(1200,130,170,"U3","Raspberry Pi Pico 2 W",
 [(3,"GND","GND"),(4,"GP2","PH0_3V"),(5,"GP3","PH1_3V"),(6,"GP4","PH2_3V"),(7,"GP5","PH3_3V"),(8,"GND","GND"),(9,"GP6","ENABLE_3V*"),(10,"GP7","WRREQ_3V*"),(11,"GP8","WRDATA_3V"),(12,"GP9","ENTER*"),(13,"GND","GND"),(14,"GP10","RDDATA_3V"),(15,"GP11","WRPROT_3V"),(16,"GP12","SDA"),(17,"GP13","SCL"),(18,"GND","GND"),(19,"GP14","UP*"),(20,"GP15","DOWN*")],
 [(40,"VBUS",None),(39,"VSYS","VSYS"),(38,"GND","GND"),(36,"3V3(OUT)","+3V3"),(33,"GND","GND"),(30,"RUN","RESTART*"),(28,"GND","GND"),(23,"GND","GND"),(21,"GP16","LED_ACT")])
t(1285,715,"Unlisted pins: no connect.",12,"middle","#444")
t(1285,732,"USB may be plugged in while",12,"middle","#444")
t(1285,749,"the Apple is powered (D1).",12,"middle","#444")

# J3 OLED
ic(640,930,150,"J3","OLED 0.96in SSD1306 or 1.54in SH1106-class (I2C)",
 [(1,"GND","GND"),(2,"VCC","+3V3"),(3,"SCL","SCL"),(4,"SDA","SDA")],[])
t(715,1090,"module pin order: GND VCC SCL SDA",12,"middle","#444")

# passives, right column
t(1010,800,"Power / passives",15,weight="bold")
y=845
two(1100,y,"D1","1N5817","+5V_A2","VSYS","D"); y+=52
two(1100,y,"R1","470","RDDATA_3V","RDDATA_R"); y+=52
two(1100,y,"R2","470","WRPROT_3V","WRPROT_R"); y+=52
two(1100,y,"R3","10k","ENABLE*","+3V3"); y+=52
two(1100,y,"R4","10k","WRREQ*","+3V3"); y+=52
two(1100,y,"R5","180","LED_ACT","LED_A"); y+=52
two(1100,y,"D2","LED","LED_A","GND","D"); t(1330,y+4,"activity (GP16)",12,fill="#444")
# left column
t(110,600,"Decoupling / buttons",15,weight="bold")
y=645
two(200,y,"C1","0.1uF","+3V3","GND","C"); t(420,y+4,"at U1 pin 20",12,fill="#444"); y+=52
two(200,y,"C2","0.1uF","+5V_A2","GND","C"); t(420,y+4,"at U2 pin 14",12,fill="#444"); y+=52
two(200,y,"C3","10uF","+5V_A2","GND","C"); t(420,y+4,"bulk, by the Pico",12,fill="#444"); y+=56
two(200,y,"SW1","UP","UP*","GND","S"); y+=56
two(200,y,"SW2","DOWN","DOWN*","GND","S"); y+=56
two(200,y,"SW3","ENTER","ENTER*","GND","S"); y+=56
two(200,y,"SW4","RESTART","RESTART*","GND","S"); t(420,y+4,"resets the Pico",12,fill="#444")

n=["NOTES",
"1. J1 pin numbers are the Disk II 20-pin connector (Apple dwg 050-0007-01). Odd row = grounds/power, even row = signals.",
"2. R1/R2 limit current into U2 inputs if the Pico is on USB power while the Apple is off. 470 ohm suits both HCT125 and LS125.",
"3. R3/R4 pull up to +3V3 so the Pico sees deselected, not-writing when the ribbon is unplugged or the Apple is off.",
"4. ENABLE*, WRREQ* active low; WRPROT high = protected. RDDATA pulse sense is a firmware constant (working emulators use both).",
"5. SW1-SW4 momentary, normally open. No resistors: GP9/GP14/GP15 use Pico internal pull-ups; RUN is pulled up on the Pico.",
"6. OLED runs from +3V3 on I2C0 (GP12 SDA, GP13 SCL); the module carries its own I2C pull-ups.",
"7. To reflash in place: hold BOOTSEL on the Pico, tap SW4, release BOOTSEL.",
"8. D2 is the activity light, driven by GP16 (firmware v0.13.3+): on while the drive is selected or writes are unsaved."]
for i,s_ in enumerate(n):
    t(40,1130+i*21,s_,13 if i else 14,weight="bold" if i==0 else "normal",fam="DejaVu Sans, Helvetica, sans-serif")
import sys
open(sys.argv[1] if len(sys.argv) > 1 else "wifi-disk-ii-schematic-r0.4.svg","w").write(f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {W} {H}" width="{W}" height="{H}">'+"\n".join(o)+"</svg>")
