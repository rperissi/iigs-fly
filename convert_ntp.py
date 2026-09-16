#!/usr/bin/env python3
"""Port of Ninjaforce ntpconverter_lib.php (Jesse Blue). STREAM_FORBIDDEN only."""
from __future__ import annotations

import struct
import sys
from pathlib import Path

STOPPER = 8
OPTIMUM = (256, 512, 1024, 2048, 4096, 8192, 16384, 32768)
DOC_PAGES = (1, 2, 4, 8, 16, 32, 64, 128)
DOC_REGS = (0b000000, 0b001001, 0b010010, 0b011011, 0b100100, 0b101101, 0b110110, 0b111111)
MOD_NOTES = [
    1712, 1616, 1524, 1440, 1356, 1280, 1208, 1140, 1076, 1016, 960, 906,
    856, 808, 762, 720, 678, 640, 604, 570, 538, 508, 480, 453,
    428, 404, 381, 360, 339, 320, 302, 285, 269, 254, 240, 226,
    214, 202, 190, 180, 170, 160, 151, 143, 135, 127, 120, 113,
    107, 101, 95, 90, 85, 80, 75, 71, 67, 63, 60, 56,
    53, 50, 47, 45, 42, 40, 37, 35, 33, 31, 30, 28,
]
STEREO = [1, 0, 0, 1] * 8
MOD_TYPES = {
    "M.K.": 4, "FLT4": 4, "FLT8": 8,
    **{f"{n}CHN": n for n in range(1, 10)},
    **{f"{n:02d}CH": n for n in range(1, 32)},
    **{f"{n:02d}CN": n for n in range(1, 32)},
}
TYPE_SINGLE, TYPE_LOOPED, TYPE_LOOPHEADER, TYPE_LOOP = 0, 1, 2, 3


def be16(data: bytes, off: int) -> int:
    return (data[off] << 8) | data[off + 1]


def pstr(data: bytes, off: int, n: int) -> str:
    raw = data[off : off + n]
    if b"\x00" in raw:
        raw = raw[: raw.index(b"\x00")]
    return raw.decode("latin-1", errors="replace")


def to_ensoniq(src: list[int]) -> list[int]:
    out = []
    for b in src:
        e = (b + 128) & 255
        if e == 0:
            e = 1
        out.append(e)
    return out


def optimal(data: list[int]) -> bool:
    return len(data) in OPTIMUM


def optimize_loop(data: list[int]) -> list[int]:
    length = len(data)
    if length in OPTIMUM or length >= 512:
        return data
    factor = 512 / length
    stopper = 0 if factor == int(factor) else STOPPER
    loop = list(data)
    while len(data) + len(loop) + stopper <= 512:
        data = data + loop
    return data


def zero_cross(data: list[int]) -> int:
    best_i, best_d = 0, 1000
    for i, b in enumerate(data):
        d = abs(b - 128)
        if d < best_d:
            best_d, best_i = d, i
    return best_i


class ModInst:
    def __init__(self, number, name, length, finetune, volume, rep_ptr, rep_len, data):
        self.number = number
        self.name = name
        self.length = length
        self.finetune = finetune
        self.volume = volume
        self.repeat_pointer = rep_ptr
        self.repeat_length = rep_len
        self.data = data

    def kind(self) -> int:
        if self.repeat_length < 3:
            return TYPE_SINGLE
        if self.repeat_pointer > 2:
            return TYPE_LOOPHEADER  # loop-with-header in the MOD sense
        return TYPE_LOOPED


