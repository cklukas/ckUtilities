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
or initial acquisition preparation can explicitly retain it. Neither mode attests that an original
pathname still resolves to that directory. Navigation labels/context need
their separately qualified binding. Raw discovery rows are not sealed HeldChild refresh successors. PaneRefreshJob
can prepare successors for previously held entries; unproven continuity still
invalidates selection. PaneAcquireJob can initially bind requested discovery rows.
Scheduling, large indexes and UI ownership/publication remain unfinished. No snapshot,
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
The final result requires the original generation gate. By default it releases
its scan pin; the explicit result-retention option can retain the exact scanned
directory for a subsequent acquisition stage. Externally retained input payloads
keep their own pins. Successful
publication applies rows, directory context and issue details together. It
preserves marks/focus only through sealed families. Hidden marked rows still
require review. Failed/cancelled/newer requests cannot overwrite the published
state. Current issue details survive failure/cancellation; successful publication
replaces the report, while unacknowledged selection invalidation counts persist.
Explicit valid selection commands acknowledge/clear those diagnostics.

This phase handles entries already bound before refresh. Initial binding uses
PaneAcquireJob below. UI ownership/scheduling, large-view indexes, operation
planning and execution-time revalidation remain required work.
[Refresh evidence](evidence/ck-files-pane-refresh-20261024.json) includes injected
lifecycle/fault/resource tests and native bound-selection, replacement and
cross-directory tests on all three platforms.

## Initial held-entry acquisition

`PaneAcquireJob` consumes a sealed native-End discovery or prepared refresh
payload with its exact scanned directory retained, plus ownership of a bounded vector of requested
native names. The existing `RetainForRefresh` retention option supports both
preparation stages. Requests are exact native-unit matches against this scan;
there is no display decoding, case folding, normalized-name substitution or
path reopening. Every request and discovery row is validated before any child
open. Duplicate requests, platform/encoding mismatch, invalid configuration and
count/name/display limits fail without publishing partial results. The requested
vector transfers ownership without a constructor copy; indexed name copies are
bounded before allocation. Requested/indexed names, copied rows, issue names and
held objects have separate bounded counts/bytes, not a total memory quota.

For every matched request without an existing sealed binding, `HeldChild::observe`
opens metadata-only through the
retained parent without following a final link. Successful acquisition replaces
the row's metadata and recomputes its hidden observation. Existing sealed
bindings pass through without reopening or changing their family/revision;
their canonical held metadata and hidden policy are prepared for the requested
model. Unbound, unrequested rows stay as observed. A name absent from discovery produces Unknown/not-listed without a
native lookup or a claim of absence; native open/inspection failures remain
named issues with their exact states/codes. A successfully completed payload
can contain both held rows and unresolved named issues; it is not an entirely
bound operation-source selection or permission to execute. Input metadata and
identities never prove continuity with the newly opened object. Even equal
reported IDs can have been recycled before acquisition; no such comparison is
used to preserve old marks or focus. For new bindings, pane publication allocates new row IDs,
and a refresh of previously unheld selections reports invalidation. The user
must select/review the freshly represented objects. Subsequent refresh can use
the acquired immutable held families through `PaneRefreshJob`.

Each advance performs at most 256 preparation/acquisition steps and observes
an injected monotonic slice (20 ms default, positive through one second).
Components have a 1 MiB hard cap; pane row, request/held-count, native-byte and
display-byte limits apply. Native calls and individual allocation/encoding steps
may exceed the cooperative slice. Cancellation discards incomplete native
observations and can resume at that request; completed requests survive yields.
Clock/resource failures poison and release staging. Exceptions preserve a
nonallocating poison record before rethrow. Abandonment releases private state;
issued immutable results remain independently owned and require generation
revocation by the receiving pane. Successful preparation releases its scan pin;
held rows retain their own parent ownership. All native-owner retirement belongs
on the worker. No directory membership/content snapshot, pathname attestation,
provider-hydration guarantee, operation plan, scheduler or UI integration is
established by acquisition.

For a mixed preparation, run refresh with explicit `RetainForRefresh` result
retention, pass its sealed output to acquisition, then publish only the final
result through the original generation gate. The default refresh retention is
still `ReleaseAtEnd`. Acquisition reserves the union of existing held rows and
matched requested rows before any new child open; a request for an already-held
row consumes no additional slot and issues no native call. Inherited issues are
count/name-byte bounded and copied incrementally before acquisition I/O. Their
states and codes survive even if a later initial acquisition of that name
succeeds: that success cannot repair failed continuity with an old selection.
At most the configured held-count of inherited issues plus the requested-count
of new issues is retained. Issue names have their own configured native-byte
budget, separate from requested/indexed names and row names. These are bounded
representations, not a total process-memory quota. Neither stage publishes an
intermediate pane. Existing marks/focus can survive only the family checks in
the final pane publication; replacements and unproven continuity still invalidate
old selections. The combined result is a sequence of live observations, not
a simultaneous filesystem snapshot.

[Mixed preparation evidence](evidence/ck-files-pane-mixed-20261024.json) covers
union budgets, inherited errors, yields/cancellation, canonical hidden state and
native mixed-publication/replacement fixtures on all three hosts.

[Acquisition evidence](evidence/ck-files-pane-acquire-20261024.json) records
simulated fault/resource/lifecycle tests and native acquisition/publication/
refresh, parent-path replacement, removed-name and hard-link fixtures.

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

## Native parent references and bounded ancestor observations

`DirectoryCursor::inspect_basic` queries the held directory independently of
its enumeration state. Both `DirectoryCursor` and `OpenedEntry` provide
`open_parent_directory`; success owns a metadata-only `OpenedEntry` parent
reference, not an enumeration cursor. Unsupported implementations remain explicit.
Regular files and final links/reparse objects are not silently followed as
directories. Native-owner creation/query/retirement belongs on workers.

