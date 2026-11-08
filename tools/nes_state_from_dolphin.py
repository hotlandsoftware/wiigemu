#!/usr/bin/env python3
"""Lift the NES machine's state out of a Dolphin save state, for measuring a
slow scene with the autotest build.

    tools/nes_state_from_dolphin.py STATE.sNN ROM.nes OUT.nesstate [DOL]

STATE is a Dolphin save (e.g. .dolphin/StateSaves/ID-gemu.s01) taken while
GEMU was running ROM; DOL is the build that was running (default gemu.dol),
used to find the Wii's RAM in the state.  OUT is the raw NesState (Wii
layout).  Put it next to the ROM and run, e.g.:

    tools/run-dolphin.sh --autotest=2040 roms/nes/smb.nes OUT.nesstate

The autotest build resumes from <rom>.nesstate (machine_nes.c,
load_test_state), so the timing overlay shows that scene.  The state must
come from a build with the same NesState layout.

Dolphin's state here: a header, then one LZ4 block (uncompressed size at
0x34) holding the memory images among the rest.
"""
import os, struct, subprocess, sys, tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def lz4_block(d, p):
    out = bytearray()
    end = len(d)
    while p < end:
        tok = d[p]; p += 1
        lit = tok >> 4
        if lit == 15:
            while True:
                b = d[p]; p += 1; lit += b
                if b != 255: break
        out += d[p:p + lit]; p += lit
        if p >= end: break
        off = d[p] | d[p + 1] << 8; p += 2
        ml = tok & 15
        if ml == 15:
            while True:
                b = d[p]; p += 1; ml += b
                if b != 255: break
        ml += 4
        start = len(out) - off
        while ml > 0:
            n = min(off, ml); out += out[start:start + n]; ml -= n; start += n
    return bytes(out)


def nes_layout():
    """offsetof(NesState, prg / cart) and sizeof(NesState) on the Wii."""
    gcc = os.path.join(os.environ.get("DEVKITPPC", "/opt/devkitpro/devkitPPC"), "bin", "powerpc-eabi-gcc")
    objdump = gcc.replace("gcc", "objdump")
    src = ('#include "nes.h"\n#include <stddef.h>\n'
           'const unsigned nes_off[3] = { offsetof(NesState, prg), sizeof(NesState),'
           ' offsetof(NesState, cart) };\n')
    with tempfile.TemporaryDirectory() as t:
        c, o = os.path.join(t, "off.c"), os.path.join(t, "off.o")
        open(c, "w").write(src)
        inc = [f"-I{ROOT}/{d}" for d in ("compat", "core/include", "cpu/mos", "vga/mos", "audio",
                                         "hardware", "hardware/mos", "hardware/device")]
        subprocess.run([gcc, "-c", "-O2", "-mcpu=750", "-meabi", "-mhard-float", *inc, c, "-o", o], check=True)
        dump = subprocess.run([objdump, "-s", "-j", ".rodata", o], check=True,
                              capture_output=True, text=True).stdout
    words = dump.strip().splitlines()[-1].split()[1:4]
    return [int(w, 16) for w in words]


def main():
    if len(sys.argv) < 4:
        sys.exit(__doc__)
    state, rom_path, out = sys.argv[1:4]
    dol = open(sys.argv[4] if len(sys.argv) > 4 else os.path.join(ROOT, "gemu.dol"), "rb").read()
    d = open(state, "rb").read()
    raw = lz4_block(d, 0x40)
    if len(raw) != struct.unpack_from("<I", d, 0x34)[0]:
        sys.exit("unexpected state format")

    # MEM1 (0x80000000) located by the DOL's first text section
    toff, taddr = struct.unpack_from(">I", dol, 0)[0], struct.unpack_from(">I", dol, 0x48)[0]
    t = raw.find(dol[toff:toff + 64])
    if t < 0:
        sys.exit("the DOL's code isn't in the state: pass the build that was running")
    mem1 = t - (taddr - 0x80000000)

    off_prg, size, off_cart = nes_layout()
    rom = open(rom_path, "rb").read()
    prg_banks, chr_banks = rom[4], rom[5]
    mapper = (rom[7] & 0xF0) | (rom[6] >> 4)
    prg = rom[16 + (512 if rom[6] & 4 else 0):][:4096]
    at = raw.find(prg)
    while at >= 0:                       # every copy; NesState.prg points at one
        ptr = struct.pack(">I", 0x80000000 + at - mem1)
        h = raw.find(ptr)
        while h >= 0:
            st = h - off_prg
            cart = raw[st + off_cart:st + off_cart + 3]
            if st >= 0 and h % 4 == (mem1 % 4) and tuple(cart) == (prg_banks, chr_banks, mapper):
                open(out, "wb").write(raw[st:st + size])
                pc = struct.unpack_from(">H", raw, st + 4)[0]
                print(f"NesState at 0x{0x80000000 + st - mem1:08X}, PC=${pc:04X} -> {out}")
                return
            h = raw.find(ptr, h + 1)
        at = raw.find(prg, at + 1)
    sys.exit("no NES machine state found for that ROM")


if __name__ == "__main__":
    main()