class NtpInst:
    def __init__(self, mod: ModInst, typ: int, number: int, rotate: bool):
        self.type = typ
        self.number = number
        self.number_original = mod.number
        self.name = mod.name
        self.volume = mod.volume
        self.finetune = mod.finetune
        self.repeat_pointer = mod.repeat_pointer
        self.repeat_length = mod.repeat_length
        self.src = to_ensoniq(mod.data)
        self.original_length = len(self.src)
        self.has_stopper = False
        self.data = self._build(rotate)
        self.length = len(self.data)
        self.page_size = (self.length + 255) // 256

    def _split(self, rotate: bool):
        head = self.src[: self.repeat_pointer]
        loop = self.src[self.repeat_pointer : self.repeat_pointer + self.repeat_length]
        pos = zero_cross(loop)
        if not rotate or pos == 0:
            return head, loop
        return head + loop[:pos], loop[pos:] + loop[:pos]

    def _build(self, rotate: bool) -> list[int]:
        if self.type == TYPE_SINGLE:
            data = list(self.src)
        elif self.type == TYPE_LOOPED:
            data = optimize_loop(self.src[self.repeat_pointer : self.repeat_pointer + self.repeat_length])
        elif self.type == TYPE_LOOPHEADER:
            data, _ = self._split(rotate)
        elif self.type == TYPE_LOOP:
            _, loop = self._split(rotate)
            data = optimize_loop(loop)
        else:
            raise ValueError(self.type)
        add = not optimal(data)
        self.has_stopper = add
        if add:
            data = data + [0] * STOPPER
        return data

    def type_byte(self) -> int:
        return self.type + (4 if optimal(self.data) and not self.has_stopper else 0)

    def osc_count(self) -> int:
        if self.type == TYPE_SINGLE or (
            self.type == TYPE_LOOPED and optimal(self.data) and not self.has_stopper
        ):
            return 1
        return 2


def reserve(pages: list[int], page_size: int, typ: int) -> int:
    align = next((b for b in DOC_PAGES if page_size <= b), None)
    if align is None:
        return -2
    start = -1
    for i in range(0, 256, align):
        if pages[i] == 0:
            start = i
            break
    if start >= 0:
        for i in range(start, start + page_size):
            pages[i] = typ
    return start


def doc_reg(page_size: int) -> int:
    if page_size == 0:
        return 0
    for i, lim in enumerate(DOC_PAGES):
        if page_size <= lim:
            return DOC_REGS[i]
    return -1


def arrange(insts: list[NtpInst]):
    pages = [0] * 256
    stuff = [{"type": 1, "size": n.length, "inst": n, "pages": n.page_size} for n in insts]
    stuff.sort(key=lambda s: s["pages"], reverse=True)
    doc_ptr, doc_sz = {}, {}
    for s in stuff:
        ps = (s["size"] + 255) // 256
        start = reserve(pages, ps, 1)
        if start < 0:
            raise RuntimeError(f"DOC RAM full placing instrument {s['inst'].number_original} ({s['size']} bytes)")
        doc_ptr[s["inst"].number] = start
        doc_sz[s["inst"].number] = doc_reg(s["inst"].page_size)
    return doc_ptr, doc_sz, pages


def convert_period(period: int) -> int:
    if period == 0:
        return 0
    if period in MOD_NOTES:
        return MOD_NOTES.index(period) + 1
    best = min(range(len(MOD_NOTES)), key=lambda i: abs(period - MOD_NOTES[i]))
    return best + 1


def gs3(n: int) -> bytes:
    return bytes((n & 255, (n >> 8) & 255, (n >> 16) & 255))


