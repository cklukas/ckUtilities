# Files volume-to-archive enablement plan

This is the implementation plan for selecting a volume row in one pane and
copying it to the other pane as a sparse `.ckarchive` filesystem archive. The
operation is deliberately separate from ordinary file copy: a volume row is a
read-only archive source, never a movable or deletable entry.

## User-visible behavior

1. At the namespace boundary, the pane shows volumes/mounts as rows. The
   selected row supplies the archive source identity, mount point, filesystem,
   read-only state, total space and free space.
2. Copy is enabled only when the current volume capability record says that a
   complete archive plan can be built. Move, rename, trash, delete, marking and
   ordinary folder-entry planning remain disabled.
3. Copy opens an archive-specific review containing the source volume identity,
   consistency mode, destination folder, archive name, estimated logical/data
   bytes, sparse extents, links, hard links, unsupported objects and all
   metadata/stream losses. Cancel is the default whenever a required fact is
   unknown.
4. After approval, a worker freezes the immutable source plan, revalidates the
   volume identity and destination, writes an exclusive sparse temporary file,
   verifies the manifest and every written extent, flushes it, and publishes
   without replacing an existing archive. The footer reports planning,
   running, cancellation, completion, failure or indeterminate durability.
5. Every command surface uses the same availability predicate. When Copy is
   disabled, the active pane status gives the specific reason, such as
   `archive capability is unknown`, `snapshot provider unavailable`,
   `destination is not writable`, `source contains an unsupported object`, or
   `volume archive review is not connected`.

## Privilege and mount policy

No elevation or unmount is requested for ordinary live traversal. A user can
archive only content readable with the current credentials and only when the
destination folder is writable. The operation must fail closed if the source
mount disappears, its identity changes, or a required object cannot be read.
Unreadable objects are not silently omitted.

Snapshot mode is preferred but capability-specific:

- Windows: use a qualified VSS snapshot provider when available. Snapshot
  creation may require the VSS service and administrative/provider policy; the
  UI must report that requirement and keep Copy disabled if it cannot be
  established. A live read is a distinct, explicitly reviewed mode.
- macOS: use a qualified APFS snapshot path when the mounted filesystem and
  current account permit it. Do not substitute an unmount, `fsfreeze`, or a
  path-only copy for snapshot consistency.
- Linux: use a separately qualified filesystem/provider snapshot mechanism.
  `/proc/self/mountinfo` and `statvfs` identify and measure a mount but do not
  make a consistent snapshot. If no provider is available, offer live mode
  only after the full live-read capability probe succeeds.

The capability states are `Unknown`, `LiveReadable`, `SnapshotPreferred`,
`Denied`, and `Unsupported`. Probe failures remain `Unknown` or `Denied`; they
are never upgraded because a filesystem label looks familiar.

## Implementation slices

### A. Native source probe and immutable inventory

- Revalidate the opaque volume identity and mount handle immediately before
  planning and again in the archive worker.
- Enumerate below the mount without following symlinks, junctions, mount
  boundaries, sockets, devices, FIFOs, offline placeholders or unknown reparse
  points. Classify each object in the review; refuse the plan when its content
  cannot be represented safely.
- Read regular-file data through native handles, preserve holes as extents,
  record hard-link groups, and preserve symlink targets as links.
- Inventory named streams/resource forks, extended attributes, ACLs/security
  descriptors, ownership, timestamps, flags, compression/encryption and
  provider-managed state. The archive format must either encode and verify a
  feature or list the exact affected paths and obtain explicit consent for the
  loss. Unknown metadata is a hard planning stop.
- Use descriptor/handle-relative operations and identity checks to limit
  symlink races and source replacement. A path string is never an identity.

### B. Archive-specific planning and review

- Add a worker-owned source adapter that returns a complete immutable
  `FilesVolumeArchiveDocument` or an explicit unavailable observation.
- Add an archive review model rather than reusing ordinary file-copy
  assumptions. It must show consistency, logical/data bytes, sparse behavior,
  object counts, preservation categories and destination conflicts.
- Generate a safe default name from the displayed volume name, but require a
  single `.ckarchive` component and refuse collisions without replacement.
- Bind approval to the source identity, capability result, destination
  identity, archive name and loss report. Any change requires a new review.

### C. Worker execution and recovery

- Connect the existing `FilesVolumeArchiveService` only after A and B provide
  a complete document; its current unknown-capability gate must remain in
  place until then.
- Add a durable operation journal record before source reads and before final
  publication. Journal only identities, extents, states and paths—not file
  contents.
- Verify header, manifest, every non-hole extent and the final logical size;
  flush the file and the containing directory where the platform exposes that
  boundary. Treat a post-publication durability failure as an explicit
  indeterminate result with the published path retained for inspection.
- Cancellation stops at safe points, removes only the worker-owned temporary
  file, and reports any already-published result. It never deletes a source or
  an existing destination.

### D. Platform qualification

- macOS: live and APFS snapshot capability tests, sparse allocation checks,
  xattrs/resource forks, ACLs, symlinks and hard links.
- Windows: drive-letter/GUID identity, ReFS and NTFS sparse files, alternate
  streams, reparse-point refusal, security descriptors, VSS policy and
  v143/sanitizer/race gates on `V:` build outputs.
- Linux: mountinfo identity, ext4/xfs/btrfs capability matrix, xattrs/ACLs,
  sparse extents, hard links, provider snapshot policy and ARM64 sanitizer or
  race gates where supported.
- Each platform needs an ordinary-user case, denied-read case, disappearing
  mount case, source-change case, destination-conflict case, cancellation
  case, crash/restart recovery case and full-disk/short-write case.

## Current state

The versioned sparse writer, single-worker boundary, archive-specific review
model, native POSIX source adapter, read-only live capability probe, and Files
planning/review/execution wiring are implemented and focused-qualified on
macOS, Linux ARM64 and Windows x64. The format records exact POSIX path
encoding and explicit loss records; unreviewed losses and hard losses cannot
reach the writer. Windows UTF-16 paths remain gated until a native-unit codec
is implemented. Snapshot/provider qualification, durable journal integration,
and real-volume interactive acceptance remain open. The macOS broad app ASan
fixture currently reports an existing libc++ container-overflow diagnostic in
`DirectoryCursor::read`; this is retained as an explicit validation blocker,
not suppressed.
