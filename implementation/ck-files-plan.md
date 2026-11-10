# Files (`ck-files`) — implementation work packages

Status: implementation active; WP-00 and WP-01 checkpoints are complete.
WP-02 remains open. Worker-owned settings persistence now replaces the
synchronous UI writes found on 2026-11-10. Current Mac normal and matching
ThreadSanitizer Files suites pass, including blocked-save responsiveness,
revision conflicts and shutdown draining; a real PTY check confirms that a
final save warning follows terminal restoration. Current Linux normal validation
also passes a selected 40-entry application/archive subset, including 271
application cases and both package consumers; the full Files label is still
being qualified. A current Mac native PTY confirms Right-to-accept and mouse routing
in both panels. Linux sanitizer, Windows and native-window acceptance remain open.
The held-creator archive integration also needs its full application and
cross-platform qualification. Earlier POSIX-PTY and Windows-ConPTY results
remain evidence for their recorded source cohorts; they do not qualify these
new changes. Real-window mouse and restricted-key acceptance, and the remaining
sanitizer/runtime gates, are documented in the
[implementation evidence](ck-files-progress.md). Windows v143 qualification
uses the mandatory `V:` workstation development drive.
Updated: 2026-11-10. Original baseline review:
`81aaf009db23ca8558211722107bd2122351e565` (2026-10-23).

Execution: continue WP-02 implementation and its acceptance gates before
claiming completion. See [implementation evidence](ck-files-progress.md),
[behavioral and adapter contracts](ck-files-contracts.md)
and the [SDK validation record](../docs/migration/ckvision-1.4.0-validation.md).

## Product commitment

Build a professional, keyboard-first, two-pane file manager for ckUtilities,
using the current ckVision SDK and the suite's native application conventions.
Use **Files** in the UI and `ck-files` as the proposed executable name. Borrow
the efficient two-location workflow of Norton Commander and Midnight Commander;
design the vocabulary, layout, commands, and implementation for ckUtilities.

Correctness and safe use are release requirements. Every advertised feature
must be complete, documented, and tested on Windows, Linux, and macOS. No shell
copy fallback, silent metadata loss, fake progress, unchecked filesystem
assumptions, or success messages for incomplete operations. Intermediate work
packages are development milestones, not permission to ship incomplete behavior.
No program can guarantee preservation against every hardware failure or
concurrent external writer; state the precise guarantees and verified limits.

## Baseline and source evidence