def convert(mod_path: Path, out_path: Path) -> str:
    data = mod_path.read_bytes()
    name = pstr(data, 0, 20)
    sig = data[1080:1084].decode("latin-1")
    tracks = MOD_TYPES.get(sig)
    if tracks is None:
        raise RuntimeError(f"unsupported MOD type {sig!r}")
    max_inst, ptr_len, headlen, song0, song1 = 31, 950, 1084, 952, 1080
    max_pat = max(data[song0:song1]) + 1
    order_len = data[ptr_len]
    order = list(data[ptr_len + 2 : ptr_len + 2 + order_len])
    pat_len = tracks * 64 * 4

    insts: list[ModInst] = []
    off = headlen + pat_len * max_pat
    for i in range(1, max_inst + 1):
        p = 20 + (i - 1) * 30
        ilen = be16(data, p + 22) * 2
        if ilen == 0:
            continue
        vol = data[p + 25]
        fine = data[p + 24] & 15
        rp = be16(data, p + 26) * 2
        rl = be16(data, p + 28) * 2
        raw = list(data[off : off + ilen])
        off += ilen
        insts.append(ModInst(i, pstr(data, p, 22), ilen, fine, vol, rp, rl, raw))

    track_used: dict[int, list[int]] = {}
    for pn in range(max_pat):
        base = headlen + pat_len * pn
        for line in range(64):
            for tr in range(tracks):
                ptr = base + line * tracks * 4 + tr * 4
                w0, w1 = be16(data, ptr), be16(data, ptr + 2)
                ins = ((w0 & 0xF000) >> 8) + ((w1 & 0xF000) >> 12)
                if ins:
                    track_used.setdefault(tr, [])
                    if ins not in track_used[tr]:
                        track_used[tr].append(ins)

    ntp: list[NtpInst] = []
    num = 1
    for m in insts:
        k = m.kind()
        if k == TYPE_SINGLE:
            ntp.append(NtpInst(m, TYPE_SINGLE, num, True))
        elif k == TYPE_LOOPED:
            ntp.append(NtpInst(m, TYPE_LOOPED, num, True))
        else:
            ntp.append(NtpInst(m, TYPE_LOOPHEADER, num, True))
            num += 1
            ntp.append(NtpInst(m, TYPE_LOOP, num, True))
        num += 1

    imap = {}
    for n in ntp:
        imap.setdefault(n.number_original, n.number)

    doc_ptr, doc_sz, pages = arrange(ntp)

    osc_need = {t: 1 for t in range(1, tracks + 1)}
    for t0, used in track_used.items():
        t = t0 + 1
        for mi in used:
            for n in ntp:
                if n.number_original == mi:
                    osc_need[t] = max(osc_need[t], n.osc_count())
    if sum(osc_need.values()) > 31:
        raise RuntimeError(f"too many oscillators: {sum(osc_need.values())}")

    oscs = [0] * 31 + [-1]
    track_byte = {}
    for t, need in osc_need.items():
        for i in range(0, 31, need):
            if need == 1 and oscs[i] == 0:
                oscs[i] = t
                track_byte[t] = i
                break
            if need == 2 and i + 1 < 31 and oscs[i] == 0 and oscs[i + 1] == 0:
                oscs[i] = oscs[i + 1] = t
                track_byte[t] = 128 + i
                break
        else:
            raise RuntimeError(f"no oscillators for track {t}")

    notes = []
    for pn in range(max_pat):
        base = headlen + pat_len * pn
        for line in range(64):
            for tr in range(tracks):
                ptr = base + line * tracks * 4 + tr * 4
                w0, w1 = be16(data, ptr), be16(data, ptr + 2)
                period = w0 & 0x0FFF
                ins = ((w0 & 0xF000) >> 8) + ((w1 & 0xF000) >> 12)
                eff, ev = (w1 & 0x0F00) >> 8, w1 & 0x00FF
                notes.extend([convert_period(period), imap.get(ins, 0) if ins else 0, eff, ev])

    out = bytearray()
    out += b"nfc!"
    out.append(2)
    out.append(tracks)
    out.append(len(ntp))
    out.append(max_pat)
    out.append(len(order))
    nb = name.encode("latin-1")[:255]
    out.append(len(nb))
    out += nb
    for t in range(1, tracks + 1):
        out.append(STEREO[t - 1])
        out.append(0)  # no stream buffers
        out.append(track_byte[t])
    for n in ntp:
        out.append(n.type_byte())
        out += gs3(n.length)
        out += gs3(n.repeat_pointer)
        out += gs3(n.repeat_length)
        out.append(n.volume)
        out.append(n.finetune)
        out.append(doc_ptr[n.number])
        out.append(doc_sz[n.number])
        ib = n.name.encode("latin-1")[:255]
        out.append(len(ib))
        out += ib
    out += bytes(order)
    out += bytes(notes)
    lines = []
    for n in ntp:
        pos = len(out)
        out += bytes(n.data)
        lines.append(
            f"number={n.number:3d} mod={n.number_original:3d} type=%{n.type_byte():04b} "
            f"len={n.length:5d} doc=${doc_ptr[n.number]*256:04X} osc={n.osc_count()}"
        )
    out_path.write_bytes(out)
    report = [
        f"Module name: {name}",
        f"Signature: {sig}  tracks={tracks}  patterns={max_pat}  order={len(order)}",
        f"NTP bytes: {len(out)}",
        *lines,
        f"DOC pages used: {sum(1 for p in pages if p)} / 256",
        f"Oscillators: {sum(1 for o in oscs if o > 0)} music + timer",
    ]
    return "\n".join(report) + "\n"


def main() -> int:
    src = Path(sys.argv[1] if len(sys.argv) > 1 else "handoff/fly-v9.mod")
    dst = Path(sys.argv[2] if len(sys.argv) > 2 else "fly-v9.ntp")
    print(convert(src, dst), end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
