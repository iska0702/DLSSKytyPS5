# Astro Bot executable-specific patch

`_Patches/PPSA23060.json` targets the supplied unencrypted `eboot.bin`:

- Embedded title ID: `PPSA23060`.
- SHA-256: `c15d0f5c6b476cd4653b20b25b8bc954f02bbeca8fe5a4f22f055df894a7b0f8`.
- APP_VER is unknown: no `sce_sys/param.json` or `param.sfo` was supplied.

The version field is `*`, accepted only by the updated loader with an exact
`executable_sha256` match. Title ID and process still must match. Older builds
reject this patch instead of applying it to an unverified version. The binary
itself is not modified or distributed.

The existing PPSA21567 patch's renderer/GI and resolution locations match this
executable byte-for-byte. All 102 replacement guards were checked against mapped
SELF segments, and original/replacement instruction boundaries were inspected.
This establishes binary compatibility, not successful gameplay.

All profiles are disabled by default. With DLSS enabled, the loader selects the
closest resolution profile; DLAA selects a native profile. Optional non-tiled
deferred rendering and GI-disable profiles can be enabled separately if needed.
No experimental timing overrides, frame-cap hooks or code caves are included.

Use a complete game installation with its original metadata. This patch cannot
make an isolated `eboot.bin` boot. Astro Bot depth/motion producers have not been
validated in a running scene; Playroom's verified DLSS guide path is not proof of
Astro Bot support. Without verified guides the current DLSS path is bypassed.

Verify locally with Python:

```powershell
python tools/Verify_AstroBot_Patch.py "D:\DLSSEmulatorPS5\eboot.bin"
```
