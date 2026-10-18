# Montserrat subset source

Copyright 2011 The Montserrat Project Authors. SIL Open Font License 1.1,
preserved in `OFL.txt`. Source is the normal variable Montserrat font at
upstream commit `cc8daf2e7085006b9c112542fc82b58afc13521d`, identified in
Google Fonts metadata and downloaded on 2026-10-10.

SHA-256: `1e2c44ef8c956ed66998649d8c832d221e22c04eaab63c044f18eb05d1c63b7c`.
`scripts/build_fonts.py` uses Pillow 12.3.0 to create committed LVGL glyphs
at weight 500. The full TTF is a build source and is never embedded in firmware.
Text roles: 12/14/18 px, ASCII, Latin-1 and selected Latin Extended-A plus common
punctuation. Score role: 48 px, digits and score punctuation. Unknown glyphs
use a visible question mark. The same 4-bpp alpha pixels are stored losslessly
with run-length encoding. One public LVGL bitmap callback writes directly into
LVGL's provided glyph target, with checked source/target bounds and no font
heap allocation, extra line buffer or external download. Native compressed
fonts stay disabled; no managed LVGL sources are patched.

Regenerate with a Python interpreter that already has the pinned Pillow 12.3.0:

```bash
python3 scripts/build_fonts.py
python3 scripts/tests/test_font_bitmaps.py
```

The generator verifies the original TTF SHA-256 above before writing. The
focused test checks the original plain bitmap hashes and every character's
metrics, then compares the production callback with the installed LVGL plain
reader for all 691 nonempty original glyphs and the existing fallbacks. Normal
firmware builds consume the committed data and do not run the generator.

When the pinned project formatter is available in `.venv-tools/`, the font
builder formats the generated C files. Otherwise run the project formatter
before committing regenerated files. Normal firmware builds do not run it.
