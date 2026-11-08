#!/usr/bin/env python3
"""Compare two tests/a26_exact runs: REF and OUT prefixes.

    python3 tools/a26_compare.py REF OUT NAME

Frames and audio must match exactly up to the shorter run (the lazy TIA
finishes its last frame / samples a little later than upstream's eager one).
"""
import sys

ref, out, name = sys.argv[1:4]
fa, fb = open(ref + ".frames", "rb").read(), open(out + ".frames", "rb").read()
aa, ab = open(ref + ".audio", "rb").read(), open(out + ".audio", "rb").read()
nf, na = min(len(fa), len(fb)) // 8, min(len(aa), len(ab)) // 4
ok = True
for i in range(nf):
    if fa[i * 8:i * 8 + 8] != fb[i * 8:i * 8 + 8]:
        print(f"FAIL {name}: frame {i} differs"); ok = False; break
if aa[:na * 4] != ab[:na * 4]:
    first = next(i for i in range(na) if aa[i * 4:i * 4 + 4] != ab[i * 4:i * 4 + 4])
    print(f"FAIL {name}: audio sample {first} differs"); ok = False
short = abs(len(fa) - len(fb)) // 8 > 2 or abs(len(aa) - len(ab)) // 4 > 4096
if short:
    print(f"FAIL {name}: runs differ in length ({len(fa)//8} vs {len(fb)//8} frames)"); ok = False
if ok:
    print(f"ok   {name}: {nf} frames, {na} audio samples identical")
sys.exit(0 if ok else 1)
