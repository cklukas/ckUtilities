# Files — WP-00 behavioral and adapter contracts

Status: implementation specification. All native capabilities remain unqualified
until their adapter tests pass. This document refines [the work packages](ck-files-plan.md)
without enabling file operations. Dated 2026-10-23.

## Dependency and ownership decisions

Consume ckVision 1.4.0 (`a791f103c3bc67f9fe398cf5c5010f7c460ae7ba`)
through the installed `ckvision::cvision` target. The following public headers
and methods were inspected in that exact release:

| Contract | Public API and application use |
| --- | --- |
| Details view | `cvision/widgets/table.hpp`: `TableModel::row_count`, `row_id_at`, `index_of`, `cell`, `request_sort`; publish prepared, bounded cells and call `Table::model_changed` on the UI thread. Disable cell editing; rename is a reviewed action. |
| Split layout | `cvision/widgets/splitter.hpp`: `Splitter`, `set_split_position`, `set_resize_anchor`, `set_anchored_extent`, `on_split_moved`; preserve the user's split and adapt the real pane views at narrow widths. |
| Path field | `cvision/widgets/input_line.hpp`, `completion.hpp`: `InputLine::set_completion_provider`, `completion_changed`, `select_completion`, `accept_completion`; Files supplies asynchronous candidates from its own lossless discovery model. |
| Thread handoff | `cvision/ui/application.hpp`: `Application::post`; publish by job/session/generation identity, with a weak lifetime token. Never capture an unguarded controller pointer. |
| Commands | Existing `SuiteCommandScope` and suite keymap catalog; one `ck.files.*` registration drives enablement and presentation. |
| Terminal/clock | Existing `ck_vision_host`, with injected native terminal and clock; deterministic `HeadlessTerminal` and manual time in tests. |

The SDK's `ckv::FileEntry` contains only a UTF-8 name and directory flag;
`FileSystem::exists` conflates inaccessible and absent objects. Its
`FileFingerprint` is not a cryptographic content verification token.
`DirectoryService` exposes pages of 64 entries/32768 text bytes, a 4096-byte
name bound, and name-oriented ordering, rather than full Files metadata/sorts.
These APIs must not be misrepresented as the mutation engine or the complete
Files directory model.

Use a Files-owned discovery service and disk index for its richer domain, with
the public `TableModel` and `CompletionProvider` extension interfaces. This is
application functionality, with no private ckVision access or alternate UI
renderer. Native names unsuitable for text entry remain selectable through
identity-backed rows; do not feed display escapes into path parsing. Existing
SDK dialogs may be used only where their supported-name and synchronous versus
asynchronous contracts match the caller's needs.

Host lifetime order: native adapters and worker/index owners → Application →
Files controller/views. Destruction reverses that order; retire sessions without
joining inside dispatch, destroy Application after all views, then join workers
and remove exclusively owned scratch indexes. Keep a separate operation worker
from discovery; coalesce pending discovery requests. A blocked system call can
occupy its slot until the OS returns; show this honestly and retain its resources.

## Native identity and name model

`NativeName` is tagged POSIX bytes or Windows UTF-16 code units; represent it
without locale conversion, normalization, separator rewriting, or truncation.
The tagged representation is portable to fake adapters on any test host. Native
directory handles plus one validated basename authorize child operations.
User-entered UTF-8 paths are converted once with explicit errors; internal
enumerated names never round-trip through that conversion. Escape invalid
encoding, controls, bidi controls and escape delimiters visibly and injectively
for display; retain the native identity in every row/action.

Object identity includes adapter instance, mount/volume identity, native file
identifier and available generation information. Names identify directory
entries; hard links identify the same object through different entries. Both
identities matter. SDK row IDs are session-owned nonzero handles into this model,
not hashes assumed collision-free or inode numbers assumed globally unique.
Recycled IDs cannot retarget a selected entry. Removed entries become stale;
refresh must not silently replace the selected source with a new same-name file.

All identity/capability/inventory results distinguish Available, Unsupported,
Unknown, Denied, Gone, Changed and Failed. Absent metadata is only established
by successful complete enumeration. An inaccessible ACL or stream list is
unknown, not empty. Preserve native error codes separately from sanitized UI
diagnostics. Never use a generic false boolean for these outcomes.

