# ASTRO BOT 01.018: DLSS / FSR resolution profiles

Choose exactly one resolution profile in the launcher patch dialog. The output window size is configured separately. The patch changes actual guest render dimensions, including three setter copies and three pairs of inlined lookup tables. DLSS/FSR quality mode and native AA must also be selected in the renderer settings where applicable. These profiles describe resolution modes, not NVIDIA model presets J/K/L/M.

Dimensions use nominal quality ratios aligned to four pixels horizontally and two vertically for this game. DLSS Balanced uses 58%; FSR Balanced uses 1/1.7. Other modes use 1, 1/1.5, 1/2 or 1/3. These are fixed profiles, not SDK optimal-size queries.

| Output | Technology and mode | Internal render |
| --- | --- | --- |
| Full HD | DLSS Native DLAA | 1920x1080 |
| Full HD | DLSS Quality | 1280x720 |
| Full HD | DLSS Balanced | 1112x626 |
| Full HD | DLSS Performance | 960x540 |
| Full HD | DLSS Ultra Performance | 640x360 |
| Full HD | FSR Native AA | 1920x1080 |
| Full HD | FSR Quality | 1280x720 |
| Full HD | FSR Balanced | 1128x636 |
| Full HD | FSR Performance | 960x540 |
| Full HD | FSR Ultra Performance | 640x360 |
| QHD | DLSS Native DLAA | 2560x1440 |
| QHD | DLSS Quality | 1708x960 |
| QHD | DLSS Balanced | 1484x836 |
| QHD | DLSS Performance | 1280x720 |
| QHD | DLSS Ultra Performance | 852x480 |
| QHD | FSR Native AA | 2560x1440 |
| QHD | FSR Quality | 1708x960 |
| QHD | FSR Balanced | 1504x848 |
| QHD | FSR Performance | 1280x720 |
| QHD | FSR Ultra Performance | 852x480 |
| 4K | DLSS Native DLAA | 3840x2160 |
| 4K | DLSS Quality | 2560x1440 |
| 4K | DLSS Balanced | 2228x1252 |
| 4K | DLSS Performance | 1920x1080 |
| 4K | DLSS Ultra Performance | 1280x720 |
| 4K | FSR Native AA | 3840x2160 |
| 4K | FSR Quality | 2560x1440 |
| 4K | FSR Balanced | 2260x1270 |
| 4K | FSR Performance | 1920x1080 |
| 4K | FSR Ultra Performance | 1280x720 |

QHD here means 2560?1440 (often called 2K in settings); Full HD is 1920?1080; 4K is 3840?2160. Only one profile may be enabled. Verified gameplay: 1708?960 ? 2560?1440 with DLSS and FSR; all 390 byte guards across 30 profiles match the supplied executable. Other sizes need gameplay and memory-budget validation. Frame generation multipliers are independent of render resolution.

Run `python tools/Verify_AstroBot_Resolution.py path/to/eboot.bin path/to/PPSA21567.json` to verify the patch against your unencrypted SELF. The game executable and game assets are not part of this repository or release.
