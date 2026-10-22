---
description: Evidence for the move of ck-todo and ck-sysmon from ckVision into CK Utilities, on macOS and Linux.
---

# ck-todo and ck-sysmon move validation

Commit `3be09b5` moved ckVision's TODO and system-monitor examples into CK
Utilities as `ck-todo` and `ck-sysmon` (ckVision decision D-151). The sources
are the ckVision v1.3.0 tag's bytes, relicensed to GPL-3.0-or-later and moved
into the `ck::` namespace. ckVision removed them in `c76786f`.

## macOS

macOS arm64, Apple Clang 21, CMake 4.4.4, ckVision v1.3.0 SDK built Release,
suite built from a fresh `dev` preset (Debug):

- Build: 0 project warnings; the moved code builds with warnings as errors.
- CTest: 114/114 passed (97 before the move).
- `verify_ckvision_rehearsal` and `verify_ckvision_terminal`: passed for all
  nine executables in xterm-256color, xterm and vt100. The terminal check
  starts ck-todo with `--demo`, because its first-run welcome dialog is
  modal and ckVision suppresses the quit chord while a modal is open.
- Regenerating every screenshot and both tools' golden frames: byte-identical
  for the moved files; the launcher screenshot gained exactly the Todo and
  System Monitor rows.

## Linux

The `git archive` of `3be09b5` in a Debian 13 aarch64 VM, against the same
ckVision v1.3.0 SDK:

- GCC 14.2.0, `dev` preset (Debug): configure, build with 0 project warnings,
  `verify_ckvision_package`, and CTest 113/113 (the macOS-only Apple GPU
  suite is not built).
- Regenerating every screenshot and golden from the Linux binaries left the
  tree unchanged, so the goldens are platform-independent.
- Clang 19.1.7 with `CKTOOLS_BUILD_FUZZERS=ON`: `fuzz_todo_codec` and
  `fuzz_sysmon` built and ran their corpora, 2/2 passed.

Not run: Windows, sanitizers.
