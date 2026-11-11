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
   running, cancellation, completion, failure or indeterminate durability. On
   startup, a separate read-only worker scans bounded private archive journals;
   Copy remains disabled until that scan completes successfully and reports no
   reconciliation-required journal.
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

### Darwin publication gate

The native writer requires no-replace publication of the exact staged file
through the retained destination directory. Apple DTS confirms that Darwin
has no public API for adding a hard link to an already-open file descriptor.
A later lookup can detect a substituted entry only after the link has changed
the namespace. [Apple DTS response](https://developer.apple.com/forums/thread/850377)

Darwin's `fclonefileat` takes a held source descriptor and an absent destination
name. It creates a separate file and returns a status code. A future clone
provider needs its own contract for obtaining the created object's identity and
proving sparse layout and durability before issuing a publication receipt.
The current backend returns typed Unsupported for Darwin staging.
[Darwin clonefile manual](https://github.com/apple-oss-distributions/xnu/blob/main/bsd/man/man2/clonefile.2)

Surface the backend's destination publication capability before Files offers
volume Copy. A known refusal should disable Copy with the same reason in the
menu, palette, and footer. Keep checks worker-owned and repeat the destination
identity and capability checks at approval. Include an ordinary-user test that
verifies the refusal creates neither a staging file nor an archive journal.

## Current state

The versioned sparse writer, single-worker boundary, archive-specific review
model, native source adapter, read-only live capability probe, durable archive
journal transitions, archive-specific read-only recovery projection, bounded
journal discovery, startup recovery worker, bounded Files recovery inspection
dialog, and Files planning/review/execution/readiness wiring are implemented and
focused-qualified on macOS, Linux ARM64 and Windows x64. The format records
exact POSIX bytes or Windows UTF-16 units plus explicit loss records;
new archive journal evidence also records the exact destination address in its
native encoding, the deterministic staging address, and a post-flush
whole-file SHA-256 digest, while legacy journals without those fields remain
inspect-only; malformed or non-monotonic evidence is rejected. Unreviewed
losses and hard losses cannot reach the writer. Snapshot/provider
qualification and worker-owned revalidated-retry actions remain open. The
worker-only cleanup boundary is implemented and qualified on Windows using the
existing handle-bound discard adapter; macOS/Linux explicitly refuse it until
their adapters provide the same race-safe protocol. The digest is not
permission to resume, publish, or delete through a pathname. The cleanup
command remains disabled for restart records when the creator's live
ownership is unavailable. The Linux terminal run archived a private ext4
mount, verified sparse allocation, and matched the reconstructed data to the
source digest. Real-volume acceptance on macOS and Windows remains open.
An earlier macOS broad app ASan run reported a libc++ container-overflow in
`DirectoryCursor::read`. A current run needs a matching instrumented SDK
with default container annotations. The archive decoder now has a deterministic bounded mutation
baseline covering truncation, corruption, reordered frames, invalid semantic
fields, identity changes and oversized lengths; it passed 18/18 on macOS and
Linux. Linux ARM64 address/undefined sanitizer qualification of the
archive and Files app suites passed 15/15 and 68/68 respectively; TSan and the
full filesystem/fault matrix remain open WP-07 work.
