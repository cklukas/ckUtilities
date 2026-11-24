# Copyright (c) 2026 C. Klukas. All rights reserved.
# SPDX-License-Identifier: GPL-3.0-or-later
"""Run an archive contract in a fresh workspace with its qualified deadline."""
from pathlib import Path
import os
import signal
import subprocess
import sys
import tempfile
import time

binary, scratch, seconds, *arguments = sys.argv[1:]
parent = Path(scratch)
if not parent.is_absolute() or not parent.is_dir():
    raise ValueError("archive fixture needs an existing absolute scratch root")
deadline = float(seconds)
if not 0 < deadline <= 40:
    raise ValueError("archive fixture deadline exceeds its qualified bound")
with tempfile.TemporaryDirectory(prefix="archive-native-", dir=parent) as work:
    env = dict(os.environ, TMPDIR=work, TEMP=work, TMP=work)
    command = [binary, work, *arguments]
    if len(arguments) == 1 and arguments[0].startswith("crash-"):
        end = time.monotonic() + deadline
        writer = subprocess.run(command, env=env, timeout=deadline, stderr=subprocess.PIPE)
        sys.stderr.buffer.write(writer.stderr)
        marker = f"Interrupting archive at {arguments[0]}\n".encode()
        if writer.returncode != -signal.SIGKILL or marker not in writer.stderr.splitlines(keepends=True):
            raise RuntimeError(f"Archive writer missed the crash boundary: exit {writer.returncode}")
        subprocess.run([*command, "inspect"], check=True, env=env, timeout=end - time.monotonic())
    else:
        subprocess.run(command, check=True, env=env, timeout=deadline)
