# Files — Commander parity work packages

Date: 2026-11-01. Status: planned; no package below is accepted yet.
Owner: the `file manager` implementation chat. This document adds the next
roadmap; it does not mark existing WP-07/WP-08 or archive gates complete.

## Target and ordering

Close missing file operations and unsafe/unavailable mutation boundaries first,
then complete the Commander productivity workflow. Preserve the guarantees in
[contracts](ck-files-contracts.md), the existing
[release plan](ck-files-plan.md), and the
[volume archive plan](ck-files-volume-archive-plan.md).

Baseline comparison: Norton Commander 5.x DOS workflows and GNU Midnight
Commander's documented commands. The [MC manual](https://source.midnight-commander.org/man/mc.html)
is the primary feature inventory. Capture the exact manual/release revision
and obtain a lawful NC 5.x manual/reference in CP-00 before claiming exhaustive
parity. Directory synchronization is an additional requested workflow; do not
imply MC supplies a full synchronizer. Historical modem/serial transfer, DOS
task switching and raw disk utilities require an explicit applicability decision,
not an unqualified claim of parity or a silently missing row.

| Priority | Package | Dependencies |
| --- | --- | --- |
| P0 | CP-00 Baseline, ownership and component inventory | none |
| P0 | CP-01 POSIX retirement and complete cross-volume moves | CP-00 |
| P0 | CP-02 Recursive deletion and complete native Trash | CP-01 |
| P0 | CP-03 Restart recovery and reconciliation | CP-01; CP-02 for tree recovery |
| P0 | CP-04 Mutation qualification and release safety gates | CP-01–03; existing WP-07/08 |
| P1 | CP-05 Create files, links and metadata operations | CP-00; CP-01–03 for mutations |
| P1 | CP-06 Selection patterns and batch copy/move/rename | CP-01–03 |
| P1 | CP-07 Integrated search, results and directory navigation | CP-00 |
| P1 | CP-08 File/directory comparison and synchronization | CP-04, CP-07 |
| P1 | CP-09 Ordinary archive operations | CP-03–04 |
| P1 | CP-10 Integrated viewer and syntax-aware editor | CP-00; CP-03–04 for saves |
| P1 | CP-11 Execution, terminal and command menus | CP-00; CP-04 for release |
| P2 | CP-12 Remote locations and shared provider model | CP-03–04, CP-09 |
| P2 | CP-13 Volume archive completion and restore | existing archive plan; CP-03–04, CP-09 |
| P2 | CP-14 Final parity acceptance | CP-00–13 |

CP-00 and read-only component extraction may run alongside the owner's current
release work. Implement P0 mutation packages before enabling additional writes.
Do not postpone ordinary copy/move/delete completion for volume imaging,
remote backends or presentation enhancements. Dependency completion means
accepted evidence, not merely a merged API. Each package is separately committed.

## Mandatory reuse and ownership rules

- Use ckVision public widgets/services wherever suitable, including its editor,
  syntax profiles and terminal-window support. Verify exact APIs and platform
  support against the installed SDK before designing replacements.
- Existing concrete reuse candidates: `src/vision/edit` (`ck_vision_edit`,
  EditorWindow/TextEditor/EditorDocument and SyntaxProfileRegistry),
  `src/vision/find` (`ck_vision_find`, `ck_find_core`, find services),
  `src/vision/du`, `src/vision/json-view`, `src/vision/host`, suite shell and
  keymap. These are candidates to inspect/extract, not claims that whole app
  controllers can safely coexist without refactoring.
- Extract reusable UI/controllers and services into libraries used by both the
  original utility and Files. Do not execute `ck-edit`, `ck-find`, `ck-du` or
  other sibling binaries as integration glue; do not duplicate their UI code
  or nest an independent app/event loop. Keep standalone tools working.
- Generic multi-app widgets, document sessions, terminal facilities and useful
  public extension points belong upstream in ckVision. File identities,
  preservation policies, operation journals and protocol-specific rules belong
  in ckUtilities core/adapters. Record consumers and API rationale before an
  upstream extraction; avoid speculative frameworks.
- Qualify upstream changes, publish/consume an installed versioned SDK, update
  dependency pins and run the independent consumer gate. Never link a sibling
  ckVision source checkout or depend on private headers.
