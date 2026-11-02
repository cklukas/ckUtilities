# ckUtilities agent instructions

## Windows workstation

All Windows workstation development must use the fast development drive `V:`.
This includes source checkouts, SDKs, builds, caches, generated intermediates,
test data and logs. Put temporary work in a unique directory beneath `V:\tmp`;
keep permanent sources in a permanent development directory on `V:`.

Verify the drive is available and set `TEMP`, `TMP`, and applicable `TMPDIR`
to the task's directory on `V:` before invoking development tools. Never fall
back to `C:` or `D:` for project work. Installed OS/toolchain binaries may stay
where installed; direct their project outputs and temporary writes to `V:`.

Before relocating existing task work, stop its exact owning processes, retain
concise evidence, verify source bytes after the move, and rebuild outputs that
embed the old paths. Preserve unrelated active work. Inherited Mac temporary
workspace and all-platform build freshness/cleanup instructions still apply.

## Windows file I/O policy

Windows filesystem code must use modern wide-character/native handle APIs and
must not rely on legacy `MAX_PATH`-limited path handling. Convert validated
absolute DOS and UNC addresses to an extended-length form (`\\?\\` /
`\\?\\UNC\\`) at the Win32 boundary, or use a handle-relative native API such
as `NtCreateFile` when that is required to preserve exact names and identity.
Do not use narrow Win32 APIs, `std::fstream`/`std::filesystem` as the final
I/O operation for long or adversarial paths, or path-based reopen fallbacks
where a held handle can be used. Preserve the existing reparse-point,
sharing, access, identity, cancellation, and error-state checks when replacing
an API. Every replacement must have Windows regression coverage for long
paths, ordinary and UNC namespaces, exact UTF-16 names, and the applicable
security/reparse behavior.

When auditing or adding a Windows dependency such as ckVision, inspect its
Windows filesystem and process boundaries for the same legacy API assumptions;
document any unavoidable compatibility call and keep it outside the exact
filesystem path used by ckUtilities.

This is a mandatory implementation rule, not a portability suggestion: new or
modified Windows file, directory, archive, profile, cache, and process-path
code must use the repository's modern wide/native boundary or an audited
ckVision native adapter. Do not add a direct `std::filesystem` or narrow Win32
final I/O call merely because the ordinary test path is short.
