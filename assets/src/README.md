# Stock artwork

The `default_*.png` files are original generic football/pitch artwork generated
by `scripts/build_assets.py` on 2026-10-10. They contain no club crest or third
party artwork. The stock sources may be modified for this project. The owner
is responsible for the rights to replacement images and club logos.

High-resolution sources remain here. The script reads the target's actual
display header and converts them to baseline JPEG in `data/`. Firmware OTA
preserves LittleFS; USB `uploadfs` replaces its content and can lose user files.
