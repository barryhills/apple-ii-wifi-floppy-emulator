# WiFi Disk II: user guide

The board plugs into a 20-pin drive connector on an Apple Disk II
controller card and behaves as a 5.25" drive, drive 1 or drive 2. You load
disk images onto it over WiFi from a computer or phone, and the Apple
reads and writes them like floppies. Nothing changes on the Apple side.

## 1. Before you plug it into the Apple

The controller card's drive header is **not keyed**. A reversed ribbon
puts ±12 V on the board's logic and destroys the buffer chips.

With the Apple **off** and the ribbon connected **only to the card**,
check the free end of the ribbon with a meter. **Pin 1 must be ground**,
and **pin 11 must read +5 V** when the Apple is on. Do this every time the
ribbon is moved.

The connector on the card decides which drive the board is: the board
doesn't care. Booting needs drive 1.

## 2. Load the firmware (once per Pico, and for updates)

You need `a2drive.uf2` (from the Releases page, or build it: see
`firmware/`).

1. Put the Pico into flash mode. Either:
   - unplugged Pico: hold the **BOOTSEL** button on the Pico while plugging
     its USB cable into the computer; or
   - Pico on the board: hold **BOOTSEL**, tap **RESTART**, release BOOTSEL.
2. A USB drive named **RP2350** appears. Copy `a2drive.uf2` onto it.
3. The Pico restarts by itself and the RP2350 drive disappears.

Updating later works the same way. Your disk images and WiFi settings are
kept. If you have the Pico SDK's `picotool`, this does the same without
any buttons: `picotool load -f -v -x a2drive.uf2`.

## 3. WiFi setup (first power-on)

A drive with no WiFi network saved starts in **setup mode**, and the
OLED display shows:

```
WiFi setup mode
Join a2drive-setup
http://192.168.4.1
No password needed
```

1. On a computer or phone, join the WiFi network **a2drive-setup** (open,
   no password; on a Mac it may be under "Other Networks").
2. Open **http://192.168.4.1**. Type the `http://`: some browsers (Chrome,
   Edge, Vivaldi) otherwise try https and fail.
3. Enter your WiFi network and password, and a **hostname**. If you have
   more than one drive, give each its own name. Click **Save**.
4. The drive restarts and joins your network. Switch your computer back to
   your own network, and open `http://<hostname>.local`, or the IP address
   shown on the OLED display (phones don't always understand `.local`).

The drive needs **2.4 GHz** WiFi.

To change WiFi settings later, use the Device section of the web page. If
the drive can't join (the OLED display keeps saying "connecting..."), hold
**UP + DOWN** and tap **RESTART** (or power on): this enters setup mode
again. Your saved settings stay until you save new ones. A plain restart
leaves setup mode.

## 4. Everyday use

**Web page** (`http://<hostname>.local`): the **image carousel** holds 10
disk images.

- Click ○ to mount an image (●). The Apple sees that disk. Click ● to eject.
- **Upload**, **Download**, **Rename** and **Delete** act on the
  highlighted row.
- Images: `.dsk` (DOS order, 143,360 bytes) or `.woz` (version 1 or 2).
  Every image is writable.

**OLED display and buttons:**

- Home screen: hostname, IP address, the head's track, the mounted image.
- Press any button to open the image list. **UP** = previous, **DOWN** =
  next, **ENTER** = mount it. It returns to the home screen after 15 s.

**From a script:**

```
curl -T mydisk.dsk "http://a2drive.local/image/3?name=mydisk.dsk"
curl -X POST http://a2drive.local/mount/3
```

When the Apple writes, the drive saves the image to flash about half a
second after the writing stops.

## 5. The lights

**Pico's own LED** (green, on the Pico):

| LED | Meaning |
|---|---|
| Solid on | Starting up, or not on WiFi (connecting, or setup mode) |
| Slow blink, once a second | On WiFi, working |
| Stops blinking (stuck on or off) | The firmware stopped. The drive restarts itself within about 4 s |

**Activity LED** (D2, only with the optional change below):

| LED | Meaning |
|---|---|
| On from power-on until the WiFi joins | Starting up (not in setup mode) |
| On or flickering later | The Apple is using the drive, or a write is being saved |
| Off | Idle. **Safe to switch off.** |

Switching off while it's lit can lose the last write. The image itself
stays intact.

## 6. Optional: activity LED (rev B board)

As designed, D2 is a power LED wired to 3.3 V through R5. One cut and one
wire make it the activity LED. The firmware already drives it. Without
the change, nothing is connected to that output and no harm is done.

1. Fit **D2** and **R5**. A **blue LED with R5 = 180 Ω** gives a bright
   flash. For a red, yellow or green LED use about 330 to 470 Ω.
2. On the **back** of the board, find the trace that leaves **R5's +3V3
   pad** (the R5 pad farther from D2). It runs about 2.5 mm toward the
   J1 (back) edge, then turns. **Cut that first short run**, next to the
   pad. That pad feeds only R5.
3. Check with a meter: R5's +3V3 pad to Pico pin 36 (3V3) now reads open.
4. Solder a wire from that R5 pad to **Pico pin 21 (GP16)**. Pin 21 has
   no other connection on the board.

## 7. If something is wrong

| Symptom | Try |
|---|---|
| The page won't open | Type `http://` in full; try the IP address from the OLED display |
| OLED display says "connecting..." | WiFi network down or wrong password: wait, or use setup mode (UP + DOWN + RESTART) |
| Apple gets I/O ERROR | Is an image mounted (● on the web page)? Is the ribbon the right way round? |
| Two drives fight over a name | Give each drive its own hostname in setup or Settings |