## Filename transport and Win32 convention diagnostics

`map_native_name` accepts a native basename, never display text. Same-encoding
mapping preserves its exact representation, including malformed Unicode.
Cross-encoding mapping requires strict Unicode scalars: POSIX input is UTF-8,
Windows input is UTF-16, and Windows-to-POSIX output is UTF-8 bytes explicitly.
No locale conversion, replacement, normalization, case folding, BOM removal,
WTF-8 or surrogate-escape fallback occurs. Unrepresentable names/components
return Unsupported; no partial name is returned. Native storage budgets apply
independently to input and output (64 KiB default, 1 MiB hard ceiling), with
Unknown on exhaustion and Failed for invalid configuration/source/tag input.
Those resource budgets are not filesystem name/path limits.

The strict scalar codecs are shared with native display preparation. Display
still escapes malformed units and unsafe/control text injectively; this is not
an encoding that operations parse back into names. Component factory predicates
also serve `NativeName::is_valid`. Moved-from objects can lose their component;
consumers reject invalid states before lookup, held-child observation, pane
publication or mapping. Unicode validity and native component validity remain
separate concepts.

`check_win32_component` reports generic convention risks using bounded result
fields: empty/navigation spellings, forbidden/control units including stream
syntax, device aliases and trailing dot/space rewriting. Device recognition is
shared with the existing Windows absolute-path inspection guard. A counted NT
child name can identify a real literal file even when Win32 conventions flag it.
Neither an ordinary-convention result nor successful name transport authorizes
creation, establishes destination acceptance or proves collision freedom.
Native filesystem, driver, directory case/normalization/alias semantics and the
actual operation/API path limits still require independent checks. No helper
silently sanitizes or renames a component to make a destination work.

## Staged pane discovery and completed publication

`PaneListingJob` owns discovery and private staging on one worker. Only an actual
End creates an initial `CompletedPaneListing`, whose private construction binds
immutable rows, the original pane scope/generation and the directory identity
observation. The pane checks that generation before allocating publication
context; stale and foreign completions cannot replace rows or consume a newer
request. Directory observation and rows are applied together with all allocating
work completed first. This is single-owner exception safety, not concurrent
thread-safe access to a mutable pane.

Cancelled batches can contain rows: they remain private and the job becomes
Paused until explicitly resumed or discarded. Enumeration/resource failure,
exact-name duplicates, mixed encodings and exceptions discard staging and close
discovery. Exceptions mark the job failed before rethrow. None of those states
can produce a completed payload or overwrite the published pane. The receiving
pane also validates its own limits; it may be stricter than the staging job.

`PaneEntry::from_listed` preserves whole-entry metadata errors separately from
unknown basic fields. Rows and operation-source candidates retain that error;
no permission denial or disappearance becomes a successful empty metadata value.
Candidates remain review inputs and do not grant mutation authority. Hidden
policy is explicit: Linux dot names; Darwin dot names plus observed UF_HIDDEN;
Windows observed hidden attribute. Unknown/denied flag observations stay visible.
This does not implement desktop-specific hidden-name lists. Exact native-unit
ordering is shared with pane sorting and is not destination collision equivalence.

Staging enforces the existing row, native-name and eventual-display-byte caps.
The duplicate index keeps a second bounded name copy plus node overhead. A
receiver can retain its old pane, completed payload and prepared projection
simultaneously; these caps are not a global process-memory quota. Allocation
failure remains explicit, without partial publication or unlimited-memory
fallback. Disk-backed indexes remain required for larger views.

A completed payload neither retains the source directory handle nor attests
that an original pathname still resolves to it. Navigation labels/context need
their separately qualified binding. These discovery rows are not sealed
HeldChild refresh successors, so the existing invalidation policy applies to
unproven selection continuity. Selected-entry refresh integration, scheduling,
large indexes and UI ownership/publication remain unfinished. No snapshot,
provider hydration or mutation guarantee is implied.

[Pane-job evidence](evidence/ck-files-pane-job-20261023.json) includes injected
publication/failure tests and native streaming-to-pane tests on all three hosts.

## Listing cancellation and worker resource ownership

