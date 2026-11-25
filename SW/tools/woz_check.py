#!/usr/bin/env python3
"""Independent checker for a2drive's WOZ 2 output.

Parses the WOZ container (CRC, INFO, TMAP, TRKS), then reads every track the
way a Disk II controller does: bits shift into a latch, and a byte is
complete when its high bit is set (so the two zero bits after each self-sync
FF are absorbed). It finds address fields (D5 AA 96) and data fields
(D5 AA AD), checks every checksum and epilogue, decodes 6-and-2, puts the
sectors back in DOS logical order with the RWTS interleave table, and
compares the result with the original .dsk.

Written separately from firmware/src/gcr.c; it shares only the published
format, not code. The interleave here comes straight from the DOS 3.3 RWTS
table at $BFB8 (table[logical] = physical), not the encoder's inverse.

usage: woz_check.py image.woz original.dsk
"""
import struct
import sys
import zlib

RWTS_INTERLEAVE = [0x00, 0x0D, 0x0B, 0x09, 0x07, 0x05, 0x03, 0x01,
                   0x0E, 0x0C, 0x0A, 0x08, 0x06, 0x04, 0x02, 0x0F]


def gcr62_table():
    """64 valid 6-and-2 disk bytes, from the rules in Beneath Apple DOS ch. 3."""
    out = []
    for b in range(0x96, 0x100):
        bits = [(b >> i) & 1 for i in range(7, -1, -1)]
        zero_pairs = sum(1 for i in range(7) if bits[i] == 0 and bits[i + 1] == 0)
        low = b & 0x7F
        adjacent_ones = any(((low >> i) & 3) == 3 for i in range(6))
        if b & 0x80 and zero_pairs <= 1 and adjacent_ones and b not in (0xAA, 0xD5):
            out.append(b)
    assert len(out) == 64
    return {v: i for i, v in enumerate(out)}


DECODE62 = gcr62_table()


def fail(msg):
    print("FAIL:", msg)
    sys.exit(1)


def track_bits(data, start_block, bit_count):
    raw = data[start_block * 512:]
    return [(raw[i >> 3] >> (7 - (i & 7))) & 1 for i in range(bit_count)]


def read_nibbles(bits, revolutions=2):
    """Disk II latch model over a circular track. Returns (nibbles, syncs)."""
    nibs, lead_zeros = [], []
    latch, zeros = 0, 0
    n = len(bits)
    for i in range(n * revolutions):
        b = bits[i % n]
        if latch == 0 and b == 0:
            zeros += 1
            continue
        latch = ((latch << 1) | b) & 0xFF
        if latch & 0x80:
            nibs.append(latch)
            lead_zeros.append(zeros)
            latch, zeros = 0, 0
    return nibs, lead_zeros


def decode_4and4(a, b):
    return ((a << 1) | 1) & b


