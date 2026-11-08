#!/usr/bin/env python3
"""Assemble GEMU for Wii's built-in Atari 2600 test cart (4K) into
data/machine/atari2600/testcart.a26.  Pure stdlib: a tiny 6502 assembler
for just the instructions used here.

The cart draws scrolling rainbow bars and exercises every input:
  joystick left/right  scroll the colours (player 1)
  joystick up/down     scroll faster / stop
  fire                 tone on channel 0 while held
  Game Reset           recentre (offset 0)
  Game Select          reverse the scroll direction
  Color/B&W switch     grey bars in B&W
  right joystick fire  tone on channel 1 (player 2)

    python3 tools/make_a26_testcart.py [out.a26]
"""
import sys

out = sys.argv[1] if len(sys.argv) > 1 else "data/machine/atari2600/testcart.a26"

# TIA / RIOT registers
VSYNC, VBLANK, WSYNC, COLUBK = 0x00, 0x01, 0x02, 0x09
AUDC0, AUDC1, AUDF0, AUDF1, AUDV0, AUDV1 = 0x15, 0x16, 0x17, 0x18, 0x19, 0x1A
INPT4, INPT5 = 0x0C, 0x0D
SWCHA, SWCHB = 0x0280, 0x0282
# RAM
OFFSET, MASK, STEP, SPEED, PREVB = 0x80, 0x81, 0x82, 0x83, 0x84

code, labels, fixups = bytearray(), {}, []
ORG = 0xF000

def label(n): labels[n] = ORG + len(code)
def emit(*b): code.extend(b)
def zp(op, a):  emit(op, a & 0xFF)
def ab(op, a):  emit(op, a & 0xFF, a >> 8)
def br(op, n):  emit(op, 0); fixups.append(("rel", len(code) - 1, n))
def jmp(n):     emit(0x4C, 0, 0); fixups.append(("abs", len(code) - 2, n))

LDA_IMM, LDX_IMM, STA_ZP, STX_ZP, LDA_ZP, LDA_ABS = 0xA9, 0xA2, 0x85, 0x86, 0xA5, 0xAD
AND_IMM, AND_ZP, ADC_ZP, CMP_IMM, EOR_IMM, EOR_ZP = 0x29, 0x25, 0x65, 0xC9, 0x49, 0x45
BIT_ZP, INC_ZP = 0x24, 0xE6
BNE, BEQ, BMI, BPL, BCS, BCC = 0xD0, 0xF0, 0x30, 0x10, 0xB0, 0x90

label("start")
emit(0x78, 0xD8)                    # SEI, CLD
emit(LDX_IMM, 0xFF, 0x9A)           # LDX #$FF / TXS
emit(LDA_IMM, 0x00)
label("clear")
emit(0x95, 0x00, 0xCA)              # STA 0,X / DEX
br(BNE, "clear")
emit(LDA_IMM, 0x04); zp(STA_ZP, AUDC0)     # pure tones
emit(LDA_IMM, 0x0C); zp(STA_ZP, AUDC1)
emit(LDA_IMM, 0x0F); zp(STA_ZP, AUDF0)
emit(LDA_IMM, 0x0A); zp(STA_ZP, AUDF1)
emit(LDA_IMM, 0x01); zp(STA_ZP, STEP); zp(STA_ZP, SPEED)

label("frame")
emit(LDA_IMM, 0x02); zp(STA_ZP, VSYNC); zp(STA_ZP, VBLANK)
zp(STA_ZP, WSYNC); zp(STA_ZP, WSYNC); zp(STA_ZP, WSYNC)
emit(LDA_IMM, 0x00); zp(STA_ZP, VSYNC)

# --- logic (in vertical blank) ---
ab(LDA_ABS, SWCHA)                  # P0: bit7 right, 6 left, 5 down, 4 up (0 = pressed)
emit(0x0A); br(BCS, "noright")      # ASL -> C = right
zp(INC_ZP, OFFSET); zp(INC_ZP, OFFSET)
label("noright")
emit(0x0A); br(BCS, "noleft")       # left
emit(0xC6, OFFSET, 0xC6, OFFSET)    # DEC, DEC
label("noleft")
emit(0x0A); br(BCS, "nodown")       # down: stop
emit(LDA_IMM, 0x00); zp(STA_ZP, SPEED)
jmp("dirdone")
label("nodown")
emit(0x0A); br(BCS, "dirdone")      # up: fast
emit(LDA_IMM, 0x04); zp(STA_ZP, SPEED)
label("dirdone")

