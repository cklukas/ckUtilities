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
