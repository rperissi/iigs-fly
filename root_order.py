#!/usr/bin/env python3
"""root_order.py IMG.2mg NAME [NAME ...]

Put the named root entries first, in that order, so the Finder's default
icon layout (directory order, left to right) shows them top-left. Any other
root entries keep their relative order after the named ones. The root
Finder.Data, if present, must already be deleted (it carries saved icon
positions from the template volume, which is what put GSFLY off the bottom
of the window; 2026-10-21, Andrew + Rob).

Walks the whole root directory chain (AppleCommander leaves new entries in
later blocks). ProDOS directory entries are position-independent except
that each subdirectory's header block points back at its parent block and
entry number, so those are rewritten. 2IMG (64-byte header) or raw .po.
"""
import struct
import sys

E = 0x27
PER = 13


def main():
    p = sys.argv[1]
    want = [w.upper() for w in sys.argv[2:]]
    f = bytearray(open(p, 'rb').read())
    hdr = 64 if f[:4] == b'2IMG' else 0

    def off(n):
        return hdr + n * 512

    blocks = []
    blk = 2
    while blk:
        blocks.append(blk)
        blk = struct.unpack('<H', f[off(blk) + 2:off(blk) + 4])[0]

    def name(e):
        return e[1:1 + (e[0] & 15)].decode('ascii', 'replace').upper()

    header = bytes(f[off(2) + 4:off(2) + 4 + E])
    live = []
    for bi, blk in enumerate(blocks):
        b = off(blk)
        for i in range(PER):
            if bi == 0 and i == 0:
                continue
            e = bytes(f[b + 4 + i * E:b + 4 + (i + 1) * E])
            if e[0] >> 4:
                live.append(e)
    byname = {name(e): e for e in live}
    for w in want:
        if w not in byname:
            sys.exit(f'root_order: no root entry {w}')
    if 'FINDER.DATA' in byname:
        sys.exit('root_order: delete the root Finder.Data first')
    rest = [e for e in live if name(e) not in want]
    order = [byname[w] for w in want] + rest

    slots = [(2, 0, header)]
    k = 0
    for bi, blk in enumerate(blocks):
        for i in range(PER):
            if bi == 0 and i == 0:
                continue
            e = order[k] if k < len(order) else bytes(E)
            k += 1
            slots.append((blk, i, e))
    for blk, i, e in slots:
        b = off(blk)
        f[b + 4 + i * E:b + 4 + (i + 1) * E] = e
        if e[0] >> 4 == 0xD:
            key = struct.unpack('<H', e[0x11:0x13])[0]
            h = off(key) + 4
            f[h + 0x23:h + 0x25] = struct.pack('<H', blk)
            f[h + 0x25] = i
    open(p, 'wb').write(f)
    print('root order:', ' '.join(name(e) for e in order))


if __name__ == '__main__':
    main()
