# ck-files

`ck-files` is the CK Utilities two-pane file manager, designed for the fast
location-to-location workflow of Norton Commander and Midnight Commander while
keeping every mutating operation reviewed and capability-checked.

![Files two-pane manager](../generated/screenshots/ck-files.svg)

At a filesystem namespace root, **Backspace** opens the read-only volume and
mount view shown below. It is a system-information view, not a writable parent
directory.

![Files volume and mount information](../generated/screenshots/ck-files-volumes.svg)

## Usage

```text
ck-files [LEFT_DIRECTORY [RIGHT_DIRECTORY]]
```

Without arguments, both panes start from the current directory. Use
`--help` (or `-h`) for the command-line synopsis. The application does not
execute files or provide an embedded shell.

## Features

- Two independent panes with an explicit active-pane indicator, resizable
  layout, narrow-terminal fallback, location fields, breadcrumbs, history,
  bookmarks, parent/home/root navigation, and Windows drive/UNC entry.
- At a namespace root, pressing **Backspace** once opens a read-only volume
  and mount view above the normal filesystem boundary. It lists Windows drive
  letters or POSIX mount points, source/name, filesystem, read-only status, and
  current free/total space. The view uses ordinary read permissions, requests
  no elevation, and never exposes volume rows as operation targets.
- Asynchronous directory loading with completion, bounded results, refresh,
  folders-first display, sorting by name/type/size/date, hidden-item toggle,
  and quick filename filtering.
- Keyboard and mouse navigation, focused-row identity, mark/unmark, range
  selection, select all, invert, clear selection, and an always-visible exact
  source count for operations.
- Bounded read-only text and hexadecimal preview, properties, and path copy.
  Preview never runs a file, hydrates provider-managed content implicitly, or
  treats a display name as a rewritten native path.
- Rename and create-folder actions with strict name validation. Case-only
  renames and platform-specific name rules are capability-checked rather than
  silently normalized.
- Copy and move planning that shows the exact sources, destination, conflicts,
  metadata/stream preservation, unresolved risks, and expected losses before
  execution. Conflict choices are scoped to the reviewed job.
- Cancellable worker-owned jobs with honest progress, per-item outcomes,
  source/destination identity revalidation, durable journal boundaries, and
  explicit committed, retained, skipped, failed, and indeterminate results.
- Same-filesystem moves use identity-bound native rename when qualified.
  Cross-filesystem moves use copy, independent stream verification, durable
  destination publication, and only then qualified source retirement.
  Recursive trees use the same safety model and never follow links or silently
  include children that appeared after planning.
- A separate **Move to Trash / Recycle Bin** action and confirmation. It uses
  the native platform boundary where qualified; unsupported or uncertain
  behavior retains the source and reports the reason instead of falling back.
- A separate irreversible **Delete permanently** action with explicit scope
  confirmation and identity-bound checks. It does not follow links, recurse
  through non-empty directories, or claim secure erasure.
- Preservation and capability reporting for data streams, extended
  attributes, permissions and ACLs, ownership, timestamps, flags, links,
  sparse/compressed storage, names, provider-managed objects, and filesystem
  boundaries. Unknown or unavailable preservation is a reason to stop, skip,
  or choose another destination—not a silent success.
- Shared suite help, themes, configurable shortcuts, visible menu alternatives,
  durable display settings and exact native bookmarks, and CLI
  help/version behavior. Settings are versioned, bounded, private and written
  atomically; invalid settings and failed or conflicting writes are reported,
  and failed writes never replace the last known file.

## Default commands

| Key | Action |
|---|---|
| Enter | Enter a directory or preview a file |
| Space | Mark or unmark the focused entry |
| Backspace | Go to the parent directory |
| Ctrl+L | Edit the location |
| F3 | Preview |
| F5 | Copy |
| F6 | Move |
| F7 | Create folder |
| F2 | Rename |
| Ctrl+R | Refresh |
| F1 | Context help |
| Alt+X | Quit |

All actions also have visible menu or dialog commands, and shortcut bindings
can be reviewed through the suite configuration UI. Completion retains
ckVision's Right-to-accept behavior.

## Platform qualification and safety boundary

The Files implementation is cross-platform and refuses unsupported operations
explicitly. Current qualification records document the exact platform matrix:
Windows has qualified Recycle Bin and regular-file/empty-directory retirement;
macOS and Linux qualify same-device native Trash transactions with restore
metadata, and the mount-specific paths are qualified on macOS against a
write/search-only `.Trashes` root and on Linux against a foreign `/dev/shm`
volume. POSIX permanent source-retirement remains explicitly unsupported. The
current macOS terminal verifier has
qualified `Alt+X`, resize, and terminal-mode restoration for xterm-256color,
xterm, and vt100; the Windows ConPTY contract has qualified launch, drawing,
and clean `Alt+X` shutdown for the installed utility set. The same three
profiles pass for all ten utilities in the Linux aarch64 archive. This is still
an implementation checkpoint, not a claim that every release gate is complete:
recovery execution, full filesystem-matrix coverage, and final
mouse/restricted-key acceptance remain tracked work.

The operation plan is not a global transaction and does not promise rollback
after a partial failure. Cancellation stops new work at a safe boundary and
reports completed effects. Recovery verifies identities and journals before
offering inspect, cleanup, or revalidated retry; it never automatically
deletes an artifact or source by pathname.

See the [Files implementation plan](../../implementation/ck-files-plan.md),
[contracts](../../implementation/ck-files-contracts.md), and
[qualification evidence](../../implementation/ck-files-progress.md) for the
precise guarantees and remaining release gates.
