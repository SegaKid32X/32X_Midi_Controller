#!/usr/bin/env python3
"""Build patch files into a test ROM (the emulator has no SD card).

Appends a directory and the files after the program, inside the first
512 KiB, and writes "PATFS@" + the offset into the ROM header at 0x1C8
(src/md/md_vfs.c reads it).

usage: mkvfs.py ROM_IN ROM_OUT PATCH_DIR [DRUM_DIR]
(drum WAVs are stored as "!drums/<name>", where src/md/md_drums.c looks)
"""
import struct
import sys
from pathlib import Path


def main() -> int:
    if len(sys.argv) not in (4, 5):
        print(__doc__)
        return 1
    rom = bytearray(Path(sys.argv[1]).read_bytes())
    root = Path(sys.argv[3])
    files = sorted(p for p in root.rglob("*")     # folders too: "Bass/Slap.tfi"
                   if p.is_file() and p.suffix.lower() in (".tfi", ".dmp", ".y12"))
    entries = [(f.relative_to(root).as_posix(), f) for f in files]
    if len(sys.argv) == 5:
        entries += [("!drums/" + f.name, f) for f in sorted(Path(sys.argv[4]).glob("*.wav"))]
    at = (len(rom) + 15) & ~15
    rom += b"\0" * (at - len(rom))
    head = bytearray(b"PATFS1\0\0" + struct.pack(">I", len(entries)))
    data_at = at + len(head) + 40 * len(entries)
    body = bytearray()
    for rel, f in entries:
        rel = rel.encode("ascii")
        if len(rel) > 31:
            sys.exit(f"mkvfs: {rel!r} is too long (31 characters at most)")
        name = rel.ljust(32, b"\0")
        blob = f.read_bytes()
        while (data_at + len(body)) % 4:
            body += b"\0"
        head += name + struct.pack(">II", data_at + len(body), len(blob))
        body += blob
    rom += head + body
    if len(rom) > 0x80000:
        sys.exit("mkvfs: the patches must end inside the first 512 KiB")
    rom[0x1C8:0x1CE] = b"PATFS@"
    rom[0x1CE:0x1D2] = struct.pack(">I", at)
    Path(sys.argv[2]).write_bytes(rom)
    print(f"mkvfs: {len(entries)} file(s) at {at:#x}, ROM {len(rom):,} bytes")
    return 0


if __name__ == "__main__":
    sys.exit(main())
