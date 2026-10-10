# Montserrat subset source

Copyright 2011 The Montserrat Project Authors. SIL Open Font License 1.1,
preserved in `OFL.txt`. Source is the normal variable Montserrat font at
upstream commit `cc8daf2e7085006b9c112542fc82b58afc13521d`, identified in
Google Fonts metadata and downloaded on 2026-10-10.

SHA-256: `1e2c44ef8c956ed66998649d8c832d221e22c04eaab63c044f18eb05d1c63b7c`.
`scripts/build_fonts.py` uses Pillow 12.3.0 to create committed LVGL glyphs
at weight 500. The full TTF is a build source and is never embedded in firmware.
Text roles: 10/14/18 px, ASCII, Latin-1 and selected Latin Extended-A plus common
punctuation. Score role: 28 px, digits and score punctuation. Unknown glyphs
use a visible question mark. No dynamic font allocation or external download.

When the pinned project formatter is available in `.venv-tools/`, the font
builder formats the generated C files. Otherwise run the project formatter
before committing regenerated files. Normal firmware builds do not run it.