- One Files application owns focus, commands, windows and job shutdown. Use
  scoped command registration and shared document/job ownership. Disk/network
  work stays outside UI dispatch. Editor, archive, search and terminal sessions
  coordinate identity/path conflicts with mutation jobs.
- Execution of a user-requested program is a product feature in CP-11;
  shell commands are never a fallback for native file operations.

## CP-00 — Freeze the inventory and integration contracts

Deliver a feature-by-feature matrix with reference command/version, current
implementation, platform limits, package owner and acceptance evidence. Include
view/edit, create/copy/move/delete, links/attributes, masks, recursive search,
directory tree/hotlist/history, comparison, archives, remote VFS, execution,
user menus/macros and terminal behavior. Audit advanced editor/viewer commands
instead of equating syntax coloring with complete editor parity.

Inventory installed ckVision editor/terminal APIs and suite library seams.
Specify shared component targets, lifetime/focus contracts and required upstream
extensions. Split each package into bounded implementation and qualification
commits; update the matrix as facts change.

Acceptance: every reference feature maps to an accepted implementation, an open
package, or an explicitly justified non-applicable item. No missing operation
is hidden behind an aggregate “parity” checkbox.

## CP-01 — POSIX retirement and cross-filesystem moves

Implement qualified macOS/Linux identity-bound source capture/retirement using
the existing contract, including private source-side quarantine when necessary.
Specify concurrent namespace changes and open-writer behavior before enabling.
Preserve unexpectedly captured objects for reconciliation; path stat/unlink,
advisory locks or unchanged timestamps alone do not prove safe removal.

Complete cross-volume file/tree moves only after independently verified streams,
approved metadata losses, durable destination publication and a proven source
stability predicate. Remove planned directories bottom-up; retain new children,
unverified sources and unsupported objects with exact per-item outcomes.

Acceptance: native tests on all three OSes cover replacement races, open writers,
hard links, symlinks/reparse points, identity reuse, disconnects and crashes
before/after source capture. No source retirement before verified destination
commit. Unsupported filesystems remain disabled with specific reasons. A kernel
or filesystem limitation is documented as blocked capability, never bypassed.

## CP-02 — Recursive permanent deletion and Trash completion

Add reviewed deletion of non-empty trees with frozen inventory, no link/mount
traversal, per-item identity checks and bottom-up retirement. Confirm exact
scope and irreversibility; block namespace roots, volume rows and operation
artifacts. Do not include newly appeared children. Cancellation reports already
removed items and retained remainder; there is no secure-erasure claim.

Current implementation checkpoint: the native worker and `CKDE` recovery
evidence are implemented. The worker journals every planned/prepared/removal
intent/retired/complete boundary, revalidates each held parent and child
identity, and processes the sealed inventory in reverse order. The Files
command surface routes recursive directory deletion through that worker on the
qualified Windows path; POSIX recursive retirement remains disabled
with a status reason. Native Trash is still single-entry only, so recursive
Trash remains disabled rather than falling back to permanent deletion.

Qualify recursive Trash/Recycle Bin on native and foreign mounts, including
restore metadata, permissions, collisions, quota and unsupported shares. Restore
through the OS must recover fixture bytes/metadata. No fallback to permanent
deletion. Link deletion removes the link alone.

Acceptance: adversarial tree replacement, permission changes, new children,
mount substitution, partial failure and cancellation cannot delete an unplanned
object; every outcome and interrupted tree is reviewable after restart.

## CP-03 — Recovery as a usable workflow

Unify operation and archive recovery presentation while keeping their protocols
distinct. Provide inspect, identity-proven cleanup, revalidated retry/resume and
publication reconciliation only for states with sufficient native receipts.
Extend the current Windows-only unpublished archive cleanup boundary to
macOS/Linux before exposing it there. Legacy/corrupt/ambiguous records remain
inspect-only. No automatic source deletion or pathname-based cleanup.

Acceptance: kill/restart at every intent/effect/receipt boundary, torn journals,
missing mounts, replaced staging objects, old versions, repeated recovery and
post-publication flush failure. Repeated actions are safe; completed copies are
not recopied or sources retired based on an uncertain receipt. UI explains
remaining effects and manual reconciliation without promising global rollback.

## CP-04 — Close the actual safety and release matrix

