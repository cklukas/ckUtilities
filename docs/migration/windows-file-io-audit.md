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

The remaining Windows-compiled consumers now use `ck::io` (`ck_file_io`), a
small shared boundary that uses `CreateFileW`, extended-length DOS/UNC names,
native handles, bounded `ReadFile`/`WriteFile` loops, and `FlushFileBuffers` on
Windows. This covers options/config persistence, saved search specifications,
content search, JSON loading, SSH configuration discovery, chat transcripts
and logs, and the Linux GPU snapshot helper (which is compiled into the
cross-platform sysmon target but only used on Linux). POSIX-only `/proc`,
`/sys`, and archive branches retain their platform-native stream code behind
non-Windows preprocessor branches.

The shared boundary has regression coverage for long paths, exact UTF-16 names,
non-ASCII bytes, append/truncate behavior, bounded reads, recursive directory
creation/listing, and extended-length tree cleanup that does not recurse
through reparse points. UNC spelling is handled by the same extended-path
conversion; an environment-provided UNC fixture can be added to workstation
qualification when a share is available. Windows search-model and dialog path
buffers are heap-backed, so the extended path capacity cannot exhaust the
worker or UI stack.

Windows-compiled product callers use this boundary for file contents,
directory creation, profile enumeration, and cleanup. `std::filesystem` may
still be used for lexical path composition or in POSIX-only branches, but it
is not an accepted final Windows I/O operation for product paths.

The volume-archive writer is also audited at its native boundary: archive and
staging names are decoded strictly from UTF-8 to exact UTF-16 components,
combined with `NativeAbsolutePath`, and only then passed to extended wide
file APIs. This covers long and non-ASCII archive names without allowing a
locale-dependent `std::filesystem::path(std::string)` conversion to become a
Windows I/O operation.

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