def decode_sector(nibs, i):
    """nibs[i:] starts right after D5 AA AD. Returns 256 bytes."""
    vals, acc = [], 0
    for k in range(342):
        n = nibs[i + k]
        if n not in DECODE62:
            fail(f"invalid data nibble {n:02X}")
        acc ^= DECODE62[n]
        vals.append(acc)
    chk = nibs[i + 342]
    if chk not in DECODE62 or DECODE62[chk] != acc:
        fail("data field checksum mismatch")
    if nibs[i + 343:i + 345] != [0xDE, 0xAA]:
        fail("data field epilogue missing")
    out = bytearray(256)
    for j in range(256):
        two = (vals[j % 86] >> (2 * (j // 86))) & 3
        two = ((two & 1) << 1) | (two >> 1)          # pairs are stored bit-swapped
        out[j] = (vals[86 + j] << 2) | two
    return bytes(out)


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        sys.exit(2)
    woz = open(sys.argv[1], "rb").read()
    dsk = open(sys.argv[2], "rb").read()
    if len(dsk) != 143360:
        fail(f"{sys.argv[2]} is {len(dsk)} bytes, expected 143360")

    if woz[:8] != b"WOZ2\xFF\x0A\x0D\x0A":
        fail("bad WOZ2 signature")
    crc = struct.unpack_from("<I", woz, 8)[0]
    if crc != zlib.crc32(woz[12:]):
        fail(f"file CRC {crc:08X} != computed {zlib.crc32(woz[12:]):08X}")
    if woz[12:16] != b"INFO" or struct.unpack_from("<I", woz, 16)[0] != 60:
        fail("INFO chunk not at offset 12 with size 60")
    info = woz[20:80]
    ver, dtype, wp, sync, clean = info[0:5]
    creator = info[5:37].decode("utf-8").rstrip()
    sides, bootfmt, timing = info[37], info[38], info[39]
    largest = struct.unpack_from("<H", info, 44)[0]
    print(f"INFO v{ver} type={dtype} wp={wp} sync={sync} cleaned={clean} "
          f"creator='{creator}' sides={sides} boot={bootfmt} timing={timing} largest={largest}")
    if (dtype, sides, bootfmt, timing) != (1, 1, 1, 32):
        fail("unexpected INFO values for a 5.25\" 16-sector disk")
    if woz[80:84] != b"TMAP" or woz[248:252] != b"TRKS":
        fail("TMAP/TRKS chunks not at expected offsets")
    tmap = woz[88:248]
    trks = woz[256:256 + 1280]

    image = bytearray(143360)
    min_gap = 999
    for t in range(35):
        if tmap[4 * t] != t:
            fail(f"TMAP[{4*t}] = {tmap[4*t]}, expected {t}")
        start, count, nbits = struct.unpack_from("<HHI", trks, 8 * t)
        if count * 512 * 8 < nbits:
            fail(f"track {t}: {nbits} bits do not fit {count} blocks")
        nibs, zeros = read_nibbles(track_bits(woz, start, nbits))
        found = {}
        i = 0
        while i < len(nibs) - 400 and len(found) < 16:
            if nibs[i:i + 3] == [0xD5, 0xAA, 0x96]:
                f = [decode_4and4(nibs[i + 3 + 2 * k], nibs[i + 4 + 2 * k]) for k in range(4)]
                vol, trk, sec, chk = f
                if vol ^ trk ^ sec != chk:
                    fail(f"track {t}: address checksum")
                if trk != t:
                    fail(f"track {t}: address field says track {trk}")
                if nibs[i + 11:i + 13] != [0xDE, 0xAA]:
                    fail(f"track {t}: address epilogue")
                # Data prologue must follow within a short gap.
                j = i + 14
                gap = 0
                while nibs[j] == 0xFF:
                    gap += 1
                    j += 1
                min_gap = min(min_gap, gap)
                if nibs[j:j + 3] != [0xD5, 0xAA, 0xAD]:
                    fail(f"track {t} sector {sec}: no data prologue after address")
                if sec not in found:
                    found[sec] = decode_sector(nibs, j + 3)
                i = j + 3 + 345
            else:
                i += 1
        if sorted(found) != list(range(16)):
            fail(f"track {t}: found sectors {sorted(found)}")
        for phys, data in found.items():
            logical = RWTS_INTERLEAVE.index(phys)
            off = t * 4096 + logical * 256
            image[off:off + 256] = data
        if t in (0, 17, 34):
            print(f"track {t:2}: {nbits} bits, 16 sectors OK, volume {vol}")

    print(f"shortest gap 2: {min_gap} sync bytes")
    if bytes(image) != dsk:
        bad = [k for k in range(560) if image[k*256:(k+1)*256] != dsk[k*256:(k+1)*256]]
        fail(f"{len(bad)} sectors differ from the .dsk, first: track {bad[0]//16} sector {bad[0]%16}")
    print("PASS: all 560 sectors decode and match the .dsk byte for byte")


if __name__ == "__main__":
    main()
