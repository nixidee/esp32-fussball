#!/usr/bin/env python3
"""Whole production PWM setter with the existing backlight profile and SDK faults."""
from pathlib import Path
import argparse
import subprocess
import tempfile
from test_runtime_readiness import function

ROOT = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--source', type=Path, default=ROOT / 'components/display/display_port.cpp')
args = parser.parse_args()
with tempfile.TemporaryDirectory(prefix='football-pwm-') as folder:
    scratch = Path(folder)
    (scratch / 'sdkconfig.h').write_text('#pragma once\n#define CONFIG_IDF_TARGET_ESP32S3 1\n')
    (scratch / 'backlight_pwm.inc').write_text(function(args.source.read_text(), 'void setBrightness('))
    command = ['clang++', '-std=c++20', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
               '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
               '-I', str(scratch), '-iquote', str(ROOT), '-I', str(ROOT / 'include'),
               str(ROOT / 'scripts/tests/fixtures/backlight_pwm_host.cpp'),
               '-o', str(scratch / 'check')]
    subprocess.run(command, check=True)
    subprocess.run([str(scratch / 'check')], check=True)
