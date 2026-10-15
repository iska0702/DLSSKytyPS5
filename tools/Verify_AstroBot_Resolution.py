"""Check ASTRO BOT resolution profiles against the unencrypted SELF, without modifying it.

Usage: python tools/Verify_AstroBot_Resolution.py path/to/eboot.bin [path/to/PPSA21567.json]
Both out-of-line setters and the three inlined width/height lookup tables must agree.
"""
import hashlib
import json
from pathlib import Path
import re
import struct
import sys


def verify(source, patch_path):
    data = source.read_bytes()
    patch = json.loads(patch_path.read_text(encoding="utf-8"))
    assert patch["id"] == "PPSA21567" and patch["version"] == "01.018.000"
    expected_hash = patch.get("executable_sha256")
    if expected_hash:
        assert hashlib.sha256(data).hexdigest().lower() == expected_hash.lower(), "Executable SHA-256 mismatch"
    assert data[:4] == bytes.fromhex("5414f5ee"), "Expected SELF container"
    count = struct.unpack_from("<H", data, 24)[0]
    elf = 32 + 32 * count
    assert data[elf:elf + 4] == b"\x7fELF", "Missing ELF header"
    phoff = struct.unpack_from("<Q", data, elf + 32)[0]
    phnum = struct.unpack_from("<H", data, elf + 56)[0]
    segments = []
    for index in range(count):
        flags, offset, size, unpacked = struct.unpack_from("<4Q", data, 32 + 32 * index)
        if not flags & 0x800:
            continue
        assert not flags & 3 and size == unpacked, "Encrypted/compressed segment unsupported"
        phindex = (flags >> 20) & 0xfff
        assert phindex < phnum
        kind, _, _, va, _, filesz, _, _ = struct.unpack_from("<II6Q", data, elf + phoff + 56 * phindex)
        if kind == 1:
            segments.append((va, filesz, offset))

    widths = (0x739e99d, 0x7405d3d, 0x7405dd8, 0x8484994, 0x8714510, 0x87f1e40)
    heights = (0x739e9a3, 0x7405d43, 0x7405dde, 0x8484980, 0x8714524, 0x87f1e54)
    checked = profiles = 0
    for mod in patch["mods"]:
        if not mod["name"].startswith("Fixed render resolution:"):
            continue
        profiles += 1
        width, height = map(int, re.findall(r"\((\d+)x(\d+)\)", mod["name"])[-1])
        writes = {int(w["offset"], 0): w for w in mod["memory"]}
        assert len(writes) == len(mod["memory"]), "Duplicate profile address"
        assert bytes.fromhex(writes[0xc62a97]["on"]) == bytes.fromhex("b80000000090"), "Must select mode zero"
        for address, dimension in [(a, width) for a in widths] + [(a, height) for a in heights]:
            assert bytes.fromhex(writes[address]["on"])[-4:] == struct.pack("<I", dimension), f"Dimension mismatch at {address:#x}"
        for address, write in writes.items():
            before, after = bytes.fromhex(write["off"]), bytes.fromhex(write["on"])
            assert before and len(before) == len(after), "Invalid replacement size"
            va, size, offset = next(s for s in segments if s[0] <= address and address + len(before) <= s[0] + s[1])
            actual = data[offset + address - va:offset + address - va + len(before)]
            assert actual == before, f"Guard mismatch at {address:#x}"
            checked += 1
    assert profiles == 30, "Expected 3 output sizes x 2 technologies x 5 modes"
    enabled = [m["name"] for m in patch["mods"] if m.get("enabled", True) and "render resolution" in m["name"]]
    assert len(enabled) <= 1, "Conflicting enabled resolution profiles"
    print(f"PASS: {profiles} profiles, {checked} byte guards, all setters and lookup tables agree")
    print("Selected:", enabled[0] if enabled else "automatic selection")
    print("Byte verification does not replace a runtime camera/depth extent check.")


if __name__ == "__main__":
    source = Path(sys.argv[1])
    patch_path = Path(sys.argv[2]) if len(sys.argv) > 2 else Path(__file__).resolve().parents[1] / "_Patches/PPSA21567.json"
    verify(source, patch_path)