Carry forward all unclosed WP-07/WP-08 gates: native NTFS/APFS/ext4 plus
Btrfs/XFS, FAT32/exFAT, SMB/NFS, removable/read-only/provider storage, feature
loss and both cross-filesystem directions. Record actual OS, driver, options and
fixture features. Add 100,000-entry directories/trees, low memory, bounded index
storage, quota/full disks, repeated cancellation, crashes and controlled storage
fault tests. Set measurable responsiveness/memory budgets before acceptance.

Acceptance: native independent verification, relevant fuzz/ASan/UBSan/TSan gates,
no unresolved unsafe retirement/silent loss/recovery defects, and real-window
keyboard/mouse/restricted-key acceptance on all OSes. Record build duration,
source/assets/output fingerprints and exact running process under standing
workspace/freshness rules. Missing infrastructure blocks the affected claim.
This gate is repeated for each later package's added capabilities.

## CP-05 — Missing elementary file operations

Add new empty file; hard-link creation; absolute/relative symbolic links;
inspect/edit symlink target; and platform-qualified junction/reparse operations.
Support individual and marked-item permissions, ownership, ACLs, timestamps and
native attributes with explicit per-field semantics and recursive scope review.
No numeric-mode approximation of foreign ACL semantics or silent elevation.

Acceptance: no-replace creation, dangling/relative links, cross-device hard-link
refusal, inherited ACLs, denied ownership changes, readonly/immutable flags and
recursive partial failures verified natively. Unknown reparse types are refused.

## CP-06 — Commander batch operations and selection

Add glob/regex marking/unmarking with case policy, selection save/restore,
destination masks for batch copy/move/rename, append only if explicitly scoped,
and rename previews. Freeze source sets; show hidden marks. Detect collisions,
cycles, case/Unicode equivalence, overwrite and self/descendant targets before
execution. Reuse the existing planner/journal and native mutation adapters.

Acceptance: cyclic renames, many-to-one masks, invalid names, mixed files/trees,
refresh/filter changes and partial cancellation preserve exact reviewed scope.
No append or destructive replacement bypasses preservation/recovery rules.

## CP-07 — Integrated search and navigation

Extract/reuse Find search UI and services in-process. Add recursive name/content
search with glob/regex, case and metadata filters, bounded cancellable results,
encoding/binary policies and search-within-results. Results retain exact native
locations; support jump to pane, view/edit, mark and reviewed operations.
Reuse DU components for directory sizes and tree navigation where suitable;
include hotlist/history, pane exchange/same-location and directory-tree workflows.

Acceptance: standalone Find/DU regression checks plus embedded focus/lifetime
tests; inaccessible directories, stale results, arbitrary native names, huge
trees and cancellation stay bounded. Revalidate results before every mutation.

## CP-08 — Compare, diff and reviewed synchronization

Provide quick size/time and thorough content directory comparison with explicit
recursion, link and timestamp-resolution policies; mark differences in panes.
Add text/binary file diff, reusing suitable ckVision views/editor sessions.
Then add one-way and two-way synchronization with explicit conflict direction,
deletion policy and preview; two-way deletion needs a durable baseline, otherwise
remain compare/copy-only. Synchronization is an extension beyond basic compare.

Acceptance: equal-size changed content, clock skew, case collisions, divergent
edits, removed files and interrupted sync never produce an inferred destructive
action. Every write uses existing reviewed jobs, revalidation and recovery.

## CP-09 — Ordinary archives

Add archive-as-directory browsing, view/search entries, create, extract, add,
replace and delete entries via a shared archive library/provider. Stage ZIP and
TAR first, then gzip/bzip2/xz and other reference formats; inventory exact codec,
encryption and read/write support in CP-00. Document licensing and unsupported
formats explicitly. No shell-out archive tools or claiming `.ckarchive` is ZIP.

Acceptance: path traversal, absolute/drive paths, symlink/hard-link escapes,
duplicate/colliding names, bombs, malformed headers, quotas and cancellation.
Extraction uses the native planner; archive updates publish a verified replacement
with recovery, preserving the old archive until commit. Remote/archive nesting
has explicit depth/byte limits. Never auto-execute content.

## CP-10 — Embedded viewer and editor

