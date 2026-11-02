# Windows file-I/O audit

Audit date: 2026-11-02

The project policy is recorded in the repository `AGENTS.md`. Windows path
boundaries must use wide Win32/native-handle APIs with validated extended
length paths, or handle-relative native APIs. Narrow Win32 calls and legacy
`MAX_PATH` assumptions are not acceptable for product I/O.

## ckUtilities

The current product sources were audited for ANSI Win32 calls. There are no
production uses of `CreateFileA`, `FindFirstFileA`, `FindNextFileA`,
`GetFileAttributesA`, `DeleteFileA`, `MoveFileA`, `CopyFileA`,
`CreateProcessA`, or `GetModuleFileNameA`.

The Windows-specific boundaries now use extended paths for native metadata,
directory, journal, archive, disk-usage, volume-information, launcher, host
capture, and keymap operations. The recycle-bin volume-path query uses a
dynamically sized wide buffer. Windows search and disk-usage UI path buffers
are sized for the extended Windows path contract rather than the platform's
legacy `PATH_MAX` fallback.

The remaining ordinary C++ streams in `ck-find`, configuration persistence,
and a few utility-specific readers are tracked as the next migration slice;
they are not treated as long-path-safe Windows boundaries. POSIX-only
`/proc`/stream code remains behind non-Windows compilation branches.

## ckVision 1.4.2

The pinned source revision was audited directly at
`7ad5bd67553414e7dc0882069482864034cb31f9`. No ANSI Win32 filesystem or
process APIs were found in `src/`. The relevant Windows filesystem code uses
`CreateFileW` and converts validated paths to extended form before opening
them. The `MAX_PATH` references in process launch and ConPTY discovery are
initial capacities or a compatibility-spelling choice; both paths grow their
buffers and switch to extended spelling at the boundary. The remaining
`std::ifstream`/`std::ofstream` members in `directory_service.cpp` are POSIX
branches and are not compiled on Windows.

This audit does not waive the policy for future ckVision upgrades. Re-run it
against every new SDK revision before changing the pinned baseline.