`PaneModel::cancel_listing` revokes only the current request from that pane.
Foreign, stale and zero requests are rejected without changing the pending
request. Revocation preserves published rows, directory context, focus, marks
and selection invalidation diagnostics. A late success or failure cannot apply;
a subsequent listing receives a new generation. This operation belongs to the
pane owner and performs no native I/O.

`PaneListingJob::abandon` belongs to its owning worker, after `advance` returns.
It releases the cursor, any prepared child, staged rows, duplicate-name index,
directory observation and the job's reference to a completed payload. Abandoned
jobs cannot resume I/O or produce a completion. Repeated abandonment is a no-op;
a failed job instead retains its original diagnostic. Failure and successful
completion also release private staging and its accounting state.

These are separate responsibilities: abandoning a job cannot revoke an
immutable payload already handed to another owner. The receiver must revoke
the generation to reject queued late results. A stop token only requests a
cooperative pause; abandonment discards the private scan. Neither API permits
concurrent access to a job, interrupts a blocked native call or makes immediate
handle release possible during that call. Scheduling and owner-thread wiring
remain future work.

[Cancellation evidence](evidence/ck-files-cancellation-20261023.json) covers
prepared-child cleanup, stale completion rejection, retained diagnostics and
native restart on all three hosts.

## Injected monotonic time and cooperative slices

`MonotonicClock` supplies process-local steady-clock readings through shared
ownership; the native implementation uses `std::chrono::steady_clock`. Readings
are nondecreasing, with an arbitrary epoch, not UTC, file timestamps or durable
journal values. `CooperativeTimeSlice` accepts a positive native clock duration.
It checks chronological order before unsigned elapsed subtraction, avoiding
signed range overflow and deadline addition. Negative epochs are valid. Observed
backwards readings fail permanently, including when discovery begins its next
slice. Clock exceptions poison discovery before being rethrown.

Directory discovery accepts a positive slice up to one second, default 20 ms.
The injected clock is checked between bounded cursor-read/open/inspection stages.
More with `time_slice_exhausted` indicates a detected time yield. This is not a
hard deadline: cursor reads can include bounded skipped-record loops, and any
native/provider call can block beyond the slice. Root opening is a separate
worker call. No process interruption or automatic scheduling is implemented.

A successful slow open may yield with at most one privately prepared child
handle, allowing the next slice to inspect that same object rather than repeatedly
reopening without progress. A completed inspection closes the handle before its
row is published. Cancellation closes it and retains the uninspected component;
explicit resumption reopens the component as a fresh observation. Clock failures,
exceptions and destruction also release it. Results expose no child handle or
mutation authority. Prepared handles can restrict ancestor renames on Windows;
future worker/mutation coordination must respect those restrictions without
silently dropping qualified identity pins or weakening sharing protection.

[Clock and slice evidence](evidence/ck-files-clock-20261023.json) covers exact
boundaries, negative epochs, extreme signed-representation ranges, reversal
within/across slices, injected exceptions, slow stage progress, cancellation and
native directory resumption. Existing fake discovery tests use manual time;
no sleeps or mutation of the host clock are required.

## Bounded directory discovery

`DirectoryListing` owns one cursor and runs on one worker. Its root factory uses
the injected `BasicFileSystem` with explicit final-link rejection; a caller can
also transfer an already opened relative directory cursor. Reads use that
cursor and metadata-only child opens, without absolute path reconstruction,
cwd changes, content reads or privilege changes. Temporary child handles close
after each inspection. Returned rows contain lossless names and explicit
`Observation<BasicMetadata>`, retaining entry-specific denial, disappearance,
unknown and other errors instead of dropping the names.

Per-batch row and native-name-byte limits have hard ceilings, with one pending
component bounded separately and at most one prepared child across time yields. UTF-16 storage is counted in bytes for this resource
budget, not filesystem length units. A consumed component that cannot fit the
remaining batch budget is retained for the next batch. An individually excessive
component produces terminal Error, never a truncated name or completed listing.
The scanner does not retain all batches; consumers must separately bound their
projection/index storage. Disk-backed indexes remain required for large views.

Cancellation is cooperative between cursor-read, child-open and inspection.
A consumed but uninspected name remains pending, and a cancellation arriving
during inspection can return completed rows with Cancelled. Consumers must
process returned rows for every state. Enumeration/resource failures are sticky
Error; only actual cursor exhaustion is End. Exceptions after consumption poison
the stream before rethrow, so later reads cannot pretend a lost batch completed.
A bounded native read containing only skipped records yields More immediately,
without a loop waiting indefinitely for a visible name.