POSIX opens the fixed native `..` component relative to the held descriptor,
with directory/no-follow/close-on-exec protections. Linux uses O_PATH; Darwin uses
O_EVTONLY. This does not require directory-listing access on Linux and does not
reopen an original absolute pathname or change cwd. Normal kernel search/access
checks still apply. Darwin provider flags are checked before and after opening.
The [openat contract](https://man7.org/linux/man-pages/man2/open.2.html) defines
relative resolution against a directory descriptor. Parent references are live
edge observations and provide no mount/namespace confinement guarantee.

Windows queries the held directory with the documented
[FSCTL_READ_FILE_USN_DATA](https://learn.microsoft.com/en-us/windows/win32/api/winioctl/ni-winioctl-fsctl_read_file_usn_data)
metadata control. The existing identity scheme selects USN v2 (64-bit) or v3
(128-bit); there is no guessed conversion or ID truncation. The portable decoder
validates exact record length/alignment, requested major/minor version, header,
UTF-16 name bounds and nonzero child/parent IDs, without retaining names or
interpreting the API's invalid time/reason/source fields. The API is optional;
unsupported drivers, inactive journals and permission failures stay explicit.
No journal is created, changed or enumerated.

The parent is opened using
[OpenFileById](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-openfilebyid),
with the live child as volume/share hint, metadata/synchronization rights,
share-read/write/delete, no-recall and final-reparse-point protection. No backup
intent, data-read access or privilege elevation is requested. Source and parent
identity/kind/provider reports are qualified; a second USN observation with the
parent handle live must retain the same child/parent IDs. Changes decline the
edge. ReFS research and native tests verify full IDs, current parent after a
move, root self-parent behavior and metadata-only access without enumeration or
backup flags. Native `..` opens were rejected and directory enumeration's `..`
ID was zero on this host; neither rejected route is used in production. This
qualification is not a universal provider/no-hydration or filesystem-support
claim. USN v2 has deterministic decoding coverage; this host qualifies native v3.

`AncestorWalkJob` accepts a bound directory source and held destination directory.
It qualifies fresh source/destination identity/kind observations against their
retained bindings, then iteratively opens metadata-only parent references.
Successful reports retain source, destination and all observed parent owners.
An `ObservedSource` witness identifies the source's position in that sequence;
`RepeatedDirectory` identifies a repeated native identity, including a root or
loop. There is deliberately no NotDescendant/SafeToCopy result. Concurrent moves,
mount aliases or unsupported edges prevent a walk from certifying universal
absence of an ancestor. This is evidence for later self/descendant refusal, not
a frozen namespace, current-name proof, operation plan or execution permission.

The default 256-directory cap includes the destination and repeated endpoint;
the hard cap is 4096. The last allowed node can still yield a positive/repeat
witness before budget refusal. Advances have 256 preparation/query/comparison
steps and an injected cooperative slice (20 ms default, positive through one
second). Repeat comparisons also consume steps; there is no unbounded comparison
loop hidden inside an advance. Actual OS descriptor limits can fail sooner.
These are representation bounds, not process-memory/handle-availability promises.
Native operations may include several calls and can block beyond the slice.

Whole metadata/field/parent errors and limits produce unavailable outcomes with
an explicit bounded observation prefix. Completion means the attempt ended;
callers must inspect the outcome, not infer successful full ancestry. Fatal
configuration/clock/resource exceptions poison and release private staging;
malformed null parent replies cannot publish an incomplete report. Cancellation
exposes no result and retains completed private observations/pending handles;
resumption does not repeat a completed query. Abandonment releases private state,
while issued immutable reports retain their owners independently.

Simulation supplies declared directory-parent links, independent native-query
faults and ownership counts. Same-parent aliases retain the parent until their
last link is removed; ambiguous graph parents remain Unknown. This is not mount,
provider, native name-equivalence or permission emulation.
[Ancestor evidence](evidence/ck-files-ancestor-20261024.json) records decoder,
state/fault/resource tests, native parent/walk fixtures, Windows API research,
failed fixture corrections and cleanup. Execution still requires appropriate
name/edge/capability revalidation and operation-specific safety policy.

## Held source and existing-target identity comparison

`SourceTargetJob` consumes ownership of a bounded vector of immutable
`HeldChild` sources and a completed `DestinationBatchJob` result. Every source
must match the corresponding batch source component in exact native units;
all rows and component-byte limits are validated before any metadata query.
The job never performs a new lookup, reopens a pathname or follows a final link.
It obtains fresh source metadata through `HeldChild::inspect_basic` and fresh
existing-target metadata through the probe's retained child handle. The completed
result retains both source owners and the entire destination batch, including
its parent pin and the original lookup/mapping/failure evidence.

Available comparisons report Same/Different native identities only after both
identity and kind reports remain qualified against their retained observations.
Source/target identity or kind changes produce Changed; unavailable fields and
unqualified scopes/schemes/generations remain unavailable. Zero object IDs cannot
prove Same or Different. A Same report with conflicting source/target kinds is
also Changed. Size and time changes are retained as fresh observations but are
not content equality or snapshot checks. The
[Windows file-ID contract](https://learn.microsoft.com/en-us/windows/win32/api/winbase/ns-winbase-file_id_info)
qualifies handle comparisons using file and volume IDs on one computer; the
[POSIX attribute contract](https://www.gnu.org/software/libc/manual/html_node/Attribute-Meanings.html)
describes device/inode identity and open-object lifetime. These native reports
do not provide durable/cross-host identity or universal alias exclusion across
unqualified providers, mounts or namespaces. Operation-specific capability and
execution-time qualification remain required.

A missing target remains the batch's earlier native absence observation. It
produces NotApplicable for comparison, never Different, and is not looked up
again by this stage. Mapping, lookup and query errors retain their states/codes;
`source_now` and `target_now` keep independent observations when more than one
condition is unavailable. Previously unavailable retained metadata cannot gain
comparison certainty merely because a later query succeeds. No successful empty
metadata or cached-value substitution hides failed live queries.

Advances perform at most 256 validation/query/comparison steps and observe an
injected cooperative clock (20 ms default, positive through one second). Rows
are bounded to 4096 and components to 1 MiB, with lower configurable defaults;
these are bounds on this result's representations, not process-memory or OS
handle-capacity promises. Each native basic query returns one complete
observation. Its reply is committed privately before a cancellation/time yield,
so resumption does not repeat it. Cancelled jobs expose no partial result.
Fatal validation/resource/clock failures release staging; exceptions poison
before rethrow with nonallocating diagnostic construction. Abandonment releases
private results, while issued immutable payloads retain owners independently.
All native-owner destruction belongs on workers.

This is self-target evidence, not a copy/move refusal engine, ancestry walk,
current name-membership proof, frozen operation plan or execution permission.
A Same identity can inform refusal of a self-target; Different does not establish
collision freedom, descendant safety or safe replacement. Handles can continue
to describe old objects after either original basename is replaced. Execution
must separately revalidate names, parents and relevant capabilities.
[Source-target evidence](evidence/ck-files-source-target-20261024.json) records
simulated faults/lifecycle/budgets and native self/hard-link, distinct-file,
replacement, scope/absence and final-link fixtures.

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
subject-specific hard-link limits,
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

## Native directory access observations

`DirectoryCursor::check_access` checks List, Search, AddFile or AddDirectory
through the held directory on a worker. A successful `DirectoryAccessCheck`
records the exact request, query family, native mask and qualified directory
identity. It is evidence of a momentary native access check, not an operation
capability, reservation, loss decision or permission to mutate. The query does
not create a probe child or inspect/derive access from mode bits or ACL bytes.
The default adapter returns Unsupported and invalid requests fail before I/O.
Native denial and other native errors retain their domain/code/operation.

Linux directly invokes `faccessat2` with `AT_EMPTY_PATH | AT_EACCESS`, avoiding
older libc permission emulation and its ACL limitations. Missing syscall support
is Unsupported; there is no old syscall, `/proc` path or permission-bit fallback.
Darwin uses `faccessat` with the fixed internal component `.` relative to the
held directory and `AT_EACCESS`. This includes any kernel search checks required
by that resolution; it is not a promise of equivalence with Linux empty-path
semantics. List requests `R_OK`, Search requests `X_OK`, and both Add requests
check `W_OK | X_OK`. POSIX access modes do not separately establish file versus
directory creation capability. Current effective credentials and kernel policy
apply; no process credential changes are made.

Windows requests the corresponding `FILE_LIST_DIRECTORY`, `FILE_TRAVERSE`,
`FILE_ADD_FILE` or `FILE_ADD_SUBDIRECTORY` right with `FILE_READ_ATTRIBUTES |
SYNCHRONIZE` using `OpenFileById`, with the original held directory as its
volume/share hint. It uses the exact qualified 64/128-bit identity family,
read/write/delete sharing, open-reparse/no-recall flags, and no backup flag or
privilege enablement. The temporary access-bearing handle is checked against
the original directory identity and closed on the worker. Native driver refusal
remains unavailable; unsupported IDs are not converted or reopened by pathname.
The result does not reserve these rights for a later operation.

Fresh directory kind/identity observations surround successful checks. Known
Darwin/Windows provider/recall states block the query before an access-bearing
open; Linux has no generic provider-state query and this access check makes no
locality or hydration claim. Changed/unqualified identities, malformed kinds,
metadata failures and provider uncertainty cannot become successful reports.
Native queries do not change enumeration position. Reports carry identity
context, not independent parent-handle authority; callers must keep the cursor
and revalidate it when needed.

Neither a positive check nor a denial describes all possible operations.
Execution still must check the exact child/name, exclusive creation/publication,
mount state, quotas/capacity, immutable flags, inherited ACLs/labels, provider
behavior and concurrent changes. No delete/replace/rename/ownership/ACL replay
capability is inferred from Add access. In particular POSIX write/search checks
do not waive sticky-directory rules. Filesystem advertisements and permission
observations remain separate inputs to later operation-specific planning.

## Qualified directory name-rule observations

`DirectoryNameRulesProbe` inspects a held directory on a worker, retaining it
independently of its original pathname. A potentially usable rule requires two
independent live location reports qualified against the cursor's cached native
identity. Unavailable/unqualified first reports are not retried; `after` then
records Unknown/`name_rules.after_not_attempted`. The probe preserves both
attempted reports and the rule's explicit unavailable state/diagnostic.
Available(probe) means collection finished, not that a usable rule exists.
`refresh` produces independent observations through the same retained directory;
it neither reopens a pathname nor rewrites the old report.

The initial qualified rule is `LinuxExtendedFsBytewise`: the native Linux
location-query family, extended-filesystem type code `0xef53`, qualified POSIX
device/inode identity, and successful `FS_IOC_GETFLAGS` reports with both
`FS_CASEFOLD_FL` and `FS_ENCRYPT_FL` clear. The type code is shared by the Linux
extended-filesystem family; it is not a filesystem label or a unique volume ID.
Type code and relevant bits must agree between reports. Changed/unqualified
identities, changed families/types/bits, contradictory case reports and required
field/query errors prevent a qualified rule. Unrelated inode-flag changes are
retained without pretending that they change name matching. Native-header
assertions check the constants used by this qualification.

This follows the native extended-filesystem matching contract for directories
without case folding or filename encryption. Encrypted or casefold directories
remain unqualified here, including when an encryption key happens to be loaded.
Other Linux filesystems, Darwin and Windows also remain unqualified. Neither
per-directory case sensitivity nor volume feature advertisements alone supply
a complete name-equivalence algorithm. There is no locale conversion, Unicode
normalizer, OS case-conversion fallback or assumption that all POSIX names use
bytewise comparison. Further native policies and the operation-level conflict
flow remain required; this does not complete cross-platform collision handling.

`compare` returns one of two available relations, or an explicit unavailable
result:

- `IdenticalNativeUnits` is a lexical witness only, available for identical valid
  components in the same encoding even if native rules are unavailable. Its
  directory context retains any unavailable native identity/query diagnostic.
- `DistinctUnderReportedRules` requires a qualified rule and its applicable
  encoding. For the initial rule, distinct POSIX byte sequences remain distinct
  names, including case-only differences, different Unicode normalization forms
  and invalid UTF-8. No display conversion participates.

Names must share an encoding; mapping belongs to the separate lossless mapping
boundary. Each component has a configurable native-byte budget (default 64 KiB,
hard maximum 1 MiB), checked before scanning/copying its units. UTF-16 budgets
count two bytes per unit, not characters. Invalid/moved-from components and
invalid limits fail; no truncated or rewritten name is returned. These are
comparison resource bounds, not filesystem name/path limits or a process-memory
quota. No additional native queries occur during comparison.

Distinct names can be hard links to the same object. These reports do not prove
object inequality, absence, name acceptance, creation capability, permissions,
mount confinement, current name membership, a stable namespace or execution-time
collision freedom. Identical units do not establish native acceptance either.
Later planning/execution must retain and requalify the exact parent and rules,
inspect current occupants and use the required native exclusive/publication
primitives. The repeated observations cannot rule out intermediate or later
changes. No source/destination mutation or private probe creation is performed.

Inspection/refresh and final native-owner retirement belong on workers. There
are at most two native location calls, with cancellation checked around them;
OS calls can block beyond cancellation. Cancellation exposes no partial probe,
and exceptions release private staging. Comparison values retain identity
context only and do not extend native handle ownership.

Native validation on the Linux scratch filesystem independently creates case,
normalization and invalid-byte pairs using exclusive creation and checks distinct
native IDs. A hard-link fixture verifies that name distinction does not become
object distinction. All hosts verify enumeration independence, lexical evidence,
held-directory refresh after pathname replacement and owner retirement. Other
filesystem policies remain explicit limitations rather than presumed passes.

## Destination batch name-group assessment

`DestinationNamesJob` connects a sealed `CompletedDestinationBatch` to fresh
name-rule observations through that batch's retained parent. It retains the
whole batch, including original names, mapping failures, exact-name leaders,
existing occupants and their independent object-identity alias evidence. It
never performs a new child lookup, reopens the original pathname, creates a
probe or mutates a source/destination.

The immutable completed assessment records mapped/unmapped row counts, exact
component groups, duplicate groups and rows within duplicate groups. `row`
reports the exact group leader/count and number of other mapped groups.
Mapping failures remain explicit per-row observations, including batches with
no successfully mapped rows. `OtherNameGroups::None` means there are zero other
mapped groups: duplicates within the one group, unmapped source rows and
existing-target conflicts can still require refusal/review. It is never a
no-conflicts or creation-permission result.

For two or more mapped groups, the job obtains a live parent location report,
qualifies a potentially usable rule, then obtains and qualifies a second report.
An unavailable/unqualified first report is preserved without a retry or second
query. The name-rule boundary's native identity/family/type/relevant-flag checks
apply unchanged. The initial bytewise rule must also apply to the mapped-name
encoding. An available native policy with an inapplicable encoding remains
visible while group comparisons explicitly fail applicability. Unknown policies
and changed/denied/failed queries never become evidence of distinct names.

For a qualified applicable bytewise rule, distinct exact-name groups are
`DistinctUnderReportedRules`. Otherwise comparisons against distinct groups
retain the rule's unavailable state/diagnostic. Pair comparison is O(1) in the
number/length of input names: it uses the sealed batch's exact group index,
without rescanning names, new I/O or allocating a quadratic pair table.
Comparing rows in the same exact group reports `IdenticalNativeUnits` even when
native rules fail, preserving any unavailable native directory context. Bounds
errors and either row's mapping failure remain explicit. Existing-object alias
IDs never establish name equivalence; distinct hard-link names stay separate.

Validation and summary construction are linear in rows and complete before new
native I/O. Row limits have a hard maximum of 4096; per-component native-byte
limits default to 64 KiB and have a hard maximum of 1 MiB. UTF-16 counts bytes,
not characters. The component limit applies to successfully mapped destination components;
unmapped sources retain their mapping errors and the original batch bounds.
The original batch retains its separate name-storage bounds;
this assessment stores summaries and two location observations, not new copies
of every name. None of these bounds is a process-memory or OS-handle guarantee.

Each advance has 256 steps and an injected cooperative time slice (default
20 ms, maximum 1 s). Each location call is a separate phase, and its completed
reply is committed before cancellation/time yields, so resume does not repeat
native I/O. Native calls can still block beyond a slice. Invalid configuration
throws before new queries; row/component resource failures, backwards clocks
and exceptions fail/poison staging without exposing a prefix. Abandonment
releases private ownership; issued immutable results keep the entire batch
alive independently. Native-owner retirement belongs on workers.

This is name-group evidence for later operation planning, not a frozen plan,
namespace snapshot, name acceptance, object inequality, absence, permission or
execution-time collision proof. Even a qualified group result does not update
the batch's older occupant observations. Later execution must revalidate the
exact parent/rules and current occupants and use the required exclusive/native
publication operations. Cross-platform rules that remain unqualified in the
native boundary remain unresolved here; they are not waived by this assessment.

## Linux directory fscrypt policy observations

`DirectoryCursor::inspect_fscrypt_policy` is a read-only worker query through
the already-held directory. Linux uses `FS_IOC_GET_ENCRYPTION_POLICY_EX` with a
fixed, zero-initialized native buffer; it never reopens a pathname or changes
enumeration position. Unsupported build headers, kernels, drivers and other
platforms return explicit unavailable results. There is no deprecated v1 ioctl
fallback, feature-flag-to-absence inference, key loading, unlocking, policy
assignment, content access or privilege change in the product query.

An available `DirectoryFscryptPolicy` carries a qualified directory identity
and either exact policy data or `FscryptPolicyAbsent` with its native `ENODATA`
witness. Only that native error establishes absence in this query namespace.
`ENOTTY`/unsupported queries are not absence. Native `EINVAL` (unknown policy
version) and `EOVERFLOW` (policy does not fit) remain Unknown with original codes;
other native states remain explicit. Malformed positive return values or lengths
fail without returning a prefix. Native ABI sizes/offsets/constants are checked
against the build headers.

Fresh directory kind/identity observations bracket successful/absent queries.
Changed or unqualified IDs, non-directories, malformed kinds and metadata errors
cannot yield available policy reports. When statx supplies a known encryption
bit, it must agree with the queried presence at both observations; contradictions
are Changed. Missing flag coverage is not treated as a clear bit and does not
override independent policy evidence. This is still a live observation, not a
namespace or metadata snapshot; execution must query and qualify again.

`FscryptPolicyData` validates exact v1 (version byte 0, 12 bytes) and v2 (version
byte 2, 24 bytes) shapes. It retains every byte, including unknown mode/flag,
data-unit/reserved and key-reference bytes. Accessors expose structural fields
and the v1 descriptor/v2 identifier; there is no cryptographic-strength,
algorithm-availability, unlock-state or replay-equivalence claim. Unknown versions
remain Unsupported. The parser uses bounded byte copies, not native structure
casts, normalization or partial records. Key references are identifiers, not key
material, and should not be displayed unnecessarily in the UI.

Absence means no fscrypt policy was reported for that directory at query time.
It says nothing about dm-crypt/FileVault/EFS, whole-volume or underlying-device
encryption, all contents, inherited security equivalence, preservation capability
or permission to create. An available policy does not prove that its key exists
or that encrypted data can be read/copied. This initial API is directory-only;
regular-file policy inspection and the operation-level security/preservation
assessment remain required work.

Native positive qualification uses an exclusively created disposable ext4 image
in the Lima task workspace, with encryption enabled on that image only. A private
setup helper assigns a v2 policy to an empty fixture directory using documented
fixture-only privilege, verifies its key is absent, and never adds key material.
The product query and tests then run as the ordinary guest user. Tests inspect
that locked policy, preserve its bytes/owner after a rename, verify an unencrypted
replacement at the original name, and confirm the existing name-rule boundary
refuses bytewise qualification for the encrypted directory. Fixture names are
restored between runs. No encrypted-content copying is claimed by these metadata
fixtures; the image/mount/helper are audited and removed after validation.

## Immutable pane display pages and state revocation

`capture_pane_page` runs on the same serialized worker that owns `PaneModel`.
It copies only a requested visible window into `shared_ptr<const PanePage>`.
The page contains prepared escaped display text, row IDs, basic observations,
visibility/inspection diagnostics, mark/focus bits, view settings, row counts,
listing status/error and invalidated-selection diagnostics. It contains no raw
native names, `HeldChild`, cursor, model reference or native resource owner.
A UI may retain the page after the worker destroys the model without retaining
native handles or performing native cleanup on the UI thread.

Default capture is at most 256 rows and 1 MiB of copied string payload; hard
limits are 4096 rows and 16 MiB. The string budget includes the filter, listing
error and every retained row diagnostic operation, including all 17 basic
metadata observations. Fixed object/row overhead is bounded separately by the
row limit; the string budget is not a claim about exact allocator consumption.
An oversized field/page produces `LimitExceeded` with no partial page and no
truncation. An offset beyond the visible count produces `OutOfRange`; an offset
exactly at the end produces a valid empty page. Counts are subtracted before
adding bounded offsets, so maximum-sized caller offsets cannot wrap. Capture
makes two bounded window traversals, with no whole-directory scan, sorting,
raw-name conversion or filesystem call. Allocation failure propagates without
publishing a page or changing pane state. Capture belongs on the worker even
though its inputs have already been prepared; it has no real-time latency claim.

Each page carries a `PaneStateStamp`. A stamp owns only a shared atomic
revocation bit. A newly captured stamp remains current across reads/captures
of unchanged state. Successful listing requests, publications, cancellations,
failures, view changes and selection changes revoke the previous stamp before
publishing the new state. Accepted selection/view requests may conservatively
revoke even when their visible value is unchanged. Rejected requests and
validation failures preserve the existing stamp. Model destruction revokes it.
Revocation allocates nothing and cannot wrap a generation counter. Retained
old tokens prevent address reuse from causing an old stamp to match a new one.

The UI may query only `stamp.current()` concurrently. All model reads, capture,
mutation and `model.accepts(stamp)` remain serialized on the owning worker.
A command must return its page stamp and row ID to that worker, which validates
both against the current model before acting. A current stamp does not prove
filesystem freshness, selection readiness, object/name continuity or mutation
permission. Native revalidation and operation review remain mandatory. This
viewport boundary does not yet implement ckVision command dispatch, full
properties/diagnostics presentation, the Files UI or filesystem operations.

## Pane selection summaries and guarded model commands

`PaneModel::selection_summary()` and the copied `PanePage::selection()` provide
selection status without copying candidate filenames or traversing rows.
The model caches total and visible mark counts. Individual mark changes adjust
those counts together with the stored bit; range/bulk commands adjust them for
each affected visible row. A view change prepares its projection and visible
mark count before publishing either. A listing publication prepares its marks
and counts before swapping state; only sealed held-child refresh continuity
can retain marks. Navigation resets them. Failed/rejected publication, view and
selection requests leave counts unchanged. Cursor visibility uses the existing
row-ID index; no metadata query or native-name decoding is performed.

Summaries always expose retained total/visible/hidden mark counts. During a
pending listing or invalidated selection, candidate counts remain zero and
status explicitly blocks source readiness. Otherwise any marks take precedence
over the cursor, including marks hidden by a filter/visibility setting. Hidden
marks produce `HiddenMarksNeedReview`; clearing visible marks cannot silently
remove hidden marks. With no marks, exactly one visible focused row is the
cursor candidate; an absent/hidden cursor produces Empty. `sources()` uses the
same decision and only then builds exact worker-owned candidates. A summary
or Ready status never constitutes operation permission or a frozen inventory.

`apply_pane_command` is the serialized worker boundary for focus, individual
marks, row-ID range marking, visible mark-all/unmark-all, inversion, clear-all
and view changes. It validates the supplied state stamp before examining or
applying a command. Empty, foreign and revoked stamps return Stale with no
change. Row/range endpoints must belong to the current pane and be visible.
Both range endpoints are validated before changing any mark, then their current
visible positions define the inclusive range; an old page cannot retarget it
after sorting/filtering/publication. Invalid targets return InvalidTarget;
invalid view enums/filter limits return InvalidCommand. Oversized view input is
validated before copying it; allocation failures propagate with old model state
and stamp preserved. No invalid request clears selection/binding diagnostics.

A successful command follows existing model revocation and review-reset rules.
If it changes state, a second queued command carrying the previous page stamp
is rejected; the caller must process the updated worker result/page before
issuing a new state-relative command. Empty visible bulk actions may succeed
without a change or revocation. Commands may change old displayed selection
while a scan is pending, but never cancel/complete that scan or turn pending
candidate readiness into Ready. Full refresh still validates selection
continuity. This is a pure model boundary, without filesystem opens, mutations,
planning, consent, implicit hydration or UI-thread model access. Worker queues,
ckVision command integration and native revalidation remain separate work.

## Linux filesystem-keyring status for a held directory policy

`DirectoryCursor::inspect_fscrypt_key_status()` is a worker-only read query.
On Linux with the native ioctl ABI available, it obtains this held directory's
policy, queries `FS_IOC_GET_ENCRYPTION_KEY_STATUS` with that policy's descriptor
(v1) or identifier (v2), then independently rereads the policy. Directory IDs
must remain qualified and equal, and every policy byte must match. Changed,
absent or unavailable final policies prevent publication of a key-status result.
A directory without a policy produces Unsupported/NotApplicable, rather than
an invented absent-key observation. Darwin, Windows and unsupported Linux ABIs
retain explicit Unsupported results; this query says nothing about EFS,
FileVault, volume encryption or other encryption mechanisms.

The native request is fully zero initialized, with fixed ABI sizes/offsets and
header constants checked at compile time. The adapter verifies unchanged input
specifier/reserved fields and rejects malformed positive ioctl returns. Native
errors retain their codes; ENOTTY is Unsupported and EINVAL is Unknown, without
a legacy or pathname fallback. The output retains the status code, flags,
user count and all reserved output words. Unknown status codes stay available
as raw observations while semantic helpers return Unsupported. Unknown flag
bits and reserved words are retained without interpretation. Defined v2 user
fields remain uninterpreted for v1 policies. No key bytes/passphrases are read,
keys added/removed, process keyrings searched, file contents opened or provider
hydration authorized. The original pathname and enumeration position are unused.

The three defined statuses concern the filesystem-level keyring: Absent,
Present and IncompletelyRemoved. V1 keys present only in legacy
process-subscribed keyrings are outside this ioctl's coverage; Absent therefore
does not establish that a directory or file is locked. The v2 added-by-self bit
and user count describe native reports at the query instant. They do not grant
access, certify stable key availability or establish encryption preservation.
See the [kernel fscrypt key-status contract](https://docs.kernel.org/filesystems/fscrypt.html#getting-key-status)
and [native UAPI layout](https://github.com/torvalds/linux/blob/master/include/uapi/linux/fscrypt.h).

Normal and sanitizer tests must distinguish an unconfigured optional fixture
from positive native evidence. The private Lima qualification creates only
empty v1/v2 policy directories in an exclusively owned disposable filesystem,
using public placeholder key references with no keys added. Queries run
unprivileged. Policy/version/reference, absent filesystem-keyring status, v1
user-field non-applicability and held-directory isolation are checked, with
positive markers required. This does not validate a loaded key, incomplete
removal, legacy process keyrings, encrypted data reads or key management.

## Bounded held-directory inventory job

`DirectoryInventoryJob` collects one directory's cached identity, fresh basic
metadata before/after, location reports before/after, List/Search/AddFile/
AddDirectory checks, fscrypt policy and filesystem-keyring status. It never
uses an original pathname, advances enumeration, opens children, reads content,
elevates privileges, creates probes or mutates anything. There are ten outer
query invocations; individual native getters may contain multiple OS calls.
The worker serializes access to the retained cursor with other jobs.

An advance performs at most 32 preparation/query/seal steps and uses the
injected monotonic clock with a default 20 ms cooperative slice. Cancellation
is checked between steps. A finished query commits privately before a time or
cancellation yield and is not repeated on resume. Native calls can block and
are not interrupted by this budget. No partial result is exposed. Exceptions,
clock reversal, malformed reply enums/access requests and retention overflow
poison the job, release staging and prevent further queries. Explicit abandon
also releases private state; an external completed result retains its worker
owner independently. Retirement of its last owner can close the native cursor
and belongs on the worker, not UI dispatch.

Variable retention defaults to 1 MiB, with a hard 8 MiB bound, counting every
filesystem identifier in its native encoding and every retained diagnostic
operation string, including nested basic/location fields, policy-absence
witnesses and derived errors. Fixed observations/arrays have fixed cardinality
and are bounded separately; this is not an exact allocator-memory quota.
Bounds are checked without multiplication/addition overflow. Oversized data
fails collection without truncation or a partial inventory. The cached anchor
is checked before copying its diagnostic text. Native adapter allocations are
still governed by their own contracts; this budget bounds retained payloads.

Directory continuity requires Directory kind plus qualified identical cached,
before and after IDs. A changed ID is retained as Different; denied, missing
or unqualified identity/kind states remain unavailable. `compare_owner` first
requires this context to be Same, then qualifies each individual query owner.
Foreign/unqualified owners never become Same. A native access success or a
location advertisement is not an operation capability.

Name-rule derivation uses both location reports only after directory continuity
is qualified. A positive plain-directory rule is rejected if an independent
policy/key report shows encryption, a different owner or an unqualified owner.
Raw conflicting observations are retained for review. Other cross-field
agreement, key availability, permissions, capacity and namespace stability are
not implied; queries are independent observations at different instants.
Unsupported or denied queries remain per-field results rather than invented
absence or a reason to retry them. Completion means these query families were
attempted, not a snapshot, full metadata/security inventory, preservation
assessment, reservation, execution permission or user consent. The owning
worker must still collect entry inventories and revalidate native semantics
at planning/execution. No Files UI or operation is enabled by this job.

## Descriptor-scoped file, symlink and hard-link limits

Location reports now include `_PC_FILESIZEBITS` and `_PC_SYMLINK_MAX` through
the held POSIX directory. Their scope is regular-file size representation and
symlink target-string length for entries in that directory, respectively.
Windows keeps these POSIX query fields explicitly Unsupported/NotApplicable;
no filesystem label, volume size or feature flag is substituted for a limit.

`assess_file_size_bits` compares a proposed unsigned size's required signed
bit width against the reported width, without a shift by an unbounded native
value. It handles all 64-bit input sizes, including the 65 signed bits needed
for `UINT64_MAX`. Reports below the POSIX minimum width of 32 are rejected as
malformed. This is a comparison with the reported representation width, not
an exact filesystem byte ceiling or an authoritative creatability decision.
Native reports can be coarse or implementation-dependent. Neither side of this
numeric comparison alone certifies what a filesystem/driver/API will accept.
Native offset ranges, actual file-size restrictions, quotas, space, providers
and execution-time results remain independent checks.

`assess_posix_symlink_length` compares a byte count with the reported target
length; it does not count display columns/code points, convert an encoding,
validate the target, include a NUL terminator or authorize link creation.
A within-bound result is only a numeric observation. See the
[POSIX descriptor-query scope and return contract](https://man7.org/linux/man-pages/man3/fpathconf.3p.html)
and [FILESIZEBITS definition](https://man7.org/linux/man-pages/man0/limits.h.0p.html).

`OpenedEntry::inspect_link_limits` queries `_PC_LINK_MAX` on the held POSIX
inode, with fresh kind/identity checks before and after. It preserves both
observed link counts and the independent maximum observation. Kind and
qualified identity must agree; changed or unavailable observations do not
become a limit for a different object. The query works without reopening the
name, reading contents or opening a FIFO reader. It also follows retained
objects after rename/unlink. A directory subject remains explicitly Directory:
its report must never be treated as the limit for new children. Windows/default
implementations return Unsupported for this numeric query, without declaring
hard links unsupported.

`assess_hard_link_total` compares an explicit proposed total with the subject's
reported maximum. It never adds to/subtracts from a racing link count, claims
remaining capacity, permits directory hard links or promises that a link can
be created. For a prospective destination inode, planning still needs suitable
native evidence/probes and execution revalidation rather than borrowing a
parent-directory report. `ObjectInventoryJob` now collects this query as an
independent phase, retaining failures and validating its family/kind. Each
available owner still needs qualification against the inventory.

The native call wrapper clears errno, captures it immediately after the call,
and passes it to the shared positive-limit decoder.
`-1` with no errno stays Unknown rather than infinity; native errors retain
codes, and ambiguous EINVAL stays Unknown. Zero remains Unknown/malformed;
other negative returns fail. Successful positive values are not replaced with
constants. Existing component/path-limit queries use the same decoder. All
observations remain worker queries, not mutation permission or user consent.
The held-directory collector's variable-byte accounting now includes all 22
location observation fields.


## Held-object Windows integrity configuration

`OpenedEntry::inspect_integrity` collects `FSCTL_GET_INTEGRITY_INFORMATION`
through the retained Windows handle, with no input buffer and an exact 16-byte
output. It uses metadata rights, never opens a data reader, reopens the pathname,
changes integrity settings or escalates privileges. The
[Windows control contract](https://learn.microsoft.com/en-us/windows/win32/api/winioctl/ni-winioctl-fsctl_get_integrity_information)
describes a file/directory configuration query on ReFS. Other filesystems retain
the actual native refusal; macOS/Linux return Unsupported for this query.

`WindowsIntegrityData` owns every response byte. Exact length is required;
reserved bytes, unfamiliar algorithms and unknown flag bits survive decoding.
The [MS-FSCC reply specification](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-fscc/72640484-66fb-4b8f-aec6-6ab56d63831b)
defines algorithm codes 0 (None), 1 (CRC32, ReFS v2) and 2 (CRC64), the
checksum-enforcement-off bit, and chunk/cluster sizes. The pure helpers interpret
only those codes and that bit. An unfamiliar algorithm remains Unsupported with
its raw value retained. A clear off bit does not establish enabled checksums,
effective enforcement, verified content or data health. Geometry is reported,
not certified or adopted as a destination constraint.

The native adapter checks basic metadata before/after the control. It requires
a regular file/directory, qualified equal Windows IDs and unchanged kind/integrity
attribute. Reparse and known offline/recall/pin/provider states are refused for
provider review. Unavailable observations preserve their state and error.
Other attribute changes do not imply an integrity configuration change. These
bookends cannot freeze concurrent changes or detect changes reversed between
observations. No snapshot, preservation/replay capability or mutation authority
is inferred from a successful report.

`ObjectInventoryJob` collects this family independently, preserves refusals,
validates family/kind and charges the full 16 raw bytes against both query and
retention limits. Owner qualification against the inventory remains explicit;
a completed inventory cannot turn mismatched or unavailable ownership into
permission. Cancellation and exceptions follow the existing staged collector
contract. Integrity configuration application, destination capability and
operation-specific loss/confirmation planning remain outstanding work.


## Lossless absolute navigation addresses

`NativeAbsolutePath` owns either POSIX bytes or Windows UTF-16 units on every
host. It preserves the supplied spelling, including repeated separators,
trailing separators, dot components, invalid Unicode and case. Equality means
exact tagged units, never object identity or native name equivalence. Display
text uses the existing terminal-safe escaping and must never be parsed back.
A move explicitly empties the source while preserving its encoding; an empty
address cannot select the process current directory.

Factories require an absolute filesystem address with no NUL. POSIX requires
a leading slash and retains double-slash spelling rather than assuming its
implementation-specific meaning. Windows accepts qualified drive, UNC and
extended drive/UNC/volume-GUID forms. It rejects drive-relative/root-relative
forms, device namespaces, pipe/IPC shares, explicit stream paths and ordinary
DOS-device aliases. Extended paths require literal backslash separators;
ordinary forward slashes are accepted without rewriting the stored address.
Server/share colons are refused as well as stream colons in the remaining
components. This is namespace validation, not proof that a spelling exists or
that its components are accepted by a particular filesystem/API.

The [Windows namespace documentation](https://learn.microsoft.com/en-us/windows/win32/fileio/naming-a-file)
distinguishes drive-relative addresses, UNC, device namespaces and extended
filesystem spelling. This model deliberately excludes device/stream access from
navigation. It never calls a canonicalization, cwd, environment-expansion or
Unicode conversion service. Existing operating-system navigation semantics for
parent components remain; an address is neither containment nor a pinned target.

Input limits are native bytes: 256 KiB by default, 1 MiB hard maximum, with
UTF-16 units charged twice. Limits are application resource bounds, separate
from filesystem path/component limits. Validation checks the byte budget before
scanning or allocating retained text. Invalid limits fail; over-budget input
remains Unknown/InventoryLimit with no returned prefix. The ordinary native
filesystem entry points apply the same default bound and retain that refusal.

`from_host`/`host_path` copy only the host's native representation. Foreign
encodings are Unsupported, never converted through a locale or replacement
characters. `BasicFileSystem::inspect_native_basic`/`open_native_directory`
validate the bridge before dispatching to an injected adapter, preserving its
result and explicit final-link policy. The production entry points share this
validation. Windows no longer has a separate namespace parser. No filesystem
I/O occurs inside the value or bridge conversion; adapter calls remain worker
operations and can block in the OS.

The model can retain a spelling that its filesystem refuses. Private native
fixtures independently test accepted names and invalid-encoding refusal without
renaming, substituting or converting them. Filesystem acceptance is separate
from representability: APFS rejected the invalid UTF-8 fixture with EILSEQ;
Linux and ReFS outcomes are recorded with their own native evidence. Subsequent
operation planning still requires held identities, collision/name/length rules,
preservation capabilities and execution-time revalidation.


## Held-object volume and Linux mount relationships

`ObjectLocationRelation::inspect` compares one completed directory inventory
with one completed object inventory. It performs no native calls and retains
both inventories and their held owners until worker retirement. The immutable
record is based on the collected observations; it is not a fresh inspection,
snapshot, parent/child membership or ancestry assertion.

Each subject requires qualified anchor/before/after identities and unchanged
valid kind. The directory subject must remain a directory. Missing observations
keep their state/error; changed or unqualified IDs cannot become equality.
Volume identifiers are compared only within the same adapter scope and identity
scheme. `SameReportedIdentifier` is literal native identifier equality, not
proof of the same physical filesystem: volume serials can collide, and equal
IDs say nothing about bind mounts, subvolumes or rename support. Different
schemes are not reinterpreted as comparable byte arrays.

Linux mount comparison is independent. Both location-query bookends must be
Linux descriptor observations owned by the qualified directory, and both
subjects must have stable before/after mount IDs in the POSIX identity namespace.
A changed ID remains Changed; missing mount information is never replaced with
a volume/device comparison. Other platform query families report this Linux
comparison as Unsupported. A location-query failure can leave the independent
qualified volume-identifier observation available while the mount result fails.

The [statx contract](https://man7.org/linux/man-pages/man2/statx.2.html)
defines requested fields and their returned validity mask. `STATX_MNT_ID` reports
the containing mount's ID; it differs from the newer non-reused unique mount-ID
namespace. This implementation consumes the existing `mount_id` field and does
not mix ID namespaces or promise persistence across restart/unmount. Its sealed
result retains both native references; it still cannot freeze concurrent mount
changes or detect changes reversed between the bookends.

A [Linux bind mount](https://man7.org/linux/man-pages/man2/mount.2.html)
provides another mount of an existing directory. The private native fixture
therefore explicitly qualifies equal volume/object identifiers with different
mount IDs, plus a tmpfs pair with different reported volume and mount IDs.
Its mounts exist only in an isolated private mount namespace under the task's
approved temporary root, and are unmounted and removed after testing. Production
comparison code never creates mounts, elevates, reads content or mutates paths.
Neither relation authorizes traversal, same-filesystem publication, a rename,
copy, move, deletion or user-consent bypass.


## Linux regular-file inode flag inventory

`OpenedEntry::inspect_inode_flags` queries `FS_IOC_GETFLAGS` on the retained
Linux regular file. `EntryAccess::MetadataAndInodeFlags` is a separate explicit
read-access request; layout/attribute/security requests do not grant it, and it
does not grant layout queries. Combined layout/inode requests share one checked
readable descriptor. macOS/Windows retain Unsupported for this Linux query,
without acquiring extra data-read rights for the unused request.

On Linux the initial metadata-only `O_PATH` reference pins the object before
read access is requested. A verified procfs fd directory resolves that held
kernel fd reference rather than reopening the basename. Kind/identity checks
qualify the resulting regular-file reference. A FIFO, device, directory or link
cannot become a content reader through this request. Layout requests keep their
existing size checks; an inode-only request needs stable kind/identity rather
than a stable content size. No generic Linux provider-locality guarantee is
inferred: explicit read access is not hydration consent or proof that every
provider has been identified.

The [Linux ioctl interface](https://man7.org/linux/man-pages/man2/FS_IOC_GETFLAGS.2const.html)
returns an `int` flag word. The adapter retains all 32 bits as unsigned data,
including unfamiliar flags. Before/after metadata must identify the same
qualified POSIX regular inode. These bookends do not freeze content/flags or
detect changes reversed between observations. The query never invokes a setter
or reads main content. ENOTTY remains Unsupported, ambiguous EINVAL remains
Unknown, and other native failures preserve their error code and state.

Typed helpers interpret literal bits in the
[Linux FS_IOC_GETFLAGS namespace](https://raw.githubusercontent.com/torvalds/linux/master/include/uapi/linux/fs.h):
compression/no-compression, synchronous updates, immutable/append-only,
no-dump/no-atime, encryption, data journaling, directory sync, verity, no-CoW,
DAX, project inheritance and casefold. They do not mix statx, BSD or xflags
namespaces. In particular FS_DAX_FL and STATX_ATTR_DAX use different bit values.
A clear bit does not establish absent filesystem support; a set bit does not
certify effective behavior, physical representation, meaningful application to
a regular file, replay capability or operation permission. Uninterpreted bits
remain available for subsequent preservation/loss review.

The native byte budget must admit the complete four-byte word before any query.
Invalid limits fail; insufficient space is Unknown with no prefix or native I/O.
`ObjectInventoryJob` retains an independent phase, validates family/kind, charges
four payload bytes plus one observation record, preserves refusals and leaves
owner comparison explicit. Simulation enforces requested access/budgets and
participates in existing cancellation, exception and failure-injection tests.
The getter remains read-only; the native fixture alone sets no-dump on an
exclusively created test file to qualify a nonzero metadata observation. It
never changes immutable/append-only flags or requires elevation.


## Private directory-index record format, version 1

The disk-index foundation now has a bounded, worker-side record codec. It
serializes a nonzero row number, one lossless native basename, the complete
`Observation<BasicMetadata>` and an independent hidden-state observation.
It performs no filesystem I/O and does not yet implement the backing store,
external sorting, private workspace, disk quota or index-backed pane provider.
Those remain required work; this codec does not enable large-directory UI or
file mutation by itself.

| Envelope field | Wire representation |
| --- | --- |
| Magic/schema | Eight ASCII bytes `CKFIDX01` |
| Payload length | Little-endian u32; excludes the 12-byte header and 4-byte checksum |
| Generation | Sixteen exact bytes supplied by the live index owner |
| Row | Little-endian u64, nonzero |
| Native name | One-byte encoding (0 POSIX, 1 Windows), u32 byte length, exact bytes; Windows units are little-endian u16 |
| Metadata | Tagged observation followed by its value or complete failure |
| Hidden state | Independent tagged boolean observation |
| Checksum | Little-endian CRC32C u32 over header and payload |

Observation tags are explicitly mapped: 0 Available, 1 Unsupported, 2 Unknown,
3 Denied, 4 Gone, 5 Changed and 6 Failed. A failure contains a domain byte
(0 None, 1 POSIX, 2 Win32, 3 Adapter, 4 NTSTATUS), signed 64-bit code bits, a
u32 operation-string byte length and those exact bytes. Strings can contain
NUL/non-Unicode bytes; they are data and require appropriate presentation
escaping when displayed. No locale conversion, display-name parsing, Unicode
replacement or native-name normalization occurs.

Available metadata fields appear in this fixed schema order: identity, kind,
logical/allocated sizes, link count, UID, GID, POSIX permissions, BSD flags,
Windows attributes, reparse tag, Linux attribute value/validity masks, mount ID,
and access/modification/change/birth times. The serializer binds the complete
`BasicMetadata` aggregate so adding a field requires a compile-time schema
review. Independent golden wire images cover both failed and fully populated
metadata, including every scalar field position.

Identity schemes have explicit tags (Unknown, POSIX device/inode, Windows
128-bit ID, Windows legacy index), followed by the two 16-byte native IDs and
an optional generation. The live `AdapterScope` is never serialized. Encoding
rejects an available ID in another source namespace. Decoding uses only the
trusted owner's supplied namespace and expected generation; the context must
not be reconstructed from the packet. The owner is responsible for assigning a
nonzero generation consistently with its live source scope. Unqualified IDs
remain unqualified observations rather than being promoted to held references.
A decoded record has no native owner, acquisition proof or operation authority.

Integer fields have explicit widths/endianness; timestamps contain signed
seconds and a u32 nanosecond fraction below one billion. Native kind, identity,
state/domain tags and booleans are checked. Unknown schema, wrong generation,
truncated/trailing data, checksum corruption, invalid names, impossible scalar
values and resource exhaustion never return a partial record. The decoder
checks the entire envelope/checksum before decoding variable fields, and each
length against both configured bounds and remaining input before allocation.

Default bounds are 256 KiB per complete wire record, 64 KiB of native name and
16 KiB of aggregate diagnostic strings; hard ceilings are 1 MiB, 1 MiB and
256 KiB respectively. UTF-16 consumes two bytes per unit. Invalid configuration
fails; exceeded budgets remain Unknown/InventoryLimit. Allocation exceptions
propagate with no published record. These bounds are not total-process-memory
or disk quotas; the future store must enforce aggregate limits separately.

CRC32C is checked against the independent vectors in
[RFC 3720 Appendix B.4](https://www.rfc-editor.org/rfc/rfc3720.html#appendix-B.4).
It detects accidental corruption and provides no authentication or privacy.
The future storage owner must enforce exclusive/private workspace ownership,
resource quotas and cleanup. This is a same-lifetime scratch-cache format,
not a journal/recovery identity format or a way to resume native ownership
across restarts. Selection and operations still require native acquisition,
identity/edge revalidation and the later planning/consent rules.

### Worker-side index record store

`DirectoryIndexStore::create_new` creates and owns one private `IndexByteFile`
through an injected `IndexFileFactory`. The byte backend is separate from the
read-only source adapter: it must create an empty private scratch file under an
explicit root, prevent external access and close/remove its file at destruction.
The store's public pre-created-file adoption entry point has been removed: there
are no production consumers requiring that prototype interface, and creation
must occur after file-slot reservation. Native conformance is described below.
No source mutation is added. All store methods and shared-budget access are
serialized on the owning worker; none belongs in UI dispatch or paint.

The store advances from Writing to Sealed or Failed. Ingestion appends versioned
records with contiguous nonzero row IDs beginning at one, issues opaque
same-lifetime addresses and reads them only after seal. Addresses are scoped
to the specific store, even when two stores have identical source contexts.
The store keeps only counters and at most a bounded encoded record: no table
of all row offsets, unbounded buffering or RAM fallback. External sorting and
address persistence require a separate bounded format and implementation.
Sealing checks the backend's exact logical size; it proves neither discovery
reached native End nor that the directory was a snapshot. The listing job must
separately prove completion before publishing a pane generation.

`IndexStorageBudget` is a noncopyable shared ledger for live managed stores,
including future sort runs. Callers explicitly select positive aggregate byte
and file bounds. Byte bounds cannot exceed INT64_MAX. Default per-store bounds
are one million rows and one GiB; record limits remain independently enforced.
The ledger charges complete wire bytes including envelopes and checksums, not
filesystem allocation, filesystem metadata or other processes' disk usage.
Physical disk exhaustion remains an independent native error. File-slot
accounting includes in-flight factory creation and empty-file qualification,
not only stores that have successfully published an owner.

Input, state, address and budget rejection perform no data I/O and leave prior
records usable. Encoding completes before reservation. Before the first write,
the entire requested record is charged. Short positive I/O counts are advanced
exactly; zero progress or a count exceeding the remaining span fails. A failure
after reservation may leave a partial file, so the complete charge remains
until the backend is destroyed. Backend destruction precedes quota release.
There is no rollback or successful-prefix claim. Counts, offsets and additions
are checked through subtraction against the configured limits before use.

Native error state/domain/code/operation remain intact. I/O failure, malformed
readback, cancellation and backend exceptions revoke the complete store, and
later operations do no data I/O. Exceptions propagate; the Failed state remains
even when no diagnostic could be allocated. Cancellation is cooperative between
native calls and checked after the final call before publication. It cannot
interrupt a blocked OS call. A read returns a record only after a complete
bounded read, codec verification and matching row ID. Scratch seal is not an
fsync, crash-recovery record, native acquisition proof or operation permission.

### Native private index-file backend

`open_native_index_workspace` supplies a worker file factory without adding
mutation methods to the source adapter. Its caller supplies an existing
explicit host-native directory; there is no environment/default temporary root,
path transcoding, process-cwd change or alternate-volume fallback. It refuses
final links/reparse points, trailing separators and final navigation components.
Earlier path components retain host navigation semantics. Creation uses a held
directory, an OS-generated 128-bit random component and exclusive creation;
preexisting objects are never truncated or adopted. There are at most 64 name
attempts. A successful byte owner never reopens a pathname. The factory pins
the directory once and revalidates its privacy on every creation through that
held directory, even after its original pathname is relocated or replaced.
`create_native_index_file` remains the direct native-byte conformance boundary;
managed record-store callers use the factory entry point for quota enforcement.

On POSIX, the held root must be owned by the effective user with exactly 0700
permissions. Linux currently admits the extended-filesystem family, XFS,
Btrfs and tmpfs, whose POSIX access-ACL masks enforce the mode class. Other
index-storage filesystem semantics remain explicitly Unsupported pending
qualification. macOS admits local APFS/HFS with ownership enforcement
enabled and no extended ACL entries. A successful `fstatx_np`/filesec property
query distinguishes absent ACLs from failed queries; `acl_get_fd_np`'s overloaded
ENOENT is not interpreted as absence. Present ACLs are validated before checking
entries, using Darwin's documented zero/-1 enumeration convention.

The POSIX file is created 0600 using `openat` with O_EXCL/O_NOFOLLOW/O_CLOEXEC,
then unlinked through the held root before any data can be written or an owner
published. Qualification checks regular-file kind, effective-user ownership,
private mode, zero size and zero links, plus macOS ACL state. The owner retains
only the descriptor; closing it releases the unnamed file. A failure to unlink
the newly created empty file is reported as
`index_file.unlink_empty_cleanup_required`: the selected root may then contain
an empty scratch artifact requiring cleanup. No data has been written. This
failure is never reported as successful private storage or silently retried on
another volume.

Windows first requires FILE_PERSISTENT_ACLS on the held root's volume. It
creates a new non-directory file relative to that handle with synchronous I/O,
no sharing or handle inheritance, and FILE_DELETE_ON_CLOSE. The security
descriptor is supplied at creation with an explicit user owner and protected
DACL containing exactly two non-inherited FILE_ALL_ACCESS grants: the effective
thread/process token's user and LocalSystem. The token, ACL and component buffers
are bounded. After creation, independent native queries verify owner, DACL
protection, ACE count/type/flags/masks/principals, zero length and one link.
Failure closes the empty delete-on-close file before returning. Successful
owners use only the retained handle; file deletion follows its closure.

Reads/writes validate offset plus requested length against the native signed
offset range before I/O. POSIX uses pread/pwrite; Windows uses explicit seek and
synchronous ReadFile/WriteFile on the worker's exclusive handle. A native call
may return short I/O or EOF, which the store handles explicitly. Native failures
preserve their error domain/code. No retry hides an interrupted/failed operation,
and no flush/fsync or crash-recovery durability is promised for scratch data.
These privacy controls trust same-user processes, the OS, privileged administrators
and the selected filesystem implementation; they are not encryption or secure
erasure guarantees. Source/destination filesystem support is independent of
which qualified volume holds the index.

Native tests also exercise record-store integration, non-ASCII root spelling,
POSIX literal backslashes, root replacement/relocation or native rename refusal,
EOF/overflow, cleanup, POSIX descriptor inheritance and mode, macOS ACL and
ownership-disabled refusal, and Windows inheritable Everyone permissions that
must not reach the index DACL. `CKTOOLS_FILES_PRIVATE_INDEX_TEST_ROOT` explicitly
selects a capable fixture volume, independently of build/cache storage. This
host's S8T ownership-disabled refusal and PRO-BLADE ownership-enabled positive
tests are both required evidence; the mount's ownership setting is not changed.
Positive native filesystem coverage here is APFS, ext4 and ReFS; admitting a
filesystem under its documented permission contract does not replace the
remaining filesystem-matrix qualification.
Host directory creation, quota integration for future raw sort-byte owners,
external sorting, indexed pane integration and broader filesystem/crash/
exhaustion tests remain required work. A factory neither creates nor deletes
the supplied root directory.

### Factory reservation and pinned-root lifetime

`DirectoryIndexStore::create_new` validates context, limits, factory and budget
before invoking a factory. An already requested stop also performs no creation.
A scope-owned ledger slot is acquired before the call and remains charged while
the factory runs, while its new file is checked for non-null ownership/zero size,
and through cleanup after failure or exception. Exhaustion never invokes the
factory, including a reentrant attempt while the outer factory call is in flight.
There is no temporary release/reacquisition or second charge at handoff.

The new store takes over the exact acquired slot only after construction
succeeds. Cancellation is checked before creation and after native creation/size
query, before owner publication. A factory failure preserves every unavailable
state and native diagnostic. Invalid/null/nonempty replies, native failures,
cancellation and exceptions publish no owner. Private file cleanup occurs before
slot release. A live store retains the factory through file closure; final
retirement closes the file, releases its factory reference and then releases
file/byte charges. Construction/unwind ordering also retains the factory while
its file is being destroyed. All of this remains serialized on the owning worker.

Native factories retain only a directory descriptor/handle and a binding to the
effective user at open (UID on POSIX, a bounded copied SID on Windows). Creation
under another effective user is refused. POSIX rechecks owner/mode, volume and
ACL through the held directory on each call. Windows rechecks root kind and
persistent ACL support, queries the current effective token and installs/verifies
each new file's protected DACL. Factory opening and file creation have separate
ownership: a caller may release its workspace reference while a store remains
live. Another reference can deliberately keep the directory pinned longer.
The root handle itself is not charged as a byte-file slot; host controllers must
bound factory instances and perform final retirement on their workers.

Native tests qualify POSIX creation after root relocation/path replacement,
Windows root-rename refusal while pinned and success after retirement, POSIX
mode changes after open with refusal and retry, shared quota/store lifetime,
and Windows inherited-ACL changes after factory opening. Deterministic cases
cover reservation timing, all factory failure states, invalid replies, creation/
size exceptions and cancellation, shared factories, no double charging and close
order. No different-account impersonation transition is claimed as native test
coverage; the current-user refusal branch still needs that matrix qualification.

### Bounded sequential cache scans

`DirectoryIndexStore::begin_scan` issues an opaque same-lifetime cursor only
after seal. `read_page` reads records sequentially through the existing byte
owner without retaining an offset/address table for the complete listing.
A cursor contains only the store namespace and position/offset counters; it owns
no store, native file, factory or source object. Copies allow deterministic retry
and independent scans. Foreign/moved cursors and invalid limits perform no data
I/O and leave the healthy sealed store usable. Cursor validity is checked by the
store; the token itself is not a standalone currentness/authorization signal.

The codec supplies one checked 12-byte envelope-size query using its own schema
magic and record-size bound. This query certifies framing only, never a checksum,
payload, source namespace or native object. The scanner validates declared size
against the remaining sealed bytes before allocation, reads the bounded body,
performs the complete existing codec checks, then requires the row ID to equal
the next contiguous storage ordinal for ingestion caches. Private sorter runs
instead retain original IDs within the source's original-row bound. Each emitted row includes a scoped address
usable by the same store's checked random-access read. No display/path conversion
or reconstruction of native ownership occurs.

Default page bounds are 256 rows and four MiB of aggregate encoded record bytes;
hard bounds are 4,096 rows and 64 MiB. Decoded variable names/diagnostics are
bounded by those encoded bytes, plus per-row object/container overhead and one
bounded working record. There is no allocation proportional to the full listing.
`page.wire_bytes` counts complete emitted record envelopes and checksums, excluding
lookahead. A successful page transfers at most that configured wire budget plus
one 12-byte lookahead header through the byte-file interface. These are logical
successful data-transfer bounds, not physical device/read-ahead/metadata I/O or
total-process-RSS guarantees. Native calls and final-size queries can still block.

If the first row cannot fit the requested page byte budget, the call returns
Unknown/InventoryLimit without a page or cursor advancement and keeps the sealed
store usable. The caller may increase the bounded page limit and retry the same
cursor. Once some rows fit, a too-large next row finishes that page before the
next record; the caller's returned cursor points to that unread record. The
scanner does not skip or truncate it. Retries must not busy-loop with an unchanged
insufficient limit. Row and byte budgets remain independent.

Data I/O/schema/CRC/row-sequence failure, truncation, cancellation or exception
publishes no page prefix and revokes the store, including previously issued
addresses/cursors. Charges remain until normal backend retirement. The input
cursor changes only when the caller accepts a successful returned next cursor.
End requires exact row/offset exhaustion AND a fresh native file-size equality
check; appended tails and truncated data cannot be silently treated as complete.
Empty/repeated End pages still check size. Scan End describes this private cache,
not successful source enumeration, source stability or a filesystem snapshot.

Nine deterministic scan cases cover repeated/cross-store/moved cursors, exact
independent limits and lookahead, bounded retry, corruption after a valid prefix,
valid-checksum row substitution, short/truncated/trailing data, every native
failure state, cancellation at all I/O phases and exceptions. Three framing
cases cover schema/header/limit boundaries and the distinction from integrity.
A native test writes and scans 100,000 synthetic records, checking every name,
row ID, selected metadata and hidden state while retaining one bounded page.
This qualifies cache storage/scanning only; real-directory discovery, external
sorting/address persistence, pane providers, source-change races and full-app
memory/latency thresholds remain required work.

### External disk-backed ordering

`PaneOrder` and `compare_pane_entries` are shared by the in-memory pane model
and `DirectoryIndexSortJob`. Name, kind, logical size and modification time
keep their existing deterministic semantics: observed directories can stay first
independently of direction; unavailable keys follow available keys in both
directions; secondary native-name ties ascend. The external sorter adds an
ascending original-row-ID tie for duplicate native names. It never follows
links, decodes display text, changes locale, filters hidden rows or drops any
cached metadata/failure observation.

The worker job reads a bounded source block (default 1,024 rows/four MiB wire),
sorts that block, then writes a private run through the same factory/ledger as
its source. A fixed 64-level binary-carry table compacts runs as they arrive;
there is no list of every run handle or row offset proportional to the source.
Each merge retains at most two bounded input pages, one previous record for
strict order/duplicate checks, and one encoder working record. An advance
generates one bounded block, performs one carry/finalization transition, or
emits at most the configured merge row count (default 256). CPU block comparison
checks cancellation too. This is worker work, not a UI/provider callback.

Every input cursor advances once per emitted record; every merge verifies its
combined row count, strict total order and final file size. Final output must
match the source row count and encoded-byte count before completion. The codec
and source scope/generation are retained. Only this sorter can enable a private
run's nonconsecutive-original-ID mode; ingestion keeps its strict ordinal check.
Run readers check nonzero IDs against the original source bound, frame integrity
and complete stored count/size. Runtime checks and deterministic permutation/
payload tests establish the algorithm's preservation; CRC remains corruption
detection under the existing private-cache trust boundary, not authentication.

`row_scope()` identifies the original dataset across resorted projections.
Records retain their original IDs, while scan cursor positions describe storage
order and cache addresses remain scoped to their particular store. An original
cache address cannot read a sorted cache merely because their row namespaces
match. A sorted cache can be sorted again without renumbering selections. This
namespace is a display/selection handle, never native acquisition or mutation
authority. Completed output remains sealed and owns its private file/factory;
the original cache can be retired separately on its worker.

All run creation and encoded bytes share the source ledger, including in-flight
slots and partial failed writes. Quota/native failures have no RAM fallback and
publish no partially sorted projection. When the ledger initially contains only the source store and then this job's
runs, live logical byte reservations are at most three times the source
wire bytes: original source, current run inventory, and a merge output. Fixed
levels bound the live run count logarithmically in block count, with 64 levels
as the absolute table limit; configured file quota remains independently enforced.
Ledger high-water counters record peaks at reservation time rather than sampling
between advances. They include failed reservations still owned by files and are
not physical allocation/metadata/device-I/O/RSS or total-process-descriptor bounds.

Cancelling or abandoning a sort closes its runs and leaves a healthy source
cache usable. Source reads complete one bounded page before observing sort
cancellation, so this projection cannot poison the source merely by cancelling
its read. Actual source corruption/I/O failure still revokes that source. Native
calls cannot be interrupted. An adapter interruption without a requested stop
is a failure, not a fabricated user cancellation; native error tuples remain
intact. Cancellation retires staging even if its optional diagnostic cannot be
allocated. Other exceptions fail/retire the job and propagate. Completion is
published only after full finalization; terminal advances perform no new I/O.

Tests fix expected orders for all sixteen key/direction/folders combinations,
including unavailable fields, duplicate names, links and hidden rows; compare
full re-encoded payloads; resort a projection; and exercise cancellation at every
advance, quota/no-progress limits, both merge inputs, post-write exceptions,
foreign cache addresses and source corruption. Native synthetic 100,000-record
reverse-order tests check every stable ID/name and selected metadata, final byte
count, scope, exact ledger peaks and cleanup. They qualify external cache ordering
on the measured volumes, not real-directory enumeration, full application RSS,
pane responsiveness, collision validation or operation preservation. Directory
collection, indexed providers and the remaining package acceptance still need work.

API references: [Apple ACL retrieval](https://developer.apple.com/library/archive/documentation/System/Conceptual/ManPages_iPhoneOS/man3/acl_get_fd.3.html),
[Apple ACL entry enumeration](https://developer.apple.com/library/archive/documentation/System/Conceptual/ManPages_iPhoneOS/man3/acl_get_entry.3.html),
[Linux open semantics](https://man7.org/linux/man-pages/man2/open.2.html),
[Windows native file creation](https://learn.microsoft.com/en-us/windows/win32/api/winternl/nf-winternl-ntcreatefile)
and [volume ACL capabilities](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-getvolumeinformationbyhandlew).


### Native directory collection into a private index

`DirectoryCursor::source_scope()` reports the owning adapter namespace separately
from the native directory-ID observation. Native POSIX/Windows and simulated
cursors return their actual adapter token, including when native ID queries are
unavailable. The default injected implementation reports Unsupported; it never
infers a namespace from an arbitrary child or manufactures a replacement token.
`DirectoryListing` delegates this query to its held cursor. These tokens are
provenance, never native acquisition or mutation authority.

`DirectoryIndexListingJob` binds that explicit namespace, the pane request and an
owner-supplied nonzero index generation before writing. Unavailable/invalid
namespace queries fail visibly with their diagnostics and create no scratch file.
Every available cached native identity must belong to that namespace, enforced
by the record codec; missing directory IDs do not hide otherwise observable rows.

The worker reads one bounded `DirectoryListing` batch per collection advance and
appends ordinals through the quota-first private store. Name encodings must agree
with each other and with the selected storage model. Per-entry metadata failures
remain records, and hidden-state observations use the existing pane semantics.
Enumeration, capacity and scratch errors discard staging without publishing a
prefix. Native End seals the input, then the existing external sorter prepares
native-name ascending order with folders-first disabled. A bounded sequential
pass rejects nonascending or duplicate native names, including duplicates across
page boundaries. No directory-sized name set, row vector or offset table is kept
by the production job; only the current native batch, bounded sorter pages/runs
or one check page and the previous native name are retained.

The held directory is inspected before enumeration and again after duplicate
validation. An observed foreign namespace, different directory identity, changed
modification/change timestamp, non-directory kind or Gone/Changed inspection
rejects publication. Unknown/denied/unsupported fields remain explicit in the
result's before/after observations; matching available fields never establish a
snapshot. The live scan can miss changes that native enumeration and these
observations do not expose, including child-content changes. Source namespace
continuity is checked again before publication. No original pathname is reopened.

Only this job constructs `CompletedDirectoryIndex`, after native End, full
external ordering and duplicate checks. Its canonical store, before/after
observations, request, namespace and read-only directory pin remain worker-owned.
Rows retain their original enumeration ordinals through ordering. Their row
namespace is stable within that dataset, not across refreshes or native ID reuse.
The result is an input to later indexed pane/generation and acquisition services;
its existence does not certify a current UI generation, pathname binding,
complete preservation inventory or authority for an operation. Subsequent view
orders can use the external sorter without changing original row IDs.

Cancellation is terminal for this collector, retiring the cursor, private source,
sorter runs and check page. It discards any partial listing batch rather than
resuming a possibly changed enumeration. Abandonment also retires private work;
already-issued completed results keep their own owners and need the future
pane's independent generation/revocation gate. Exceptions fail/retire staging
before propagation; failure and cancellation remain distinct. All native and
index I/O stays on a single worker, never in paint/provider/event callbacks.

The native integration case indexes 259 real top-level entries using small
listing/sort/check pages, including an untraversed directory and final link or
junction, checks every cached identity namespace and result row, and proves
result/pin/storage retirement. A second native case changes the directory's
modification time after enumeration and requires Changed with no result.
This is integration coverage, not the planned 100,000-real-entry throughput,
RSS or full-application responsiveness qualification.


### Disk row-position and original-ID lookup

`DirectoryIndexLookupJob` builds a same-lifetime bidirectional lookup for one
sealed complete index. It accepts ingestion or sorted projections whose original
ID bound equals their row count. The builder proves a permutation of IDs 1..N
using initialized on-disk ID slots: every observed ID must be in range and its
slot still zero before insertion; exactly N rows must reach source End. No
in-memory set or array proportional to the directory is used.

The private file contains N position entries followed by N original-ID entries,
64 bytes each (128*N total). Position is zero-based; original IDs are one-based.
The fixed little-endian `CKFLOC01` entry layout is: schema8, generation16, rank8,
original ID8, source offset8, source wire length4, role4, reserved-zero4, CRC32C4.
Roles distinguish the two halves, and the checksum covers the preceding 60 bytes.
Every query checks schema/generation/role/reserved bytes/CRC, expected slot,
source bounds and the matching entry in the opposite half. The live owner pins
the exact source store and its factory/context. This is not a restart format or
an authenticated structure; private ownership and existing CRC trust limits apply.

A private shared quota reservation now covers one file and optionally its known
logical bytes before factory invocation. Existing variable-record stores reserve
zero initial bytes and continue charging each attempted append. The lookup
reserves its full 128*N bytes and file slot before native creation. Creation
failure/cancellation/exception rolls those reservations back; successfully
adopted or partially written owners keep them until closure. Lifetime high-water
counters include temporary reservations even if creation subsequently fails.
Factory directory handles remain outside the byte-file slot count.

Initialization writes every byte explicitly in at most 64 KiB per advance; it
never relies on sparse holes or unwritten ID-slot contents. Later advances
consume one bounded source page, check the ID slots and write both fixed entries.
Partial transfers, zero/overcount progress, errors, cancellation and exceptions
cannot publish a partial lookup. Native End, complete permutation and exact table
size precede Ready. Empty source verifies native cache End without another file
or reservation. Invalid limits, quota exhaustion and an already requested stop
are checked before creation; arithmetic is bounded before multiplication.

`DirectoryIndexLookup::read_page(first_rank, limits)` reads two fixed entries to
locate the requested start, then one bounded sequential source page, including
normal source codec/CRC/address checks. It does not scan from the first row or
retain every source address. Page results preserve original IDs and observations.
An inadequate first-record wire budget can retry without revoking either healthy
owner. End checks native source size; each lookup query checks table size. Other
source pages retain the store's existing validation scope rather than asserting
a snapshot or rehashing the complete source on every request.

`find_row(IndexRowKey)` validates the original dataset's row namespace and ID,
then returns the zero-based cached position after checking both table entries.
It checks the source's current store state, not all source bytes or any native
directory. Foreign/moved/zero/out-of-range keys and out-of-range positions do no
I/O and do not revoke a healthy lookup. Re-sorting retains the row namespace;
a fresh dataset/refresh has another namespace even if its ordinal numbers match.

Lookup I/O/corruption/cancellation revokes that lookup. Source reads finish one
bounded page before observing projection cancellation, preserving an otherwise
healthy source; actual source I/O/corruption still revokes the source and lookup.
Exceptions revoke before propagation. Issued lookup owners keep their file and
source pin after builder abandonment, with reservations held until closure.
They must be used and retired on a worker. Cached keys, offsets and positions
are not native acquisition or mutation authority and require the later pane
request/generation gate before UI use.

The native 100,000-record sorted-cache test now builds a 12,800,000-byte lookup,
queries positions in nonsequential order, verifies original IDs and metadata,
resolves original row keys back to positions, and checks End and retirement.
Its bounded cache measurement is separate from real-directory or UI latency/RSS
qualification. The collector's 259-entry real-directory integration remains in the same run.


### Worker-side indexed pane preparation and pages

`IndexedPaneDataJob` privately constructs `CompletedIndexedPaneData` from a
completed directory index or an existing prepared dataset plus a later pane
request. It prepares the requested native sort order and a disk lookup through
bounded sorter/lookup advances. An already matching source order can reuse that
sealed record store; each preparation builds its own lookup and owns its
reservations. No directory-sized row/address vector is materialized.

Preparation retains the exact source namespace, original row namespace, request,
held directory and native before/after observations. Before completion it checks
the held directory namespace and metadata again. The shared pure directory-check
helper preserves the collector's contradiction/error semantics. Internally,
available directory identity/kind/modification/change-time evidence is retained
across reorders, including when subsequent queries are denied or unknown. This
composite baseline is private continuity evidence, not a point-in-time native
observation or a snapshot. Public before/after observations remain actual query
results, with unavailable states intact.

Each completed dataset has a distinct private revision token. Reorder requests
bind both the original row namespace and the exact current dataset revision.
An older preparation for the same rows cannot discard newer continuity evidence.
Listing and reorder origins are distinct, and requested order must match before
publication. Failed/cancelled/exceptional preparation releases its private work
without publishing a partial result; earlier datasets remain independently owned.

`IndexedPane` is serialized worker state. Navigation and reorder issue monotonic
pane-scoped request numbers, revoke old stamps and retain the previous dataset
until successful publication. Stale, foreign, wrong-order, wrong-origin or wrong
predecessor replies cannot replace current data. Native/cache failures preserve
their diagnostics. Failed or cancelled navigation preserves the displayed dataset;
while a listing is pending, focus commands are unavailable. Fresh navigation
initializes focus through one bounded lookup read. Reordering preserves the
original scoped focus key, independently of changed positions.

Page and focus requests require a current `PaneStateStamp`, including a freshness
check after bounded I/O. Older successful replies and older failures are discarded
without altering newer request status. Native I/O exceptions revoke the affected
current stamp before propagation; stale exceptions do not revoke a newer stamp.
Request cancellation finishes the bounded lookup operation and discards its
payload, leaving a healthy current dataset usable. This does not interrupt OS
calls or permit concurrent model access; all state changes remain on one worker.

`IndexedPanePage` is immutable and contains escaped display names, scoped row IDs,
basic metadata/error observations, focus, ordering, counts and pending/error
status. It contains no native name, cursor, lookup, store, model or native owner.
A page remains readable after the worker retires every data/native owner; its
revoked stamp prevents later actions. `end()` describes displayed cached rows,
including an empty/pending pane, not native enumeration or a snapshot guarantee.

Capture bounds rows (1..4096), encoded source wire bytes (16..64 MiB), and all
copied dynamic text (1..16 MiB). The shared payload-budget helper charges escaped
names, every basic-metadata diagnostic, hidden/outer inspection diagnostics and
listing errors; existing in-memory pane pages use the same accounting. A failed
budget returns no prefix, truncation or RAM fallback. Page construction/decoding
and all native/index I/O occur on the worker, never in UI paint/provider/event
callbacks. UI integration must pass only these owner-free payloads across threads.

This milestone implements ordered indexed datasets, generation gates, focus and
bounded display capture. Indexed filtering, marking, selection inventories and
native acquisition remain subsequent services; the API does not accept those
commands and silently ignore them. No Files application/view graph or mutation
commands are enabled by this backend milestone.

The native 259-entry integration now prepares the canonical dataset and lookup,
publishes it into indexed pane state, captures an 11-row display page, checks
native identity namespaces and original row IDs, and retires the worker/native
owners while retaining the readable revoked page. This is end-to-end native/core
integration, not a rendered-window or full-application responsiveness result.
