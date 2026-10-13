"""Verify every Astro Bot patch guard against the supplied unencrypted SELF."""
import hashlib
import json
from pathlib import Path
import struct
import sys

source = Path(sys.argv[1])
patch_path = Path(sys.argv[2]) if len(sys.argv) > 2 else Path(__file__).resolve().parents[1] / '_Patches/PPSA23060.json'
data = source.read_bytes()
patch = json.loads(patch_path.read_text(encoding='utf-8'))
assert hashlib.sha256(data).hexdigest() == patch['executable_sha256'], 'Executable SHA-256 mismatch'
assert data[:4] == bytes.fromhex('5414f5ee'), 'Expected SELF container'
count = struct.unpack_from('<H', data, 24)[0]
elf = 32 + 32 * count
assert data[elf:elf + 4] == b'\x7fELF', 'Missing ELF header'
phoff = struct.unpack_from('<Q', data, elf + 32)[0]
phnum = struct.unpack_from('<H', data, elf + 56)[0]
segments = []
for index in range(count):
    flags, offset, size, unpacked = struct.unpack_from('<4Q', data, 32 + 32 * index)
    if not flags & 0x800:
        continue
    assert not flags & 3 and size == unpacked, 'Encrypted/compressed segment is unsupported'
    phindex = (flags >> 20) & 0xfff
    assert phindex < phnum
    kind, perms, _, va, _, filesz, _, _ = struct.unpack_from('<II6Q', data, elf + phoff + 56 * phindex)
    if kind == 1:
        segments.append((va, filesz, offset, perms))

checked = 0
for mod in patch['mods']:
    for write in mod['memory']:
        addr = int(write['offset'], 0)
        before, after = bytes.fromhex(write['off']), bytes.fromhex(write['on'])
        assert before and len(before) == len(after), 'Unequal replacement size'
        match = next((s for s in segments if s[0] <= addr and addr + len(before) <= s[0] + s[1]), None)
        assert match is not None, f'Write outside file-backed segment: {addr:#x}'
        va, _, offset, perms = match
        assert perms & 1, 'Expected executable segment'
        assert data[offset + addr - va:offset + addr - va + len(before)] == before, f'Guard mismatch: {addr:#x}'
        checked += 1
print(f'PASS: SHA-256 and {checked} byte guards across {len(patch["mods"])} profiles')