End is not a snapshot or proof of complete preservation metadata. These rows do
not retain child objects and cannot establish held-child selection continuity
or authorize mutation. Native calls can block; there is no instant-cancel claim.
[Discovery evidence](evidence/ck-files-listing-20261023.json) records injected
fault/cancellation/ownership tests and actual native streaming on all three hosts.
Worker scheduling, large-view indexing and UI publication remain separate
unfinished work; discovery time injection is specified above.

## Relative directory enumeration cursors

`DirectoryCursor::open_directory_child` opens one lossless native component
through its held parent and returns an independently owning enumeration cursor.
It always rejects the final link/reparse point, uses native-default name
resolution and never joins an absolute path or changes cwd. The default adapter
reports Unsupported. No implicit follow, path fallback or weaker retry exists.
The returned identity is in the same adapter scope; opening captures the object
present then, without certifying an earlier listing or future mutation target.

POSIX uses read-only `openat` with O_DIRECTORY, O_NOFOLLOW, O_NONBLOCK, O_NOCTTY
and O_CLOEXEC, then fstat/fdopendir with explicit ownership transfer. ENOTDIR
is a failed directory request, not evidence of absence. Windows uses counted
relative NtCreateFile with FILE_OPEN, share-read/write/delete, list/attribute /
synchronization rights, and reparse/no-recall options. It checks FileBasicInfo
on the opened handle and rejects reparse/non-directory objects before exposing
an enumeration cursor. FILE_DIRECTORY_FILE's restricted compatible-option set
must not be combined with incompatible options. Neither platform grants write
access or enables privileges.

[Native evidence](evidence/ck-files-directory-child-20261023.json) covers held
parent replacement, child rename/replacement after parent destruction, independent
parent/child enumeration, Unicode and unusual raw names, final link/file/FIFO
rejection, and traversal through 48 components of 100 bytes. That relative
pathname exceeds 4096 bytes; production traversal supplies only each component.
Private deep-tree fixtures create/clean through appropriate native mechanisms.

This API does not implement recursive walking, mount confinement or parent
navigation. A mount/provider call may block, and no universal no-hydration claim
is made. Mount/location and provider-policy checks remain separate. Holding the
child does not retain its parent or grant namespace mutation authority. All
cursor I/O belongs on workers, with cancellation only between native calls.

