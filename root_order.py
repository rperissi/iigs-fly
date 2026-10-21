#!/usr/bin/env python3
"""root_order.py IMG.2mg NAME [NAME ...]

Put the named root entries first, in that order, so the Finder's default
icon layout (directory order, left to right) shows them top-left. Any other
root entries keep their relative order after the named ones. The root
Finder.Data, if present, must already be deleted (it carries saved icon
positions from the template volume, which is what put GSFLY off the bottom
of the window; 2026-10-21, Andrew + Rob).

ProDOS directory entries are position-independent except that each
subdirectory's header block points back at its entry number in the parent,
so those are rewritten too. Works on a 2IMG (64-byte header) or a raw .po.
"""
import struct
import sys

E = 0x27


def main():
    p = sys.argv[1]
    want = [w.upper() for w in sys.argv[2:]]
    f = bytearray(open(p, 'rb').read())
    hdr = 64 if f[:4] == b'2IMG' else 0

    def off(n):
        return hdr + n * 512

    b = off(2)
    ents = [bytes(f[b + 4 + i * E:b + 4 + (i + 1) * E]) for i in range(13)]

    def name(e):
        return e[1:1 + (e[0] & 15)].decode('ascii', 'replace').upper()

    live = [e for e in ents[1:] if e[0] >> 4]
    byname = {name(e): e for e in live}
    for w in want:
        if w not in byname:
            sys.exit(f'root_order: no root entry {w}')
    if 'FINDER.DATA' in byname:
        sys.exit('root_order: delete the root Finder.Data first')
    rest = [e for e in live if name(e) not in want]
    new = [ents[0]] + [byname[w] for w in want] + rest
    if len(new) > 13:
        sys.exit('root_order: only the first directory block is handled')
    new += [bytes(E)] * (13 - len(new))
    for i, e in enumerate(new):
        f[b + 4 + i * E:b + 4 + (i + 1) * E] = e
    for i, e in enumerate(new):
        if e[0] >> 4 == 0xD:
            key = struct.unpack('<H', e[0x11:0x13])[0]
            h = off(key) + 4
            f[h + 0x23:h + 0x25] = struct.pack('<H', 2)
            f[h + 0x25] = i
    open(p, 'wb').write(f)
    print('root order:', ' '.join(name(e) for e in new if e[0] >> 4))


if __name__ == '__main__':
    main()
