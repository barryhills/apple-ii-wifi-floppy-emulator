import sys, glob, importlib.util
from reportlab.pdfgen import canvas
from reportlab.lib.pagesizes import letter
from reportlab.lib.units import mm
sys.argv=['view.py','outb','/dev/null/x','1']
src=open('view.py').read().split('f = lambda ext')[0]; ns={}; exec(src,ns)
g=lambda e: ns['gerber'](glob.glob(f'outb/*{e}')[0])
c=canvas.Canvas('outb/fit-check-revB-1to1.pdf',pagesize=letter); ox,oy=20*mm,60*mm
c.setFont('Helvetica-Bold',12); c.drawString(ox,oy+78*mm,'WiFi Disk II rev B - 1:1 fit check. Print at 100% / "actual size", NOT fit-to-page.')
c.setFont('Helvetica',10); c.drawString(ox,oy+73*mm,'Check: the bar below must measure 50.0 mm. Then lay each part on its outline; leads must land on the dots.')
c.setLineWidth(0.3*mm); c.line(ox,oy-10*mm,ox+50*mm,oy-10*mm); c.line(ox,oy-12*mm,ox,oy-8*mm); c.line(ox+50*mm,oy-12*mm,ox+50*mm,oy-8*mm); c.drawString(ox+52*mm,oy-11*mm,'50 mm')
def draw(shapes,gray,fill=1):
    c.setStrokeGray(gray); c.setFillGray(gray)
    for k,a,b,w in shapes:
        if k=='L': c.setLineWidth(w*mm); c.setLineCap(1); c.line(ox+a[0]*mm,oy+a[1]*mm,ox+b[0]*mm,oy+b[1]*mm)
        else: c.circle(ox+a[0]*mm,oy+a[1]*mm,w/2*mm,stroke=0,fill=1)
draw(g('.GKO'),0); draw(g('.GTS'),0.75); draw(g('.GTO'),0)
for p in glob.glob('outb/*.DRL'):
    for x,y,d in ns['excellon'](p): c.setFillGray(1 if d>2 else 0); c.setStrokeGray(0); c.setLineWidth(0.1*mm); c.circle(ox+x*mm,oy+y*mm,d/2*mm,stroke=1,fill=1)
c.save()
