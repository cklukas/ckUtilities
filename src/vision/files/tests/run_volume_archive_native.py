# Copyright (c) 2026 C. Klukas. All rights reserved.
# SPDX-License-Identifier: GPL-3.0-or-later
"""Run an archive contract in a fresh workspace with its qualified deadline."""
from pathlib import Path
import os
import subprocess
import sys
import tempfile

binary, scratch, seconds, *arguments = sys.argv[1:]
parent = Path(scratch)
if not parent.is_absolute() or not parent.is_dir():
    raise ValueError("archive fixture needs an existing absolute scratch root")
deadline = float(seconds)
if not 0 < deadline <= 40:
    raise ValueError("archive fixture deadline exceeds its qualified bound")
with tempfile.TemporaryDirectory(prefix="archive-native-", dir=parent) as work:
    env = dict(os.environ, TMPDIR=work, TEMP=work, TMP=work)
    subprocess.run([binary, work, *arguments], check=True, env=env, timeout=deadline)
