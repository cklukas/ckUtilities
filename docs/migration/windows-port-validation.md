---
description: Evidence for the Windows port of CK Utilities, and for the packaging changes it brought to every platform.
---

# Windows port validation

CK Utilities builds, tests and packages on Windows, alongside macOS and Linux.

## What changed

- **Entry points.** Every tool's `main` runs on the platform's own terminal,
  clock and filesystem (`ck::host::Native*`), with UTF-8 arguments from
  `wmain` on Windows. All nine executables are built on every platform.
- **UTF-8 everywhere.** Every executable embeds a manifest that makes UTF-8
  its process code page (Windows 10 1903 and later), so paths and text keep
  non-ASCII characters exactly as on macOS and Linux.
- **Launcher.** Starting a sibling tool in the same console and waiting for it
  is `run_foreground`: `fork`/`execve` on POSIX, `CreateProcessW` on Windows.
  The tool folder comes from the launcher's own module path on Windows.
- **Disk Usage.** File metadata goes through one record read with `lstat`
  on POSIX and the Win32 file-information and security APIs on Windows:
  allocated size, volume and file id (hard links, "stay on this drive"),
  symbolic links and junctions as links (cloud placeholders are not), creation
  time, owner and group names. Ignore masks use one portable matcher with
  `fnmatch` semantics on every platform. On macOS, "Created" is now the file's
  creation time, where it was the last status change before.
- **AI core.** Model downloads use WinHTTP on Windows and libcurl elsewhere
  behind one interface; configuration lives in `%APPDATA%\cktools`, models
  and prompts in `%LOCALAPPDATA%\cktools` (POSIX paths unchanged).
- **llama.cpp** is built as C++17 (its own standard) and statically, so every
  tool is one self-contained executable; googletest follows.
- **Packages** contain the product only: the executables, documentation,
  license and sample configuration. Development headers and libraries, ours
  and llama.cpp's, are no longer packaged. The archive verifier rejects any
  `include/` or `lib/`, and a new check requires the DEB and RPM to install at
  `/usr/bin` and `/usr/share`.

Find's custom-command action stays macOS-only (it runs under the macOS
sandbox), as on Linux; its command preview shows the equivalent POSIX `find`
line on every platform.

## Windows

Windows 11 (build 26300) x64 workstation, Visual Studio 2022 Build Tools
17.14, MSVC 14.44.35207, bundled CMake and Ninja, Release, static C runtime,
ckVision v1.3.0 SDK (`57b379d`) built the same way.

- `scripts/windows/build-and-package.ps1` from a fresh work root: SDK, build
  with 0 project warnings, CTest 115/115, ZIP packaged, extracted, and every
  tool started from it.
- The ConPTY contract started all nine tools in a 100x30 pseudo-console with
  their user folders in a non-ASCII scratch path, saw each draw, and quit each
  with Alt+X (exit 0). A mutant that starts ck-todo without `--demo`, so its
  modal welcome blocks the chord, fails it.
- `ck-sysmon --sample` reported the CPU, memory, both GeForce GTX 1080 Ti
  GPUs and the running Ollama server's loaded model.

## Linux and macOS

- Linux (Debian 13 aarch64 VM, GCC 14.2.0, Debug): 0 project warnings,
  CTest 115/115, `ck-chat` without shared llama/ggml libraries. Release
  packages: rehearsal, archive, terminal and DEB/RPM layout checks passed; the
  layout check rejects the packages a top-level folder had broken during the
  change.
- macOS (Apple Clang 21, Debug): CTest 116/116; rehearsal, archive and
  terminal verification passed with the product-only archive.

Not run: sanitizers; GitHub Actions (the new Windows job runs the same script).
