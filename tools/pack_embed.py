#!/usr/bin/env python3
"""Pack a <machine>/<file> tree into one blob for the embed: device
(core/src/embed_fs.c), which lists it as embed:/gemu/<machine>/<file>.

    tools/pack_embed.py DIR OUT

Format (big-endian): "GEMB", u32 count, then per file u32 name offset,
u32 data offset, u32 size (offsets from the blob start); names are
NUL-terminated "gemu/<machine>/<file>", data 32-byte aligned.  OUT is only
rewritten when it changes, so make doesn't rebuild for nothing.
"""
import os, struct, sys


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    root, out = sys.argv[1:]
    files = []
    for m in sorted(os.listdir(root)):
        d = os.path.join(root, m)
        if not os.path.isdir(d):
            continue
        for f in sorted(os.listdir(d)):
            p = os.path.join(d, f)
            if os.path.isfile(p) and not f.startswith("."):
                files.append((f"gemu/{m}/{f}".encode(), open(p, "rb").read()))

    head = 8 + 12 * len(files)
    names = b"".join(n + b"\0" for n, _ in files)
    pos = (head + len(names) + 31) & ~31
    table, blobs, noff = [], b"", head
    for n, data in files:
        table.append(struct.pack(">III", noff, pos + len(blobs), len(data)))
        noff += len(n) + 1
        blobs += data + b"\0" * (-len(data) % 32)
    pack = b"GEMB" + struct.pack(">I", len(files)) + b"".join(table) + names
    pack += b"\0" * (pos - len(pack)) + blobs

    if not (os.path.exists(out) and open(out, "rb").read() == pack):
        open(out, "wb").write(pack)
        print(f"embedded {len(files)} file(s), {len(pack) // 1024} KB")


if __name__ == "__main__":
    main()
