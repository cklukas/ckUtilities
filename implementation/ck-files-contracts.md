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

A default completed payload releases the source directory handle; refresh
preparation can explicitly retain it. Neither mode attests that an original
pathname still resolves to that directory. Navigation labels/context need
their separately qualified binding. Raw discovery rows are not sealed HeldChild refresh successors. PaneRefreshJob
can prepare successors for previously held entries; unproven continuity still
invalidates selection. Initial pinning, scheduling, large indexes and UI
ownership/publication remain unfinished. No snapshot,
provider hydration or mutation guarantee is implied.

[Pane-job evidence](evidence/ck-files-pane-job-20261023.json) includes injected
publication/failure tests and native streaming-to-pane tests on all three hosts.

## Sealed bound-entry refresh preparation

`PaneListingJob` can opt into `ListingDirectoryRetention::RetainForRefresh`.
Its completed native-End payload then retains the exact scanned directory cursor;
`retains_scan_directory()` reports only that pin. Default ReleaseAtEnd behavior
is unchanged. `DirectoryListing::pin_directory` shares lifetime without providing
mutable enumeration. Every native owner, including retained scan pins and held
row bindings, must retire on the worker. A false scan-pin indicator does not
make a payload containing held entries safe for UI-thread destruction.

`PaneRefreshJob` consumes this sealed, retained discovery result plus prior
immutable HeldChild bindings (available through worker-side `PaneModel::held_entries`).
Both parent handles remain alive during adapter-scoped native-ID comparison.
Unknown/different parent observations decline old-child I/O and produce named
issues. Only a live HeldChild refresh can produce a sealed successor; cached
names/IDs or independently opened observations cannot preserve selection through
the receiving pane's family check. These remain native identity observations,
not pathname attestation, a content snapshot or mutation authority. Windows'
[FILE_ID_INFO contract](https://learn.microsoft.com/en-us/windows/win32/api/winbase/ns-winbase-file_id_info)
describes its host-local ID comparison; no cross-host/durable identity is inferred.

Preparation incrementally validates/copies rows and matches prior names, then
refreshes bindings. Each advance performs at most 256 preparation/binding steps
and checks a shared injected monotonic clock (20 ms default, positive through
one second). Each component is capped at 1 MiB, with configured pane row/name /
display/held-count limits. Previous bindings are bounded before capture; overflow,
null/duplicate bindings, bad model transport and allocation failures cannot
publish a partial listing. These are retained-state/count bounds, not a global
memory quota. Scanned rows, old pane, staged rows and the result can coexist.

A slow completed binding observation is committed privately before yielding, so
resumption does not repeat it merely because its slice expired. Cancellation is
checked during copying and between native calls in HeldChild observation/refresh;
new private children close on cancellation. Paused jobs can resume; abandonment
and fatal failures discard private state. Exceptions poison the job before
rethrow. The poison descriptor uses empty diagnostic text and nonthrowing payload
construction/destruction, avoiding allocation while handling bad_alloc. The
[optional emplace contract](https://eel.is/c++draft/optional.assign#lib:emplace,optional)
permits exceptions only from constructing its payload, whose move is statically
checked here. Native calls and individual allocation/encoding steps can block or
outlast the cooperative slice; no hard deadline is claimed.

Fresh held metadata replaces a row only through its actual refresh result;
hidden policy is recomputed. Replacements, failed/unqualified continuity,
unlisted prior names and parent mismatches carry `PaneBindingIssue` details.
An unlisted name means not observed in this scan, not definitive native absence.
The final result requires the original generation gate and releases its own
scan pin; externally retained input payloads keep their own pins. Successful
publication applies rows, directory context and issue details together. It
preserves marks/focus only through sealed families. Hidden marked rows still
require review. Failed/cancelled/newer requests cannot overwrite the published
state. Current issue details survive failure/cancellation; successful publication
replaces the report, while unacknowledged selection invalidation counts persist.
Explicit valid selection commands acknowledge/clear those diagnostics.

This phase handles entries already bound before refresh. Initial selection
pinning, UI ownership/scheduling, large-view indexes, operation planning and
execution-time revalidation remain required work.
[Refresh evidence](evidence/ck-files-pane-refresh-20261024.json) includes injected
lifecycle/fault/resource tests and native bound-selection, replacement and
cross-directory tests on all three platforms.

## Held-directory entry probes

`DirectoryEntryProbe` integrates native child lookup, location observation,
reported component length, generic Win32 spelling diagnostics and existing-child
metadata. Its factory and refresh run on a worker. The record owns the held
parent and any found final child; native-owner destruction also belongs on the
worker. It is not an immutable UI projection. All fields are observations of
individual calls, not a filesystem snapshot or mutation authority.

The component byte budget (64 KiB default, 1 MiB hard maximum) is checked before
copying/validating the name; UTF-16 storage counts two bytes per unit. Invalid
parent/mode/limits/name fail before native queries. An oversized component gives
Unknown/InventoryLimit without a prefix or inspection queries. No display decoding, native-name
conversion, path joining or process-cwd change occurs. Reported filesystem length
bounds and Win32 conventions remain separate diagnostics and never override
actual read-only lookup behavior.

An Available record does not mean an available destination name. Location errors
remain visible while lookup can proceed independently. Lookup errors propagate
to existing metadata; a found child's metadata can separately fail while its
handle remains retained. Missing has explicit native lookup evidence and
NotApplicable existing metadata. It neither reserves the name nor promises that
creation will succeed. New files and permission/mount changes can invalidate an
observation immediately.

Lookup replies must carry the exact requested component/mode and matching parent
observation context. Unknown/mismatched replies become Failed/InvalidNativeResult,
never fabricated absence. Context consistency includes unqualified IDs and is
not semantic identity proof. Native lookup policy remains explicit; the probe
never emulates Unicode/case equivalence. Refresh uses the same held parent, even
when its former path now names another directory, and produces a separate record
while the old record/child remain independently owned.

A stop token is checked before work and between native calls. Cancellation
returns Unknown/InterruptedRead with operation `entry_probe.cancelled`, releases
private owners and returns no partial record. It cannot interrupt a blocked
native call. Exceptions unwind private owners without retries. Execution-time
revalidation, alias/descendant checks, planned-name collision grouping,
operation capabilities and user consent remain separate required work.

The shared simulator now supplies exact-unit NativeDefault child lookup with an
explicit Adapter-domain missing witness and configurable per-node location
observations. WindowsIgnoreCase stays Unsupported; no native comparison algorithm
is simulated. Unconfigured location queries retain Unsupported. These explicit
query families support deterministic probe tests without changing host storage.

[Entry-probe evidence](evidence/ck-files-entry-probe-20261023.json) includes native
path replacement, final dangling-link inspection, encoding refusal and a required
casefold-enabled Linux filesystem test. The latter qualifies ASCII native lookup,
not a universal name-equivalence algorithm.

## Linux held-directory inode flag inventory

`LocationInfo::linux_directory_inode_flags` retains the raw 32-bit result of
`FS_IOC_GETFLAGS` on the already-held Linux directory descriptor. This is a
separate namespace from `statx` attributes and mount flags. The query takes an
`int*` despite its historical request encoding; the adapter checks native bit
constants and preserves unknown returned bits. It neither reopens a pathname,
consumes enumeration, requests synchronization, retries errors nor sets flags.
`ENOTTY` is Unsupported; other failures retain native state/code. macOS and
Windows explicitly mark this Linux query NotApplicable/Unsupported.

`reported_linux_directory_flag` decodes casefold, no-COW and project-inherit
bits only for the Linux query family. Invalid feature selectors fail. Every
unavailable observation retains its state and diagnostic. Clear means the bit
was reported clear, not that the filesystem lacks the feature. A set bit is
not an operation capability, actual extent relationship, project quota ID or
permission to create/move anything.

In particular, the adapter does not derive generic directory case sensitivity
or Unicode collision equivalence from these bits. Linux filesystems may use
different encoding/strictness rules; native lookup and further destination
qualification remain required. See the Linux [inode flag API](https://www.man7.org/linux/man-pages/man2/ioctl_iflags.2.html),
[UAPI flag namespace](https://github.com/torvalds/linux/blob/master/include/uapi/linux/fs.h)
and [ext4 casefold contract](https://docs.kernel.org/admin-guide/ext4.html#case-insensitive-file-name-lookups).

[Inode-flag evidence](evidence/ck-files-inode-flags-20261023.json) includes
independent native descriptor queries and a private casefold-enabled ext4 image.
The latter requires successful flag activation and verifies ASCII native lookup;
it does not qualify every Unicode/name rule or every filesystem.

## Deterministic discovery simulation

`tests/support/simulated_filesystem.hpp` implements the injected `BasicFileSystem`
discovery boundary without host filesystem access. A simulator owns an adapter
scope; object IDs increase without reuse. Absolute path bindings and directory
membership are independent of held objects. Rebinding/removing a path or child
cannot retarget an existing cursor/entry handle. File aliases may share an
object. Directory cycles and foreign simulator nodes are refused.

Each cursor captures a deterministic sequence of names on open; child opens
resolve live membership. This deliberately permits enumerated names to become
Gone and later additions to be absent from that cursor. It models one possible
scan interleaving, not universal native enumeration behavior or a filesystem
snapshot. Child directory cursors own independent iteration positions. Final
link nodes are inspectable as links and cannot become directory cursors.

Per-operation queued actions inject typed failures, manual-clock elapsed time,
side effects and exceptions. Call records and live cursor/entry counts allow
assertions about retries, cleanup and excessive work. Default metadata is
minimal; unknown fields remain unknown. Native POSIX bytes and Windows UTF-16
components are compared exactly and may be exercised on any host. Absolute
fixture paths are keys only; opening them performs no host I/O.

The fixture is single-owner. Nodes (4096 creations), path bindings (4096), child
bindings (4096 per directory), queued actions (4096) and retained call records
(100000) have explicit test limits. These counts are not a process-memory quota.
It does not emulate case folding, normalization, native error witnesses, link
traversal, provider behavior or mutation guarantees. Unconfigured location queries
retain Unsupported. NativeDefault lookup uses exact simulated name units;
WindowsIgnoreCase remains Unsupported. Mutation simulation remains unimplemented.

[Simulation evidence](evidence/ck-files-simulation-20261023.json) covers the
fixture with the production discovery, pane and held-child code on all hosts.

### Explicit inventory replies

`tests/support/simulated_inventory.hpp` extends each simulated held object with
independent storage, link/reparse, data-channel catalog, attribute catalog/value
and six native security-part replies. Configuration is explicit: with the
requested access, an unconfigured family/key returns Unsupported, never an empty
catalog, missing attribute or absent ACL. Exact POSIX, Windows UTF-16 and Windows
EA identifiers inhabit separate map namespaces, without folding or decoding.
Available replies retain supplied owners, coverage, opaque bytes, flags and
per-field unavailable observations. Malformed or inconsistent records can be
configured deliberately; this fixture does not validate native structures or
claim preservation capabilities.

Every family participates in the action queue (failure, elapsed time, side
effect, exception). Calls record object, feature/security key, inventory limits
and requested access. Query effects run before selecting the current reply,
including attribute/security map lookup. Rebinding a pathname cannot retarget
an open entry. Attribute/security reads require their respective requested
access; a full Windows descriptor additionally requires audit-security access.
These are explicit fixture rules, not emulation of host access checks or grants.

Invalid limits, invalid security-part keys and attribute query identifiers over
the 8 MiB hard bound are refused before tracing or consuming actions. Valid
catalog queries enforce record counts and the sum of identifier bytes; values,
security and links enforce returned value/native-payload byte limits. No partial
prefix escapes. These byte-accounting rules are fixture rules, not native wire
buffer layouts. A node configuration cumulatively bounds binary payloads,
identifiers and decoded link names to 8 MiB, and counts configured replies and
catalog records against 65536. Checked addition and UTF-16 multiplication prevent
overflow. Validation precedes replacement, so an invalid configuration leaves
previous replies intact. These bounds do not constitute a total process-memory
quota or limit caller-owned configuration construction or diagnostic strings.

[Inventory simulation evidence](evidence/ck-files-inventory-simulation-20261024.json)
covers cross-platform execution, all failure states, limits, access requests,
exact transport, ownership and injected query changes. Native inventory coverage
still needs its own feature-bearing fixtures; simulation does not close WP-01.

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

## Destination batch inspection

`DestinationBatchJob` maps an owned vector of source components to one explicit
destination encoding, then inspects that batch through one retained directory.
It has one owning worker; abandonment waits until an advance has returned.
Mapping uses the lossless/strict `map_native_name` contract. There is no display
conversion, normalization, case folding, truncation or automatic renaming.
Unrepresentable names keep their original native units and per-row mapping
failure and never reach lookup. The batch is read-only and supplies neither
source-object binding nor mutation authority.

All names are mapped before any native probe. Exact equal mapped components
share their first row as `exact_name_leader`, an exact group count and one
`DirectoryEntryProbe`; every original source row remains present. Getters resolve
duplicates to their leader's probe/failure. Win32 naming diagnostics, reported
component length, native lookup and existing metadata retain independent states.
Missing needs the lookup adapter's native witness; a denial or unreadable object
does not become absence. A failed individual probe remains row evidence rather
than discarding other successfully inspected names.

After lookup, the batch indexes available scoped object ID reports while all
opened targets remain held. The index key contains scheme, volume, object and
generation; scope is deliberately not sorted or hashed. Every positive match
still passes `compare_identity`, including scope validity/nonzero-ID rules.
Different scopes with identical index bytes cannot become a match. Bucket
comparisons are individual cooperative steps, including adversarial collisions.

`reported_object_leader` is set only for positive matches between distinct exact
name groups (and their representative). It describes the same **reported scoped
object ID**, not equivalent directory-entry names. Hard links and namespace
aliases can both report one object. An absent match is not proof of different
objects, different names under the filesystem's equivalence rules, or collision
freedom. Distinct missing names remain unqualified for native case, normalization,
short-name and other alias collisions, regardless of filesystem advertisements.
The result does not resolve those remaining native-equivalence rules.

Only a finished job can seal `CompletedDestinationBatch`. It retains the actual
directory even for an empty/all-missing batch and owns each found target through
its probe. An earlier result stays immutable when a later batch observes name
replacement. The original parent pathname is never reopened. Neither pinned
targets nor the parent freeze directory membership, metadata or names; all
evidence requires operation-time revalidation. Final result retirement belongs
on a worker. This is not a UI publication job or frozen operation plan.

The default batch bounds are 256 rows, 4 MiB native name storage and 64 KiB per
component, with hard bounds of 4096 rows, 16 MiB aggregate and 1 MiB per component.
Source and mapped strings consume the aggregate budget; each unique mapped name
also reserves an index key and both probe/context name copies once. Reservations
are conservative when a probe has no lookup context. UTF-16 counts bytes with
checked arithmetic. Resource failure is terminal and exposes no partial result;
an excessive input row count fails before queries. The caller-owned input vector
already exists before ownership transfer; mapping checks precede further copies.
These bounds are not a process-memory quota, native handle-availability promise
or disk-backed index for large operation inventories.

Advances perform at most 256 map/probe/group steps with injected cooperative
clock slices (20 ms default). A completed probe commits before time/cancellation
yield and is never repeated merely for yielding. The nested entry probe discards
an incomplete probe on cancellation; that name can be retried on resume without
repeating earlier completed names. Already-stopped calls make no queries.
Abandonment releases private ownership; issued results survive independently.
Clock failure, aggregate limits and exceptions poison staging, release native
owners and prevent retries. Exception error text is constructed without
allocation. A native probe can make several syscalls; native calls, allocations,
comparisons and destruction can overrun a slice and remain worker work.

[Destination batch evidence](evidence/ck-files-destination-batch-20261024.json)
covers deterministic and native validation. Self/descendant checks, operation
capabilities, native equivalence for distinct planned names, conflict decisions,
consent and publication protocols remain required before mutations.

## Reported file data ranges

`OpenedEntry::inspect_data_layout` returns `FileDataLayout`: the regular file's
reported logical size, owner, native query family and ordered potential-data
ranges. It covers main data only. Allocated ranges may contain zeros; a complete
file range does not prove a dense file, and omitted ranges do not constitute a
physical extent/clone/deduplication map. The native APIs' reporting precision and
remote/mapped-file coherency limits still apply. See
[POSIX/Linux seek semantics](https://man7.org/linux/man-pages/man2/lseek.2.html)
and [Windows allocated-range semantics](https://learn.microsoft.com/en-us/windows/win32/api/winioctl/ni-winioctl-fsctl_query_allocated_ranges).

Access is explicitly requested with `EntryAccess::MetadataAndLayout`. Default
metadata opens keep their existing permissions and return Denied/InvalidAccess
for this query. The opt-in can require native data-read access but does not
authorize content reads, mutation, elevation or hydration.
Only regular files pass the access guard; directories, final symlinks/reparse
objects, FIFOs and devices are refused before access promotion. Identified
Darwin dataless and Windows provider/recall cases require review before promotion
or querying. Linux has no generic provider-locality query, so opt-in is not a
universal no-hydration promise. No file-content read API is used.

Native access remains bound to the original held object, not a second basename
lookup. macOS seeks the existing event-only descriptor. Linux obtains read
access via its held descriptor's kernel link in a verified procfs fd directory;
it checks the filesystem type, retains the original descriptor, then verifies
the readable handle's identity/kind/size/change witnesses. Missing or denied
procfs access remains explicit; the original pathname is never a fallback. This
uses the [documented procfs descriptor interface](https://man7.org/linux/man-pages/man5/proc_pid_fd.5.html)
and preserves native read-permission denial rather than duplicating O_PATH
access as if it were a readable handle.

Windows opens the reported native ID through
[OpenFileById](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-openfilebyid),
using the live original file as its volume/share hint. It requests FILE_READ_DATA,
read/write/delete sharing, OPEN_REPARSE_POINT and OPEN_NO_RECALL; it does not
enable backup privileges or fall back to a name. Both handles stay live while
identity/kind/size/time observations and provider guards are checked. The
[reported ID scheme](https://learn.microsoft.com/en-us/windows/win32/api/winbase/ns-winbase-file_id_descriptor)
selects the native descriptor type; unsupported identity opening is not weakened
into another scheme or hidden by a path reopen.

POSIX queries use SEEK_DATA/SEEK_HOLE; Windows uses
FSCTL_QUERY_ALLOCATED_RANGES. Failures remain native states/codes. Unsupported
queries never produce a fabricated whole-file range. Empty files still require
a successful native end query. Windows rounding is clipped to the captured
logical interval. Zero-length, overflowing, nonprogressing or malformed replies
are refused; POSIX data beyond the initial size is treated as Changed. Required
identity/kind/size and any available modification/change times are checked again
after collection, without asserting a snapshot or unchanged contents.

`InventoryLimits` bound retained records and range bytes. At most capacity+1
native range callbacks run, permitting a final end witness at an exact limit.
Cancellation is checked between callbacks (and between POSIX data/hole seeks),
and errors/cancellation/limits return no prefix. A native callback or metadata
query can still block. Native file offsets are private worker state; no stream
reader shares these descriptors. Native signed offset limits are checked before
conversion. This is neither an exact physical allocation inventory nor a copy
or sparse-preservation guarantee.

`ObjectInventoryJob` now collects this independent field and incrementally
checks its envelope, family, ordering, logical bounds and aggregate budgets.
Incomplete cancelled layout queries are retried on resume; completed layout
observations are not repeated merely for yielding. The simulator has explicit
layout replies, access requirements, limits and injectable layout failures.

[Data layout evidence](evidence/ck-files-data-layout-20261024.json) records native
range/permission/provider tests. Sparse test objects are exclusively created:
APFS uses native hole punching, Linux sparse extension/writes, Windows sparse
marking/writes. Hole punching exists only in the fixture, never the reader.

## Held-object inventory collection

`ObjectInventoryJob` collects one already-open `OpenedEntry` on its owning worker.
It never reopens a pathname, follows a link, elevates privileges or mutates the
object. It requests basic metadata before/after collection, storage and data-layout observations,
link/reparse payloads, channel and attribute catalogs, each catalogued attribute
value and all six explicit native security query parts. No main-data/named-stream
content API is called. Attribute values are explicitly read and may themselves
contain additional file data, quarantine/security markings or other opaque data.
Native adapter provider guards and native access failures still apply; this job
does not grant hydration consent or promise universal no-hydration behavior.

A private factory seals `CompletedObjectInventory` only after all phases finish.
The result retains the exact object handle and immutable observations; final
retirement and further `object_for_worker()` queries belong on workers. This
does not attest the original pathname or parent membership. Renaming, removing
or replacing that name cannot retarget collection. Issued results can outlive
job abandonment and retain their object until their last owner retires.

Unsupported, Unknown, Denied, Gone, Changed and Failed native query replies stay
independent fields. An unavailable attribute catalog has no value reads; an
available catalog records an indexed reply for every descriptor, even when a
value read is denied or hits its native limit. Security replies are indexed by
`inventory_security_parts`; no descriptor is silently weakened and no failed
query becomes empty/absent. Completion means the collection phases finished,
not complete native feature coverage, preservation feasibility or permission to
operate. Consumers must evaluate every field and its exact coverage.

`identity_continuity` compares only before/after IDs while the object is pinned.
Same does not mean stable data, sizes, times, flags, attributes, ACLs or catalog
membership. `compare_owner()` separately compares a query payload's owner with
the first object observation; unavailable/unqualified IDs stay unavailable,
different IDs stay Different. Neither comparison proves snapshot consistency.
Remaining recursive/frozen planning must qualify feature owners, concurrent
changes, coverage and operation-specific capabilities before consent/execution.

Catalogs are checked incrementally before attribute value reads: native record
count limits, known coverage/kind/role enum values, exact namespace identifiers
and duplicate identifiers. POSIX bytes, Windows UTF-16 and Windows EA bytes are
distinct; no Unicode decoding, case folding or display conversion participates.
Attribute replies must match the requested catalog identifier; security replies
must match the requested part. Missing link payloads, invalid outer enums and
native replies exceeding their value/payload bounds fail collection. This is
envelope validation, not ACL semantics or native reparse-buffer parsing; native
decoders retain their own contracts.

Per-query `InventoryLimits` pass unchanged to native methods, whose wire buffer
accounting differs by query family. Separate cumulative retained limits count
query observations, catalog descriptors and attribute reads, plus native
identifier bytes, binary values and link payload/decoded names. A long
identifier is not rejected merely because a native value buffer budget is
smaller. Checked addition/subtraction and UTF-16 multiplication refuse excess
without returning a prefix. Limits bound these payloads/record counts, not total
process memory, diagnostic strings, a native adapter's temporary allocations or
caller-owned fixtures. A native reply can temporarily coexist with staging.

Each advance performs at most 256 phase/descriptor steps with an injected
cooperative time slice (20 ms default). Completed queries commit privately
before time/cancellation yields, so resume does not repeat them. Already-stopped
calls make no queries. Cancellation pauses and retains private state; explicit
abandonment releases it. Clock reversal, malformed replies and aggregate limits
are terminal; exceptions poison with nonallocating error text, release ownership
and rethrow. Terminal advances make no I/O. Individual native methods (which
may contain several syscalls), comparisons, allocations and retirement can
outlast a time slice; this job neither preempts native I/O nor supplies a worker
scheduler, UI publication gate or disk-backed large inventory store.

[Object inventory evidence](evidence/ck-files-object-inventory-20261024.json)
records deterministic and native validation on all three platforms. This is a
read-only collection boundary, not a copy/move implementation or release gate.

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