ab(LDA_ABS, SWCHB)                  # bit0 reset, bit1 select, bit3 colour (0 = pressed / B&W)
emit(0x4A); br(BCS, "noreset")      # LSR -> C = reset
emit(LDA_IMM, 0x00); zp(STA_ZP, OFFSET)
emit(LDA_IMM, 0x01); zp(STA_ZP, SPEED)
label("noreset")
ab(LDA_ABS, SWCHB); emit(AND_IMM, 0x02)    # select: on the press edge, negate STEP
br(BNE, "selup")
zp(LDA_ZP, PREVB); br(BNE, "seldone")      # already handled
emit(LDA_IMM, 0xFE); zp(EOR_ZP, STEP); zp(STA_ZP, STEP)   # 1 <-> $FF
emit(LDA_IMM, 0x01); zp(STA_ZP, PREVB)
jmp("seldone")
label("selup")
emit(LDA_IMM, 0x00); zp(STA_ZP, PREVB)
label("seldone")

ab(LDA_ABS, SWCHB); emit(AND_IMM, 0x08)    # colour mask
br(BEQ, "bw")
emit(LDA_IMM, 0xFE); jmp("maskset")
label("bw")
emit(LDA_IMM, 0x0E)
label("maskset")
zp(STA_ZP, MASK)

# scroll: OFFSET += STEP * SPEED (SPEED 0, 1 or 4 times)
emit(LDX_IMM, 0x00)
label("spd")
emit(0xE4, SPEED); br(BEQ, "spddone")      # CPX SPEED
emit(0x18); zp(LDA_ZP, OFFSET); zp(ADC_ZP, STEP); zp(STA_ZP, OFFSET)
emit(0xE8); jmp("spd")                     # INX
label("spddone")

emit(LDA_IMM, 0x00); zp(BIT_ZP, INPT4); br(BMI, "f0")   # P0 fire: bit7 = 0 pressed
emit(LDA_IMM, 0x08)
label("f0"); zp(STA_ZP, AUDV0)
emit(LDA_IMM, 0x00); zp(BIT_ZP, INPT5); br(BMI, "f1")   # P1 fire
emit(LDA_IMM, 0x08)
label("f1"); zp(STA_ZP, AUDV1)

emit(LDX_IMM, 33)                           # rest of the 37 vblank lines
label("vb"); zp(STA_ZP, WSYNC); emit(0xCA); br(BNE, "vb")
zp(STA_ZP, WSYNC); emit(LDA_IMM, 0x00); zp(STA_ZP, VBLANK)

# --- 192 visible lines: one colour per 2 lines ---
emit(LDX_IMM, 192)
label("line")
emit(0x8A, 0x4A, 0x18); zp(ADC_ZP, OFFSET)   # TXA / LSR / CLC / ADC OFFSET
zp(AND_ZP, MASK)
zp(STA_ZP, WSYNC); zp(STA_ZP, COLUBK)
emit(0xCA); br(BNE, "line")

emit(LDA_IMM, 0x02); zp(STA_ZP, WSYNC); zp(STA_ZP, VBLANK)
emit(LDA_IMM, 0x00); zp(STA_ZP, COLUBK)
emit(LDX_IMM, 30)
label("os"); zp(STA_ZP, WSYNC); emit(0xCA); br(BNE, "os")
jmp("frame")

for kind, pos, name in fixups:
    target = labels[name]
    if kind == "rel":
        d = target - (ORG + pos + 1)
        assert -128 <= d <= 127, (name, d)
        code[pos] = d & 0xFF
    else:
        code[pos], code[pos + 1] = target & 0xFF, target >> 8

rom = bytearray([0xFF]) * 4096
rom[:len(code)] = code
start = labels["start"]
rom[0xFFC:0xFFE] = bytes([start & 0xFF, start >> 8])    # RESET
rom[0xFFE:0x1000] = bytes([start & 0xFF, start >> 8])   # IRQ/BRK
open(out, "wb").write(rom)
print(f"wrote {out} ({len(code)} bytes of code)")