References: [POSIX openat semantics](https://man7.org/linux/man-pages/man2/open.2.html)
and [public Windows NtCreateFile contract](https://learn.microsoft.com/en-us/windows/win32/api/winternl/nf-winternl-ntcreatefile).

## Reported native component lengths

`assess_component_length` compares the exact native component storage length
with the applicable reported bound from `LocationInfo`: POSIX bytes for Linux /
Darwin, UTF-16 units for Windows. It returns the measured count, reported maximum,
unit and parent identity observation. It performs no normalization, conversion,
truncation or allocation sized by the reported bound. Invalid encoding/family
and zero bounds fail; unavailable queries preserve their native error and state.

An assessment within the reported maximum is not proof of native name acceptance
or permission. Supplemental character-count restrictions, normalization, aliases,
complete paths, native request-buffer limits and operation capability require
separate checks. Directory identity context is not a retained parent handle.
The operation planner must not treat this single comparison as authorization.

[Component-length evidence](evidence/ck-files-limits-20261023.json) qualifies
ASCII boundary fixtures on the actual APFS/ext4/ReFS locations. Each accepts the
reported maximum of 255 and rejects the next length through native lookup.
The Windows fixture uses explicit extended paths to avoid conflating the
ordinary Win32 full-path convention with component length; independent Win32
lookup also rejects the overlong name. ReFS returns STATUS_OBJECT_NAME_INVALID,
while the tested POSIX hosts return ENAMETOOLONG. Neither is a Missing result.

Native references: [Linux fpathconf](https://man7.org/linux/man-pages/man3/fpathconf.3.html),
[Darwin pathconf](https://developer.apple.com/library/archive/documentation/System/Conceptual/ManPages_iPhoneOS/man2/pathconf.2.html)
and [Windows volume query](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-getvolumeinformationbyhandlew).
Native reports do not establish all filesystem acceptance rules.

## Native child lookup observations

`DirectoryCursor::lookup_child` requires a native component and an explicit
comparison request. `NativeDefault` delegates to native system/filesystem
semantics; it does not guarantee exact stored spelling. `WindowsIgnoreCase`
requests public `OBJ_CASE_INSENSITIVE` through counted-relative `NtCreateFile`.
POSIX reports Unsupported for that request without Unicode folding or an
enumeration fallback. Invalid components, encodings and enum tags fail explicitly.

A Found result owns the final opened object, without following its final link
or reparse target. It retains the requested native name, comparison request and
parent identity observation. Missing retains the original native error and is
produced only for Gone plus POSIX ENOENT or NT STATUS_OBJECT_NAME_NOT_FOUND /
STATUS_NO_SUCH_FILE from the qualified relative query. Path-not-found, denial,
delete-pending and generic Gone do not establish a missing child. The default
adapter reports Unsupported rather than deriving absence from legacy queries.
Moves leave the source Unknown without stale context or ownership.

The parent identity in context does not pin the parent directory. Callers must
retain and revalidate their parent authority. Neither result reserves a name,
grants write permission, nor proves a future create succeeds. Future execution
must use its separately qualified exclusive publication protocol. Found may
outlive its parent cursor, rename and unlink. Equal IDs from distinct hard-link
names do not prove namespace alias equivalence. The result records requested
spelling, not canonical stored spelling. Collision equivalence for uncreated
names, short aliases and complete path restrictions remain WP-01 work.

[Native lookup evidence](evidence/ck-files-lookup-20261023.json) records real
held-parent replacement, dangling links/junctions, owning-child lifetime and
case/normalization observations on APFS, ext4 and ReFS. These observations are
specific to the tested hosts and directories.

## Held-child refresh continuity

`HeldChild` owns one opened child and its held parent directory, and records the
exact native basename and immutable basic-metadata observation. Factory objects
are allocated const; copy/move construction and assignment are disabled so a
caller cannot introduce a mutable alias through a public special member. Creation always
starts an independent family, even if another observation has matching numeric
IDs. Refresh reacquires the same basename through the same held directory with
MetadataOnly access and without following the final link. It keeps old and new
handles alive together, re-inspects the old object, and compares the original,
current-old and fresh identity/kind observations. Only qualified Same identities
and unchanged observed kinds create a later sealed family revision. Reusing a
cached snapshot is not a successor. Unknown/denied/errors and observed changes
remain explicit; zero native object IDs never establish equality. Refresh does
not retain an unbounded chain of prior handles.

Pane publication consumes these observations without filesystem I/O. A proven
successor with the same basename can retain its row handle, mark and cursor;
other old sources remain explicitly invalidated. Metadata for a bound pane row
comes from its immutable held observation. A caller cannot substitute another
basename or independently open a matching-ID object to forge continuity.
Retained hidden marks still require review. Row lookup uses full integer keys
with equality checks; hash collisions are not treated as identity. This is
selection continuity under the native identity contract, not content stability,
a frozen pathname-membership snapshot or mutation authority. Operations must
still freeze and revalidate sources through their own engine.

The pane bounds retained observations separately from listing rows (default
1024, configurable up to 4096). Actual native handle/resource errors may occur
earlier and must stay explicit. A caller must not silently discard/reopen pins
to bypass a resource or mutation restriction. On the qualified Windows/ReFS
fixture, holding a child prevents classic parent-directory rename with
ERROR_ACCESS_DENIED even with delete sharing. Mutation packages must account for
this limit in their plan/review and recovery semantics; they cannot claim POSIX
rename behavior or automatically release safety holds and retry. macOS/APFS and
Linux/ext4 qualify parent rename while the child is held; all three hosts
qualify refresh through a held parent whose path was already replaced. This
qualification does not establish arbitrary remote/virtual filesystem behavior.

## Inventory and preservation decisions

Inventory every selected object, including directories, additional streams,
attributes and relationship groups. Record discovery completeness and feature
enumeration completeness independently. Bound in-memory pages; spool full
inventories to a private indexed store. Hash/serialize lengths with checked
arithmetic, reject corrupt indexes, and never decode native names as UI text.

A feature record contains category, exact native feature identifier, source
value/digest or explicit unreadable state, source object identity/version,
destination capability evidence, planned treatment and verification method.
Categories include main data, additional data channel, xattr, access/security,
ownership, times, flags, symbolic link, hard-link relationship, sparse extents,
compression, clone sharing, encryption, provider state and native name.

Destination capabilities are scoped to parent identity, mount/driver, current
credentials, operation and object kind. Labels such as NTFS or APFS are hints;
per-directory settings and server behavior take precedence. Query first and
use exclusive non-destructive probes where query evidence is insufficient.
Probes never change user objects or request extra privileges. Cache only within
the validated evidence generation; revalidate at the operation boundary.

| Planned treatment | Meaning and user decision |
| --- | --- |
| Preserve exactly | Verified native representation and value are retained. No loss consent needed. |
| Preserve semantically | A documented platform-specific equivalence holds; identify it in the review. ACL translation between unrelated principal models is never assumed equivalent. |
| Change representation | Main data stays identical but sharing/allocation/compression changes; show size/security consequences and obtain specific consent. |
| Omit or round | Show the exact feature/channel/time precision and affected objects. Additional-channel omission is data loss. Obtain explicit consent; distinguish move source removal from copy. |
| Cannot establish | Explain what could not be inspected or preserved. Continue only for a bounded, understood risk that the user can meaningfully approve; otherwise block. |
| Cannot safely perform | Invalid/unrepresentable name, impossible file size, unidentified object, unreadable main data, unsafe target substitution, or failed verification: hard failure. Consent cannot turn it into success. |

Consent includes plan ID/revision, operation, exact source and destination
identities, feature differences, affected item set and consequence classes.
The planner supplies an immutable review token; the UI cannot create generic
authority by passing `true`. Cancellation yields no authority. Changed evidence
invalidates consent; a retry must replan. A per-job repeated-conflict choice
does not cover a new preservation difference. Consent is journaled privately
for that operation and is never imported as a global preference.

Security properties need their own review: preserving bytes while changing who
can read them, decrypting EFS data, or dropping a quarantine/security attribute
must never be described as equivalent preservation. New transaction content is
private from exclusive creation through metadata application; do not expose it
under permissive inherited access while waiting to apply the final ACL.

## Transaction and recovery state machine

Each item has a durable, monotonically sequenced record. Legal transitions:

1. **Planned** — complete inventory, reviewed destination and accepted decisions;
   no user content modified.
2. **Prepared** — private transaction object exclusively created under a pinned
   destination directory; object identity and ownership receipt persisted.
3. **Copied** — all agreed streams written; no published success yet.
4. **Verified** — independent destination reads match source digests for every
   agreed data channel; metadata/relationships satisfy the accepted contract;
   source changes checked and final file flush succeeds.
5. **Commit intended** — exact publication/replacement action and preconditions
   durably recorded, including recoverable old-target identity when replacing.
6. **Destination committed** — native publication outcome reconciled by identity,
   destination namespace durability established and recorded.
7. **Source removal intended** (move only) — revalidated original content,
   accepted losses, complete destination receipt and source identity persisted.
8. **Source retired** (move only) — source safely captured/removed using the
   adapter's qualified identity protocol; directory removal never includes new
   unplanned children. Record partial source retention honestly.
9. **Complete** — all required steps confirmed; cleanup only by ownership receipts.

Any step can yield Failed, Cancelled, Retained source or Needs reconciliation.
An unknown syscall outcome cannot become Failed-with-no-effects merely because
an API returned an error. Inspect by pinned identities; never retry a rename,
delete or replacement blindly. Journal corruption, missing journal writes,
unavailable mounts or unexplained identities require preservation and inspection.
If a crash occurs between an action and its receipt, its prewritten intent is
the recovery anchor. Torn/truncated tail records are detected with framing,
length limits and integrity checks. Older committed records remain readable.

Ordinary POSIX `stat` followed by `unlink` is not an atomic compare-and-delete.
Design the source retirement adapter around pinned parents and exclusive
private source-side quarantine/capture where necessary, verify the captured
identity before deletion, and retain any unexpectedly captured object for
reconciliation. Do not advertise a race-free delete until adversarial rename
tests pass. Native handle-based Windows disposition must receive equivalent
tests for reparse, sharing, identity reuse and mount changes.

Content verification does not imply a point-in-time snapshot of a file being
modified by another process. Report whether an enforced sharing lock, stable
snapshot, or only before/after observations are available. Define and test
the source-stability predicate before enabling cross-filesystem source removal;
if stability cannot be established under the supported guarantee, retain the
source and report copy completed / source retained. Advisory locks alone cannot
prove exclusion of noncooperating writers. Do not solve this by disabling
verification, silently authorizing data loss, or claiming stat equality proves
unchanged content. Resolving this adapter proof is a WP-04/05 enablement gate.

Replacements need an explicit native publication protocol with recoverable old
content, namespace durability and crash reconciliation. Multi-step renames are
not a global atomic transaction; state intermediate visibility accurately.
Do not use remove-then-rename. Tree jobs commit item by item and return the
complete outcome partition, never one boolean that hides partial changes.

## Native API research and qualification targets

The following are API candidates, not claims of implemented guarantees. Check
SDK/OS availability and exact semantics in native conformance tests before use.

| Platform | Adapter strategy to qualify |
| --- | --- |
| Windows | Wide native paths, `CreateFileW` handles and sharing modes, `GetFileInformationByHandleEx` for IDs/streams, native security APIs, explicit reparse classification, handle-based rename/disposition, flush and private transaction directories. Use native stream enumeration/copy/verification; do not assume `CopyFile2` preserves security. Separate tests for EFS, inherited ACLs, sparse/compressed files and network shares. |
| Linux | `openat`/`fstatat`/`readlinkat` with no-follow policy and pinned parent descriptors; `statx` where available; fd-based xattr/ACL access; checked streaming and sparse extents; `renameat2` no-replace/exchange where actually supported, file and directory `fsync`; source retirement protocol above. libc/kernel/filesystem support is queried and tested; no shell fallback. |
| macOS | Pinned POSIX descriptors plus Darwin xattr/ACL/flags APIs, `fcopyfile` under an explicit policy with independent verification, qualified native rename/exchange and durability behavior, native Trash API. Resource forks are data channels even when exposed through xattrs. Qualify AppleDouble behavior on non-native volumes rather than assuming it is harmless. |
| All | No privilege elevation, shell execution, automatic cloud hydration, undocumented private API, or assumption that a mounted share has local-disk guarantees. Native errors may represent indeterminate remote outcomes. |

Primary sources reviewed for these decisions:

- [Microsoft CopyFile2](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-copyfile2): its copy behavior does not provide the full Files preservation contract.
- [Microsoft handle information](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-getfileinformationbyhandleex): typed stream and native identity queries.
- [Linux rename](https://man7.org/linux/man-pages/man2/rename.2.html): filesystem-dependent flags and indeterminate NFS error outcomes.
- [Linux fsync](https://man7.org/linux/man-pages/man2/fsync.2.html): a flushed file alone does not establish directory-entry durability.
- [Apple copyfile manual source](https://github.com/apple-oss-distributions/copyfile/blob/main/copyfile.3): native copy controls are inputs to an adapter, not a replacement for independent preservation checks.
- [freedesktop Trash specification](https://specifications.freedesktop.org/trash/1.0/): native interoperable layout and per-mount handling.

Trash qualification must prove that an unavailable native trash path never
becomes permanent deletion and that the OS facility can restore its result.
The permanent-delete command requires independent explicit consent and a
qualified non-following retirement protocol. A general Undo action is absent.

## Test evidence required before mutation enablement

Every feature category needs exact-preserve, accepted-loss, refused-loss,
inaccessible-inventory and source/destination-change tests. Each destructive
state transition needs injected syscall/journal/flush failure and restart tests.
Use independent native inspection for all stream/ACL/metadata preservation
claims, not the writer's own success flag. Mocked capabilities establish planner
behavior; real filesystem tests establish backend capabilities.

WP-00 can finish after dependency validation and this specification are reviewed
against public APIs. Subsequent packages must resolve their explicit adapter
proofs in order; unknown preservation, retirement or durability cannot be
papered over by an enabled command. The release matrix in the plan remains
mandatory; a passing macOS run does not qualify Linux or Windows.