The latest published ckVision release checked on this date is
[v1.4.2](https://github.com/cklukas/ckVision/releases/tag/v1.4.2), released on
2026-11-02. The tag resolves to commit
`7ad5bd67553414e7dc0882069482864034cb31f9` (verified with `git ls-remote`).
At initial review, ckUtilities required 1.3.0 and recorded revision
`57b379d0dddd6a3b517b614a4eb2d1a1c99b474b` in
`cmake/CkVisionPackage.cmake` and `docs/migration/ckvision-baseline.md`.

Upgrade through an installed `ckvision::cvision` package; never link a sibling
source checkout. At implementation start recheck the latest stable release,
record its immutable commit, and validate it before updating the baseline.

Relevant upstream contracts:

- [Platform services](https://github.com/cklukas/ckVision/blob/v1.4.2/docs/platform-services.md): injected filesystem and streamed directory enumeration outside UI dispatch.
- [Data views](https://github.com/cklukas/ckVision/blob/v1.4.2/docs/data-views.md): provider-backed views with stable item identity and UI-thread publication.
- [Completion](https://github.com/cklukas/ckVision/blob/v1.4.2/docs/completion.md): asynchronous directory services, bounded pages, explicit index storage, and nonblocking view retirement.
- [Commands and dialogs](https://github.com/cklukas/ckVision/blob/v1.4.2/docs/dialogs-and-commands.md): one command definition drives menus, shortcuts, enablement, and dialogs.

The SDK supplies presentation and discovery infrastructure. Do not assume its
filesystem abstraction supplies transactional copy, native metadata preservation,
or recovery. Verify public APIs in WP-00; implement product-owned native
adapters where required. Fix an SDK defect upstream and consume a verified
release instead of bypassing its contract locally.

## First-release scope

The follow-on [Commander parity work packages](ck-files-parity-plan.md) close
the remaining mutation safety limits first, then add the deferred Commander
workflows. They require reuse of ckVision editor/terminal facilities and shared
ckUtilities components. They do not close the current release gates below.

The release includes all of these, subject to the explicit filesystem capability
and confirmation rules below:

- Two independent locations, navigation history, editable paths with completion,
  parent/home/root navigation, location bookmarks, and Windows drive/UNC entry.
- Details listing, sorting by name/type/size/date, folders-first option, hidden
  item toggle, quick filename filtering, explicit refresh, and properties.
- Keyboard and mouse focus, marking multiple entries, range/select-all/invert,
  clear selection, and an always-visible count of the operation's exact sources.
- Copy, move, rename, create folder, move to Trash/Recycle Bin where supported,
  and separately confirmed permanent deletion.
- Conflict decisions, preservation/loss previews, cancellable jobs, progress,
  per-item outcomes, durable operation records, and interrupted-job recovery.
- Bounded read-only text/hex preview and copying a path. No automatic execution
  of files. Full-text/advanced search will reuse existing Find UI/services
  in-process under CP-07 of the parity plan; basic filtering remains available
  inside Files. Do not invoke a sibling tool executable as integration glue.
- Saved layout, display settings and bookmarks; suite help, themes, shortcut
  configuration, launcher registration, CLI help/version, installation and docs.

Defer remote protocols (SFTP/FTP/SMB clients), archive editing/extraction, folder
synchronization, executable launch associations, embedded shells, arbitrary
commands, bulk metadata editing, and a general undo system. Mounted network
locations are accessed through the OS and must still pass capability checks.
These extensions need their own complete specifications; do not leave enabled
placeholder commands in the first release. This scope does not exempt unusual
local or mounted filesystems from safe refusal and loss reporting.

## Interaction specification

Use the shared `SuiteShell`, menus, command palette, help, theme roles and
`SuiteCommandScope`. The primary window contains two resizable panes, each
with a full-location field/breadcrumb, details list, and selection summary.
Highlight the active pane with a textual focus indicator as well as color.
Copy and Move always show **From**, **To**, selected names/count, and relevant
preservation decisions. The inactive pane supplies the initial destination;
switching panes after opening a dialog must not retarget its operation.

Proposed menu groups: Location, Selection, Actions, View; retain the suite's
application controls. Use `ck.files.*` command keys in the suite catalog.
Proposed pane-focused defaults are Enter to enter a folder/preview a file,
Space to mark, Backspace for parent, Ctrl+L for location, F3 for preview,
F5 for Copy, F6 for Move, F7 for New folder, F2 for Rename, and Ctrl+R for
Refresh. F1/help and Alt+X/quit follow suite conventions. Validate all defaults
against suite and terminal bindings before accepting them. Provide visible
commands for every action; function keys are never the only access path.
Tab follows normal focus traversal; provide a separate configurable Switch
pane command. Do not break ckVision completion's Right-to-accept behavior.

Marked entries take precedence over the cursor only when clearly indicated.
Filtering must not invisibly enlarge an operation: show hidden marked entries
and require review or clear them. Parent navigation is never a mutable entry.
Escape cancels the current interaction; it must not silently discard a job.
Closing with active work offers Stay or Cancel jobs and exit, explains already
completed effects, and waits outside dispatch for safe worker termination.

At narrow widths switch to one visible pane with an explicit other-location
indicator and pane switch; preserve both models. Test 80×24 and 120×34, plus
smaller supported sizes with usable dialogs or a clear minimum-size message.
Long names, combining marks, wide characters, and control characters must not
mislead selection or inject terminal escape sequences.

## Filesystem and preservation contract

### Detect actual capabilities

Model capabilities by operation and actual source/destination location, not
just by OS or filesystem label. Record supported, unsupported, unknown, and
permission-denied separately. Consider filesystem/volume identity, driver and
mount options, current credentials, network/server behavior, and actual object
features. Use non-destructive native queries first; any probe must use a
private, exclusively created object in the approved test/work area, be cleaned
up, and never modify an existing user object. An inconclusive probe is unknown,
not success. Revalidate at execution because mounts and permissions can change.

The inventory and preservation report must cover at least:

| Feature | Required checks and behavior |
| --- | --- |
| Main data and additional channels | Enumerate and preserve NTFS named streams, macOS resource forks, and platform equivalents where present. Independent byte/stream verification; copying the main stream alone is insufficient. |
| Extended attributes | Inventory names and values, including security/quarantine attributes. Preserve faithfully or identify exact omissions; never silently strip security markings. |
| Permissions, ownership and ACLs | POSIX modes, UID/GID, ACLs/default ACLs, Windows security descriptors and inheritance. Define semantic equivalence across platforms; do not claim equivalent security from similar numeric bits. Report ownership mapping, elevation requirements, and access changes. |
| Times and flags | Modification/access/birth times where representable, timestamp resolution/range, read-only/hidden/system/archive and native immutable/append-only flags. Distinguish system-managed change times that cannot be set. |
| Links and object identity | Symlinks, dangling links, junctions/reparse points, hard-link groups and aliases to the same object. Preserve links as links by default; do not recurse through them. Unknown reparse types require a documented refusal. |
| Storage representation | Sparse extents, compression, filesystem encryption/EFS, clone/reflink relationships, deduplication, and allocated size. Distinguish byte identity from representation, storage growth, and encryption/security changes. |
| Names and structure | Component/path limits, reserved Windows names, forbidden characters, case sensitivity per directory, Unicode normalization collisions, case-only renames, large file limits, and directory depth. Never auto-sanitize or silently rename. |
| Special and provider-managed objects | Devices, sockets, FIFOs, mount boundaries, placeholders/offline cloud data, package directories, and unavailable content. Classify explicitly; avoid blocking reads or implicit hydration. Report unavailable features before mutation. |

Keep native names/handles lossless internally, including POSIX names that are
not valid UTF-8. A display escape is not a rewritten filename. ckVision 1.4.2's
documented directory service has a sanitized UTF-8/name-size contract; this is
a specific integration gap to resolve in WP-00/WP-02. Unsupported entries must
remain identifiable with a clear diagnostic and blocked unsafe actions. Do
not hide them or accidentally operate on a different normalized name.

### User decisions about limits and loss

Before mutation, produce a complete operation plan with item identities,
destinations, source features, achievable preservation, and unresolved risks.
Long scans report Planning with cancellation and unknown totals; do not
pretend discovery is complete. Group repeated differences for readability,
with expandable affected paths, exact metadata/stream names, data sizes and
consequences. Sensitive attribute values need not be exposed in the report.

Default to Cancel when loss or preservation uncertainty exists. Offer Preserve
by choosing another destination, Skip affected items, or explicitly Continue
with the listed losses where a meaningful and safe result is possible.
Loss of an additional data stream must say **data loss**, not merely a cosmetic
attribute warning. A generic overwrite confirmation cannot authorize loss.
For a move, state explicitly that confirming source removal will make omitted
features unrecoverable from the original. Bind consent to the reviewed plan,
feature categories, affected objects and destination; new differences require
a new decision. No global 'ignore all errors' setting or remembered silent
consent across unrelated jobs. Impossible paths, unidentified targets, unsafe
object changes, or inability to verify main data remain hard failures that a
confirmation cannot override.

Directory merges are an explicit conflict choice, not replacement. Resolve
file/file and file/directory conflicts separately. Default to Skip/Cancel;
choices include Skip, Keep both with a reviewed valid name, Replace, and Merge
directories where appropriate. 'Apply to remaining' is scoped to this job and
the same reviewed conflict class. New preservation differences still interrupt.

### Execution, verification and recovery

Use native handle/descriptor-relative operations and object-identity checks to
limit symlink/reparse and time-of-check/time-of-use races. Canonical path-string
comparison alone is insufficient. Detect self-copy, hard-link aliases,
copy/move into descendants, mount changes, and destination substitutions.
Overlapping selections are deduplicated without losing hard-link semantics.
Revalidate the complete frozen plan before starting mutation, then each object
at use; do not silently include newly appeared directory children.

Copy into an exclusive destination-side transaction file, verify its main data,
all agreed additional channels and metadata, then publish with the platform's
safe no-clobber/replace primitive. Native copy acceleration is acceptable only
if the adapter proves the same contract. Byte counts and timestamps alone do
not verify content: use an independently read cryptographic digest for copied
streams, detect source changes, and define supported snapshot/locking behavior.
Metadata application order must preserve directory access during traversal and
apply final permissions/times afterward. Preserve hard links within the
planned set when supported; explain any authorized conversion to independent
copies and space impact. A replacement must retain a recoverable old target
until commit is durable; never truncate it before validation. If destination
semantics cannot provide the promised replacement guarantees, block or offer
a separately explained safer action; never emulate atomicity and call it atomic.

Same-filesystem moves use identity-checked rename where its semantics preserve
the agreed features; assess inherited-security and provider effects as well.
Cross-filesystem moves are copy → verify → durable destination commit → source
removal. Retain originals when verification, journal persistence or commit fails.
Recheck source identity/content before removal. Remove source directories only
after all planned children have succeeded and no unplanned children remain.
Batch/tree operations are not globally atomic: show committed, retained, skipped,
failed and indeterminate items separately, especially after partial moves.

Cancellation stops new work, reaches a safe point in current work, cleans only
owned incomplete artifacts, and reports completed effects. No generic rollback
promise. Durable per-item journal transitions must precede destructive steps;
flush files and containing directories using verified platform/filesystem
semantics. Distinguish process-crash recovery from power-loss durability, and
report weaker/unknown storage guarantees before the relevant operation.
Recovery verifies identities and journals; never automatically delete a source
or replay a destructive step after restart. Offer inspect, safe cleanup,
revalidated retry, or manual recovery with exact paths. Journals require private
permissions, exclusive ownership, versioning, and bounded retention; never copy
all file contents into a log. Do not remove unidentified artifacts by filename.

Trash uses native macOS Trash, Windows Recycle Bin and freedesktop-compatible
Linux behavior, including mount-specific support and native restore metadata.
If unavailable, report why and offer cancellation or a **separate** permanent
delete confirmation. Never fall back silently. Permanent deletion has no undo,
does not follow links, and confirms scope/count/location; secure erasure is not
claimed. Access denial is reported without automatic elevation or permission
changes. Enumerate unreadable subtrees as incomplete; never report full success.

## Architecture and repository changes

Proposed new modules (names are design targets, not existing APIs):

| Area | Responsibility |
| --- | --- |
| [`ck-core-fs-lib`](https://github.com/cklukas/ck-core-fs-lib) → `ck_files_core` | Lossless paths/identities, pane state, selection, inventory, capability comparison, operation plans, consent and job/recovery state machines; no UI dependency. The sibling checkout is selected with `CK_CORE_FS_LIB_SOURCE_DIR`. |
| [`ck-core-fs-lib/src/files/platform`](https://github.com/cklukas/ck-core-fs-lib/tree/master/src/files/platform) → `ck_files_platform` | POSIX/Windows/macOS discovery, native metadata/stream adapters, safe publication, journal durability and capability queries. Platform-specific sources are selected by the library's CMake project. |
| `src/vision/files/` → `ck_vision_files` | `FilesApp`, pane/list providers, dialogs, preview, worker coordination and UI-thread publication. |
| `src/vision/files/main.cpp` → `ckfiles_ckvision` | Host ownership, native arguments, explicit scratch/index roots, settings and executable `ck-files`. |
| `ck-core-fs-lib/tests/`, `src/vision/files/tests/` | State-machine/fault-injection tests, native conformance, headless/golden and lifecycle tests. |

Reuse `ck_vision_host`, `SuiteShell`, `SuiteCommandScope`, suite keymap metadata,
and strict warning/test conventions. Use provider-backed Table/ListView and
Splitter rather than a second terminal renderer. Separate entry identity from
display index and from long-lived native object identity. Pane generations and
job IDs reject stale asynchronous responses. All filesystem I/O, large sorting,
hashing and index operations run outside event dispatch; publish bounded immutable
results to the UI. Separate discovery slots from operation workers so one
blocked mount does not consume every capability. OS calls can block: show that
state honestly, never free buffers/handles still in use, and never promise an
instant cancellation that the backend cannot deliver.

Use bounded pages/disk indexes for large listings; keep operation inventories
and journals scalable too. Disk-full/index failures are visible failures, not
fallbacks to unlimited RAM. Preview reads bounded data through read-only handles,
does not execute terminal controls, and handles binary/invalid encoding and
changing files. Settings use validated, versioned, atomic persistence and do
not store preservation consent. Avoid changes to process cwd for pane navigation.

Update `src/common/app-info/tool_info.cpp`, `src/vision/keymap/`, launcher tests,
`src/vision/CMakeLists.txt`, `src/native-core/CMakeLists.txt`, boundary check
scripts, package executable lists, Windows packaging/manifest integration,
Homebrew generation, PTY/ConPTY smoke lists, docs capture targets, README,
installation/build docs, `docs/tools/ck-files.md`, and `ckdocs.yml` as appropriate.
Audit hard-coded tool lists rather than updating only the launcher.
Extend `docs/migration/action-policies.md` with the reviewed Files operation
policy and its enablement tests. Existing Find execution and Disk Usage cloud
permissions do not confer file-manager mutation capabilities.

## Work packages and acceptance

Completed packages are checked below. Each completion record names its commit, tests,
platform/filesystem evidence and remaining limitations. A failing acceptance
criterion keeps the package open; do not replace it with a workaround.

| WP | Deliverable | Depends on |
| --- | --- | --- |
| WP-00 | SDK baseline and approved behavioral/capability specification | — |
| WP-01 | Deterministic domain model and native capability adapters | 00 |
| WP-02 | Complete responsive read-only two-pane application | 01 |
| WP-03 | Inventory, preservation planning and explicit consent | 01–02 |
| WP-04 | Verified copy engine and recovery journal | 03 |
| WP-05 | Move, rename, folder creation, Trash and deletion | 04 |
| WP-06 | Complete interaction, jobs and suite integration | 02–05 |
| WP-07 | Cross-platform/filesystem qualification and fault testing | 01–06 |
| WP-08 | Packaging, documentation, real-terminal acceptance and release | 07 |

### WP-00 — baseline and contracts

- [x] Upgrade and validate the installed latest stable SDK; update CMake,
  CI/release pins, Windows script, Homebrew resource, screenshot expectations,
  baseline docs and a new validation record consistently.
- [x] Map each selected widget/service to its public API. Resolve lossless-name,
  scale and lifecycle gaps without depending on private SDK internals.
- [x] Specify the feature support/preservation matrix, native API choices,
  security/permission equivalence rules, atomicity/durability guarantees,
  consent schema and journal transitions. Review threat cases and race windows.
- [x] Accept concrete layout/command flows and the complete release scope above.

Acceptance: existing tools, independent SDK consumer, architecture boundaries,
installation/archive and terminal checks pass with the new baseline. Every
capability has a defined unknown/unsupported behavior; no unverified platform
API is described as implemented. Record clean/incremental build baselines.

#### WP-00 extension — volume archives and command availability

The volume row exposed above a namespace root is a source for a distinct
**Volume archive** operation. Copying a volume to the other pane creates a
versioned `.ckarchive` filesystem archive in that pane's selected directory;
Move, Rename, Delete, Trash, Mark, and Enter are not volume-row operations.
The archive is not a raw block image and is never presented as one. A raw
image/restore workflow requires a separate specification and command family.

The archive contract is:

- The source is identified by native volume identity plus the revalidated
  mount/session identity, never by a display label or a stale path alone.
- The writer records a manifest, format version, source identity and
  consistency mode; directories, regular data, links, hard-link groups,
  sparse extents, timestamps, permissions/ACLs, extended attributes and
  platform-specific additional streams are either preserved or listed as an
  exact loss/unsupported diagnostic. Unknown or unreadable objects are not
  silently skipped.
- Sparse logical files remain sparse in the archive. The writer must verify
  logical length, data extents, allocated size and read-back digests. A dense
  fallback is not automatic because it can exhaust the destination.
- A snapshot is preferred when the platform and volume provider expose one:
  Windows VSS, native APFS snapshots on macOS, and an explicitly qualified
  filesystem/provider snapshot on Linux. A generic freeze/unmount is not a
  snapshot substitute. If only live traversal is available, the plan marks
  the archive as live/inherently concurrent and requires the consistency
  policy to be visible before execution; no unmount is required for ordinary
  live traversal.
- The archive is written to a private temporary object in the destination,
  journaled, flushed and independently verified before atomic publication.
  Cancellation, source changes, destination conflicts, quota/space failures,
  and crash recovery leave no apparently complete archive. The source is
  never deleted by this operation.
- Capability probing is read-only where possible and is revalidated in the
  worker immediately before traversal. Permission denied, unsupported,
  unavailable and unknown are separate outcomes. Elevation is requested only
  by a future, explicitly approved native snapshot/metadata adapter; the
  normal archive path must not silently relaunch elevated or ask for admin
  rights merely to read a mounted volume.

Every command surface uses one operation-specific availability result
`{enabled, reason}` derived from the active pane, selection, destination and
current job state. Menus, palette/status actions, footer commands and keyboard
handlers consume the same result. A disabled command remains visible, is
inert, and exposes its concise reason in the status bar; generic statements
about permissions or changing space are not used as filler. The reason is
selection- and state-specific, for example “Move unavailable: volume rows are
archive sources, not movable entries” or “Copy unavailable: destination pane
is still loading”.

WP-00 extension acceptance requires deterministic tests for the volume-copy
routing table, all forbidden volume-row actions, destination/read-only/loading
states, live-versus-snapshot capability states, sparse-archive policy, and
the shared disabled-reason/status-bar behavior. These tests specify the
worker contract; native snapshot and archive execution remain later package
work and must not be claimed complete until independently verified.

### WP-01 — model and native services

- [x] Add core/platform targets with strict warnings, injected filesystem/time,
  identities, fake adapters and explicit resource ownership.
- [x] Implement entry classification, lossless paths, metadata/stream inventory,
  native identity queries, permissions and destination capability inspection.
- [x] Implement pane sorting/filter/mark semantics, links and collision rules.

Acceptance: deterministic tests cover native name edge cases, all inventory
categories, capability unknown/denied states and selection stability. Native
adapter tests run on all three OSes; UI-free core boundary check passes.

Completed 2026-10-24; implementation through `acf7d7e`. See the
[requirement-by-requirement acceptance review](ck-files-wp01-acceptance.md) and
[machine-readable evidence](evidence/ck-files-wp01-acceptance-20261024.json).
All 82 suites pass on each supported OS; the separate required Linux casefold
configuration also passes against current sources. Query coverage and unavailable
states remain explicit. This acceptance grants no preservation or mutation
authority and does not close the application or release requirements below.

### WP-02 — responsive two-pane Files application

- [x] Implement the real two-pane app, asynchronous navigation/completion,
  bounded providers, history/bookmarks, sorting/filtering, properties and preview.
- [x] Preserve pane state on errors; support roots, drives, UNC and external
  removal. Define refresh semantics; do not present a stale listing as current.
- [x] Test pending scans, stale replies, blocked storage, rapid navigation,
  resize, narrow layout, all suite themes, mouse and keyboard operation.

- [x] Move settings persistence out of UI callbacks into a bounded, joined
  worker; preserve revision conflicts and errors, and drain accepted writes
  during shutdown. UI admission is memory-only; the production binding has no
  synchronous save callback. Current Mac normal/TSan and real-PTY evidence is
  recorded in `evidence/ck-files-settings-worker-integration-20261110.json`.
- [ ] Qualify the settings worker on Linux and Windows against matching SDKs.
  Linux normal and matching Clang19 ASan/UBSan each pass all 147 registered
  Files CTests with zero skips on the settings-worker cohort plus approved native
  fixture corrections, and both package consumers pass in each lane. Windows
  gates remain open; the later Store save-preflight delta is not included. See `evidence/ck-files-linux-full-label-20261110.json`.
  Current Mac PTY completion and
  both-panel mouse routing pass. See
  `evidence/ck-files-terminal-linux-normal-20261110.json`.
  Direct Store save preflight also passes current Mac normal/TSan 109/109,
  including an allocation-fault regression that fails against the old writer.
  See `evidence/ck-files-settings-save-preflight-20261110.json`; this later
  delta still needs Linux/Windows qualification. Startup reads remain an open
  bounded-I/O SDK integration gap.
- [ ] Complete current-source archive integration, sanitizer and real-window
  acceptance. Earlier passing cohorts do not close gates after source changes.

The implemented view and navigation features have application-suite coverage;
WP-02 remains open until the cross-platform and interactive acceptance gates
are resolved.

Acceptance: real view graph works headlessly and interactively; no synchronous
disk access in paint/provider/command callbacks; stable selections survive
refresh and reorder; unsupported names are visible and cannot target a different
object. Mutations are available only through reviewed, revalidated worker
flows; their cross-platform release qualification remains in WP-07/WP-08.

### WP-03 — complete planning and loss consent

- [x] Implement frozen recursive inventories with mount/link policies,
  overlap/self/descendant detection, destination conflicts and capability diffs.
- [x] Implement review dialogs for preservation loss, uncertainty, collisions,
  destination replacement and explicit directory merges.
- [x] Bind user decisions to plan identity and invalidate them on relevant change.

Acceptance: tests demonstrate separate consent for missing ADS/resource forks,
ACL/security changes, sparse expansion, timestamp precision, hard-link loss,
encryption changes, file-size/name restrictions and unknown preservation. Cancel
causes no mutation. Concurrent substitutions force revalidation/new decisions.

### WP-04 — copy, verification and recovery

- [x] Implement native streaming/copy adapters, per-stream digest verification,
  ordered metadata preservation, safe commit/replacement and durable journals.
- [x] Implement bounded progress, conflicts during execution, cancellation,
  per-item outcomes, startup inspection and recovery of owned artifacts.
- [x] Add deterministic failure injection at each journal/commit boundary and
  during read/write/flush/metadata operations; test process termination recovery.

Acceptance: failures never destroy the old target or expose an incomplete copy
as complete. All unapproved feature losses block. Tests cover ENOSPC/quota,
short I/O, disconnects, permission changes, source modifications, corruption,
crash-after-commit and journal persistence failure. Recovery never guesses.

### WP-05 — remaining mutations

- [~] Implement same-filesystem and cross-filesystem moves with separate
  semantics and independently verified source-removal predicates. Same-volume
  moves and the Windows handle-exclusion path are implemented; POSIX
  cross-volume source retirement remains disabled until CP-01 supplies a
  journaled, recoverable source-side quarantine protocol.
- [x] Implement case-only rename, folder creation, native Trash/Recycle Bin and
  explicit permanent deletion with no link traversal or hidden fallback.
- [x] Test nested partial failures, new children, read-only/immutable objects,
  mount-boundary changes, reused paths and handles, and unavailable Trash.

Acceptance: cross-filesystem source removal is impossible before verified,
durable destination commit and approved loss consent. Interrupted directory
moves retain every uncommitted/unverified source. Native Trash results can be
restored through the platform facility in acceptance testing. Permanent delete
always states its irreversibility; cancellation outcomes reflect actual effects.

### WP-06 — complete product interaction

- [x] Finish command registry/keymap integration, menus, help, status actions,
  job details, error summaries, settings, launcher and CLI behavior.
- [x] Serialize mutations initially; allow browsing and planning while a job
  runs. Lock conflicting paths/identities across panes; test interactions with
  external processes. No unbounded job queue or concurrent conflicting writes.
- [x] Ensure every progress/result message is derived from engine state.
  Keep Cancel, Skip and Retry distinct; retry creates a revalidated plan.

Acceptance: full workflows work without function keys or mouse; focus returns
correctly after dialogs, active jobs survive pane navigation, close is safe,
late callbacks cannot touch destroyed controllers, and settings failures are
reported. All first-release features are present with no placeholder actions.

### WP-07 — qualification matrix and reliability

- [ ] Execute the matrix below with actual feature-bearing fixtures and native
  independent inspection, not only mocked capability responses.
- [ ] Add model/journal decoder fuzzing, address/undefined/thread sanitizer runs
  where supported, resource-lifetime tests, and deterministic cost budgets.
  The deterministic archive-journal mutation baseline is now committed and
  separately qualified; broad model fuzzing and the remaining resource/cost
  gates are still required before this item can be checked off.
- [ ] Test very large directories and operation inventories (including at least
  100,000 entries), long names, low memory, index exhaustion, slow/disconnected
  network mounts, full disks and repeated cancellation. Report measured input,
  memory, index size and responsiveness; establish thresholds before approval.
- [ ] Test process crashes at every destructive transition; perform controlled
  VM/storage fault tests for any claimed power-loss guarantees.

Acceptance: every matrix row has a result, exact OS/driver/mount/feature details,
and evidence. No unresolved data-loss, incorrect-source-deletion, silent-loss,
security-metadata, crash-recovery or stale-target defect remains. Missing test
access is a release blocker for that advertised capability, not a presumed pass.

### WP-08 — release and documentation

- [ ] Add all package/install/archive/terminal verification entries and verify
  `--help`, `--version`, launcher launch/return and installed executable paths.
- [ ] Document supported semantics and tested limits, loss dialogs, interrupted
  moves, Trash behavior, operational artifacts, and recovery procedures.
- [ ] Capture screenshots from the actual app through the deterministic docgen
  pipeline; review goldens rather than blindly accepting regenerated output.
- [ ] Run the full suite and fresh release rehearsal, Linux DEB/RPM/archive,
  macOS archive/Homebrew and Windows ZIP/ConPTY paths. Inspect the user-facing
  app in actual terminals on all OSes, including mouse and restricted-key paths.

Acceptance: one release candidate with provenance, matrix, tests, build records,
real-window observations and no undocumented preservation limits. Only then
advertise Files in the public tool list and release notes.

## Minimum qualification matrix

Test same-filesystem operations and both directions for each cross-filesystem
pair. Test observed feature support, not a presumed property of the label.

| Environment | Required cases |
| --- | --- |
| Windows / NTFS | ADS, security/inheritance, EFS where available, sparse/compressed files, hard links, symlinks/junctions, case-sensitive directories, long/UNC paths, sharing violations and Recycle Bin. |
| macOS / APFS | Case-sensitive and insensitive volumes, resource forks/xattrs, ACLs/flags, sparse/clone features, Unicode collisions and Trash. |
| Linux / ext4 plus Btrfs or XFS | POSIX ACL/default ACL, ownership, xattrs/security labels, hard/symbolic links, sparse/reflink features, immutable flags, byte filenames and freedesktop Trash. |
| FAT32 and exFAT | Cross-copy/move from each native filesystem; missing ACL/streams/xattrs, FAT32 size ceiling, time resolution, invalid/colliding names and space expansion. |
| SMB and NFS mounts | At least one real share of each type where supported; server-specific metadata, identity/rename/flush limits, credentials, latency and disconnect during each operation phase. |
| Read-only/removable/provider storage | Read-only mount, removal/remount, quota, offline placeholders, denied hydration, unavailable Trash and unsupported object types. |

For every preservation class, test both a destination that preserves it and one
that does not, including refusal, explicit authorized loss, cancellation, and
source retention on move failure. Inspect data streams/attributes independently
with native APIs/tools. A hash of the main stream alone is never a complete pass.
Add other filesystems as tested capabilities; unsupported combinations remain
visible with explicit decisions or refusal, never silently reduced behavior.

## Execution discipline and definition of done

For all implementation work on the Mac, first verify `/Volumes/S8T` is mounted,
then allocate a unique task directory beneath `/Volumes/S8T/tmp`; use
`/Volumes/PRO-BLADE/tmp` only if necessary. Set `TMPDIR` explicitly for tools;
put builds, SDK staging, indexes, filesystem images, caches and test data there.
Never use `/tmp` or `/private/tmp`; stop if neither permitted volume is mounted.
Remote Linux work uses explicit sensible local workspaces. All Windows
workstation development uses `V:` exclusively, with a unique `V:\tmp` task
directory and `TEMP`, `TMP` and applicable `TMPDIR` set beneath it. Verify `V:`
first and never fall back to another drive. Production
destination-side transaction objects are part of the defined file operation;
tests of them must remain inside task-owned test volumes/directories. Durable
plans and concise validation evidence belong in the repository, not S8T.

Before builds identify exact existing outputs and running PIDs; coordinate
ownership and remove only obsolete task-owned outputs. Record build start/end,
elapsed time, target/configuration, source/asset fingerprints and absolute
output path. Compare with this project's measured comparable baseline; investigate
unexpected duration, cache/input and log evidence before trusting the output.
Launch only after proving build/input/asset freshness; close older instances,
launch the exact verified path and confirm the PID. Inspect that process's real
window before claiming visual correctness. Preserve concise evidence and clean
superseded builds/test images/indexes promptly; keep only the verified runnable
build and workspaces still needed by active work.

The release is done only when every first-release feature meets its contract on
all three OSes, the capability/loss decisions are accurate for tested filesystem
combinations, independent preservation checks pass, recovery and cancellation
are proven, and installation plus real-terminal acceptance is complete.
No shortcut, workaround, or undocumented loss can substitute for these gates.