Reuse ckVision EditorWindow/TextEditor/EditorDocument and syntax profiles plus
shared ck-edit components. Provide F4/edit and new-file editing in the same app,
multiple documents, find/replace, undo/redo, encoding/newline handling, syntax
selection and safe Save/Save As. Extend streaming text/hex viewing with search,
offset navigation and large-file support. Use json-view components where useful.
Audit MC editor macros, block operations, hex editing and reference-specific
commands in CP-00; map remaining features to explicit slices in this package.

Acceptance: shared tools retain behavior; dirty buffers survive focus/navigation;
external changes, rename/delete jobs, hard links, symlinks, streams and metadata
preservation have explicit save policies. A generic SDK save API is not assumed
to satisfy Files durability. Conflict detection and safe replacement use native
operation services; cancellation/close cannot lose unsaved work silently.

## CP-11 — Execute and use a terminal inside Files

Use ckVision terminal windows and qualified PTY/ConPTY facilities for explicit
program launch, command line/history/completion, interactive shell and foreground
jobs. Add extension associations and configurable user menus with reviewed
placeholder expansion for current/marked paths. Reuse host/keymap UI and provide
return-to-panes/focus/resize behavior. Distinguish direct argv execution from an
explicit shell command; names are data, not injected shell syntax.

Acceptance: spaces/control characters/metacharacters, arbitrary native names,
working-directory identity, interactive input, resize, cancellation, child exit,
window close and terminal restoration on all OSes. No silent privilege escalation,
automatic downloaded-file execution, or hidden execution from preview. Define
how external writers invalidate plans; launching commands cannot bypass job
revalidation. Extend generic terminal APIs upstream if required.

## CP-12 — Remote locations and provider parity

Introduce a shared bounded provider contract for local/archive/remote browsing
without weakening native guarantees. Implement SFTP first, then FTP/FTPS and
MC-style shell transport as separately qualified adapters; inventory optional
SMB/extfs/package providers and reference applicability in CP-00. Mounted shares
remain distinct from protocol clients. Do not execute sibling utilities.

Acceptance: host-key/certificate verification, secret storage/redaction, credential
expiry, reconnect, partial transfers, uncertain commit, cancellation, remote path
encodings and metadata limits. Per-provider capabilities determine copy/move/
delete/edit availability. A reconnect cannot turn an uncertain destructive action
into a blind retry. Expose exact transport coverage in the parity matrix.

## CP-13 — Finish volume archives and enable verified restore

Complete existing snapshot/live-mode, native inventory, preservation and real
volume fixtures under the archive plan. Add inspect/browse and restore to an
explicit directory through CP-09/provider and native operation services, including
sparse extents, hard links, streams and platform metadata losses. Never overwrite
a device or restore a live system volume through ordinary copy commands.

Acceptance: independent format validation, hostile/corrupt archives, interrupted
restore, exact loss review, native sparse/stream inspection, snapshot lifecycle
and recovery. This is an additional ckUtilities feature, not a prerequisite for
claiming ordinary ZIP/TAR support or Commander file-operation coverage.

## CP-14 — Accept parity against evidence

Run the frozen reference matrix command-by-command on Windows/macOS/Linux,
including standalone suite regressions and installed SDK consumer checks.
Qualify combined workflows: search → edit → compare → copy/move; archive →
extract → Trash/restore; remote edit → reconnect; terminal writer → plan
invalidation; crash → recovery. Inspect the actual verified application process.

Acceptance: publish separate local-file-operation, Commander-workflow and
provider/format coverage reports. Label unavailable platform features precisely.
No broad “full parity” claim while applicable matrix rows remain open; record
explicit historical exclusions and remaining extensions. Ship current docs,
screenshots, packages and concise durable evidence, then clean obsolete builds.

## Coordination record

The user requested this roadmap and coordination with the `file manager` peer
on 2026-11-01, then explicitly required ckVision and suite component reuse.
The peer was sent the proposed priority order and reuse constraints. This chat
owns this new file and the roadmap link while preparing the handoff; the peer
retains active implementation/build ownership. No builds or app launches were
started by this planning task. The review is recorded in
[ck-files-parity-inventory.md](ck-files-parity-inventory.md). It confirms
CP-00 and WP-02 implementation facts, keeps POSIX cross-volume retirement and
recursive deletion disabled until their P0 protocols are journaled and
qualified, and orders the remaining work as CP-01 through CP-04 before the
Commander feature packages.
