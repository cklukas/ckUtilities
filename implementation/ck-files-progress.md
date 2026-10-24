# Files implementation evidence

The complete scope and acceptance criteria remain in [the work-package plan](ck-files-plan.md).
Progress here does not enable unimplemented capabilities or relax release gates.

## WP-00 — complete

Commit `3dd2034` upgrades the installed SDK baseline to ckVision 1.4.0 and
records the behavioral, native adapter, preservation, consent and recovery
contracts. See the [SDK validation record](../docs/migration/ckvision-1.4.0-validation.md).

## WP-01 — in progress

### Lossless native filename foundation

`ck_files_core` is a new strict-warning, UI-independent library.
`NativeName` stores tagged POSIX bytes or Windows UTF-16 code units without
normalization, case conversion or replacement characters. Component validation
rejects separators, embedded NUL, empty and navigation names. Filesystem naming
restrictions remain a separate, not-yet-implemented capability check: observing
an existing reserved or unusual name must not silently rewrite it.

The display representation is separate from native bytes. Invalid encoding,
terminal and bidi controls, line separators and noncharacters are escaped;
literal backslashes are escaped too, making literal escape spellings distinct
from malformed or control-bearing names. Normal Unicode, combining sequences,
emoji joiners and original spelling are retained. Display text is never an
operation path. The model deliberately has no copy/move/delete methods.

Validation on 2026-10-23, with ckVision 1.4.0 and warnings treated as errors:

| Host/configuration | Build interval (Europe/Berlin) | Elapsed | Result |
| --- | --- | --- | --- |
| macOS arm64, Apple Clang 21, Release | 00:51:17.010–00:51:19.242 | 2.232 s (incremental test edit) | Names suite passed |
| Lima Debian 13 arm64, GCC 14, Release | 00:52:27.194–00:52:39.375 | 12.182 s (new target/configuration) | Names suite passed |
| Windows workstation x64, MSVC 19.44, Release/static CRT | 00:52:39.524–00:52:51.621 | 12.097 s (new target/configuration) | Names suite passed |

These establish the first incremental/new-target timing observations for this
module; they are not comparable full-suite build baselines. Logs show the new
sources compiled and tests linked. The initial test declarations used quoted
names instead of cktest identifiers; that compile error was corrected before
any tests ran. No warnings were suppressed.

Eight test cases exercise exact native-unit storage, component boundaries,
encoding tags, valid Unicode boundaries, invalid UTF-8 (including overlong,
surrogate and out-of-range encodings), all 2048 unpaired UTF-16 surrogate
units, all 253 valid one-byte POSIX basenames, control/escape ambiguity,
normalization differences and names beyond the SDK completion size bound.
The long-name test establishes model preservation only, not filesystem support.

All three hosts verified the same eight exported source files by SHA-256 before
building. Core source fingerprints:

| File | SHA-256 |
| --- | --- |
| `native_name.cpp` | `3391edb2f26c06b4c35871b684e2c95602ca65571d679814d49fa92587e1b983` |
| `native_name.hpp` | `ee0023056a46eb72e2943759fd874cbc93c826adc6ad7ce601529acb388a29e1` |
| `test_files_names.cpp` | `51c6542c3e37d787eda81043e2e164662ce72011f47457fd5c4952bb6a28347b` |

An additional macOS build directly compiled the core, its eight tests and the
header-only cktest entry point with AddressSanitizer and UndefinedBehaviorSanitizer,
`-O1 -g -fno-omit-frame-pointer` and warnings as errors. Build interval:
2026-10-23 00:53:59.130–00:54:07.886 Berlin (8.756 seconds). All eight cases
passed under the sanitizers (0.316 seconds), with no reported violations. This
also compiled the core without linking any UI library. The disposable sanitizer
executable was removed after validation; logs and timing evidence were retained.

The current task root on macOS is
`/Volumes/S8T/tmp/ckutilities-files-20261023-01a165ae`, with the native CMake
test at `build/tests/unit/ck_files/ck_files_core_tests`. Linux uses
`/home/klukas.guest/build/ckutilities-files-20261023-01a165ae` and the same
build-relative test path. Windows uses only
`V:\tmp\ckutilities-files-20261023-01a165ae`, with its executable at
`work\build\tests\unit\ck_files\ck_files_core_tests.exe`. Native test paths
remain inside their selected task workspaces. No Files application is launched.

### Explicit observations and native basic metadata

The next WP-01 milestone adds `Observation<T>`, scoped native identities,
lossless timestamps and an injected, read-only `BasicFileSystem` boundary.
Unavailable fields distinguish unsupported, unknown, denied, gone, changed and
failed states, retaining native error domains/codes. Available zero or an empty
value cannot become an unknown value. Throwing-copy tests verify that assignment
cannot leave an observation without an active variant state.

Identity comparison retains the complete 128-bit Windows ID, native volume,
identity scheme and optional generation. Independent adapter instances cannot
assert identity equality. Equal native IDs establish neither content equality
nor durable identity across remounts/restarts, and never authorize mutation.
Timestamp conversion preserves pre-epoch values and native fractional units
without overflow, truncation to seconds or fabricated zero values.

`ck_files_platform` uses native absolute paths without a UTF-8 round trip or
process-current-directory fallback. macOS uses `lstat`; Linux uses no-follow
`statx`, respecting each returned field mask. Only unavailable kernel `statx`
(ENOSYS) invokes the documented older `lstat` query; denial and I/O errors are
preserved. Linux attribute values retain the supported-bit mask. Windows uses
attribute-only, shared handles with open-reparse-point/no-recall flags and
handle-based queries. Unsupported 128-bit identity queries may use the explicit
legacy 64-bit identity scheme; an unqualified zero legacy index remains unknown.
Direct Windows device, pipe and ambiguous DOS-device path spellings are rejected
before opening. Ordinary drive/UNC, extended filesystem and volume-GUID path
namespaces are handled explicitly. Earlier path components retain host
navigation semantics: this is not a pinned-parent security boundary.

Native fixtures cover regular/empty files, directories, hard links, independent
adapter scopes, missing/invalid paths, POSIX final/dangling links and FIFOs,
permission denial, mode zero, exact pre-epoch/nanosecond POSIX timestamps, Linux
raw non-UTF-8 names, Windows unpaired UTF-16 names, exact 100-nanosecond Windows
timestamps, direct device rejection and extended/volume-GUID aliases. Core tests
also cover all observation states, full-width IDs, optional generation and
extreme timestamp conversions. These are basic metadata tests, not qualification
of all filesystems, ACLs, streams, provider data or operation preservation.

Validation on 2026-10-23, Release, warnings as errors, targets
`ck_files_core_tests` and `ck_files_native_tests`:

| Host | Final incremental build interval (Europe/Berlin) | Elapsed | Result |
| --- | --- | --- | --- |
| macOS arm64 / Apple Clang 21 | 01:49:09.058–01:49:22.754 | 27.764 s | 20 cases, three CTest entries passed |
| Lima Debian 13 arm64 / GCC 14 | 01:29:40.103–01:30:32.326 | 51.902 s | 21 cases, three CTest entries passed |
| Windows x64 / MSVC 19.44, static CRT | 01:31:14.244–01:31:24.436 | 10.192 s | 21 cases, three CTest entries passed |

Final test-command elapsed times were 3.981 s, 1.167 s and 1.722 s respectively.
All three final incremental logs show compilation of the changed native fixture
and linking of the native test executable. Earlier builds compiled the new core
and platform sources. The Windows alias fixture initially used forward slashes
in an extended path and failed; its construction now uses native preferred
separators. Production paths were not rewritten to conceal this fixture error.

Both remote hosts verified exactly the same 16-file SHA-256 input manifest as
the current local sources before compiling. Manifest SHA-256:
`8648fca6b901bca0b0e86f094d80ea4bc6a153cd185262514de02de9a44a6a5c`.
The Windows adapter fingerprint is
`f529154bd33e2b88c1da695a1ae1f11851b98c3e5b4746f9a2324b97b7ecf101`;
the POSIX adapter fingerprint is
`0b6d06ad2a207fa8f21116f75a74d90e7a6fab709485c7e15249c137af1cf7d5`.
Detailed command logs, timing JSON and the manifest remain under the task root.
The current verified binaries reside in the same build directories recorded
above: `ck_files_core_tests` and `ck_files_native_tests` (Windows `.exe`).

A separate macOS direct compile with AddressSanitizer/UndefinedBehaviorSanitizer,
`-O1 -g`, frame pointers and strict warnings passed all 20 core/POSIX-native
cases with no reported violations. Build interval: 01:25:47.160–01:27:52.582
Berlin, 125.421 s; test elapsed: 1.205 s. The subsequent source edit changed only
the Windows-conditional alias fixture, excluded from this sanitizer build.
The sanitizer command directly compiled seven translation units without a build
cache. Host load was unusually high during these runs (observed load averages
over 190); logs and actual compiled inputs were checked because elapsed times
were above earlier incremental observations. Timing alone was not treated as
freshness evidence. Disposable sanitizer output is removed after retaining this
record; current CMake test binaries remain available. No Files app was launched.

### Owning directory cursors and bounded enumeration

Directory cursors now own the opened directory object. POSIX uses
`open(O_DIRECTORY | O_CLOEXEC)` and `fdopendir`/`readdir`; Windows uses a shared
native directory handle and `FileIdBothDirectoryInfo` continuation queries.
Neither adapter reconstructs/reopens the original pathname while enumerating.
The implementation follows the public [Windows directory-query contract](https://learn.microsoft.com/en-us/windows/win32/api/winbase/ns-winbase-file_id_both_dir_info),
[POSIX descriptor ownership](https://man7.org/linux/man-pages/man3/fdopendir.3p.html)
and [readdir end/error rules](https://man7.org/linux/man-pages/man3/readdir.3.html).
Native tests rename the opened directory, create a replacement at its former
path and destroy the filesystem adapter, then prove that the cursor still lists
the original directory. The captured native identity remains scoped to that
adapter and differs from the replacement's identity.

Final links/reparse points require an explicit follow policy. POSIX rejects
final symlinks with and without trailing slashes; Windows rejects junctions with
and without trailing separators. Earlier components retain host navigation
semantics. These handles do not freeze child entries or create a filesystem
snapshot, and names are not authority to mutate an object later.

A read returns at most the requested number of names (1–4096) and performs at
most 4096 native steps, including omitted navigation entries. Native order is
unsorted. Cancellation is checked between steps; a cancelled partial batch can
resume without dropping names. Native calls may block and belong to workers.
End and errors are sticky; a late error retains the names already read and its
native error details. If an adapter or allocation throws after cursor progress,
resumption reports failure rather than falsely claiming a complete listing.
Explicit extraction of an owning observation payload resets its state to Unknown.
POSIX directory-open ENOTDIR is a failure with its native code preserved: it can
mean an existing non-directory or a rejected final link, not a vanished object.

Windows buffers have a fixed 128 KiB bound. The portable counted UTF-16 decoder
checks record/name/next offsets, alignment, lengths and invalid basename data;
its layout constants are statically checked against the Windows SDK. Synthetic
malformed/truncated buffer tests run on all hosts, including under sanitizers.
Native tests read 601 entries with long names across multiple native buffers and
small requested batches, without truncation, duplicates or navigation entries.
They also cover cancellation, unusual native units, invalid/missing/non-directory
paths, POSIX permission denial and Windows sharing denial/device rejection.

The first macOS unusual-name fixture attempted invalid UTF-8 on APFS, which
rejected its creation. It now uses valid Unicode, backslash and newline there;
Linux retains a raw invalid-byte name and Windows an unpaired UTF-16 surrogate.
Portable core tests still retain arbitrary valid POSIX component bytes. The
initial Windows sharing fixture used attribute-only access, which did not
establish a share conflict; the corrected fixture holds directory-list access.
That initial assertion failed and a subsequent test dereference of absent error
details crashed the test process; the detail assertion is now guarded. The
corrected sharing-denial fixture passes. No adapter behavior was weakened to
accommodate either fixture error.

Final validation on 2026-10-23 used warnings as errors and the same 22-file
SHA-256 manifest on all hosts. The full manifest, commands and machine-readable
timings are in [the evidence record](evidence/ck-files-directory-20261023.json).

| Host / Release toolchain | Final incremental build interval (Europe/Berlin) | Elapsed | Files test result |
| --- | --- | --- | --- |
| macOS arm64 / Apple Clang 21 | 01:49:09.058–01:49:22.754 | 13.692 s | 35 cases, five CTest entries passed |
| Lima Debian 13 arm64 / GCC 14 | 01:48:59.412–01:49:18.238 | 18.826 s | 36 cases, five CTest entries passed |
| Windows workstation x64 / MSVC 19.44, static CRT | 01:49:01.342–01:49:04.863 | 3.521 s | 36 cases, five CTest entries passed |

Test commands took 2.086 s, 0.307 s and 0.687 s respectively. Final logs
show the changed platform/test inputs compiled and native tests linked; earlier
incremental runs compiled the new core/decoder and linked both test executables.
CMake regeneration builds earlier in this milestone took 31.866 s and
35.156 s on macOS, including configuration; the final incremental build did not
need regeneration. These are target-specific observations, not full-app clean
build baselines. Current binaries remain in the previously recorded active
build directories, at `tests/unit/ck_files/ck_files_core_tests` and
`ck_files_native_tests` (Windows `.exe`). No Files application was launched.

A direct macOS AddressSanitizer/UndefinedBehaviorSanitizer build compiled all
11 core/POSIX/test translation units with `-O1 -g`, frame pointers and strict
warnings. Build interval: 01:49:09.109–01:50:26.479 Berlin (77.364 s); all 35
cases passed in 0.918 s without reported violations. Its input tree fingerprint
matches both the final CMake build and test records. This direct compiler command
uses no build cache; its explicit source list and logs were checked because
host contention made earlier timing observations unusually variable. Disposable
sanitizer output and known abandoned failed-fixture directories are cleaned
after retaining evidence; source, current test builds and logs are preserved.

### Pinned child object observation

`DirectoryCursor::open_child` now opens exactly one native basename relative
to the owned parent handle, without following the final link/reparse point.
The returned `OpenedEntry` owns that object independently of the cursor and
filesystem adapter. `inspect_basic()` queries the held object, including after
its name is renamed, replaced or unlinked. A repeated query observes changes;
it is not cached metadata or a content snapshot. Comparing these observations
still does not prove that a former pathname names the held object or authorize
mutation. Full plan revalidation and retirement protocols remain outstanding.

Linux uses `openat(O_PATH | O_NOFOLLOW | O_CLOEXEC)` and empty-path `statx`, with
`fstat` as the documented older query when statx is unavailable. macOS uses
`openat` with public event-only/symlink/nonblocking/no-controlling-terminal and
close-on-exec flags, followed by `fstat`. These event-only handles do not prevent
volume unmount. Native errors and permission failures remain explicit.
Absolute POSIX observations now use the same conversion via `fstatat`; they
retain no-follow and absolute-path semantics.

Windows uses the documented user-mode
[NtCreateFile](https://learn.microsoft.com/en-us/windows/win32/api/winternl/nf-winternl-ntcreatefile)
interface, linked through the SDK's `ntdll.lib`. The owned directory is the
`RootDirectory`; the counted UTF-16 name is one component, with exact-spelling
lookup, reparse/no-recall flags, metadata/synchronization rights, shared access
and FILE_OPEN disposition. No absolute pathname is reconstructed and no create,
overwrite or privilege-bypass flag is used. Native NTSTATUS values are retained
in their own error domain; conversion to Win32 errors only classifies the
observation state. Unsupported or denied opens do not retry through a path.

Foreign name encodings, malformed/moved-from components, excessive Windows
counted-string lengths and stream syntax are refused before native lookup.
Windows absolute-object inspection also refuses stream syntax: a stream handle
can have its owner's ID with different data, so it must not masquerade as an
ordinary main-data object. Additional channels will be inventoried separately.
Literal reserved names and trailing dots/spaces use native relative lookup;
they are not stripped, rewritten or interpreted as DOS devices.

Eight new native cases on each host prove parent-handle binding after pathname
replacement; child-handle survival after rename, replacement, unlink and parent
closure; live metadata changes; missing/foreign-name errors; directory and
hard-link identities; and unusual native name units. POSIX cases include final
and dangling symlinks, FIFOs and the mode-zero access contract. Windows cases
include real reserved/trailing-name files, a real additional data stream, stream
path refusal, literal-name cleanup and a junction whose handle differs from its
target. The junction fixture is shared with the earlier directory tests.

The initial macOS event-only FIFO open waited for a peer. A one-second process
sample confirmed the stack in the FIFO case at `PosixDirectoryCursor::open_child`
→ `openat`. The exact task-owned test PID was verified and stopped; the failed
CTest command recorded 56.473 s. Adding O_NONBLOCK/O_NOCTTY corrected the native
open protocol; the original FIFO fixture then passed. The next test reported
EACCES for a mode-zero child. This established a platform access limit rather
than an adapter fallback opportunity: Linux's O_PATH observation succeeds;
the macOS test explicitly verifies Denied/native EACCES and unchanged mode bits.
The earlier broad expectation was corrected; no chmod, elevation or unpinned
production query was introduced. See Apple's public
[open flags](https://github.com/apple/darwin-xnu/blob/main/bsd/man/man2/open.2)
and Linux's [O_PATH/openat contract](https://man7.org/linux/man-pages/man2/open.2.html).

Final validation on 2026-10-23 used ckVision 1.4.0, warnings as errors and the
same 27-file source manifest on all three hosts. The full manifest, native test
log digests, commands and timing records are in
[the child-observation evidence](evidence/ck-files-child-20261023.json).

| Host / Release toolchain | Final incremental build interval (Europe/Berlin) | Elapsed | Files test result |
| --- | --- | --- | --- |
| macOS arm64 / Apple Clang 21 | 02:01:26.659–02:01:29.337 | 2.677 s | 43 cases, six CTest entries passed |
| Lima Debian 13 arm64 / GCC 14 | 02:02:16.126–02:02:19.749 | 3.623 s | 44 cases, six CTest entries passed |
| Windows workstation x64 / MSVC 19.44, static CRT | 02:02:19.595–02:02:27.178 | 7.582 s | 44 cases, six CTest entries passed |

Test commands took 0.507 s, 0.073 s and 1.269 s respectively. Logs show the
changed native sources/tests compiled and native tests linked. Earlier builds
compiled the new core boundary and regenerated CMake (initial macOS build:
26.674 s, later regeneration: 20.232 s); the final builds were incremental.
The existing comparable incremental observations and actual compiled inputs
were checked, rather than using duration as freshness proof. Current binaries
remain in the same active build directories and exact test executable paths
recorded above; no Files application has been launched.

A direct macOS AddressSanitizer/UndefinedBehaviorSanitizer build compiled all
12 core/POSIX/test translation units, strict warnings, `-O1 -g` and frame
pointers. Build interval: 02:02:42.665–02:03:02.402 Berlin (19.737 s); all 43
cases passed in 0.560 s without reported violations. Its input tree fingerprint
matches the final CMake build and test records. Disposable sanitizer output and
identified abandoned failed-fixture directories are removed after preserving
this evidence. Source, current CMake test builds and concise logs remain.

### Native data-channel catalog and explicit coverage

`OpenedEntry::inspect_data_channels` now reports native identifiers, main/named/
resource-fork/opaque classification, logical size, native
allocation size when available and the held owner's identity. It reads no channel contents.
Coverage names the exact native query family; an unavailable query is never an
empty collection, and a successful family query is not complete metadata.
These live observations do not establish content equality, snapshot stability,
continued pathname membership or authority for file operations.

Feature identifiers have their own lossless namespace, with POSIX-byte or
Windows-UTF-16 tags. They reject empty/NUL identifiers but preserve separators,
colon syntax, unpaired units and original spelling. They cannot be used as
validated basenames. Filename and feature presentation now share the existing
strict UTF-8/control escape implementation; operation identifiers remain raw.
All original filename boundary/encoding tests continue to pass.

Windows uses handle-based
[FileStreamInfo](https://learn.microsoft.com/en-us/windows/win32/api/winbase/ns-winbase-file_stream_info)
queries with 8-byte-aligned buffers. The portable decoder checks SDK-confirmed
offsets, counted UTF-16, forward/aligned record links, signed sizes, duplicates,
truncation and resource bounds. Empty named streams are retained; unfamiliar
native types remain opaque. Failed short-buffer queries restart the entire
read-only query with bounded growth; no truncated prefix becomes a catalog.
Denied/I/O errors do not retry through another path. The documented HANDLE_EOF
no-stream outcome is tested independently using FindFirstStreamW on an empty
directory. Stream enumeration on directories and ownership after rename,
replacement and unlink are qualified on the workstation test volume.

The default native buffer limit is 1 MiB, adjustable within an 8 MiB hard bound;
record limits default to 16384 within a 65536 hard bound. Invalid limits fail
before querying. Exceeding a valid limit returns Unknown with an explicit
limit diagnostic and no falsely complete prefix. These are catalog-memory
limits, not channel-content-size limits. The future index/planner must preserve
this failure and complete discovery or refuse unsafe operations.

macOS queries `com.apple.ResourceFork` size through
[fgetxattr](https://developer.apple.com/library/archive/documentation/System/Conceptual/ManPages_iPhoneOS/man2/fgetxattr.2.html)
on the owned descriptor. ENOATTR establishes absence for that exact native
feature at query time; allocation remains Unknown. Coverage is explicitly
MacResourceFork, not all attributes or provider channels. Independent named-fork
writes qualify a binary resource fork, changes to its size, a 10 MiB fork with a
one-byte native-query-buffer budget, retention after rename/replacement/unlink,
and non-following inspection of a final symlink whose target has a fork.

The generic Linux channel query remains UnsupportedQuery. This is a limitation
of the current backend query, not proof that a Linux-mounted filesystem has no
additional channels. Driver-specific/xattr discovery, binary attribute values,
security descriptors/ACLs and provider/storage features remain WP-01 work.
No mutation is enabled while that inventory/capability work is incomplete.

The first Windows native catalog test aborted with 0xc0000409. Its helper used
`for (... : path.filename().native())`: under C++20 the native string reference
outlived the temporary path. Keeping a named filename object corrected the
lifetime. Query exceptions now print diagnostics and still fail the test. An
added independent directory-stream fixture also had a local-name collision,
corrected before execution. Production catalog checks/expectations were not
relaxed. Native suites and an additional Windows AddressSanitizer build passed
with the corrected helper. Abandoned fixtures are identified by exact task paths
and removed only after the owning processes are terminal.

Final validation on 2026-10-23 used warnings as errors and the same 34-file
SHA-256 manifest across all hosts. Exact commands, source hashes, CTest-log
digests and sanitizer inputs are in
[the channel-catalog evidence](evidence/ck-files-channels-20261023.json).

| Host / Release toolchain | Final incremental build interval (Europe/Berlin) | Elapsed | Files test result |
| --- | --- | --- | --- |
| macOS arm64 / Apple Clang 21 | 02:28:17.745–02:28:24.151 | 6.406 s | 53 cases, eight CTest entries passed |
| Lima Debian 13 arm64 / GCC 14 | 02:28:09.735–02:28:17.228 | 7.493 s | 51 cases, eight CTest entries passed |
| Windows workstation x64 / MSVC 19.44, static CRT | 02:26:54.635–02:26:58.793 | 4.158 s | 54 cases, eight CTest entries passed |

Test commands took 1.088 s, 0.456 s and 0.920 s respectively. Final logs show
the changed native fixture compiled and native tests linked; earlier builds
compiled the new core/native queries and all affected targets. Initial macOS
CMake regeneration/build took 20.366 s. Timing differences from previous
incremental/regeneration observations were checked against actual compiled
inputs; elapsed time was not used alone to establish freshness. Current
CMake test executables remain in the same exact active paths recorded above.
No Files application was launched.

Direct sanitizer builds compiled all 17 core/platform/test translation units:

| Host / sanitizer configuration | Build interval (Europe/Berlin) | Build / test elapsed | Result |
| --- | --- | --- | --- |
| macOS arm64, ASan + UBSan, strict warnings, -O1 -g, frame pointers | 02:30:11.025–02:30:50.007 | 38.983 / 0.539 s | 53 cases passed |
| Windows x64, MSVC ASan, /O1 /Zi /MT /W4 /WX, halt_on_error=1 | 02:30:07.333–02:30:31.760 | 24.427 / 0.804 s | 54 cases passed |

Neither reported sanitizer violations or suppressed product warnings. The
macOS input-tree fingerprints match the final CMake build/test records. Windows
verified the shared source manifest plus every directly compiled translation
unit (including the shared cktest main). The Windows ASan runtime was already
installed; all project sources, objects, PDBs, executables, temporary writes and logs used
V:. These are the first direct 17-unit observations for this module/configuration;
commands compile the listed inputs without a build cache. Disposable sanitizer
outputs are removed after retaining concise evidence and the current normal
CMake test builds are preserved.

Remaining WP-01 work includes xattr/value and full metadata inventories,
driver-specific channel coverage, destination capabilities, filesystem/time
fakes and pane sorting/filter/selection semantics. No WP-01 checkbox is marked
complete by these foundational milestones. No mutation command exists.


## WP-01: binary extended-attribute observations (2026-10-23)

Added bounded attribute catalogs and explicit binary value reads to owned entries.
Identifiers distinguish POSIX raw bytes, UTF-16 channel identifiers, and Windows
8-bit EA names; display escaping never changes the native identifier. Catalogs
carry owner observations and exact coverage, with individual size/flag errors.
Known security, resource-fork, compression-representation and required-EA roles
are classified without asserting cross-platform equivalence or safe replay.

POSIX list/value readers confirm even zero-length values with a real read,
retry only size-growth races up to three attempts, and reject malformed or
incomplete lists without returning a successful prefix. Limits report Unknown;
access denial and native I/O failures retain their native codes. macOS uses
owned-descriptor flistxattr/fgetxattr with public XATTR_SHOWCOMPRESSION so hidden
compression attributes are not deliberately omitted. Linux O_PATH observations
use the kernel /proc/self/fd namespace, verify procfs and the held object's
identity before/after queries, and never reopen the original name or acquire a
data-read descriptor. If that namespace or identity cannot be established, the
query remains unavailable. This requires a usable procfs; it is not a generic
promise for every Linux namespace. Successful coverage is explicitly limited
to attributes visible to current credentials; inaccessible namespaces are not
claimed absent. These live reads do not establish a filesystem snapshot.

Windows uses the documented native EA query contract through the exported
NtQueryEaFile service, one complete record at an explicit one-based index.
Bounded retries repeat that index, avoiding partial-prefix/cursor assumptions.
FILE_READ_EA is requested explicitly with MetadataAndAttributes; metadata-only
handles retain their narrower rights. Missing filtered results and zero-length
EA deletion witnesses are Gone, distinct from valid empty POSIX values. Native
flags are preserved; FILE_NEED_EA is identified as requiring interpretation.
Native tests actually wrote and queried feature-bearing EAs on V:'s ReFS volume;
no NTFS or other filesystem was substituted to obtain passing results.

New conformance tests cover binary and empty values, native raw/Unicode names,
large values, limits, owner identity after rename/replacement/unlink, live
updates, directories, and final symlinks/junctions with attributes on targets.
Linux mode-zero denial preserves permissions; Windows metadata-only access
cannot read EAs. Portable tests cover malformed/truncated lists/records,
namespace separation, exact binary bytes, size races and retry limits.

Validation results and exact fingerprints are recorded in
[evidence](evidence/ck-files-attributes-20261023.json). No mutation API, automatic
elevation, hydration consent or metadata-preservation claim is introduced.
Full security/ACL and storage/provider inventories, driver-specific channel
coverage, destination capabilities, filesystem/time fakes and pane semantics
remain WP-01 work. The package's acceptance checkboxes remain open.


| Host / Release configuration | Final build interval (UTC) | Build / test elapsed | Result |
| --- | --- | --- | --- |
| macOS arm64 / Apple Clang 21 | 01:07:22.462–01:07:24.075 | 1.613 / 0.598 s | 63 cases, ten CTest entries passed |
| Lima Debian 13 arm64 / GCC 14 | 01:06:35.352–01:06:37.231 | 1.879 / 0.083 s | 62 cases, ten CTest entries passed |
| Windows x64 / MSVC 19.44, static CRT | 01:06:37.959–01:06:40.463 | 2.504 / 1.407 s | 64 cases, ten CTest entries passed |

The final build logs show attributes.cpp and its decoder tests recompiled and
both test executables linked. Earlier native implementation/fixture builds are
also recorded in task logs; the same final 38-file source manifest was checked
on Linux and Windows. The short final increments match those actual inputs;
they are not clean full-application build observations. No Files app was launched.

Direct sanitizer builds compiled all 20 core/platform/test translation units.
macOS ASan + UBSan (-O1 -g, strict warnings, halt-on-error) built from
01:06:25.894–01:06:58.858 UTC in 32.964 s and ran 63 cases in 0.612 s.
Windows MSVC ASan (/O1 /Zi /MT /W4 /WX, halt-on-error) built from
01:07:09.226–01:07:40.853 UTC in 31.628 s and ran 64 cases in 0.789 s.
Both passed without sanitizer diagnostics. Their timings were compared with
prior direct 17-unit observations (38.983 s Mac, 24.427 s Windows); logs confirm
all listed units compiled, and no cache substitutes those direct inputs.
The Windows diagnostic launcher initially used an incorrect renamed source
filename and stopped before compilation; correcting the launcher required no
product change. Concise records survive cleanup of disposable sanitizer outputs;
current normal builds remain at the recorded absolute paths. Boundary checks
passed (44 domain files, 385 ckVision-only files), as did ckdocs (24 pages).


## WP-01: native security observations (2026-10-23)

Added explicit, bounded security queries on owned entries. Query scopes and
native encodings distinguish Linux access/default POSIX ACLs, Darwin extended
ACLs, Windows owner/group/DACL, mandatory integrity information and the native
full-descriptor query. A default value means Unknown. Successful absence is a
separate presence value; unavailable, unsupported and denied queries retain
native diagnostics. Native bytes, ACE order, principal identifiers and control
flags are retained without interpreting unlike ACL models as equivalent.
Bounds validators check Linux versioned record framing and self-relative
Windows descriptor/SID/ACL/ACE extents and entry limits. These checks do not
perform semantic ACL validation, principal translation or effective-access
calculation, and do not authorize replay on another filesystem or host.

Linux reads the exact system.posix_acl_access/default values through its
existing owned-object attribute boundary. Only ENODATA establishes absence;
other native failures propagate. Native ext4 fixtures include a named uid and
mask, directory default ACLs, limits, final symlinks and retention after
rename/replacement/unlink. Basic owner/group/mode observations remain separate.

macOS uses public fstatx_np/filesec APIs to distinguish a successfully queried
missing ACL property from an unavailable object. Absence additionally requires
native extended-security support via fpathconf(_PC_EXTENDED_SECURITY_NP).
Working ACLs are owned/freed explicitly and exported with acl_copy_ext; entry
counts and exported bytes are bounded. APFS fixtures qualify ordered deny/allow
entries, UUID qualifiers, directory inheritance flags, absence, final symlinks,
and owned-descriptor retention after rename/replacement/unlink. Public SDK
interfaces are used; implementation-private filesec properties/syscalls are not.

The symlink fixture exposed acl_get_fd_np's overloaded ENOENT when no ACL
property exists. Treating that as object disappearance was incorrect; the
explicit filesec query fixes the classification. An initial compile used an
incorrect pathconf constant spelling; it was corrected to the SDK's public
_NP spelling without warning suppression. A shell sequence then invoked old
test output after that failed compile; that result is excluded from validation.
The final driver stops immediately on build failure and all final tests ran
only after successful builds from the recorded inputs.

Windows requests READ_CONTROL explicitly for ordinary security inspection and
ACCESS_SYSTEM_SECURITY only for the explicit full-security access mode.
The backend never enables privileges, changes credentials, reopens by name or
silently weakens a query after denial. GetKernelObjectSecurity fills bounded
caller-owned buffers in self-relative form. Query masks remain attached to the
result so an owner/group/DACL result cannot be mistaken for full SACL coverage.
Native ReFS fixtures qualify a protected two-ACE DACL, integrity queries,
metadata-only denial, retained owner identity after unlink, and explicit
full-security access compared with an independent native open. Positive audit
ACE/SACL preservation is not established by these fixtures. No security-write
API is exposed by the product; native fixture writers modify only owned tests.

Exact source fingerprints, commands, timing records and log digests are in
[the security evidence](evidence/ck-files-security-20261023.json).

| Host / Release toolchain | Final build interval (UTC) | Build / test elapsed | Files result |
| --- | --- | --- | --- |
| macOS arm64 / Apple Clang 21 | 01:21:15.254–01:21:24.790 | 9.536 / 0.693 s | 70 cases, twelve CTest entries passed |
| Lima Debian 13 arm64 / GCC 14 | 01:21:11.538–01:21:23.380 | 11.842 / 0.180 s | 68 cases, twelve CTest entries passed |
| Windows x64 / MSVC 19.44, static CRT | 01:21:14.656–01:21:30.823 | 16.167 / 1.599 s | 71 cases, twelve CTest entries passed |

The final header change rebuilt all affected core/native test targets (18
steps); logs and the verified 42-file manifest establish compiled inputs.
These are increments without CMake regeneration, compared against earlier
security builds including regeneration (14.851 s Mac and 15.543 s Linux).
The earlier Windows attribute build took 26.065 s including CMake regeneration
and 24 compile/link steps; this final Windows increment has 18 affected steps.
Elapsed time alone was not used as freshness evidence. No Files GUI was launched.

Final direct sanitizer builds compiled all 23 core/platform/test translation
units. macOS ASan + UBSan, strict warnings, -O1 -g and frame pointers built from
01:21:27.270–01:22:08.562 UTC in 41.293 s and ran 70 cases in 0.615 s.
Windows MSVC ASan, /O1 /Zi /MT /W4 /WX built from
01:21:35.898–01:22:18.231 UTC in 42.332 s and ran 71 cases in 0.993 s.
Both passed with halt-on-error and no sanitizer diagnostics. Direct commands
compiled the enumerated sources; no build-cache hit substitutes those inputs.
The preceding 23-unit Mac sanitizer observation took 43.830 s. The earlier
20-unit Windows attribute milestone took 31.628 s; this run adds the security
implementation and fixtures, and its log shows all 23 units compiled. These
observations are configuration-specific comparisons, not invented baselines.
Disposable sanitizer outputs are removed after preserving concise evidence;
current normal CMake outputs remain. All Windows project writes used V:.

Boundary checks passed (46 domain files and 389 ckVision-only files). This
milestone leaves storage/provider and driver-specific inventories, destination
capabilities, injected filesystem/time services and pane semantics in WP-01.
No package acceptance checkbox is closed by this security-read milestone.


## WP-01: held-directory location facts (2026-10-23)

Added read-only location inspection on DirectoryCursor. Observations remain
attached to its held directory and use no original-path fallback. The result
keeps directory identity, native query family, filesystem query identifiers,
raw mount flags, name-limit reports, directory case settings, native feature
advertisements and space-counter results separate. Per-field native errors
survive successful partial results. Default fields remain Unknown. Filesystem
query IDs/serials are not durable/global volume identity or mutation authority.

Feature advertisements are a distinct type, never a boolean operation promise.
Darwin capabilities retain all four capability/validity groups and require
both the mapped feature bit and its valid bit before reporting known support.
Windows preserves its native volume flags; reparse support is not mapped to
symbolic-link support, and block-refcounting is distinct from Darwin cloning.
SDK-based single-bit checks qualify portable mappings independently of which
features happen to be enabled on the test volume. A false advertisement does
not become proof of unsupported operations; missing mappings remain unavailable.

Linux uses fstatvfs/fstatfs/fpathconf on the owned directory. Generic volume
feature advertisements and per-directory case comparison are explicitly
unavailable; neither is guessed from Linux or the filesystem type code.
macOS uses fstatfs, fpathconf and the public fgetattrlist volume-capability
query. The current APFS conformance fixture accepts this query on a held
non-root directory. Older documentation specifies volume-root restrictions;
query failures stay explicit and do not trigger a pathname/root-reopen fallback.
Raw POSIX name/path/character reports remain advisory native values, not a
universal Unicode validation rule. No names are rewritten from these reports.

Windows uses GetVolumeInformationByHandleW and FileCaseSensitiveInfo on the
held directory. The case-sensitive-search volume flag is separate from the
directory setting. NtQueryVolumeInformationFile's documented full-size query
provides caller/actual/total allocation counters from that same handle. Its
public WDK wire layout is declared locally because the desktop SDK does not
provide it; the native API is resolved from loaded ntdll. No private query,
syscall number, elevation, original-path query or shell fallback is used.
Space arithmetic checks zero units, signed negatives and multiplication
overflow; it never clamps an unknown capacity into a usable value. These are
live native reports, not reserved space, physical-allocation proofs or write
permission. A false read-only mount flag also does not grant write access.

Native fixtures qualify retained directory identity and location queries after
rename/replacement/unlink, unchanged enumeration progress, native mount/case
and pathconf results, volume flags and allocation-counter units. Windows test
reference queries resolve the actual fixture mount instead of assuming that
its drive root is the filesystem root. Dynamic free-byte counters are not
asserted equal across independent calls while other workloads run. Deterministic
core checks cover arithmetic extremes, missing validity bits, denied/failed
queries, unknown domains, and feature-family distinctions.

Exact commands, fingerprints, native scope and log digests are in
[the location evidence](evidence/ck-files-location-20261023.json).

| Host / Release toolchain | Qualified build interval (UTC) | Build / test elapsed | Files result |
| --- | --- | --- | --- |
| macOS arm64 / Apple Clang 21 | 01:35:47.005–01:35:56.788 | 9.783 / 0.816 s | 78 cases, fourteen CTest entries passed |
| Lima Debian 13 arm64 / GCC 14 | 01:36:51.200–01:37:05.279 | 14.080 / 0.101 s | 76 cases, fourteen CTest entries passed |
| Windows x64 / MSVC 19.44, static CRT | 01:36:53.925–01:37:18.044 | 24.119 / 1.439 s | 79 cases, fourteen CTest entries passed |

All hosts used the verified 46-file input manifest and warnings as errors.
The Mac qualification increment regenerated CMake and compiled the two new
fixture units; its earlier 15.661 s implementation build compiled the changed
core/native code. Linux/Windows regeneration builds compiled all affected
implementation/fixture targets. Their durations were compared with previous
regeneration observations (security: 14.851 s Mac / 15.543 s Linux; attribute:
26.065 s Windows), then checked against actual compile/link logs. No build
time alone was treated as freshness evidence. No Files GUI was launched.

Direct sanitizer builds compiled all 26 core/platform/test units. macOS ASan +
UBSan, strict warnings, -O1 -g and frame pointers built from
01:38:04.316–01:38:48.144 UTC in 43.828 s and ran 78 cases in 0.624 s.
Windows MSVC ASan, /O1 /Zi /MT /W4 /WX built from
01:38:04.901–01:38:47.708 UTC in 42.808 s and ran 79 cases in 0.991 s.
Both passed with halt-on-error and no sanitizer diagnostics. Prior direct
23-unit observations took 41.293 s Mac and 42.332 s Windows; logs confirm the
additional implementation and two fixture units compiled in these runs.
Owned sanitizer outputs are removed after preserving concise evidence, while
current normal CMake builds remain. Windows project writes stayed on V:.
Boundary checks passed (48 domain files and 393 ckVision-only files).

This milestone does not complete operation-specific destination capabilities.
Timestamp range/resolution, Unicode collision rules, per-object storage/provider
inventory, driver-specific channels, injected filesystem/time services and pane
semantics remain WP-01 work. No package acceptance checkbox is closed here.


## WP-01: reported storage metadata and provider-read guards (2026-10-23)

Added held-object storage observations with explicit Linux statx, Darwin stat
flags and Windows handle-attribute domains. Raw values, native validity masks,
owner, object kind and logical/allocation counts are retained. Queries expose
reported compression/encryption, immutable/append-only, verity/DAX, dataless,
offline/recall, pin/unpin intent, integrity/scrub, reparse and hidden flags only
where the native model supplies them. Missing bits or mappings remain
unavailable. A Clear flag is not a physical representation or locality claim.
Directory compression/encryption bits can describe defaults for children.
Allocation smaller than logical size does not establish sparse extents,
cloning or deduplication; no such inference is implemented.

Windows additionally queries FileCompressionInfo on ordinary file handles,
retaining size, native format and shift fields without assuming that these
fully describe compression (for example external backing). Links/reparse
objects do not trigger target compression inspection. Native fixtures compare
this query with an independently opened attribute-only handle, preserving
native unsupported/error outcomes. A real FSCTL_SET_SPARSE fixture qualifies the
sparse attribute and size observations on ReFS. Darwin's live hidden flag,
statx validity masks, public SDK bit mappings, and held identity after
rename/replacement/unlink are also tested. Final symlinks/junctions keep their
own identity, with no regular-target size/compression claim.

Provider hints distinguish reported recall from pin/unpin intent, cloud-family
and known backing/virtualization tags, and unclassified reparse tags. NoRecall-
FlagReported explicitly does not mean local content or read authorization.
Windows RECALL_ON_OPEN is not interpreted from handle attributes: it is valid
only in directory enumeration and aliases an internal EA bit in other contexts.
All sixteen cloud tag variants and this alias distinction have model coverage.
No generic Linux provider/locality equivalent is fabricated.

Darwin's reported dataless state now defers resource-fork size, attribute-name
and attribute-value queries for provider review. Windows identified provider
states similarly defer stream/EA catalogs, EA values and compression details.
Unavailable metadata retains its original diagnostic; positive hints report
Unknown / ProviderReviewRequired without a bypass or privilege adjustment.
The Windows conformance fixture sets/restores OFFLINE only on its own temporary
file; it verifies review-required results before the missing-EA/stream queries
could produce ordinary results. It is a synthetic flag fixture, not a real
cloud provider, callback or download qualification. Darwin dataless and Windows
cloud/backing classifiers are model-tested; real coordinated provider access
and transitions remain unqualified. These checks do not establish a universal
no-hydration guarantee for unknown drivers or concurrent provider changes.
No user file was dehydrated or provider configuration changed.

Exact source fingerprints, native scope, commands and log digests are in
[the storage evidence](evidence/ck-files-storage-20261023.json).

| Host / Release toolchain | Final build interval (UTC) | Build / test elapsed | Files result |
| --- | --- | --- | --- |
| macOS arm64 / Apple Clang 21 | 01:56:26.216–01:56:28.989 | 2.768 / 0.887 s | 88 cases, sixteen CTest entries passed |
| Lima Debian 13 arm64 / GCC 14 | 01:56:23.448–01:56:26.782 | 3.334 / 0.179 s | 85 cases, sixteen CTest entries passed |
| Windows x64 / MSVC 19.44, static CRT | 01:56:26.374–01:56:30.688 | 4.314 / 1.034 s | 91 cases, sixteen CTest entries passed |

The same 50-file manifest was verified on all hosts. Final increments compiled
the changed storage fixture and linked the native tests; preceding builds
compiled all changed implementation and affected tests. The initial Mac build
took 37.548 s: its log shows 12.3 s CMake configuration, 0.5 s generation and
30 compile/link steps after the shared observation/header changes. The Linux
implementation build took 34.683 s with CMake regeneration and the same broad
recompilation. These are broader inputs than the preceding location builds
(19 affected steps, 14.080 s Linux; 16 initial Mac steps, 15.661 s). The slower
runs were checked against actual compiled inputs rather than assumed fresh
from elapsed time. The final two-step increments are consistent with those
inputs. No Files GUI was launched.

Direct sanitizer builds compiled all 29 core/platform/test units. macOS ASan +
UBSan, strict warnings, -O1 -g and frame pointers built from
01:56:31.836–01:57:30.591 UTC in 58.750 s and ran 88 cases in 0.715 s.
Windows MSVC ASan, /O1 /Zi /MT /W4 /WX built from
01:56:34.999–01:57:24.148 UTC in 49.150 s and ran 91 cases in 0.920 s.
Both passed with halt-on-error and no sanitizer diagnostics. Prior direct
26-unit observations took 43.828 s Mac / 42.808 s Windows. These runs add the
storage implementation and two fixture units plus shared storage/observation
headers; direct compiler commands contain all inputs and no build-cache wrapper.
Concise records survive cleanup of owned sanitizer output directories. Current
normal CMake builds remain; Windows project writes stayed on V:.
Boundary checks passed (50 domain sources, 397 ckVision-only sources).

This is not complete storage/provider preservation. Extent and clone/dedup
relationships, positive encryption/compression fixtures, provider coordination
and race conformance, driver-specific inventories, operation-specific capability
checks, filesystem/time fakes and pane semantics remain WP-01 work. No package
acceptance checkbox is closed by this milestone; no mutation command exists.

### WP-01 — held-object link and reparse observations (2026-10-23)

Added bounded `OpenedEntry::read_link_metadata` observations and a UI-free
structural decoder. [Validation evidence](evidence/ck-files-links-20261023.json)
records the shared 54-file manifest, commands, source fingerprints, timestamps,
results, output paths and cleanup. This remains a partial WP-01 milestone.

POSIX targets remain raw bytes, including relative, dangling and invalid-UTF-8
names. Linux uses `readlinkat` with the held O_PATH descriptor and an empty path.
macOS uses public `freadlink`, guarded for macOS 13 availability; earlier systems
report Unsupported. Neither adapter reopens the original pathname. The bounded
reader grows only when the native result could be truncated, uses a budget+1
witness for an exact-budget target, preserves native errors and never returns a
truncated prefix as a successful observation.

Windows queries `FSCTL_GET_REPARSE_POINT` through the held reparse handle, with
bounded whole-query buffer growth. The decoder validates complete native
lengths, counted UTF-16 fields and offsets. It retains raw bytes, unknown flags,
opaque Microsoft payloads and GUID-owned payloads; only symbolic-link and
mount-point records receive structural name decoding. A changed observed tag
returns Changed. Decoding never authorizes traversal or replay, and identity
plus separate queries does not imply a snapshot or protection from same-tag
content changes.

All eighteen Files CTest entries passed on each host:

| Host | Build interval UTC | Build / test seconds | Cases |
| --- | --- | --- | --- |
| macOS arm64 / Apple Clang 21 | 02:15:51.904–02:16:54.526 | 62.617 / 0.823 | 97 |
| Lima Debian 13 arm64 / GCC 14 | 02:17:16.465–02:18:13.502 | 57.037 / 0.195 | 94 |
| Windows x64 / MSVC 19.44, static CRT | 02:17:19.352–02:17:52.647 | 33.295 / 1.532 | 101 |

Native POSIX fixtures preserve the held symlink target after rename,
replacement and unlink. Windows ReFS fixtures qualify a long dangling junction
that forces buffer growth, exact and insufficient budgets, retained ownership
through rename/replacement/unlink, and a 1500-byte GUID-owned opaque payload.
Positive native Windows symbolic-link-tag qualification remains pending;
parser tests cover counted names, unpaired UTF-16, unknown flags, malformed
lengths, offsets, embedded NULs and truncation. No Files GUI was launched.

The build logs show CMake regeneration and broad header-driven recompilation:
33 compile/link steps on Mac and Linux, 33 on Windows. Mac configuration took
16.4 s plus 0.4 s generation; Linux took 4.9 s plus 0.2 s. These runs cover more
inputs than the preceding storage implementation build (30 affected steps,
37.548 s Mac / 34.683 s Linux); no stale-output inference is based on timing.

Direct sanitizer builds compile all 32 core/platform/test units, including the
three added link units, without a compiler-cache wrapper. Mac ASan + UBSan
(-O1 -g, strict warnings, frame pointers) built 02:22:26.220–02:24:56.487 UTC
in 150.267 s and passed 97 cases in 1.245 s. Windows MSVC ASan
(/O1 /Zi /MT /W4 /WX) built 02:22:26.607–02:23:21.992 UTC in 55.386 s and
passed 101 cases in 0.990 s. Both used halt-on-error and produced no sanitizer
diagnostics. The Mac run was substantially slower than the preceding 29-unit
58.750 s run: live compiler inspection confirmed progress through the expected
translation units, the full direct command records all inputs, and an uptime
sample during compilation showed load averages 26.63 / 34.16 / 38.70 alongside
several unrelated CPU-intensive project processes. This establishes observed
contention, not an exact attribution of elapsed time. No unrelated work was
interrupted. Current normal builds remain; owned sanitizer outputs were removed
after recording executable hashes and confirming no active owners.

UI boundary checks passed (52 domain sources, 401 ckVision-only active files),
and ckdocs checked 24 pages. Full metadata inventories, provider coordination,
operation-specific destination capabilities, filesystem/time fakes, pane state
and collision semantics remain WP-01 work. Unknown provider behavior and
hydration races are not qualified by these metadata fixtures. No package
acceptance checkbox is closed and no mutation command exists.

### WP-01 — bounded pane state and explicit source selection (2026-10-23)

Added a UI-free `PaneModel` with prepared escaped display names, lossless native
names, basic metadata observations and scoped nonzero row handles. Handles are
monotonic within the pane and cannot be reused to retarget an old source or
cross into another pane. Hard-link names remain distinct entries. Sorting and
filtering retain row handles, marks and cursor association; reverse visible-index
lookup is constant-time. [Validation evidence](evidence/ck-files-pane-20261023.json)
records the final 57-file manifest, commands, timestamps, hashes and cleanup.

The model implements name/kind/size/modified sorting, descending order and an
optional folders-first group. Only an observed Directory enters that group;
symlinks/junctions are not followed. Native names use deterministic unsigned
byte/code-unit ordering; secondary name order breaks metadata ties. Unavailable
metadata sorts after available values in either direction. Timestamp ordering
uses seconds/nanoseconds comparisons without overflowing subtraction. Filename
filtering matches prepared escaped display text, with exact UTF-8 bytes and
optional ASCII-only case folding; it does not normalize Unicode or become a
native-name comparison. Unknown/denied hidden flags remain visible.

Marking supports individual visible rows, inclusive/reversed ranges, visible
select-all, invert and explicit clear. Marked sources take precedence over the
cursor, with exact native names/observed identities and visible/hidden counts.
Hidden marked entries yield HiddenMarksNeedReview; pending scans yield
ListingPending. These states do not permit direct operation execution. No source
list is a frozen inventory, preservation consent or native mutation authority.

Listing publication is bounded and commits only after preparing the replacement
state. Duplicate native names, mixed name encodings and resource-limit failures
leave the prior state intact. Request tokens reject stale and foreign success or
failure callbacks. A failed scan retains the old listing, marks, cursor and
original native error. Callers must explicitly distinguish navigation from
refresh. Refresh currently invalidates old source handles, reports the lost mark
count/cursor state and returns SelectionInvalidated until an explicit new
selection; it never falls back automatically to a same-name replacement cursor.
Repeated refreshes retain that unresolved notice. Proven same-object refresh
continuity still needs a held-object reconciliation layer; clearing selection is
not claimed to finish that requirement. Navigation starts a new context.

All nineteen Files CTest entries passed on each platform, including eighteen
new pane cases and a 5000-entry projection test that repeatedly verifies unique
handles, reverse lookup and marks/native-name association across every sort key
and direction:

| Host | Final build interval UTC | Build / test seconds | Cases |
| --- | --- | --- | --- |
| macOS arm64 / Apple Clang 21 | 02:34:38.179–02:34:42.165 | 3.985 / 0.803 | 115 |
| Lima Debian 13 arm64 / GCC 14 | 02:34:52.025–02:34:58.123 | 6.098 / 0.177 | 112 |
| Windows x64 / MSVC 19.44, static CRT | 02:34:55.962–02:35:01.920 | 5.959 / 2.034 | 119 |

Final increments compiled the changed pane implementation and tests, rebuilt the
core library and linked both test executables (five steps). The initial Mac
build took 13.396 s including CMake configuration 8.2 s / generation 0.2 s and
the same five affected steps. Initial Linux/Windows builds took 11.613 / 15.147 s
including regeneration. These are narrower inputs than the preceding link
milestone's broad header rebuild; the shorter duration is consistent with the
logged inputs. No Files GUI exists or was launched.

Focused sanitizer builds compiled five units: native names, basic metadata,
pane model, pane tests and the installed ckVision test host. Mac ASan + UBSan
(-O1 -g, strict warnings, frame pointers) built 02:35:12.514–02:35:25.587 UTC
in 13.073 s and passed all eighteen pane cases in 0.466 s. Windows MSVC ASan
(/O1 /Zi /MT /W4 /WX) built 02:35:49.497–02:35:56.309 UTC in 6.811 s and
passed the same eighteen cases in 0.629 s. Both ran halt-on-error without
sanitizer diagnostics. The initial five-unit Mac sanitizer build took 7.026 s
before adding refresh invalidation and its two tests; the final direct compiler
command, source fingerprints and executed test list were checked, with no
compiler-cache wrapper. These focused checks are not represented as another
full native-adapter sanitizer run. The full Files suite ran separately on each
host. Disposable sanitizer outputs were removed after recording executable
hashes and confirming no active owners; current normal builds remain.

Boundary checks passed (54 domain sources, 404 ckVision-only active files), and
ckdocs checked 24 pages. Limits are explicit (up to one million in-memory rows,
bounded aggregate native/display names); no truncated listing is accepted.
Page-backed larger listings, held-object refresh continuity, destination
collision/capability semantics, remaining native inventories/provider
coordination and filesystem/time fakes remain WP-01 work. None of its package
acceptance boxes is closed by this milestone.

### WP-01 — qualified held-child refresh continuity (2026-10-23)

Added `HeldChild`, an immutable factory-only observation that owns its opened
child and parent directory, exact native basename and basic metadata. A refresh
opens that basename through the same held directory with MetadataOnly access,
retains both child handles while checking identities, and re-inspects the old
object rather than trusting only cached IDs. Qualified matching identities and
unchanged observed kinds yield a later sealed family revision. A cached snapshot
or unrelated matching-ID observation does not establish continuity. Objects are
allocated const and public copy/move construction and assignment are disabled;
compile-time assertions prevent mutable aliases through those special members.
No chain of historical child handles is retained.

Pane refresh now preserves row handles, marks and cursor for these qualified
successors. Unbound, unknown, replaced or changed entries remain explicitly
invalidated; partial retention never silently falls back to another source.
Hidden retained marks still require review. Bound metadata is canonicalized
from the immutable held observation, and a substituted basename is rejected.
Full integer-key indexing supports noncontiguous retained/new row handles with
ordinary equality checks; hash collisions cannot become source identity.
Per-field/query errors remain explicit in continuity observations. The pane
bounds held entries separately (1024 default, maximum configuration 4096);
actual OS resource errors can occur earlier. Publication itself performs no I/O.
The contracts now distinguish this selection continuity from a content or
pathname-membership snapshot, operation consent and mutation authority.

Zero native object IDs no longer establish equality in any supported identity
scheme. A successful Windows FileIdInfo query with an all-zero object ID yields
Unknown without falling back to a narrower legacy ID. The documented
[open-handle identity contract](https://learn.microsoft.com/en-us/windows/win32/api/winbase/ns-winbase-file_id_info)
and [zero-ID rule](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ntifs/ns-ntifs-file_id_information)
were checked against the implementation. [Evidence](evidence/ck-files-continuity-20261023.json)
contains the shared 61-file manifest, fingerprints, commands, timestamps,
diagnostic findings, final results and cleanup.

Eleven injected domain cases exercise handle lifetime, missing/null/denied
objects, successors versus cached/independent observations, replacement,
unqualified identity, kind change, old-handle query failure, canonical metadata,
held-resource limits, partial retention, hidden marks and separate hard-link
names. Five native cases per host qualify stable-file refresh with changed size,
same-name replacement while the old object stays held, held-parent path
replacement, separate hard-link names and final link handling. macOS/APFS and
Linux/ext4 additionally qualify parent rename while the child is held. Windows
uses a distinct refusal test: the classic parent rename returns
ERROR_ACCESS_DENIED (5) while a child is open, continuity remains intact, and a
controlled fixture proves rename succeeds after child observations leave scope.
This matches the documented
[Windows rename restriction](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-fsa/87f86c9b-6c2a-4803-84b7-131a74a434fa).
All hosts separately qualify refresh when the parent path was replaced before
child observation. No production code releases a hold to retry a mutation.

The initial Windows fixture incorrectly expected POSIX-style ancestor rename
while a child was open; diagnostic output established native error 5. That
expectation was replaced with explicit refusal conformance rather than bypassing
the restriction. A separate junction fixture was corrected to create the link
directory required by its helper. The initial standalone sanitizer link command
omitted data_channels.cpp (InventoryLimits::valid); its dependency was added.
The failed diagnostics are retained as hashes/records in the evidence. Three
orphaned private Windows fixture directories from aborted diagnostic tests were
inventoried and removed after verifying zero native-test owners and no reparse
points. No unrelated data or processes were changed.

All twenty-one Files CTest entries passed on the final immutable API:

| Host | Final build interval UTC | Build / test seconds | Cases |
| --- | --- | --- | --- |
| macOS arm64 / Apple Clang 21 | 02:55:27.175–02:55:30.092 | 2.916 / 1.026 | 131 |
| Lima Debian 13 arm64 / GCC 14 | 02:56:35.056–02:56:41.512 | 6.456 / 0.180 | 128 |
| Windows x64 / MSVC 19.44, static CRT | 02:56:39.093–02:56:47.881 | 8.788 / 1.720 | 135 |

Final increments recompiled held-child/pane implementation and affected domain
and native tests, rebuilt the core library and linked both executables (eight
steps). The initial Mac eight-step build took 12.153 s including configuration
8.2 s / generation 0.3 s; the final build needed no regeneration. Intermediate
two-step native-fixture builds and their logs were checked against actual input
changes. Successful test commands ran after their matching builds; code hashes
in the same final 61-file manifest were verified on every host. Fingerprints for the project and
ckVision SDK checkout are recorded; the installed SDK remains ckVision 1.4.0. No
Files GUI exists or was launched.

Focused sanitizer builds compiled ten domain/test/dependency units, including
the real directory/opened-entry default boundaries and injected held-object
fixtures. Mac ASan + UBSan (-O1 -g, strict warnings, frame pointers) built
02:56:55.459–02:57:11.093 UTC in 15.634 s and passed all 29 pane/continuity
cases in 0.364 s. Windows MSVC ASan (/O1 /Zi /MT /W4 /WX) built
02:58:12.135–02:58:25.070 UTC in 12.935 s and passed the same 29 cases in
0.631 s. Both ran halt-on-error with no sanitizer diagnostics. The preceding
comparable ten-unit builds took 23.529 / 13.581 s Mac / Windows; final direct
compiler commands, all listed inputs and hashes were checked, without a compiler
cache wrapper. These focused checks are not a complete native-adapter sanitizer
rerun; the native suite passed separately on all hosts. Sanitizer outputs were
removed after executable hashing and zero-owner checks, retaining current normal
builds and concise evidence. Windows development outputs stayed on V:.

Boundary checks passed (56 domain sources, 408 ckVision-only active files), and
ckdocs checked 24 pages. This implements qualified refresh continuity at the
domain/native seam; worker scheduling, binding acquisition/release policy,
resource-limit UI and mutation review for held-handle restrictions still require
integration. Arbitrary network/virtual filesystem and provider behavior is not
qualified by APFS/ext4/ReFS fixtures. Larger page-backed listings, remaining
inventories/provider coordination, destination collision/capability rules and
full filesystem/time fakes remain WP-01 work. No package checkbox is closed.

### WP-01 — strict filename transport and Win32 convention diagnostics (2026-10-23)

Added `map_native_name` and strict scalar codecs shared with existing native
name display preparation. Mapping within one native encoding preserves its
bytes/units, including malformed Unicode. Mapping across encodings requires
strict UTF-8/UTF-16 scalars; Windows-to-POSIX output is explicitly UTF-8 bytes.
There is no locale conversion, normalization, case folding, BOM removal,
replacement, WTF-8 or surrogate-escape fallback. Unrepresentable encodings or
component boundaries return Unsupported, while byte-budget exhaustion returns
Unknown. Neither returns a partially mapped basename. Invalid source/tag/limit
input returns Failed. Resource budgets (64 KiB default, 1 MiB hard ceiling for
each native representation) do not describe filesystem name/path limits.
[Evidence](evidence/ck-files-names-20261023.json) records the 69-file final
manifest, fingerprints, commands, timestamps, results and cleanup.

`NativeName::is_valid` shares factory component predicates and distinguishes
native component validity from Unicode validity. Moved-from objects can become
invalid: mapping, pane publication, held-child acquisition and both native child
lookup adapters reject that state before it can become a root/cwd lookup or an
empty operation name. Tests cover the actual implementation's moved-from state
without assuming the C++ standard requires strings to become empty after move.

Added fixed-size Win32 convention diagnostics for empty/navigation spellings,
forbidden/control characters and stream syntax, DOS/console device-alias risk
(including superscript digits), and trailing dot/space rewriting. Device-risk
recognition is shared with the Windows absolute-path metadata guard. The
checker does not rewrite a component. Generic convention matches and successful
transport do not establish destination acceptance, collision freedom or creation
authority. The implementations were checked against
[RFC 3629](https://www.rfc-editor.org/info/rfc3629/) and the
[Windows naming documentation](https://learn.microsoft.com/en-us/windows/win32/fileio/naming-a-file).
Per-directory case/normalization/alias behavior and actual operation/API limits
still need native checks. No collision rule is inferred from display ordering,
Unicode equivalence or an OS/filesystem label.

Twelve new domain cases cover every Unicode scalar (1,112,064 values including
zero and noncharacters), independent UTF-8/UTF-16 boundary vectors, malformed,
truncated, overlong, surrogate and out-of-range sequences, exact budget limits,
invalid tags, raw encoding preservation, BOM/normalization distinction, forbidden
units, device aliases, trailing rewrites and separation of display/transport
from Win32 compatibility. Existing display tests still pass after extraction of
the shared codecs. Four new native cases per host exercise mapped BMP/astral
names, unusual native components, colon handling and literal names flagged by
Win32 conventions. Fresh bounded enumeration must reach End and return the exact
native bytes/units, separately from successful lookup; display strings are not
compared. Linux/ext4 qualifies an actual invalid-UTF-8 filename byte
0xFF with backslash/newline; macOS/APFS qualifies backslash/newline without
claiming arbitrary invalid-UTF-8 filename acceptance. Windows/ReFS qualifies a
raw unpaired UTF-16 filename, rejects stream syntax in ordinary file lookup,
and inspects actual literal CON/trailing-dot files through counted-relative
names. Those private literal fixtures are created/deleted through explicit
literal paths; no production write API was added.

All twenty-three Files CTest entries passed:

| Host | Final build interval UTC | Build / test seconds | Cases |
| --- | --- | --- | --- |
| macOS arm64 / Apple Clang 21 | 03:42:56.460–03:42:59.995 | 3.534 / 1.486 | 147 |
| Lima Debian 13 arm64 / GCC 14 | 03:44:01.502–03:44:05.749 | 4.247 / 0.355 | 144 |
| Windows x64 / MSVC 19.44, static CRT | 03:44:04.341–03:44:08.054 | 3.712 / 1.237 | 151 |

Final read-back increments compiled the changed native fixture and linked its
executable (two steps on each host). The preceding Mac test increment took
1.886 s for five steps; its implementation/header rebuild compiled
all affected domain/platform/test inputs (forty steps, 16.314 s); the initial
staged build took 23.338 s with 43 steps and CMake configuration 7.0 s /
generation 0.2 s. The initial
Linux/Windows runs took 30.056 / 44.736 s for 46 steps, including regeneration
2.5 s / 0.1 s and 10.5 s / 0.8 s respectively. These are broader inputs than the
preceding held-child increments; actual compile/link logs and matching final
code hashes were checked rather than inferring freshness from elapsed time.
All successful test commands followed their matching builds. Project/ckVision
SDK checkout fingerprints are recorded; the installed SDK remains 1.4.0. No
Files GUI exists or was launched.

Focused sanitizers compiled seven units: native names, strict scalar codecs,
name mapping, Win32 diagnostics, old/new name tests and the ckVision test host.
Mac ASan + UBSan (-O1 -g, strict warnings, frame pointers) built
03:44:14.655–03:44:26.617 UTC in 11.948 s and passed all twenty cases in 0.382 s.
Windows MSVC ASan (/O1 /Zi /MT /W4 /WX) built
03:45:04.143–03:45:09.780 UTC in 5.638 s and passed the same twenty cases in
0.334 s. Both ran halt-on-error with no sanitizer diagnostics, including the
exhaustive scalar loop. The preceding seven-unit builds took 7.899 s Mac /
6.771 s Windows. After the native read-back refinement the same focused units were rebuilt against the
final source snapshot; direct commands and hashes were checked without a
compiler-cache wrapper. The focused unit scope is unchanged. These checks do
not claim another complete native-adapter sanitizer run. The full Files suite
ran separately on all three hosts. Sanitizer outputs were removed after hashing
executables and confirming zero active owners. Fixture audits found no remaining
files-name-* scratch directories on any host; current normal builds remain.
All Windows project outputs stayed on V:.

Boundary checks passed (62 domain sources, 416 ckVision-only active files), and
ckdocs checked 24 pages. Destination case/normalization/alias collision rules,
filesystem restrictions and path/component length enforcement remain WP-01
requirements. Remaining native inventories/provider coordination, operation
capabilities, full filesystem/time fakes and page-backed larger listings are
also unfinished. This milestone closes no package acceptance checkbox and adds
no file mutation command.


## WP-01 milestone — native child lookup (2026-10-23)

Added explicit, read-only lookup through held native directories. Found owns
the non-followed final object; Missing retains a narrowly qualified native
absence error. Both retain the requested name, comparison request and parent
identity observation. The result does not retain the parent handle or authorize
creation. Default adapters remain Unsupported; generic Gone is not absence.
Windows comparison requests use public OBJ_CASE_INSENSITIVE explicitly; POSIX
does not simulate it. NativeDefault follows actual native semantics, without
claiming exact spelling. No mutation command was introduced.

Eight new domain cases qualify ownership, move reset, invalid inputs, missing
evidence and native absence error classification. Seven new native cases per
host cover query-time absence versus later creation, held-parent path replacement,
child lifetime after parent destruction/rename/unlink, dangling final links or
junctions, distinct hard-link names, independent case/normalization queries and
explicit policy/encoding/stream boundaries.

All 25 Files CTest entries passed against the same 73-file source manifest:

| Host | Final build interval UTC | Build / test seconds | Cases |
| --- | --- | --- | --- |
| macOS arm64 / Apple Clang 21 | 04:09:17.377–04:09:20.316 | 2.939 / 1.216 | 162 |
| Lima Debian 13 arm64 / GCC 14 | 04:09:58.656–04:10:01.886 | 3.229 / 0.147 | 159 |
| Windows x64 / MSVC 19.44, static CRT | 04:10:02.479–04:10:07.347 | 4.868 / 1.627 | 166 |

The final increments compiled the revised native fixture and linked its test
executable. The initial Mac implementation build took 14.118 seconds for 27
steps including regeneration. Actual input hashes and compile/link logs were
checked; the short final increments reflect their narrower inputs. Initial
Linux fixture compilation caught misleading indentation under -Werror; the
statements were split without suppressing warnings, then all hosts passed the
matching final source snapshot. Test commands followed successful builds.

Actual alternate-name observations differ: APFS NativeDefault missed the ASCII
case variant and found the normalization variant; ext4 NativeDefault missed
both; ReFS WindowsIgnoreCase found the case variant and missed the normalization
variant. Independent native path queries agreed. These outcomes are local
observations, not a filesystem-wide capability claim or an algorithm for
comparing two uncreated candidate names.

Focused sanitizers compiled nine translation units and passed eight domain
cases: Mac ASan+UBSan built in 11.968 seconds (04:10:15.320–04:10:27.288 UTC),
tests 0.238 seconds; Windows ASan built in 10.266 seconds
(04:11:35.693–04:11:45.959 UTC), tests 0.129 seconds. Both used strict warnings
and halt-on-error with no sanitizer diagnostics. This scope does not instrument
the native adapters. Executable hashes and zero active owners were recorded
before removing disposable sanitizer outputs. No private files-lookup-* fixture
directories remain on any host; current normal builds are retained. All Windows
project outputs remained on V:. No Files GUI exists or was launched.

[Durable lookup evidence](evidence/ck-files-lookup-20261023.json) includes source
hashes, build/test records, log digests, comparison observations and cleanup
records. WP-01 remains in progress: full collision/alias semantics, path limits,
remaining inventories/provider coordination, operation capabilities, complete
filesystem/time fakes and larger listing backing are unfinished. No package
acceptance checkbox is closed by this milestone.


## WP-01 milestone — reported component-length assessment (2026-10-23)

Added unit-explicit comparison of a native component against its held-directory
location's reported byte/UTF-16 component bound. Missing/denied query states
retain their errors, zero bounds fail, and no conversion, normalization,
truncation or allocation by native bound occurs. Parent identity is context,
not a retained handle. Within-bound never establishes that creation is safe or
possible; full path, character-count, alias and operation rules remain separate.

Four domain cases cover exact boundaries, raw malformed Unicode, surrogate
pairs versus units, UINT64_MAX without native-sized allocation, unknown/denied
query propagation and invalid family/encoding/zero-bound rejection. A new native
case per host exclusively creates a private ASCII name at the reported maximum,
finds it through the held directory and verifies native rejection one unit over.
All actual locations reported 255. macOS/ext4 return ENAMETOOLONG (63/36);
ReFS returns STATUS_OBJECT_NAME_INVALID (0xC0000033), preserved unchanged.
An independent extended-path Win32 query also rejects the overlong component.
These are qualification observations for these locations, not universal rules.

All 26 Files CTest entries passed against a common 76-file source manifest:

| Host | Final build interval UTC | Build / test seconds | Cases |
| --- | --- | --- | --- |
| macOS arm64 / Apple Clang 21 | 04:24:33.606–04:24:37.586 | 3.978 / 0.702 | 167 |
| Lima Debian 13 arm64 / GCC 14 | 04:24:30.858–04:24:35.807 | 4.949 / 0.330 | 164 |
| Windows x64 / MSVC 19.44, static CRT | 04:24:34.793–04:24:40.311 | 5.518 / 1.679 | 171 |

The final two-step increments compiled the revised native fixture and linked
its executable; the preceding Mac six-step implementation build took 4.288 s.
Actual compile/link logs and final hashes were checked, with no time-only
freshness inference. The initial strict Mac compile caught an unnecessary loop
copy; changed it to a const reference without suppressing warnings. The first
Windows fixture failed because its full ordinary Win32 path exceeded MAX_PATH;
explicit extended-path fixture creation separates that convention from native
component length. The next run exposed the incorrect expected status assumption;
final tests check documented native rejection alternatives and an independent
Win32 query. No production error mapping was weakened.

After confirming zero owned test processes, removed the one empty Windows
scratch directory left by the aborted fixture. No files-name-limits-* directories
remain on any host. Current normal builds remain; no disposable sanitizer build
was added for this bounded pure comparison. No GUI was launched, and Windows
outputs remained on V:. UI-free boundary (66 sources), ckVision-only boundary
(423 files), documentation (24 pages) and whitespace checks passed.

[Durable evidence](evidence/ck-files-limits-20261023.json) records inputs,
builds/tests, native boundary results, diagnostic corrections and fixture cleanup.
WP-01 remains open, including full path/component enforcement in operation
planning, collision equivalence, remaining inventories/provider coordination,
operation capabilities, complete filesystem/time fakes and larger listing backing.
No package acceptance checkbox is closed by this milestone.


## WP-01 milestone — relative directory cursors (2026-10-23)

Implemented `open_directory_child` through a held parent on all three platforms,
providing real read-only traversal without absolute-path rebuilding or cwd
changes. Child cursors independently own native enumeration state and adapter
identity scope. POSIX uses directory-only non-following openat plus fstat and
fdopendir ownership transfer; Windows uses counted relative NtCreateFile with
reparse/no-recall flags and checks the opened handle's kind before enumeration.
Final links/reparse points and non-directories are rejected. No write API,
privilege enablement, fallback path or weaker retry was introduced.

One new domain case qualifies default Unsupported behavior. Six native cases
per host qualify held-parent path replacement, child rename/replacement after
parent destruction, final link/junction/file/FIFO rejection, independent cursors,
Unicode/wrong-encoding/stream boundaries, unusual native names and deep trees.
Windows uses an actual unpaired UTF-16 directory component; Linux an invalid
UTF-8 byte; macOS backslash/newline. Traversal succeeds through 48 components
of 100 ASCII bytes, exceeding 4096 bytes in relative full-path spelling. Fixture
creation and bottom-up cleanup are private, while production opens only one
component each step.

All 27 Files CTest entries passed against the final common 77-file manifest:

| Host | Final verification build UTC | Build / test seconds | Cases |
| --- | --- | --- | --- |
| macOS arm64 / Apple Clang 21 | 04:32:34.084–04:32:34.172 | 0.088 / 0.846 | 174 |
| Lima Debian 13 arm64 / GCC 14 | 04:32:31.975–04:32:32.004 | 0.029 / 0.147 | 171 |
| Windows x64 / MSVC 19.44, static CRT | 04:32:36.149–04:32:43.416 | 7.267 / 1.406 | 178 |

The final POSIX verification builds correctly reported no work: only the excluded
Windows adapter changed after their successful native-fixture rebuilds. The Mac
fixture increment compiled/linked two steps in 3.225 seconds; its initial broad
implementation build took 35.462 seconds for 28 steps with CMake configuration
8.3 seconds / generation 0.2 seconds. Other project compiler processes were
observed concurrently and left untouched. This differed from the previous
27-step 14.118-second lookup build; actual regenerated inputs, compiled targets
and logs were investigated, rather than treating elapsed time as freshness proof.
Final hashes match native tests and both remote source copies.

Initial Linux/Windows compilation caught malformed Windows fixture path literals;
changed to raw string literals without suppressing warnings. The first Windows
native run then caught an incompatible FILE_DIRECTORY_FILE/options combination
(STATUS_INVALID_PARAMETER). Corrected the single open protocol to retain reparse /
no-recall flags and validate FileBasicInfo before exposing enumeration, matching
the existing absolute-directory open pattern. The compatible public protocol
passes all native cases and existing regressions. No error was hidden or retried
with weaker protection.

Native sanitizer builds instrument all 19 core units, their platform adapter,
the six-case native fixture and test host (22 translation units). Mac ASan+UBSan
built 04:32:46.312–04:33:16.960 UTC in 30.648 seconds and passed in 0.452 seconds.
Windows ASan built 04:32:58.812–04:33:34.964 UTC in 36.152 seconds and passed in
0.216 seconds. Both use strict warnings and halt-on-error with no diagnostics.
This instruments the native implementation for these six cases, not every
Files test. Direct compile commands and source hashes were checked; their scope
is broader than the preceding nine-unit diagnostic builds. Sanitizer executables
were hashed and disposable outputs removed after zero-owner checks.

Verified the exact paths/content and absence of reparse points in the one
aborted Windows owner fixture before removal. No files-dirchild-* fixture trees
remain on any host. Current normal builds remain; all Windows project outputs
stayed on V:. No Files GUI exists or was launched. Boundaries pass (66 core
sources, 424 ckVision-only files), documentation checks 24 pages, whitespace clean.

[Durable evidence](evidence/ck-files-directory-child-20261023.json) retains source
hashes, normal/sanitized build/test records, deep-tree observations, diagnostics
and cleanup audits. WP-01 remains open: full collision/path planning rules,
remaining inventories/provider coordination, operation capabilities, complete
filesystem/time fakes and larger listing backing remain required. This adds
no recursive walker, confinement, provider no-hydration or mutation guarantee,
and closes no package acceptance checkbox.


## WP-01 milestone — bounded injected directory discovery (2026-10-23)

Added worker-side streaming `DirectoryListing` using the injected filesystem /
owned cursor boundary. It opens roots with final-link rejection and inspects
children through the held directory with metadata-only access. Batches retain
lossless names and explicit metadata observations, including entry failures.
Temporary child handles close after each inspection. Results do not retain
children, freeze namespace/content or confer selection/mutation authority.

Rows/native-name storage have configured limits and hard ceilings; only one
consumed pending component is retained across batches/cancellation. No name is
truncated or converted. Oversize components fail the stream explicitly. A byte
budget boundary retains the next name without skipping or duplicate output.
Cancellation between each native stage preserves resume position; completed
rows may accompany Cancelled. Enumeration errors and exceptions after consumption
are terminal, never a false End. Bounded skip-only reads yield More without
spinning. Callers still need bounded retained pages/indexes and UI publication.

Ten deterministic cases use an injected filesystem/cursor/opened-entry script.
They cover row/byte limits, original per-entry errors, cancellation after read /
open/inspect, handle live counts, sticky enumeration failure, UTF-16 storage
budgets, injected exceptions, bounded skips, invalid limits/paths, root errors
and null adapter results. Three native cases per host cover streamed batches,
final link/junction classification, held-root rename/replacement, 257-entry
cancellation/resume and unchanged process cwd.

All 29 Files CTest entries passed against the same 81-file source manifest:

| Host | Build interval UTC | Build / test seconds | Cases |
| --- | --- | --- | --- |
| macOS arm64 / Apple Clang 21 | 04:40:42.838–04:40:53.338 | 10.500 / 1.155 | 187 |
| Lima Debian 13 arm64 / GCC 14 | 04:41:21.085–04:41:29.313 | 8.229 / 0.214 | 184 |
| Windows x64 / MSVC 19.44, static CRT | 04:41:25.062–04:41:43.830 | 18.768 / 2.655 | 191 |

Each build compiled the new core/fixture sources and relinked the library and
both test executables. CMake configuration/generation took 7.2/0.2 s Mac,
4.0/0.1 s Linux and 11.2/0.6 s Windows. This scope includes regeneration,
unlike the preceding two-step directory-fixture increment; actual inputs,
compile/link logs and source hashes were verified, not elapsed time alone.
No build or test failure required a correction in this milestone.

Sanitizer builds instrument all 20 core units, their platform adapter, both
listing fixtures and test host (24 translation units). Mac ASan+UBSan built
04:42:20.062–04:43:02.703 UTC in 42.642 seconds and passed all 13 new cases in
0.401 seconds. Windows ASan built 04:42:21.426–04:42:59.379 UTC in 37.953 seconds
and passed the same 13 cases in 0.411 seconds. Both use strict warnings and
halt-on-error with no sanitizer diagnostics. This is a broader source/test scope
than the preceding 22-unit directory-cursor checks (30.648/36.152 seconds),
using direct compiler commands without a compiler-cache wrapper. It does not
claim full Files-suite sanitizer coverage.

Source/unit hashes, output paths and executable/log digests were preserved.
Disposable sanitizer outputs were removed after successful tests and zero-owner
checks. No files-listing-* fixture directories remain on any host; current normal
builds remain. All Windows project writes stayed on V:. No Files GUI exists or
was launched. UI-free boundary (68 sources), ckVision-only boundary (428 files),
documentation (24 pages) and whitespace checks pass.

[Durable evidence](evidence/ck-files-listing-20261023.json) records limits,
source fingerprints, builds/tests, sanitizer scope, output paths and cleanup.
WP-01 remains in progress: full filesystem/time fakes, clock/scheduling, retained
large-view indexes, collision/path planning rules, remaining inventories/provider
coordination and operation capabilities are still required. No package checkbox
is closed by this milestone and no production mutation was added.


## WP-01 milestone — injected monotonic discovery slices (2026-10-23)

Added an owned injectable monotonic-clock boundary and a checked cooperative
elapsed-time slice. Native steady readings have arbitrary epochs and are not
UTC or persistent timestamps. Chronological ordering plus unsigned subtraction
avoids signed elapsed/deadline overflow, including negative epochs and the full
signed-representation range. Observed backwards readings within or across
slices fail explicitly; injected clock exceptions poison discovery.

Discovery now has positive slices up to one second, default 20 ms, and reports
a detected time yield as More plus `time_slice_exhausted`. It checks between
bounded cursor-read/open/inspection stages; this does not interrupt blocking
calls or impose a hard OS timeout. A slow successful open retains one prepared
child so the following slice can inspect it, preventing repeated reopen
starvation. Cancellation closes the prepared handle and retains its name;
explicit resume reopens. Inspections, clock failures, exceptions and destruction
release handles correctly. Published rows never retain these child objects.
Worker scheduling and UI publication are still separate unfinished work.

Four clock cases cover exact boundaries, negative epochs/extreme spans, sticky
reversal and cross-slice ordering, invalid durations, exceptions and the native
nondecreasing clock without sleeps. Five additional listing cases cover slow
read/open/inspection progress, prepared-handle cancellation, reversal/exception
cleanup and configuration validation before root I/O. Existing ten fake listing
cases now use injected deterministic time. A fourth native listing case drives
time yields and real prepared-handle resumption with a manual clock on each OS.

All 30 Files CTest entries passed against the same 85-file source manifest:

| Host | Build interval UTC | Build / test seconds | Cases |
| --- | --- | --- | --- |
| macOS arm64 / Apple Clang 21 | 04:50:42.710–04:50:53.260 | 10.550 / 1.103 | 197 |
| Lima Debian 13 arm64 / GCC 14 | 04:51:49.418–04:51:58.609 | 9.192 / 0.211 | 194 |
| Windows x64 / MSVC 19.44, static CRT | 04:51:53.744–04:52:14.370 | 20.626 / 2.498 | 201 |

Builds compiled changed core/fixture sources and relinked the core library and
both executables, with configuration/generation 6.8/0.2 s Mac, 3.5/0.1 s Linux
and 11.0/0.7 s Windows. The preceding discovery builds took 10.500/8.229/18.768 s
with a smaller compile set; actual inputs, compile/link logs and final source
hashes were checked rather than inferring freshness from time. No build or test
failure required a correction in this milestone.

Sanitizers instrument all 21 core units, their platform adapter, three fixtures
and test host (26 translation units). Mac ASan+UBSan built
04:53:23.071–04:54:11.320 UTC in 48.249 seconds and passed all 23 clock/listing
cases in 0.993 seconds. Windows ASan built 04:53:23.537–04:54:01.632 UTC in
38.096 seconds and passed the same cases in 0.517 seconds. Both use strict
warnings and halt-on-error, with no sanitizer diagnostics, including extreme
elapsed arithmetic. This is broader than the preceding 24-unit checks
(42.642/37.953 seconds) and uses direct compiler commands without compiler-cache
wrappers. It is not complete Files-suite sanitizer coverage.

Diagnostic executable hashes and zero owners were recorded before removing
disposable sanitizer outputs. No files-listing-* fixture directories remain on
any host; current normal builds remain. All Windows project writes stayed on
V:. No GUI was created or launched. UI-free boundary (70 sources), ckVision-only
boundary (432 files), documentation (24 pages) and whitespace checks pass.

[Durable evidence](evidence/ck-files-clock-20261023.json) includes normal/sanitized
builds/tests, source hashes, output paths, limits, clock guarantees and cleanup.
This implements discovery time injection but leaves WP-01 open: full simulated
filesystem, scheduling/large-view indexes, collision/path planning, remaining
inventories/provider coordination and operation capabilities are unfinished.
No package acceptance checkbox is closed and no production mutation was added.


## WP-01 milestone — staged pane discovery publication (2026-10-23)

Added a worker-owned `PaneListingJob` that privately accumulates bounded discovery
batches and creates an immutable, generation-tagged completed payload only after
actual enumeration End. Scope/generation and directory identity observation
travel with the rows. The receiving pane rejects stale/foreign completions and
applies rows/context with allocation work completed before publication. Raw
manual publications explicitly clear directory context instead of retaining a
previous location's observation.

Entry metadata errors now survive conversion into pane rows and source candidates,
instead of becoming indistinguishable empty basic metadata. Explicit hidden
policy uses Linux dot names, Darwin dot/observed hidden flags and Windows observed
hidden flags; unknown/denied hidden states stay visible. Shared exact unsigned
native-name ordering supports sorting and duplicate detection without Unicode
normalization or destination-alias inference.

Cancelled partial batches remain private/resumable. Enumeration/resource errors,
exact duplicate names, mixed encodings and exceptions fail staging and release
its cursor; no partial completion is published. Existing validated PaneLimits
bound rows/name/display bytes. The duplicate index holds an additional bounded
name copy. Current staging and receiver projections are in-memory, and may
coexist with prior rows; no global-memory-quota or large-view index claim is made.
Disk-backed indexing remains mandatory unfinished work.

Nine new deterministic domain cases cover End-only completion, old/new/foreign
requests, row/candidate error preservation, failure selection retention, paused
partial batches, resource/duplicate/encoding refusal, exception cleanup, native
hidden policies and ownership/configuration validation. Three native cases per
host exercise streaming publication, root identity/hidden filtering, held-root
path replacement between batches and capacity refusal without partial rows.

All 32 Files CTest entries passed against a common 89-file source manifest:

| Host | Build interval UTC | Build / test seconds | Cases |
| --- | --- | --- | --- |
| macOS arm64 / Apple Clang 21 | 05:05:30.301–05:05:55.635 | 25.334 / 1.432 | 209 |
| Lima Debian 13 arm64 / GCC 14 | 05:06:08.937–05:06:33.365 | 24.428 / 0.139 | 206 |
| Windows x64 / MSVC 19.44, static CRT | 05:06:12.476–05:07:26.404 | 73.927 / 2.816 | 213 |

The shared native-name header change triggered a broad rebuild (52 Mac steps;
remote logs include 53 planned steps with regeneration). Configuration/generation
took 6.0/0.2 s Mac, 2.7/0.1 s Linux and 10.9/0.7 s Windows. The preceding clock
increments took 10.550/9.192/20.626 s with only eight compile/link steps. Actual
compiled inputs, link logs and matching final hashes were inspected to explain
the broader duration; elapsed time alone was not used as freshness proof. No
build or test failure required a correction in this milestone.

Focused sanitizers instrument all 22 core units, the native platform adapter,
existing pane tests, both new job fixtures and test host (27 translation units).
Mac ASan+UBSan built 05:08:56.590–05:09:33.373 UTC in 36.783 seconds and passed
all 30 focused cases in 0.625 seconds. Windows ASan built
05:08:56.908–05:09:38.275 UTC in 41.367 seconds and passed in 0.893 seconds.
Both use strict warnings and halt-on-error with no diagnostics. These are
18 existing pane plus nine new domain and three new native cases, not complete
Files-suite sanitizer coverage. Direct compiler source lists/hashes were checked;
fixtures differ from the preceding 26-unit clock builds (48.249/38.096 seconds).

Sanitizer executable hashes and zero owners were recorded before removing
disposable outputs. No files-panejob-* fixture directories remain on any host;
current normal builds remain. Windows writes stayed on V:. No GUI was created
or launched. UI-free boundary (72 sources), ckVision-only boundary (436 files),
documentation (24 pages) and whitespace checks pass.

[Durable evidence](evidence/ck-files-pane-job-20261023.json) records hashes,
normal/sanitized builds/tests, output paths, guarantees and cleanup. Directory
IDs remain observations and do not attest the original pathname. Discovery
rows do not seal held-child selection successors; selected-entry refresh
integration remains required. WP-01 also retains full simulated filesystem,
scheduling/large indexes, collision/path planning, remaining inventories/provider
coordination and operation capability work. No package checkbox is closed and
no production mutation was added.

## 2026-10-23 — WP-01 listing cancellation and resource release

Added pane-owner cancellation of the current listing generation and worker-owned
scan abandonment after `advance` returns. Cancelling preserves published rows,
directory context, focus, marks and existing selection invalidation diagnostics;
late results cannot apply. Foreign/stale/zero cancellation cannot revoke a newer
request. Abandonment releases private staging, the native cursor and any prepared
child, and permanently prevents further I/O. Failed jobs retain their diagnostic.
An already-issued immutable result still requires receiver generation revocation.
These APIs do not interrupt blocked native calls or permit concurrent job access.

Seven added cases cover cancellation state/ownership, invalidation retention,
prepared-child release, queued late completion, failure diagnostics and native
abandon/restart. All 32 Files suites pass using the same 89-file source manifest:

| Host | Build interval UTC | Build / test seconds | Cases |
| --- | --- | --- | --- |
| macOS arm64 | 20:23:22.279–20:23:25.264 | 2.984 / 0.765 | 216 |
| Lima Debian 13 arm64 | 20:25:47.010–20:25:52.569 | 5.559 / 0.121 | 213 |
| Windows x64, static CRT | 20:26:29.556–20:26:41.445 | 11.888 / 2.536 | 220 |

Each build performed ten compile/link steps: seven affected units, the core
library and two executables. The narrower input dependency scope explains the
shorter durations than the preceding broad 52/53-step rebuild. Logs and source
hashes were inspected. The initial Windows attempt stopped before compilation:
the inherited SSH PATH (7178 characters) exceeded cmd's developer-shell limit.
Initializing the validation session from the system PATH, checking shell status
and resetting temporary variables to V: resolved the environment failure.

Focused sanitizers passed 37 cases (21 pane, 12 job-domain, four native-job),
instrumenting the same 27 translation units as the previous milestone. Mac
ASan+UBSan built 20:25:58.728–20:26:32.952 UTC in 34.224 s and tested in 0.377 s;
Windows ASan built 20:26:54.281–20:27:35.009 in 40.727 s and tested in 0.795 s.
Prior comparable builds were 36.783/41.367 s. Strict warnings and halt-on-error
produced no diagnostics. This is focused coverage, not full-suite sanitization.

Diagnostic executable hashes and zero owners were recorded before disposable
sanitizer outputs were removed. No files-panejob-* fixtures remain on any host;
current normal builds remain. Windows project writes stayed on V:. Boundaries
(72 domain sources / 436 active files), ckdocs (24 pages) and whitespace checks
pass. No GUI was launched or production mutation added.

[Cancellation evidence](evidence/ck-files-cancellation-20261023.json) records
source hashes, build/test intervals, output paths and cleanup. Scheduling and
owner-thread wiring, large indexes, selected-child refresh integration, full
filesystem simulation, collision/path planning and remaining inventories and
operation capabilities remain WP-01 work. No package acceptance box is closed.

## 2026-10-23 — WP-01 reusable discovery filesystem simulation

Added a shared, test-only `SimulatedFileSystem` implementing `BasicFileSystem`
without host filesystem I/O. It separates path/child bindings from held objects,
models independent directory cursors, exact native names, live metadata and
adapter-scoped identities, and reports live cursor/entry counts. Queued actions
inject typed failures, delays through the manual clock, side effects and
exceptions. Call records expose unintended retries. Foreign nodes, relative
bindings and directory cycles are refused. Count limits and single-owner use
are explicit; this is not a process-memory quota or native behavior emulator.

Twelve integration cases exercise production listing, pane-job, held-child and
pane code: invalid Unicode in both native encodings, path replacement/unbinding,
removed membership, unknown/denied diagnostics, failed scan disposal,
cancellation/resumption, slow-open resource cleanup, exception poisoning,
independent child cursors/link refusal, invalid binding refusal, retained
selection and replacement detection while old handles remain open.

Final reviewed source passes all 33 Files suites against a shared 91-file manifest:

| Host | Build interval UTC | Build / test seconds | Cases |
| --- | --- | --- | --- |
| macOS arm64 | 20:35:29.300–20:35:31.679 | 2.378 / 0.944 | 228 |
| Lima Debian 13 arm64 | 20:35:57.123–20:36:00.116 | 2.993 / 0.097 | 225 |
| Windows x64, static CRT | 20:36:02.047–20:36:10.406 | 8.359 / 3.024 | 232 |

Initial builds regenerated CMake projects (3.3/0.1 s Mac, 2.2/0.1 s Linux,
14.0/1.2 s Windows configure/generate), with total times 5.495/5.363/24.353 s.
Final review hardened assertions so missing results produce diagnostics rather
than invalid dereferences; validation was repeated after that change. Final
logs show one fixture compilation and core-test link, with no production source
change. The existing native executable correctly required no rebuild. Durations
were checked against actual dependency work and previous records.

Fresh sanitizer builds instrument all 22 core units, the new fixture and test
host (24 translation units), without the native platform adapter. All 12 focused
cases pass with strict warnings and halt-on-error. Mac ASan+UBSan built
20:35:57.402–20:36:24.762 UTC in 27.360 s, testing in 0.204 s; Windows ASan built
20:36:26.232–20:37:12.682 in 46.450 s, testing in 0.140 s. Pre-review same-scope
builds took 30.066/46.454 s. Source lists/hashes and emitted Windows unit names
were checked. Both rounds' diagnostic hashes and zero owners were recorded
before removing their output trees. Current normal builds remain; Windows
writes stayed on V:. No GUI was launched.

[Simulation evidence](evidence/ck-files-simulation-20261023.json) records final
source hashes, tests, direct sanitizer commands and cleanup. Boundaries (72
core sources / 438 active files), ckdocs (24 pages) and whitespace checks pass.
The simulator uses deterministic open-time enumeration names and live child
lookup, not a native snapshot guarantee. Other query families remain explicitly
Unsupported. Inventory/mutation simulation, remaining native queries,
collision/path planning, large indexes, refresh integration and scheduling remain
unfinished. No WP-01 acceptance box is closed and no production mutation added.

## 2026-10-23 — WP-01 Linux held-directory inode flags

Added raw `FS_IOC_GETFLAGS` inventory to the held Linux directory location query,
separate from statx attributes and mount flags. The adapter uses the native int
pointer ABI, retains all returned bits and errors, classifies ENOTTY as
Unsupported and performs no retry, synchronization, pathname reopen or flag
mutation. macOS/Windows explicitly report this query NotApplicable/Unsupported.
A typed helper decodes reported casefold, no-COW and project-inherit bits;
invalid selectors fail and unavailable observations retain their diagnostics.
Neither set nor clear bits establish preservation capabilities, extent sharing,
quota IDs or Unicode collision equivalence. Generic Linux directory case
sensitivity remains unqualified.

Two new portable cases cover namespace/bit decoding and unavailable states.
A native case on each host compares independent held-descriptor results or
checks the non-Linux refusal. A Linux-only case activates casefold on its own
exclusive empty test directory where the volume supports it; the ordinary
Linux test volume explicitly reported EOPNOTSUPP. All 33 Files suites pass
against a common final 91-file source manifest:

| Host | Final build interval UTC | Build / test seconds | Cases |
| --- | --- | --- | --- |
| macOS arm64 | 20:41:41.751–20:41:42.974 | 1.222 / 0.849 | 231 |
| Lima Debian 13 arm64 | 20:41:41.504–20:41:55.552 | 14.048 / 0.097 | 229 |
| Windows x64, static CRT | 20:41:47.116–20:43:16.326 | 89.210 / 4.139 | 235 |

The shared-header change caused 38 build steps on each host. Mac's initial
broad build took 9.816 s; final diagnostic edits required two native-fixture
compile/link increments (1.116/1.222 s). Windows exceeded the earlier broad
53-step 73.927 s build. Its final Ninja records confirm 88.801 s of actual work:
platform compilation 23.202 s, simulation fixture 15.744 s and pane fixture
14.000 s among the longest steps. Logs, actual compiled inputs, links and final
hashes were checked; elapsed time alone did not establish freshness.

Focused Mac ASan+UBSan and Windows ASan instrument all 22 core units, platform,
two location fixtures and test host (26 units). Each passes 11 cases. Mac built
20:42:04.064–20:42:30.229 UTC in 26.165 s and tested in 0.206 s; Windows built
in 38.710 s and tested in 0.121 s. Preceding native-adapter sanitizer builds
with different fixtures/27 units took 34.224/40.727 s. Strict warnings and
halt-on-error produced no diagnostics; exact units/flags/hashes were checked.

A separate private 64 MiB ext4 image enabled casefold on the Linux VM. A strict
fixture build required successful activation and observed native ASCII lookup
of MixedCase through mixedcase, with flags 1074266112 (casefold plus extents).
Its 25-unit normal build took 21.315 s and passed five native location cases.
A second private image ran ASan+UBSan across all 26 units and both location
fixtures: 53.495 s build, 0.031 s test, all 12 cases passed. This records the
first comparable Linux sanitizer baseline. These checks qualify the set flag
and ASCII lookup on this image, not every Unicode rule or Linux filesystem.

Both images used owned remote task workspaces and nodev/nosuid/noexec mounts.
Only exclusively created test directories received SETFLAGS; production code
is read-only. Each image was unmounted, loop devices audited empty and image /
diagnostic workspace removed. Final audits found no location fixtures, owned
test processes or casefold workspaces. Mac/Windows sanitizer hashes and zero
owners were recorded before disposal. Current normal builds remain; Windows
writes stayed on V:. No GUI was launched.

[Inode-flag evidence](evidence/ck-files-inode-flags-20261023.json) records native
and sanitizer results, exact compiler commands, mount/cleanup evidence and build
timing investigation. Boundaries (72 core sources / 438 active files), ckdocs
(24 pages) and whitespace checks pass. Other filesystem metadata APIs, complete
collision/capability planning, large indexes, refresh/scheduling integration and
operation work remain. No WP-01 acceptance box is closed.

## 2026-10-23 — WP-01 integrated held-directory entry probes

Added worker-owned `DirectoryEntryProbe` to combine native lookup, location,
reported component bounds, Win32 spelling diagnostics and existing-entry
metadata. Records retain the parent and found final child independently;
refresh queries the same held parent after path replacement. Location errors,
lookup errors and found-child inspection errors remain separate. Missing has
native evidence and NotApplicable existing metadata, without name reservation
or creation authority. Unknown/mismatched lookup contexts fail explicitly.
Context-message consistency does not qualify native identity.

Component storage is bounded before validation/copy (64 KiB default, 1 MiB hard,
two bytes per UTF-16 unit). Native names are not normalized, converted, joined
into paths or replaced by display text. Stop tokens are checked before and
between native calls; cancellation returns no partial record and releases
private handles. Exceptions unwind without retry. Native calls/owner retirement
still belong on the worker and can block. This record is not a UI projection.

Extended the shared simulator with exact-unit NativeDefault lookup and explicit
Adapter missing evidence, plus configurable location observations. Unconfigured
location and WindowsIgnoreCase remain Unsupported. Nine deterministic cases
cover ownership, refresh, independent errors, budgets/configuration, malformed
reply contexts, unknown IDs, exceptions, conventions and cancellation. Four
native cases per host cover existing/missing entries, held-parent replacement,
dangling links and encoding refusal.

The final reviewed 95-file manifest passes all 35 Files suites:

| Host | Build interval UTC | Build / test seconds | Cases |
| --- | --- | --- | --- |
| macOS arm64 | 20:56:20.740–20:56:23.396 | 2.655 / 0.940 | 244 |
| Lima Debian 13 arm64 | 20:57:29.717–20:57:35.424 | 5.707 / 0.336 | 242 |
| Windows x64, static CRT | 20:57:39.850–20:57:52.831 | 12.981 / 3.429 | 248 |

Initial seven-step builds included CMake regeneration; totals were
6.222/7.075/26.657 s. Mac configure/generate was 3.5/0.1 s. Review changed the
input parameter to const reference and moved budget checking before validation
and the owned name copy. All host/sanitizer checks were repeated afterward.
Final six-step builds compile the probe and its two fixtures, then link the core
library and both executables. Logs and matching code hashes establish freshness;
durations match the narrower dependency work rather than the preceding broad
38-step inode-flag builds.

Focused Mac ASan+UBSan and Windows ASan instrument all 23 core units, platform,
three simulation/probe fixtures and test host (28 units). Each passes 25 cases
with strict warnings and halt-on-error. Mac built 20:57:33.506–20:58:18.341 UTC
in 44.833 s and tested in 0.297 s. Windows built
20:58:19.804–20:59:23.985 in 64.181 s and tested in 0.201 s. Earlier same-scope
runs passed before the input-copy review; exact units/flags/hashes and emitted
Windows source names were checked. Both rounds' diagnostic hashes/zero owners
were recorded before their output trees were removed.

Linux ASan+UBSan uses a private 64 MiB casefold-enabled ext4 image, adding the
existing native-location fixture (29 units). Its required-feature probe case
activates casefold only on an exclusive empty fixture directory and verifies
that native lookup of mixedcase identifies MixedCase. All 31 focused cases pass;
no unavailable-feature fallback qualifies this run. The final build ran
20:58:39.544–21:00:07.491 UTC in 87.946 s, testing in 0.034 s; the pre-review
same-scope build took 87.782 s and tested in 0.032 s. This qualifies ASCII lookup
on that volume, not universal Unicode collision semantics.

Both Linux images were unmounted, their loop devices audited empty, and images /
diagnostic workspaces removed. Final audits found no entry-probe fixtures,
casefold workspaces or owned test processes. Current normal builds remain;
Windows outputs/temp stayed on V:. No GUI was launched or mutation added.
Boundaries (74 domain sources / 442 active files), ckdocs (24 pages) and whitespace
checks pass.

[Entry-probe evidence](evidence/ck-files-entry-probe-20261023.json) preserves
source/SDK fingerprints, normal/sanitized results, compiler commands and cleanup.
Operation capabilities, planned-name collision grouping, alias/descendant checks,
frozen inventories/loss consent, large indexes, selected-entry refresh integration
and scheduling remain required. WP-01 acceptance remains open.

## 2026-10-24 — WP-01 sealed bound-entry refresh preparation

Added `PaneRefreshJob`, a worker phase between native-End discovery and pane
publication. Discovery can explicitly retain its scanned directory; default
release-at-End behavior stays intact. Preparation captures bounded prior held
bindings, incrementally copies/matches rows and refreshes only through the same
qualified directory context while both parent handles remain alive. Cached names
or independently opened observations cannot bypass the publisher's sealed-family
check. Successful continuity preserves row IDs, marks and focus with current
metadata; replacements, unavailable continuity and parent changes invalidate
selection rather than retarget it.

Per-name binding issues distinguish failed checks and unlisted observations
without claiming definitive absence. Hidden policy is recomputed; hidden marks
still require review. Issues publish atomically with rows/directory context,
survive failed/cancelled requests, and are acknowledged by valid explicit
selection commands. Invalidation counts remain until acknowledgement even when
a later successful refresh replaces the latest issue report.

Preparation has a 256-step advance bound, injected cooperative clock slices
(default 20 ms) and checked pane/component limits. Copies happen incrementally;
no partial result publishes. Held-child cancellation now checks between native
calls and releases private children. Paused jobs resume, slow completed binding
checks do not repeat just because their time slice expired, and abandoned/failed
jobs release staging. Native calls/individual allocation or encoding steps can
outlast a slice. Native owners must retire on workers; a false scan-pin indicator
does not mean rows contain no held handles.

Seventeen new deterministic cases cover integrated continuity, replacement,
root mismatch/unknown IDs, absent names, denials, independent families, clock and
step yields, cancellation at three native query boundaries, abandonment, stale
generations, resource/configuration refusal, hard-link/hidden mark semantics and
bad_alloc poisoning. Three native cases per host verify retained focus/marks,
current metadata, replacement while the old object is held, and cross-directory
refusal.

Final validation used the same 99-file source manifest. Build/test intervals
below are UTC on 2026-10-23 (the milestone record uses the current local date):

| Host | Final build interval UTC | Build / test seconds | Cases / suites |
| --- | --- | --- | --- |
| macOS arm64 | 21:41:03.575–21:41:05.123 | 1.549 / 0.843 | 264 / 37 |
| Lima Debian 13 arm64 | 21:43:03.924–21:43:07.589 | 3.665 / 0.119 | 262 / 37 |
| Windows x64, static CRT | 21:43:09.542–21:43:18.901 | 9.358 / 2.803 | 268 / 37 |

The initial Mac change rebuilt 18 steps in 20.020 s, including CMake regeneration;
review tests and scan-pin naming produced later two-/11-step increments. Final
Mac repaired implementation/library/executable work used four remaining steps;
unchanged test objects from the prior attempt were valid for the final source.
Linux/Windows final updates used six steps. Actual inputs, links, source hashes
and the newly added bad_alloc case in every LastTest log establish freshness.

Review found that allocating diagnostic text in an exception catch could prevent
poisoning during allocation failure. The catch now creates an empty-text error
and emplaces a payload with statically nonthrowing movement/destruction, then
releases staging. Two overstrong noexcept assumptions about optional wrapper
operations failed compilation; the final implementation follows optional's
specified emplace exception guarantee instead of weakening warnings. One CTest
command accidentally ran after the first failed build and used the old binary.
That stale run is explicitly excluded in the evidence. Subsequent validation
required successful builds before running tests.

Focused sanitizers include all 24 core units, platform, eight affected pane /
continuity/listing/refresh fixtures and test host (34 units), with strict warnings
and halt-on-error. Mac ASan+UBSan passed 87 cases after a 50.750 s build
(21:43:05.683–21:43:56.433 UTC), testing in 0.499 s. Windows ASan passed 87 after
88.656 s (21:44:49.234–21:46:17.890), testing in 1.266 s. The earlier same-scope
Mac run took 51.641 s and passed 86 cases before the allocation-failure addition;
previous 28-unit different-fixture builds took 44.833/64.181 s. Exact unit lists,
flags, hashes and emitted Windows source names were checked.

Linux ASan+UBSan adds the native-location fixture on a required casefold-enabled
private 64 MiB ext4 image (35 units). All 92 cases passed: 121.538 s build
(21:44:45.829–21:46:47.367 UTC), 0.373 s test. This broader fixture set has its
own baseline; the preceding 29-unit different-fixture build was 87.946 s. The
image was unmounted, loop audit empty, and image/diagnostic workspace removed.
Mac/Windows diagnostics were hashed and zero-owner checks recorded before
removal. Final fixture audits are empty; current normal builds remain. Windows
writes/temp stayed on V:. No GUI or production mutation was added.

[Refresh evidence](evidence/ck-files-pane-refresh-20261024.json) preserves final
hashes, successful/failed validation records, the explicitly discarded stale run,
compiler commands and cleanup. Boundaries (76 core sources / 446 active files),
ckdocs (24 pages) and whitespace checks pass. Initial selection pinning, UI
scheduling/ownership, large-view indexes, remaining inventories and full collision /
capability planning still require implementation. WP-01 acceptance stays open.

## 2026-10-24 — WP-01 explicit inventory simulation

Extended the shared filesystem simulator with independent replies for storage,
links/reparse data, channel and attribute catalogs, attribute values and all six
native security query parts. Exact feature namespaces, opaque bytes, coverage,
owners and partial/unavailable observations survive transport. Unconfigured
queries remain Unsupported with the requested access; they never imply absence.
This is test infrastructure, not native capability evidence or mutation support.

All query families support queued faults, latency, side effects and exceptions.
Calls record requested access, exact feature/security keys and resource limits.
Reply lookup happens after injected effects. Attribute/security reads enforce
explicit requested access. Invalid inputs do not consume actions; catalog/value
limits return failure without a prefix. Cumulative per-node payload/record limits
are checked before replacing configured replies, including decoded link names
and overflow-safe UTF-16 accounting.

Fourteen additional deterministic cases cover these contracts, live ownership
after path replacement, all six failure states, unknown/changed fields, access,
exact byte/record boundaries, namespace separation, configuration refusal and
exceptions. The same 100-file source export was verified on all hosts; the
unchanged test host source was independently hash-checked as a supplemental input.
All 37 Files suites passed: **278 macOS / 276 Linux / 282 Windows cases**.
Final incremental build/test seconds were 4.376/1.592, 6.725/0.131 and
17.590/4.861 respectively (UTC validation on 2026-10-23). Mac's final two-step
test update followed an already verified four-step shared-header rebuild;
Linux/Windows compiled three dependent fixtures and linked. No native source
changed, so current verified native executables remained valid.

Focused sanitizers compiled all 24 core units, three affected fixtures and test
host without cached objects (28 units). **52 cases passed on each host**:
Mac ASan+UBSan 46.560 s build / 0.083 s test; Linux ASan+UBSan
106.328/0.101; Windows ASan 80.216/0.405. Previous different-fixture 28-unit
Mac/Windows builds were 44.833/64.181 s; the previous 29-unit Linux build was
87.946 s. Actual compilation inputs, new test markers and hashes establish
freshness; these different fixture sets are not identical duration baselines.
An initial Mac sanitizer startup requested unsupported LeakSanitizer and ran no
tests; it is explicitly excluded. The same binary passed using supported
ASan+UBSan options. No successful sanitizer run reported diagnostics.

Diagnostic outputs were hashed, checked for active owners and removed. Full
scratch-root audits ended empty. Windows had one older fixture from 2026-10-22:
its exact source test, non-reparse status, sole nine-byte `namespace` file and
zero task executable owners were verified before recording its hash and removing
only that file and empty parent. Current normal builds remain. Windows writes
stayed on V:, Mac temporary work stayed on S8T, and no GUI was launched.

[Inventory simulation evidence](evidence/ck-files-inventory-simulation-20261024.json)
records source/output/log hashes, compiler commands, timestamps, excluded startup
and cleanup. UI boundaries (76 domain sources), ckVision-only (447 active files),
ckdocs (24 pages) and whitespace checks pass. Initial selection pinning, remaining
native inventories, operation capabilities, collision rules and worker/UI/large-view
integration remain; WP-01 acceptance stays open and WP-02 has not started.

## 2026-10-24 — WP-01 held-object inventory collection

Added the worker-only `ObjectInventoryJob` and sealed immutable result. Through
one retained native object it collects before/after basic metadata, storage,
link/reparse data, channel/attribute catalogs, every listed attribute value and
all six security query parts. It preserves independent native failures and
coverage; completion does not imply a snapshot, complete feature inventory or
operation capability. Attribute values can carry additional file data and retain
that role and exact bytes. No main-data/named-stream content API, pathname
reopening, link traversal, elevation or production mutation was added.

The job incrementally checks catalog envelopes/duplicates, requested attribute
and security keys, query record/value bounds and separate cumulative retained
payload limits. Native wire-buffer budgets pass through unchanged; long feature
names have independent storage accounting. A changed attribute identifier stops
collection with Changed/MetadataChanged. Before/after identity continuity and
individual payload-owner comparisons remain separate from content stability.

Advances have a 256-step bound, injected cooperative clock slices and resumable
cancellation. Completed native queries commit privately before yielding and do
not repeat. No partial result publishes. Aggregate/malformed/clock failures and
exceptions poison staging and close native ownership; allocation-failure error
construction allocates no text. Abandonment releases private state, while issued
results retain the object until their last worker owner retires. Native methods,
allocation/comparison and destruction can still exceed a slice.

Twenty deterministic cases cover all query families/failure states, per-value
denial, cancellation at every query-family boundary, slow calls, 300 attributes,
malformed/unbounded replies, resource/configuration refusal, exact namespaces,
additional-data values, owner disagreement, changed/unknown IDs, changing sizes,
clock reversal and bad_alloc. Three native cases on each host qualify result
pinning after rename/replacement, cancellation/resume/abandonment, and collection
through a removed name. Windows delete-pending names are treated according to
their native lifetime rather than assuming immediate POSIX name reuse.

Final validation used the same **105-file manifest**, including the test host.
All **39 Files suites** passed: **301 macOS / 299 Linux / 305 Windows cases**.
Final six-step build/test seconds were **3.729/0.766**, **5.663/0.120** and
**9.782/2.334** respectively. UTC intervals are recorded in the evidence; the
validation date was 2026-10-23 and the local milestone date is 2026-10-24.
Recent comparable Mac six-step builds were 3.471/3.366 s. Actual compiled units,
links, current hashes and all 23 new test markers prove freshness.

Focused sanitizers compiled all 25 core units, platform adapter, simulator,
domain/native inventory fixtures and test host (30 units), without cached
objects. **49 cases passed per host**: Mac ASan+UBSan **49.845/0.360 s**;
Linux ASan+UBSan **120.513/0.103**; Windows ASan **55.657/0.193**. Preliminary
same-scope builds before the final additional-data/race checks were
49.694/117.740/53.824 s and passed 48 cases. Final source inputs were rebuilt
and rerun on all hosts after review; warnings remained errors and sanitizer
runs reported no diagnostics.

Both preliminary and final diagnostic outputs were hashed, checked for owners
and removed. Full scratch-root audits were empty; current normal builds remain.
Windows writes stayed on V: and Mac temporary work on S8T. No GUI was launched.
[Object inventory evidence](evidence/ck-files-object-inventory-20261024.json)
records source/output/log hashes, commands, times, case markers and cleanup.
UI boundaries (78 domain sources), ckVision-only (451 active files), ckdocs
(24 pages) and whitespace checks pass.

WP-01's core/platform targets, injected filesystem/time, identities, fake
adapters and explicit ownership item is now complete. Its inventory/capability
and pane/link/collision items remain open: initial selection acquisition,
operation-specific capability and collision/equivalence rules, remaining native
inventories and worker/UI/large-view integration still need implementation.
WP-01 acceptance remains open; WP-02 has not started.

## 2026-10-24 — WP-01 destination batch inspection

Added worker-owned `DestinationBatchJob` and its sealed immutable result. It
maps a bounded batch of source components losslessly, groups exact mapped names
before lookup, and inspects each unique destination through the held parent.
Every source row remains identifiable; unrepresentable names retain their raw
units and mapping failure and make no native lookup. Exact duplicates share one
probe and group count. Length reports, Win32 conventions, native lookup and
existing metadata retain independent failure/coverage states.

Positive reported-object groups use held targets and full scoped native IDs,
including scheme, volume, object and generation. Index buckets never replace
`compare_identity`; equal bytes from foreign scopes cannot match. Object groups
stay separate from exact name groups: hard links do not prove name equivalence.
An absent match does not prove difference or collision freedom. Native case,
normalization and other alias rules for distinct missing names remain unproven;
no filesystem-label or case-advertisement shortcut was introduced.

Advances have a 256-step bound and injected cooperative clock slices, including
bucket comparisons. Cancellation retries only an incomplete current probe;
completed probes survive yields. Row/component/native-byte limits are checked
before further copying, with explicit reservations for probe/context names and
checked UTF-16 byte accounting. Exceptions/clock/resource failures poison and
release staging; no partial result escapes. Issued results retain native owners
independently of job abandonment. This is a read-only inspection boundary,
without source binding, operation permission, namespace freezing or a UI job.

Twenty-one deterministic cases cover duplicate mapping, raw encodings,
unrepresentable names, all failure states, hard-link reports, identity scopes,
generations/schemes/zero IDs, cancellation, 300 names, adversarial identity
buckets, budget boundaries, exceptions, clocks and retirement. Five native cases
per host qualify hard links versus duplicates, missing case variants, replaced
parent paths, dangling final links, and old target ownership after replacement.

The same **109-file manifest** was verified on all hosts. All **41 Files suites**
passed: **327 macOS / 325 Linux / 331 Windows cases**. Final build/test seconds
were **3.136/0.706**, **6.711/0.115**, **19.482/3.169**. Mac's final two-step
test update followed the current core's initial four-step build (5.100 s) and
both new fixtures' four-step update (6.531 s), each including CMake regeneration.
Remote builds compiled all three new units, archived and linked both binaries,
plus regeneration. Windows configuration/generation took 8.7/0.7 s; inspected
Ninja timings show core, native fixture and domain fixture compiles of
3.326/4.950/9.018 s, explaining its longer build. Current source hashes, compiled
units, output timestamps and all 26 new case markers establish freshness.

Focused sanitizers compiled all 26 core units, platform, simulator, domain/native
batch fixtures and test host (31 units), without cached objects. **52 cases
passed on each host**: Mac ASan+UBSan **48.940/0.289 s**, Linux ASan+UBSan
**116.614/0.090**, Windows ASan **67.346/0.233**. This fixture set establishes a
new baseline; prior different-fixture 30-unit builds took
49.845/120.513/55.657 s. Strict warnings remained errors and successful sanitizer
runs reported no diagnostics. Validation intervals were UTC on 2026-10-23;
the milestone uses the current local date, 2026-10-24.

Diagnostics were hashed, checked for live owners and removed; full fixture-root
audits were empty. Current normal builds remain, Windows writes stayed on V:,
Mac temporary work stayed on S8T, and no GUI was launched.
[Destination batch evidence](evidence/ck-files-destination-batch-20261024.json)
records hashes, commands, times, case markers, Ninja timings and cleanup.
UI boundaries (80 domain sources), ckVision-only (455 active files), ckdocs
(24 pages) and whitespace checks pass. WP-01's two remaining checklist items
and acceptance stay open: source self/descendant checks, native equivalence,
operation capabilities, remaining inventories and pane/worker/large-view
integration still need implementation. WP-02 has not started.

## 2026-10-24 — WP-01 reported file data ranges

Added bounded, cancellable inspection of native reported potential-data ranges
for the regular file's main data. Results retain the native query family,
scoped held-object identity and logical size. POSIX SEEK_DATA/SEEK_HOLE and
Windows FSCTL_QUERY_ALLOCATED_RANGES remain distinct. Windows rounding is
clipped; malformed, nonprogressing or overflowing replies fail. Empty files
still require a native query. Unsupported queries never become fabricated
whole-file results; failures, cancellation and exhausted limits discard prefixes.
These reports do not establish physical extents, exact allocation, actual
nonzero bytes, stable content, clone relationships or sparse-copy preservation.

Layout inspection requires explicit `MetadataAndLayout` access. Metadata-only
queries remain denied. Darwin seeks the original event-only descriptor. Linux
obtains permission-checked read access through the held descriptor's kernel link
in a verified procfs directory; Windows opens by native ID with the original
handle live. Neither reopens the original basename, elevates privileges or
reads file content. Identity, regular-file kind, size and available change times
are checked when binding access and around queries. Identified provider/recall
states retain review-required results. Linux has no generic provider-locality
proof, and the opt-in cannot promise universal no-hydration. Native calls can
block; cancellation remains cooperative.

Object inventory now retains layout observations with incremental record/byte
accounting. Cancellation retries only an incomplete query; completed families
survive yields. Simulator access, limits and faults cover this query explicitly.
Thirteen deterministic cases cover range validation, all failure states,
budgets, cancellation, exceptions, provenance and inventory envelopes. Six native
cases per host verify sparse fixtures, access opt-in, ownership after namespace
replacement, pinned inventories, special-object refusal and empty-file evidence.
Linux adds actual unprivileged read denial; Windows adds offline-provider guards
for both existing handles and access acquisition.

The same **113-file manifest** was verified on all hosts. All **43 Files suites**
passed: **346 macOS / 345 Linux / 351 Windows cases**. Final normal build/test
seconds were **3.376/0.821**, **21.792/0.144**, **8.765/2.720**. Mac's final two-step
fixture rebuild follows the current core's four-step update (1.078 s) and broad
54-step rebuild (18.102 s), comparable to the initial 52-step build (19.573 s).
Linux completed 43 remaining steps after GCC rejected a copied range-loop pair;
the fix uses a const reference and retains strict warnings. The failed driver
stopped before tests. Windows rebuilt five steps against the verified manifest.
Compiled inputs, hashes and output records establish freshness.

Focused sanitizers rebuilt all 27 core units, the platform adapter, six fixtures
and the test host (35 units), without cached objects. **72 macOS / 73 Linux /
73 Windows cases** passed. ASan+UBSan build/test seconds were **65.882/0.453** on
Mac and **151.535/0.138** on Linux; Windows ASan took **69.694/0.309**. Mac's
comparable preliminary build took 65.263 s; Linux establishes the first successful
baseline for this fixture set. All warnings remained errors. The initial APFS
fixture exposed full allocation by ftruncate; it was corrected using native hole
punching of known-zero test ranges, preserving the sparse assertion. That failed
run and its printed range marker are excluded from successful qualification.

Native qualification covers case-sensitive APFS, ext4 and ReFS. For an 8 MiB
fixture, reported/allocated bytes were **3162112/3162112**, **8192/8192** and
**8192/12288**, respectively, illustrating why ranges and allocation stay separate.
Diagnostics and the small Darwin descriptor research probe were hashed, checked
for live owners and removed; fixture roots were empty. Current normal builds
remain. Windows development stayed on V:, Mac temporary work on S8T. No GUI was
launched. Validation intervals were UTC on 2026-10-23; this milestone uses the
current local date, 2026-10-24.

[Data layout evidence](evidence/ck-files-data-layout-20261024.json) records source
hashes, query qualification, test markers, commands, timings, output hashes,
rejected attempts, research and cleanup. UI boundaries (82 domain sources),
ckVision-only (459 active files), ckdocs (24 pages) and whitespace checks pass.
WP-01's two remaining checklist items and acceptance stay open: initial selection,
self/descendant checks, native equivalence, operation capabilities, remaining
inventories and pane/worker/large-view integration still need implementation.
WP-02 has not started.

## 2026-10-24 — WP-01 initial held-entry acquisition

Added `PaneAcquireJob`, a worker stage between sealed native-End discovery and
pane publication. It consumes the retained scanned directory and ownership of
bounded requested native names. Requests match raw discovery components exactly;
no path reopening, display decoding, normalization or case substitution occurs.
All request/row validation precedes child opens. Duplicate requests fail; names
not observed in the scan produce explicit uncertainty without guessed lookups.
Native open/inspection failures retain their exact states/codes and named issues.
Unresolved discovery rows remain identifiable alongside successfully bound rows.

Successful metadata-only acquisition replaces metadata and recomputes hidden
state from the object opened now. Cached discovery IDs never establish
continuity: even equal IDs may have been recycled before acquisition. Initial
binding creates new held families and cannot preserve previously unheld pane row
IDs, marks or focus. Refresh publication reports that invalidation; the user must
select/review freshly represented objects. Acquired families can subsequently
use the existing sealed refresh stage. Native owners retire on the worker.

Preparation has 256-step and injected cooperative time-slice bounds, per-component
hard caps and pane row/name/display/held-count limits. Request ownership transfers
without a constructor copy. Index/row/issue copies have separate bounded storage;
these are not process-memory quotas. Cancellation discards incomplete observations
and retries only that request; completed acquisitions survive yields. Exceptions
poison and release staging before rethrow. Abandonment releases private state;
issued results remain independently owned and require receiver generation
revocation. No snapshot, provider guarantee or mutation authority is introduced.

Seventeen deterministic cases cover fresh metadata, held-family refresh,
replacement, parent rebinding, separate hard-link names, not-listed requests,
all failure states, cancellation, slow calls, 300 rows, duplicate/byte/count
limits, UTF-16 byte accounting, configuration, clock reversal, exceptions,
retirement, stale publication, selection invalidation and hidden/unknown fields.
Four native cases per host qualify acquisition-to-refresh publication, replaced
parent paths, removed names and hard links with old unheld-selection invalidation.

The same **117-file manifest** was verified on all hosts. All **45 Files suites**
passed: **367 macOS / 366 Linux / 372 Windows cases**. Final normal build/test
seconds were **5.914/1.104**, **14.372/0.311**, **33.973/2.928**. Mac's final six-step
build compiled the new core and both fixtures, archived and linked both binaries;
remote header-dependent builds also regenerated projects and rebuilt affected
existing units. Current hashes, compiled inputs and executable records establish
freshness. The initial Mac fixture compile error was fixed with an explicit
optional payload initializer. Its following test run exposed counters that
included discovery opens; corrected assertions capture the baseline and require
exact acquisition-only counts. Neither failed attempt is accepted as validation.

Focused sanitizers compiled all 28 core units, platform adapter, simulator,
acquisition/refresh domain and native fixtures, and test host (35 units), without
cached objects. **67 cases passed on each host**. Mac ASan+UBSan build/test took
**151.455/0.308 s**, Linux ASan+UBSan **159.675/0.152**, Windows ASan
**68.533/0.328**. This establishes a new fixture-set baseline. The prior different
35-unit layout set took 65.882/151.535/69.694 s. Mac's longer wall time was
investigated while the original compiler continued progressing: host load was
about 80 on 10 CPUs. Contention may contribute, but causation is not established.
Strict warnings stayed enabled as errors; no diagnostic run was restarted or
replaced with stale results.

Diagnostic executables were hashed, verified owner-free and removed; all fixture
roots were empty. Current normal builds remain. Windows work stayed on V: and
Mac temporary work on S8T. No GUI was launched. Validation intervals were UTC on
2026-10-23; the milestone uses the local date, 2026-10-24.
[Acquisition evidence](evidence/ck-files-pane-acquire-20261024.json) retains source
and output hashes, commands, timings, case markers, rejected attempts, load
observation and cleanup. UI boundaries (84 domain sources), ckVision-only
(463 active files), ckdocs (24 pages) and whitespace checks pass.

WP-01 remains open. Combining refresh of existing held families with acquisition
of additional rows in one publication still requires orchestration. Self/
descendant checks, native name equivalence, operation-specific capabilities,
remaining inventories, large-view indexes and UI/worker integration also remain.
WP-02 has not started.

## 2026-10-24 — WP-01 combined refresh and acquisition

`PaneRefreshJob` can now explicitly retain its exact scanned directory in its
sealed result for a subsequent `PaneAcquireJob`. Default release behavior is
unchanged, and invalid retention values fail before refresh I/O. Acquisition
accepts the prepared held rows and diagnostics, then produces one final payload
for the original pane generation. Existing held families/revisions pass through
without reopening, including explicitly requested already-held rows. Their
metadata is canonical from the held object; hidden policy is recomputed for the
requested model. New acquisition still creates independent initial families.

Capacity reserves the union of existing held rows and matched requested rows
before any additional child opens. Requests for held rows do not double-count;
hard-link names remain separate slots. Inherited diagnostics have count and
native-name-byte bounds and are copied incrementally under the existing step/
time limits before acquisition I/O. Old failures remain visible even if later
initial acquisition succeeds: a new hold cannot repair failed continuity with
a previous selection. At most the configured held-count of inherited issues
plus the requested-count of new issues can be retained, with separate bounded
name representations. These are not process-memory or OS handle guarantees.

Only final pane publication preserves marks/focus through sealed families.
Replacements, failed refreshes and previously unheld rows retain the existing
invalidation rules. No stage publishes an intermediate pane. The sequence is
live observation, not a simultaneous filesystem snapshot, frozen operation
plan or execution permission. Native calls can outlast cooperative slices.
All native-owner retirement remains worker-owned.

Nine additional deterministic cases cover mixed publication, exact union
budgets and no duplicate opens, inherited denials followed by successful initial
binding, replacements plus new not-listed issues, inherited issue-byte refusal,
cancellation/resume, invalid retention, 300 inherited issues across yields and
hidden marks requiring review. Two native cases per host verify preserved marked
families with new acquisition and replacement diagnostics alongside new rows.

The same **117-file manifest** was verified on all hosts. All **45 Files suites**
passed: **378 macOS / 377 Linux / 383 Windows cases**. Final build/test seconds
were **4.597/1.119**, **7.627/0.272**, **13.819/2.459**. The final Mac five-step
update follows its initial nine-step build (6.534 s); both tests passed. The
final update adds canonical visibility preparation and expanded inherited-issue
checks. Actual compiled units, source hashes, output timestamps/hashes and all
32 acquisition-suite case markers establish freshness. No failed attempt was
accepted and no warning checks were relaxed.

Focused sanitizers rebuilt the same 35-unit acquisition/refresh set: all 28 core
units, platform adapter, simulator, domain/native acquisition and refresh fixtures,
and test host, without cached objects. **78 cases passed on each host**. Mac
ASan+UBSan build/test took **73.088/0.326 s**, Linux ASan+UBSan
**162.432/0.148**, Windows ASan **69.077/0.349**. Previous same-unit builds took
151.455/159.675/68.533 s with 67 cases. The shorter Mac run was reviewed against
its full compile command, current hashes, units, links and test markers; prior
observed host load was about 80 on 10 CPUs, versus about 46 during this run.
Load may contribute, but elapsed time and load alone do not prove freshness.

Diagnostics were hashed, checked for live owners and removed. Fixture roots
were empty; current normal builds remain. Mac temporary work stayed on S8T and
Windows development stayed on V:. No GUI was launched. Validation intervals
were UTC on 2026-10-23; this milestone uses the local date, 2026-10-24.
[Mixed preparation evidence](evidence/ck-files-pane-mixed-20261024.json) records
commands, source/output/log hashes, timings, case markers, checks and cleanup.
UI boundaries (84 domain sources), ckVision-only (463 active files), ckdocs
(24 pages) and whitespace checks pass.

WP-01 remains open for self/descendant safety, native name equivalence,
operation-specific capabilities, remaining inventories, large-view indexes and
UI/worker integration. WP-02 has not started.

## 2026-10-24 — WP-01 held source/target identity evidence

Added `SourceTargetJob` over bound `HeldChild` sources and a sealed completed
destination batch. It validates every raw source component and source pointer
before metadata I/O, then queries both retained objects without a new lookup,
path reopen or final-link traversal. `HeldChild::inspect_basic` supplies fresh
metadata through its existing child owner without changing its immutable
selection snapshot/family. Completed results retain every source and the entire
destination batch, including target and parent ownership.

Same/Different are available native-identity comparisons only after fresh
identity and kind reports remain qualified against retained observations.
Missing targets are NotApplicable, never Different. Mapping, lookup, whole-query
and individual-field errors stay explicit. Unqualified scopes/schemes, zero IDs,
generation coverage, changed IDs/kinds and conflicting kinds cannot turn into
safe negative evidence. Previously unavailable retained metadata cannot gain
certainty merely because a later read succeeds. Sizes/times can change without
changing native identity; no content/snapshot equality is claimed.

This supplies evidence for later self-target refusal. It does not implement a
copy/move refusal engine, descendant walk, current name-membership check or
execution permission. Native handles may still identify old objects after both
original names are replaced. Different is not collision freedom or universal
alias exclusion across unqualified providers/namespaces. Execution still needs
qualified source/destination names, parents and capabilities.

Each advance has 256 validation/query/comparison steps and an injected cooperative
slice. Rows/components have configurable caps with hard maxima of 4096/1 MiB.
Completed basic-query replies are retained privately before cancellation/time
yields and are not repeated on resume. Fatal validation/resource/clock failures
release staging; exceptions poison before rethrow. Issued results retain owners
independently after abandonment. Native calls can block beyond slices, and all
native-owner retirement belongs on workers.

Twenty-two deterministic cases cover held aliases, distinct IDs, prior absence,
namespace replacement, every failure state, missing fields, changed IDs/kinds,
scopes/schemes/zero IDs/generations, conflicting kinds, changing size/time,
unrepresentable mappings, cancellation, slow calls, 300 rows, full input
validation, exceptions, clocks, retirement, limits, previously unavailable
snapshots, UTF-16 byte accounting, empty batches and malformed kinds. Five native
cases on Mac/Linux and four on Windows qualify same-entry/hard-link aliases,
distinct files, retained objects after replacement, absence/foreign scopes and
POSIX final-link non-following. Windows excludes that POSIX-only fixture.

The final **121-file manifest** matches all hosts. All **47 Files suites** pass:
**405 macOS / 404 Linux / 409 Windows cases**. Final build/test seconds were
**6.684/1.051**, **5.349/0.179**, **8.663/2.826**. Mac's final two-step fixture update
follows its initial 21-step build (13.098 s), which compiled the new core, held
method, affected existing fixtures, both new fixtures and both executables.
The native inputs remain unchanged from that initial build. The final fixture
compile was longer than an earlier two-step update (3.372 s); Ninja records show
an actual **6.563 s compile and 0.076 s link**. Compiled inputs, current hashes,
output records and every new case marker establish freshness, not elapsed time.

The first Linux build rejected misleading indentation in two test setup lines.
Those statements were separated without relaxing warnings. The failed driver
stopped before CTest; final corrected source was rebuilt/tested on every host.
Linux completed remaining build work, while Windows's final fixture update
followed its successful broader preliminary build. Failed/stale tests are not
accepted as validation.

Focused sanitizers compiled all 29 core units, platform adapter, simulator,
acquisition/refresh/source-target domain and native fixtures, and test host
(38 units), without cached objects. **105 Mac / 105 Linux / 104 Windows cases**
passed. Mac ASan+UBSan build/test took **81.822/0.591 s**, Linux ASan+UBSan
**191.917/0.181**, Windows ASan **108.183/0.530**. This establishes the expanded
fixture-set baseline; the previous different 35-unit set took
73.088/162.432/69.077 s. A preliminary same-set Mac sanitizer run passed before
test-only formatting corrections; its diagnostic was separately hashed and
removed, and final corrected inputs were compiled/rerun. Strict warnings stayed
errors throughout.

All final diagnostics were hashed, checked for live owners and removed; full
fixture-root audits were empty. Current normal builds remain. Mac temporary work
stayed on S8T; Windows development stayed on V:. No GUI was launched. Validation
spans UTC 2026-10-23 and 2026-10-24; the local milestone date is 2026-10-24.
[Source-target evidence](evidence/ck-files-source-target-20261024.json) records
source/output/log hashes, commands, times, case markers, rejected build,
preliminary/final cleanup, Ninja timings and repository checks. UI boundaries
(86 domain sources), ckVision-only (467 active files), ckdocs (24 pages) and
whitespace checks pass.

WP-01 remains open for descendant safety, operation-specific capabilities,
native name equivalence, remaining inventories, large-view indexes and UI/worker
integration. Self-target evidence still needs operation-plan/execution integration.
WP-02 has not started.

## 2026-10-24 — WP-01 native parent and ancestor observations

Added metadata-only parent references to held entries and directory cursors,
plus fresh cursor metadata independent of enumeration position. POSIX uses
fixed-component descriptor-relative `openat("..")`, with Linux `O_PATH` and
Darwin `O_EVTONLY`, directory/no-follow/close-on-exec flags and normal access
checks. Linux native coverage verifies no directory-list permission requirement.
Windows selects exact USN v2/v3 IDs by the existing identity scheme, validates
record bounds, opens the full parent ID using the held child as volume/share
hint, and checks native identities, kind and provider before/after acquisition.
No content read, journal mutation, backup flag, privilege change, directory
scan or pathname fallback is used. Native Windows qualification is ReFS v3;
v2 has decoder coverage, not native NTFS qualification. Unsupported queries
remain explicit. Private native probes and rejected approaches are recorded.

`AncestorWalkJob` retains source, destination and observed parents. Its bounded,
resumable state machine reports an observed source, repeated directory or
unavailable edge with the observed prefix. There is no negative ancestry or
safe-to-copy verdict: live edges are not a namespace snapshot. Execution-time
name, edge and capability checks remain required. Directory, step and cooperative
time limits cover traversal and comparisons. Completed replies and pending
owners survive yields without repeated I/O. Native calls can block; ownership
must be retired on workers. Simulator parent links survive until the final
same-parent alias is removed/replaced, with explicit ambiguity across parents.

Nineteen deterministic cases cover qualification, failures, identity changes,
cancellation, slow queries, retirement, exceptions, malformed owners, clocks,
limits, long walks and USN decoding. Native cases cover enumeration independence,
held directories after movement/path replacement, source/self matches, exact
limits, actual root repetition and regular/final-link refusal; Linux adds
unprivileged no-list access.

The final **127-file manifest** matches validated sources. All **49 Files suites**
pass: **429 macOS / 429 Linux / 433 Windows cases**. Final build/test seconds were
**8.954/0.899**, **13.018/0.152**, **39.244/2.879** respectively. Mac's final
12-step update follows the initial 63-step build (26.026 s) and fixture updates.
Compiled inputs, output hashes and case markers establish freshness; elapsed
time alone is not accepted as proof.

Focused sanitizers compiled all 31 core units plus platform adapter, simulator,
source-target/ancestor domain and native fixtures, and test host (38 units),
without cached objects. **77 Mac / 78 Linux / 76 Windows cases** passed.
ASan+UBSan build/test took **65.956/0.329 s** on Mac and **158.187/0.137 s** on
Linux; Windows ASan took **83.524/0.314 s**. Strict warnings remained errors.
An initial cancellation fixture counted acquisition queries in its job total;
the baseline was corrected while retaining the exact four-query requirement.
An initial Windows junction fixture omitted directory creation and caused an
uncaught fixture exception/fast-fail. Explicit creation and scoped removal
corrected it. Both rejected runs are recorded and excluded from passing results.
The abandoned private fixture was inventoried and removed after owner/content
checks. Final tests and sanitizers passed after corrections.

Diagnostics/research were hashed, checked for live owners and removed; final
fixture roots are empty. Current normal builds remain. Mac scratch stayed on
S8T and Windows development on V:. No GUI was launched.
[Ancestor evidence](evidence/ck-files-ancestor-20261024.json) records source,
output/log hashes, native research, commands, times, case markers, rejected runs
and cleanup. UI boundaries (90 domain sources), ckVision-only (473 active files),
ckdocs (24 pages) and whitespace checks pass.

WP-01 remains open for operation-specific capabilities, native name equivalence,
remaining inventories, large-view indexes, UI/worker ownership integration and
operation/execution safety integration. These observations do not complete
self/descendant safety. WP-02 has not started.

## 2026-10-24 — WP-01 native directory access observations

Added worker-only `DirectoryCursor::check_access` for List, Search, AddFile and
AddDirectory. Successful reports retain request, native query family/mask and
qualified directory identity. They describe a native check at query time, never
operation permission or a reservation. Unavailable/denied results keep native
diagnostics; invalid requests fail before I/O and the default adapter is
Unsupported. There is no mode-bit/ACL emulation or private probe creation.

Linux calls `faccessat2` directly with `AT_EMPTY_PATH | AT_EACCESS`, with no old
syscall/libc fallback. Darwin uses fixed internal `.` relative to the held
directory and `AT_EACCESS`; its native search requirements can differ from Linux
empty-path semantics. POSIX Add requests check `W_OK | X_OK`, without claiming
separate file/directory creation capability. Windows opens the exact held ID
using the requested directory right plus metadata/synchronize rights, full
sharing and reparse/no-recall flags. It uses no backup intent or privilege
change and checks the temporary handle against the original object. Native
Windows qualification is ReFS on V:; other drivers may refuse without a path
fallback.

Fresh kind/identity checks surround successful queries. Known Darwin/Windows
provider states block the query; Linux has no generic provider-state query and
this access check makes no locality/hydration claim. Names, child rights,
capacity/quotas, mount state, immutable flags, inheritance, security semantics,
providers and concurrent changes remain independent execution requirements.
No delete/replace/rename/ACL-replay capability is inferred from Add access.

Four deterministic cases cover defaults/invalid requests, metadata/field errors,
provider checks, malformed kinds and changed/unqualified identities. Native
cases exercise every request and exact native mask without consuming enumeration
or creating children, retained objects after pathname replacement, and current
permission changes. POSIX tests run unprivileged and restore mode bits. An
additional Darwin ACL fixture proves denial despite writable mode bits; the
Windows DACL fixture proves a new denial takes effect despite a retained cursor
and distinguishes AddFile from AddDirectory. ACLs are restored before cleanup.

The **130 exported input hashes plus unchanged test-host hash** match validated
sources. All **51 Files suites** pass: **437 macOS / 436 Linux / 440 Windows
cases**. Initial broad build times were **25.887/51.736/110.317 s** (Mac/Linux/
Windows, 60/61/61 steps), consistent with actual affected interface/core/platform
and fixture compilation. Final build/test took **1.104/1.089**, **2.608/0.157**,
**4.527/2.676 s**, following the documented preceding builds. Ninja records
identify compiled inputs and links; source/output hashes and individual case
markers establish freshness. The smaller final updates are not no-op evidence.

Focused diagnostics compiled all 32 core units, platform adapter, two new
fixtures and test host (36 units), without object caches. **8 Mac / 7 Linux /
7 Windows cases** passed. Mac ASan+UBSan build/test took **58.369/0.033 s**;
Linux ASan+UBSan **108.857/0.029 s**; Windows ASan **63.985/0.150 s**. Strict
warnings remained errors. This new fixture set has its own duration baseline.

The added Darwin ACL fixture initially assumed absence would return an empty
ACL object; native `acl_get_file` reported `ENOENT`. It now recognizes that exact
absence and restores the native empty ACL representation. The denial assertion
remained unchanged; final normal/sanitizer runs pass. Apple ASan also rejected
an unsupported `detect_leaks=1` option before tests started. The same built binary
passed with supported ASan+UBSan options and `detect_leaks=0`; no Mac leak-check
coverage is claimed. Both rejected attempts are recorded and excluded from
passing results.

All diagnostic binaries were hashed, audited for live owners and removed;
fixture roots are empty and current normal builds remain. Mac temporary work
stayed on S8T, Windows development on V:. No GUI was launched.
[Directory-access evidence](evidence/ck-files-directory-access-20261024.json)
records source/output/log hashes, commands, times, case markers, rejected runs,
Ninja timings and cleanup. UI boundaries (92 domain sources), ckVision-only
(477 active files), ckdocs (24 pages) and whitespace checks pass.

WP-01 remains open for complete operation-specific capability/preservation
assessment, native name equivalence, remaining inventories, large-view indexes,
UI/worker integration and execution safety. WP-02 has not started.

## 2026-10-24 — WP-01 qualified directory name rules

Added `DirectoryNameRulesProbe` with retained directory ownership and independent
refresh through that object after pathname replacement. A potentially usable
rule needs two live location reports qualified against the cursor's native
identity. Unavailable/unqualified first reports are not retried; attempted
reports and exact diagnostics remain visible. Cancellation returns no partial
probe and query exceptions release private staging. All native queries and
final owner retirement belong on workers; calls can block.

The initial rule is `LinuxExtendedFsBytewise`: native Linux query family,
extended-filesystem type code `0xef53`, qualified POSIX identity and successful
inode flags with casefold and fscrypt filename-encryption bits clear. Identity,
type and relevant bits must remain qualified across observations; contradictory
case reports and required-field failures block a rule. Unrelated flag changes
are retained without inventing a name-policy change. Native-header assertions
check the constants. No filesystem-label, OS-wide, locale or Unicode case-table
inference is used.

Component comparison distinguishes exact native units (a lexical witness even
when native rules fail) from distinct names under a qualified reported rule.
Unavailable directory context is preserved for lexical results. Comparison
bounds each input in native bytes before scanning, requires one encoding and
never maps, normalizes, truncates or rewrites names. Distinct names can still
be hard links. These reports do not establish acceptance, absence, object
inequality, a stable namespace, creation capability or execution-time collision
freedom. Parent/rule/occupant revalidation remains mandatory for execution.

Eleven deterministic cases cover qualification, all required-field error states,
changed identities/families/types/flags, unknown policies, casefold/encryption
refusal, byte/UTF-16 budgets, invalid inputs, cancellation, exceptions, lifetime,
refresh and second-query failure. All hosts test lexical evidence, enumeration
independence and retained-directory refresh/retirement after path replacement.
Linux additionally uses exclusive creation to verify case-only, composed versus
decomposed Unicode and invalid-UTF-8 pairs with distinct native IDs, then verifies
that distinct hard-link names share an object ID. Native qualification markers
prove these positive branches ran on the actual ext4 scratch mount, with kernel
`6.12.113+deb13-cloud-arm64`.

The **134 exported input hashes plus unchanged test-host hash** match validated
sources. All **53 Files suites** pass: **450 macOS / 450 Linux / 453 Windows
cases**. Final build/test took **2.129/1.074**, **2.718/0.188**, **5.700/3.130 s**.
Compiled input/link records include the successful preceding core/native builds;
final updates compile corrected fixtures rather than treating old executables
as current. Source/output hashes and individual case markers establish freshness,
not elapsed time. Normal targets use Ninja/Release on all three hosts.

Focused diagnostics compiled all 33 configured core units, platform adapter,
two new fixtures and test host (37 units) directly from source, without object
cache inputs. **13 Mac / 14 Linux / 13 Windows cases** passed. ASan+UBSan
build/test took **52.305/0.268 s** on Mac and **118.198/0.036 s** on Linux;
Windows ASan took **67.413/0.118 s**. The preceding different 36-unit access
fixture set took 58.369/108.857/63.985 s to build. This new fixture set establishes
its own baseline; Mac LeakSanitizer coverage is not claimed.

An initial Mac native fixture incorrectly used a const cursor for non-const
enumeration; its ownership type was corrected while probe ownership remains
const. GCC then rejected a copied range-loop pair in the Linux-only fixture;
the loop now uses a const reference. Strict warnings and every assertion stayed
intact. Both failed builds stopped before CTest and are excluded from passing
validation. Final corrected inputs passed normal and sanitizer runs on all hosts.

Diagnostics were hashed, checked for live owners and removed; fixture roots are
empty and current normal builds remain. Mac temporary work stayed on mounted S8T
and Windows development on V:. No GUI was launched.
[Name-rule evidence](evidence/ck-files-name-rules-20261024.json) records source,
output/log hashes, commands, configuration, times, native qualification markers,
rejected builds, cleanup and primary native contracts. UI boundaries (94 domain
sources), ckVision-only (481 active files), ckdocs (24 pages) and whitespace
checks pass.

Darwin, Windows, other Linux filesystems and encrypted/casefold directory policies
remain unqualified here. Native positive coverage is ext4; no native ext2/ext3,
encrypted/casefold or other-driver qualification is claimed. WP-01 remains open
for complete cross-platform names/collisions, operation capabilities and
preservation assessment, remaining inventories, large-view indexes, UI/worker
ownership and execution safety. WP-02 has not started.

## 2026-10-24 — WP-01 destination batch name-group assessment

Added `DestinationNamesJob` over a sealed destination batch. It retains original
source/mapped names, mapping failures, exact-name groups, existing occupants and
independent object-alias evidence through the original held parent. Summary
construction and input validation are linear in rows; pair/row access uses the
sealed exact-group index without rescanning names or a quadratic pair table.
No new child lookup, original pathname reopen, content access or mutation occurs.

The result reports mapped/unmapped rows, exact groups, duplicate groups and rows
within duplicate groups. Zero or one mapped group needs no native policy query;
`None` means no other mapped groups, never no conflicts or permission. Duplicate
groups, mapping failures and existing-target conflicts remain explicit. Distinct
groups require a fresh qualified applicable native rule; unavailable policies,
changed/denied/failed observations and encoding inapplicability remain unresolved.
Hard-link object identity cannot become name equivalence. These observations do
not refresh old occupants or supply an execution-time collision proof.

Each parent location call is a separate phase. Completed replies commit before
cancellation/time yields and are not repeated on resume. All mapped component
limits and row limits are validated before new native I/O. The job has 256 steps
per advance, an injected cooperative slice and hard row/component caps. Fatal
resource/clock errors and exceptions poison staging without exposing a prefix;
abandonment releases private ownership while issued results retain the whole
batch independently. Native calls/retirement belong on workers and can block.

Thirteen deterministic cases cover exact/qualified/unresolved groups, hard-link
aliases, empty/duplicate-only/all-unmapped inputs, every native failure state in
either observation, identity/flag changes, encoding applicability, cancellation,
slow calls, 600 rows, pre-I/O limits, UTF-16 byte accounting at a later row,
configuration, exceptions, clock reversal, abandonment and issued-owner lifetime.
Three native cases on every host cover missing candidate groups, hard links and
held-parent queries after pathname replacement. Linux native markers prove the
positive distinct-group branch ran; other native policies remain unqualified.

All **138 exported input hashes plus unchanged test-host hash** match validated
sources. All **55 Files suites** pass: **466 macOS / 466 Linux / 469 Windows
cases**. Final build/test took **3.407/0.891**, **7.483/0.171**, **21.651/3.423 s**.
Mac's final two-step fixture update follows its initial six-step core/native/
fixture build (6.814 s); native/core code remained unchanged between those builds.
Compiled inputs, source/output hashes and case markers establish freshness.
Normal targets remain Ninja/Release on all three hosts.

Focused diagnostics compiled all 34 configured core units, platform adapter,
two new fixtures and test host (38 units) directly from source without object
cache inputs. **16 cases passed on each host**. ASan+UBSan build/test took
**55.261/0.327 s** on Mac and **135.470/0.074 s** on Linux; Windows ASan took
**79.039/0.219 s**. The preceding different 37-unit name-rule fixture set took
52.305/118.198/67.413 s to build; the new larger fixture set establishes its own
baseline. Strict warnings remained errors. No failed run is counted as evidence,
and no Mac LeakSanitizer coverage is claimed.

Diagnostics were hashed, checked for live owners and removed; fixture roots are
empty and current normal builds remain. Mac temporary work stayed on S8T and
Windows development on V:. No GUI was launched.
[Destination-name evidence](evidence/ck-files-destination-names-20261024.json)
records source/output/log hashes, configuration, commands, times, native branch
markers, cleanup and compiled-unit coverage. UI boundaries (96 domain sources),
ckVision-only (485 active files), ckdocs (24 pages) and whitespace checks pass.

WP-01 remains open for remaining cross-platform native name policies, complete
operation capabilities/preservation assessment, remaining inventories, large-view
indexes, UI/worker ownership and execution safety. WP-02 has not started.

## 2026-10-24 — WP-01 Linux directory fscrypt policy inventory

Added `DirectoryCursor::inspect_fscrypt_policy`, a read-only held-directory
query using `FS_IOC_GET_ENCRYPTION_POLICY_EX` on Linux. It preserves exact v1
(version 0, 12 bytes) and v2 (version 2, 24 bytes) policy records, including
unknown mode/flag/reserved/key-reference bytes. Structural accessors do not claim
cryptographic strength, key availability or replay equivalence. Only native
`ENODATA` produces an absent-policy report with its witness. Unsupported and
unknown queries never become absence; there is no deprecated ioctl fallback,
pathname reopen, key operation, unlocking, privilege change or content access
in the product implementation. Other platforms explicitly return Unsupported.

Fresh directory kind/identity reports bracket available/absent results. Known
statx encryption bits must corroborate policy presence at both observations;
changed or unqualified identities, malformed kinds and contradictory bits block
success. Fixed native buffers, exact returned lengths and native-header ABI
assertions prevent prefix/oversize records. Missing flag coverage is not a clear
bit. Absence is restricted to fscrypt policy and does not describe whole-volume,
dm-crypt/FileVault/EFS encryption or operation permission. Regular-file policy
inspection and preservation/capability integration remain open.

Six deterministic cases cover structural decoding, exact lengths, opaque bytes,
unknown versions, default unsupported behavior, identity/kind qualification,
known/unknown encryption bits and every metadata/field failure state. Native
cases verify enumeration independence and held directories after path replacement.
A separate Linux fixture qualifies locked v2 policy inspection as the ordinary
guest user, exact bytes/owner after rename, ENODATA on a plain replacement, and
refusal of bytewise name rules for the encrypted directory.

That fixture used an exclusively created disposable 64 MiB ext4 image with its
own encrypt feature. A private setup helper used fixture-only privilege to assign
a v2 policy to an empty directory with a public placeholder key identifier, then
verified key status ABSENT. No key was added and no encrypted content was created
or copied. The test restores fixture names between runs. Privilege is absent
from the product query and from both configured test executions.

All **142 exported input hashes plus unchanged test-host hash** match validated
sources. All **57 Files suites** pass: **474 macOS / 475 Linux / 477 Windows
cases**. The ordinary Linux suite leaves the optional locked case unconfigured;
a separate configured native-suite run passes all three cases with its required
positive marker. No unconfigured fixture is counted as positive policy evidence.
Final build/test took **1.016/0.776**, **39.908/0.165**, **176.904/5.483 s**.
Mac's fixture update follows the successful 68-step broad build (28.650 s).

Windows' 69-step broad build was slower than the previous 61-step access workload
(110.317 s). Investigation retained compiler/link logs and Ninja timings: the
affected sequence links at 161431 ms, with object-inventory fixture compilation
at 26.164 s and the Windows adapter at 21.245 s. Actual changed-input compilations,
matching manifests, output hashes and case markers establish freshness. The cause
of the duration increase is not established; elapsed time is not used as proof
of freshness or performance. Normal builds remain Ninja/Release.

Focused sanitizers compiled all 35 configured core units, platform adapter, two
fixtures and test host (39 units) directly from source without object caches.
**8 Mac / 9 Linux / 8 Windows cases** passed. Linux's configured sanitizer run
also requires the locked-policy marker. ASan+UBSan build/test took
**52.060/0.309 s** on Mac and **119.754/0.034 s** on Linux; Windows ASan took
**78.763/0.176 s**. The preceding different 38-unit destination-name fixture set
took 55.261/135.470/79.039 s to build. Strict warnings remained errors; Mac
LeakSanitizer coverage is not claimed.

The private fixture was checked for fd/cwd owners and exact mount/loop backing
identity, ordinarily unmounted, checked for loop release, hashed and removed.
An initial cleanup collector lacked explicit list mode for losetup JSON; parsing
stopped before any unmount or deletion, and the corrected collector completed.
Diagnostics were separately hashed, checked for live owners and removed. Fixture
roots are empty and current normal builds remain. Mac temporary work stayed on
S8T and Windows development on V:. No GUI was launched.
[Fscrypt evidence](evidence/ck-files-fscrypt-20261024.json) records source/output/
log hashes, fixture setup/source/cleanup, commands, times, positive markers,
Windows timing investigation and primary kernel contracts. UI boundaries
(98 domain sources), ckVision-only (489 active files), ckdocs (24 pages) and
whitespace checks pass.

WP-01 remains open for remaining inventories, full cross-platform policies and
operation capability/preservation assessment, large views, UI/worker ownership
and execution safety. WP-02 has not started.

### Immutable bounded pane display pages

The next WP-01 milestone supplies the worker-to-UI viewport boundary:
`PanePage` copies prepared display rows and observations without raw native
names, native owners or a reference to the worker model. Capturing a window
has explicit row/string limits and preserves every included observation and
diagnostic without truncation. Failure returns no partial page.

`PaneStateStamp` lets the worker reject commands originating from an older
model state. Every successful state mutator and model destruction revokes prior
stamps; rejected requests preserve them. The shared revocation bit can be read
on the UI thread without exposing mutable model state or retaining handles.
It neither freezes the filesystem nor authorizes operations.

Eight new cases cover window/end/overflow bounds, default/hard/invalid limits,
aggregate string accounting (including each of the 17 metadata diagnostics),
filter/hidden/inspection/listing errors, immutable old rows, mark/focus/view
state, pending and invalidated selection, every state mutator, foreign and
rejected stamps, worker-model destruction, native-owner retirement through the
simulated adapter, zero filesystem calls during capture, and a retained page
read on a separate UI thread. Existing pane and continuity cases also run in
the focused sanitizer executable. Validation details are recorded in
[the pane-page evidence](evidence/ck-files-pane-page-20261024.json).

WP-01 remains in progress. These are viewport values and state validation
primitives; they do not implement the UI/command dispatcher, full property
presentation, operation planning or mutation. WP-02–08 remain unstarted.

Validation on 2026-10-24: all 58 Files suites pass with 482 cases on macOS,
483 on Linux and 485 on Windows. Each host also passes 40 focused pane/page/
continuity cases in a fresh sanitizer build: ASan+UBSan on macOS/Linux and ASan
on Windows. Linux leak detection is enabled; Apple leak detection is unavailable.
All 145 exported source inputs match current bytes. Exact test output hashes,
UTC build intervals, compiler commands, SDK/test-host fingerprints, the initial
missing-test-include compile error, and cleanup audits are in the evidence.
Diagnostic builds were removed after native-owner/process and empty-fixture
checks; current normal test builds remain. No application was launched.

### Guarded pane commands and copied selection summaries

The WP-01 pane foundation now exposes retained total/visible/hidden marks and
exact source-readiness counts in immutable pages, without building native-name
candidate arrays or scanning rows for each summary. Counts are updated with
mark bits and prepared together with new projections/listings. Pending scans
and invalidated refresh selection keep zero actionable candidate counts while
still exposing retained marks; hidden marks remain an explicit review state.

`apply_pane_command` checks the page's state stamp on the owning worker before
focus, row/range marking, visible bulk marking/inversion, clear-all or view
changes. Range endpoints are row IDs; stale, foreign, hidden and invalid
requests cannot retarget selection or clear diagnostics. View limits are
validated before copying caller text. These commands only change pane state;
filesystem planning, operations and ckVision UI dispatch remain unimplemented.

Seven new cases cover every command and stale/foreign/empty stamps, queued old
page reuse, reversed row-ID ranges, reorder/filter safety, atomic invalid-target
and invalid-view rejection, 1200 deterministic mixed mark/filter/view changes
checked against independent row-bit counts, hidden marks in copied pages,
pending/error transitions, qualified versus unheld refresh continuity and
navigation reset. The existing pane/page/continuity cases remain regression
coverage. See [the command evidence](evidence/ck-files-pane-command-20261024.json).
WP-01 remains in progress; WP-02–08 remain unstarted.

Final validation on 2026-10-24: all 59 Files suites pass with 489 macOS,
490 Linux and 492 Windows cases. Each host also passes 47 focused command/
page/pane/continuity cases after freshly compiling all 43 sanitizer translation
units (ASan+UBSan on macOS/Linux; ASan on Windows). Windows ASan initially
rejected an unreachable shared visitor return under `/W4 /WX`; explicit returns
per alternative fixed the warning, and an exhaustive compile-time check now
requires a handler for any future command type. No warning was suppressed.
All three normal targets/suites and fresh sanitizer builds were rerun after
that correction. Current source bytes, output timestamps/hashes, compiler
commands, timing comparisons and diagnostics are in the evidence. The failed
Windows sanitizer driver omitted its timing record before throwing; that gap
is recorded explicitly rather than assigning an invented interval. Failed and
successful disposable diagnostics were hashed and removed after owner/fixture
audits. Current normal test builds remain; no application was launched.

### Filesystem-keyring status for held Linux encryption policies

WP-01 now adds read-only `FS_IOC_GET_ENCRYPTION_KEY_STATUS` through a retained
directory, using that directory's exact v1/v2 policy and independently checking
identity/policy afterward. Raw status/flags/user-count/reserved output is
retained; helpers distinguish defined filesystem-keyring states and reject
unknown status semantics. V1 legacy process-keyring coverage is explicitly
excluded, and v2-only fields are not inferred for v1 policies. No key material,
key management, content read or pathname fallback is introduced.

Seven deterministic cases cover unsupported defaults, known/unknown status
codes, native flags/counts/reserved-word retention, v1 field restrictions,
every policy-byte change, changed/unqualified directory IDs and all unavailable
native policy states. A native suite checks enumeration/non-mutation and, when
explicitly configured, private v1/v2 absent-key policy directories and held
identity after pathname replacement. The controlled fixtures contain no key
material or encrypted content; their native queries run as the ordinary guest
user. Optional unconfigured cases are not positive encryption coverage.

See [the key-status evidence](evidence/ck-files-fscrypt-key-20261024.json) for
source, ABI, normal/sanitizer, fixture and cleanup records. WP-01 remains in
progress. This is one native inventory observation, not complete encryption
inventory, preservation assessment, loss consent or a production operation.

Validation on 2026-10-24: all 61 Files suites pass on macOS (497 cases), Linux
(500) and Windows (500). Broad Linux runs leave the two optional encrypted
fixture cases explicitly unconfigured; a separate unprivileged native run and
the sanitizer run require positive v1/v2 markers. Focused sanitizer results:
14 macOS cases, 16 Linux cases including both real private fixtures, 14 Windows
cases. ASan+UBSan run on macOS/Linux, ASan on Windows; Linux leak detection is
enabled and Apple leak detection is unavailable. All 152 exported inputs match
current source bytes. The native filesystem image, helper binaries and loop
mount were audited, hashed, unmounted/released and removed; diagnostic builds
were removed after owner/empty-scratch checks. Current normal test builds are
retained with output hashes and build provenance. No app was launched.

### Bounded held-directory inventory collection

WP-01 adds `DirectoryInventoryJob`: a worker collection through one retained
cursor for basic/location bookends, all four directory access checks and the
existing fscrypt policy/key queries. It captures bounded variable payloads,
seals immutable results only after collection, and supports cancellation/time
yields without repeating completed query calls. Native errors stay independent;
malformed results, resource/clock failures and exceptions stop collection and
release private owners. Completed results retain their exact cursor for worker
use and retirement.

Cached/before/after directory kinds/IDs must qualify before individual owner
matches are accepted. Name-rule derivation also rejects contradictory encryption
observations or unqualified encryption owners. The output is not a snapshot,
complete preservation inventory or operation authorization.

Twelve deterministic cases cover all query families/denials, every outer query
cancellation/time/exception boundary, poisoned/abandoned lifetimes, clock
failures, malformed replies, directory/owner changes, encryption disagreement,
and independent exact payload totals for all 17 basic metadata fields, all 20
location fields, top-level diagnostics and native identifier encodings. Two
native cases verify the held directory, unchanged enumeration/content, owner
qualification and isolation after original-path replacement. See
[the directory-inventory evidence](evidence/ck-files-directory-inventory-20261024.json).
WP-01 remains in progress; WP-02–08 remain unstarted.

Validation on 2026-10-24: all 63 Files suites pass on macOS (511 cases), Linux
(514) and Windows (514). Each host also passes all 14 focused directory-inventory
cases after a fresh 43-unit sanitizer build (ASan+UBSan on macOS/Linux, ASan on
Windows; Linux leak detection enabled, Apple leak detection unavailable).
All 156 exported inputs match current bytes. Output hashes/build timestamps,
commands, scope/timing comparisons, native results and cleanup audits are in
the evidence. Diagnostic builds were removed after owner/empty-scratch checks;
current normal test builds remain. No application was launched.

### Native file-size, symlink-length and subject link limits

WP-01 adds descriptor-relative `_PC_FILESIZEBITS` and `_PC_SYMLINK_MAX` to
location inventory, plus `_PC_LINK_MAX` on the actual retained `OpenedEntry`
inode. The link report carries stable kind/identity qualification and separate
before/after link counts; a directory report cannot stand in for child-file
limits. Object inventory collects it as a separate, cancellable query phase.
Unsupported Windows/default numeric queries remain explicit, without inferring
feature absence or inventing a filesystem-specific constant.

Pure numeric assessments cover signed-width boundaries, symlink target bytes
and proposed hard-link totals with no overflow, allocation by limit, path
conversion, truncation or permission claim. Size width is not an exact file
byte ceiling, and current link counts are not a stable remaining allowance.
The shared pathconf decoder retains indeterminate/native-error states instead
of converting them to infinity. Directory inventory budgets now cover both new
fields, with independently calculated exact totals for all 22 location fields.

Seven new deterministic file-limit cases cover boundaries, families, native
return decoding, subject kind/identity changes and per-field failures. The
object-inventory fake adapter and existing boundary/cancellation tests include
the new query; an additional case checks malformed reports/owner mismatch.
Native tests compare separate descriptor queries, preserve enumeration, retain
a file after rename/replacement/unlink, collect the new object inventory field,
and on POSIX query a FIFO without opening a data reader. No huge files, link
exhaustion or data-loss operation is used. See
[the file-limit evidence](evidence/ck-files-file-limits-20261024.json).
WP-01 remains in progress; WP-02–08 remain unstarted.

Final validation on 2026-10-24: all 65 Files suites pass on macOS (523 cases),
Linux (526) and Windows (525). Reviewed fresh 46-unit sanitizer builds pass
44 focused cases on macOS/Linux and 43 on Windows. ASan+UBSan run on macOS/
Linux, ASan on Windows; Linux leak detection is enabled, Apple leak detection
unavailable. The initial Windows fixture used the wrong native-name encoding;
the adapter rejected it, and the fixture was corrected to explicit UTF-16
without changing validation. Its failed scratch leaf was removed only after
exact owner/content checks. All hosts were retested and sanitizer builds
repeated for the reviewed source. Initial Windows failure timing was omitted
by the old success-only driver; that gap is explicit, and the reviewed driver
now records failures in `finally`. All 160 current inputs, output hashes,
per-target build provenance, logs, scope/timing differences and cleanup audits
are retained in the evidence. Diagnostic builds are removed; current normal
tests remain. No application was launched.


## 2026-10-24 — held-object Windows integrity configuration

WP-01 now collects checksum configuration through the retained Windows handle
with `FSCTL_GET_INTEGRITY_INFORMATION`. The query uses metadata rights and never
reads contents, reopens the path, changes settings or elevates. Before/after
checks require qualified equal Windows identities, regular-file/directory kind,
stable integrity attribute and no known reparse/provider condition.

The fixed-size decoder retains all 16 bytes, including reserved bytes, unknown
algorithm codes and flag bits. Helpers report None/CRC32/CRC64 and the literal
enforcement-off bit; they do not certify content, enforcement, geometry or
preservation support. macOS/Linux return Unsupported for this Windows query.
`ObjectInventoryJob` adds an independent bounded phase with explicit owner
qualification, query/retention accounting and existing cancellation/failure
semantics. Simulation supports the same observation without inventing a native
filesystem capability.

Validation uses all 67 Files suites on macOS, Linux and Windows, plus a focused
46-unit sanitizer build covering the decoder/guards, native query and object
collector. Windows native tests compare against an independent metadata-only
handle for files/directories and a held object after rename/path replacement.
Successful ReFS qualification markers are required in this workstation's
normal and sanitizer evidence. Other Windows volumes may return a native
refusal, which the tests compare independently rather than assuming ReFS.
No native NTFS or all-checksum-mode qualification is claimed.

The first reviewed Windows incremental build reported no work after source
extraction: the revised fixture timestamp preceded its earlier object build.
That result was rejected as final-source evidence. Its exact native fixture
object/executable were removed and rebuilt before final validation; unchanged
core inputs were verified by hashes. See the
[integrity-query evidence](evidence/ck-files-integrity-20261024.json) for results,
source/SDK fingerprints, timing, output hashes and diagnostic cleanup.

WP-01 remains in progress. Destination integrity application, full preservation
capabilities and operation-specific consent remain outstanding, as do the
remaining cross-platform name policies. WP-02–08 remain unstarted.


## 2026-10-24 — lossless absolute navigation addresses

WP-01 adds `NativeAbsolutePath`, a bounded tagged value for exact POSIX bytes
or Windows UTF-16. It preserves spelling and escapes only presentation text.
The host bridge refuses foreign encodings, invalid/moved values and relative
addresses before invoking an injected adapter. Qualified Windows drive, UNC
and volume-GUID paths share one core namespace validator with the production
filesystem entry points; device namespaces, streams and ordinary DOS aliases
remain refused. Extended separators and UNC server/share colons are now
checked explicitly. Oversize input is Unknown/InventoryLimit, never a prefix.

The value performs no I/O, normalization, cwd resolution or cross-encoding
conversion. It describes a navigation address rather than a retained object.
Native replacement tests distinguish address lookup of the replacement from
continued access to the original held object. Existing final-link policy is
passed explicitly and the native path/directory regression suites remain part
of focused sanitizer validation.

All 69 Files suites pass on macOS, Linux and Windows: 548, 551 and 550 cases.
Eight new deterministic cases cover POSIX bytes, all unpaired UTF-16 surrogates,
namespace roots/refusals, limits, lexical equality, moved values and exact
adapter dispatch. Four native cases cover real lookup/held identity, replacement,
foreign/oversize refusal and filesystem-specific raw-encoding behavior.
Case-sensitive APFS rejects the invalid UTF-8 creation with EILSEQ (92), and its
lookup refusal matches an independent native call. Linux and workstation ReFS
accept and enumerate the tested raw units unchanged. The model preserves the
address in both outcomes; no substitute name is created.

The first Mac fixture incorrectly required APFS to create invalid UTF-8 and
aborted; its empty private directory was checked and removed. The replacement
fixture explicitly verifies real acceptance or refusal. GCC also flagged a
potential uninitialized nested-variant reset; the move operation now clears
the existing string alternative rather than changing the source variant tag.
Strict warnings remain enabled. Windows changed input timestamps are explicitly
advanced after verified extraction so Ninja cannot reuse older-content objects
whose timestamps happen to be newer than the archive's source timestamp.

See [the navigation-address evidence](evidence/ck-files-path-20261024.json) for
normal/sanitizer results, fingerprints, exact output paths, timing and cleanup.
WP-01 remains in progress for full capability/preservation inspection and
cross-platform collision rules; WP-02–08 remain unstarted. No mutation or Files
UI command is introduced here.


## 2026-10-24 — held-object volume and mount relationships

WP-01 adds `ObjectLocationRelation`, an immutable worker-owned assessment over
completed directory/object inventories. It retains both native owners without
new I/O, path reopen, enumeration or content access. Identity/kind bookends and
location-query ownership must qualify before their facts can be used. Failures
retain their state/error, and unrelated adapter scopes or identity schemes are
not compared as though their raw identifiers belonged to one namespace.

Reported volume-identifier equality and Linux mount-ID equality are independent
fields. Equal volume IDs never become a same-mount assumption. Mount facts need
a qualified Linux query context and stable mount IDs at both bookends; other
platforms return Unsupported for this Linux-specific comparison. These are
cached facts, not membership, ancestry, same-filesystem rename capability or
operation permission. Equal serials do not establish a unique physical volume.

All 71 Files suites pass on macOS, Linux and Windows: 559, 563 and 561 cases.
Nine new deterministic cases cover independent volume/mount combinations,
all failure states, identity/kind changes, namespace mismatch, wrong query
owners/families, unsupported platforms, special-object identity and owner
lifetime without adapter calls. Native ordinary-pair and path-replacement
checks run on all three OSes. The replacement fixture keeps both references
open, moves the file and directory independently, replaces both original paths
and verifies both retained identities. The pair deliberately ceases to have
parent/child membership, which the relationship must not infer.

The first Windows fixture attempted to rename a directory while its held file
remained inside it; ReFS/Windows returned Access Denied. The corrected fixture
uses independent moves for the two references, preserving the full retention
assertion. Production code contains no rename or fallback. The aborted fixture
was recorded and removed after exact process checks; subsequent scratch roots
are checked empty.

A separate private Linux mount namespace qualifies the important counterexample:
a bind-mounted directory has equal native object/volume IDs but different mount
IDs. A tmpfs target differs in both reported volume and mount IDs. Independent
`statx` queries agree. Native mounts are test-only, under the task temporary
root, private to the subprocess, and unmounted/removed afterward. Normal and
sanitizer records include the positive fixture marker and cleanup evidence.

See [the location-relationship evidence](evidence/ck-files-location-relation-20261024.json)
for source/SDK fingerprints, normal/sanitizer runs, timings, exact executable
paths, native fixtures and cleanup. WP-01 remains in progress for the remaining
capability/preservation inspection and cross-platform collision rules.
WP-02–08 remain unstarted; no Files UI or mutation command is enabled here.


## 2026-10-24 — explicit Linux regular-file inode flags

WP-01 adds read-only `FS_IOC_GETFLAGS` through a retained Linux regular-file
reference. The separate `MetadataAndInodeFlags` access request obtains a checked
readable handle via the already-held kernel fd reference, not the old basename.
Layout and inode access remain independent, including when combined. The query
never reads main content, sets flags or elevates. Non-Linux queries remain
Unsupported; special files cannot become readers through the Linux request.

The report preserves all 32 native bits and a qualified before/after owner.
Literal helpers cover compression/no-compression, synchronous, immutable,
append-only, no-dump/no-atime, encryption, journaling, directory sync, verity,
no-CoW, DAX, project inheritance and casefold in their own ioctl namespace.
Clear bits are not feature-absence evidence; set bits are not effective-behavior
or preservation guarantees. Unknown bits survive. Inode-only access needs stable
kind/identity, not an unchanged content size. Directory inode flags remain in the
existing held-directory location inventory.

`ObjectInventoryJob` collects the new family independently with four-byte
query/retention accounting, one record and explicit owner qualification.
Simulation supports access, limits and raw-bit observations; the new phase also
participates in cancellation, exception and every-state failure injection.
No partial inventory is published on malformed or over-budget replies.

All 73 Files suites pass on macOS, Linux and Windows: 572, 577 and 574 cases.
Eight new deterministic cases exercise literal mapping, unknown bits,
namespace separation, subject qualification, failures and access. Two additional
object-collector cases test raw retention/owner mismatch and invalid/budgeted
replies. Native tests qualify independent ioctl agreement, held identity after
rename/replacement, a no-dump bit on a private file, FIFO refusal and unprivileged
read denial. Other filesystems may refuse GET/SET flags; tests preserve and
compare the actual native refusal instead of assuming support. This host's
positive getter/no-dump/permission markers are required in normal and sanitizer
evidence. No native no-CoW/DAX/verity-enable qualification is claimed.

The shared Linux fd-upgrade diagnostic is now `openat.read_access_fd`. An initial
layout regression test correctly retained Denied but still expected the former
layout-only diagnostic name; the assertion was updated and the independent
inode-read-denial test added. The read denial itself was never bypassed. Strict
warnings remain enabled. No unvalidated or failed binary is used as final-source
evidence.

See [the inode-flag evidence](evidence/ck-files-inode-flags-20261024.json) for the
53-unit focused sanitizer build, full-suite runs, exact source/SDK/output
fingerprints, timing, native qualification and scratch/diagnostic cleanup.
WP-01 remains in progress; destination preservation capabilities and remaining
cross-platform collision rules still need work. WP-02–08 remain unstarted.


## 2026-10-24 — bounded lossless directory-index records

WP-01 now has the record-format foundation for the required disk-backed listing
index. The worker-side codec preserves a nonzero row ID, exact POSIX bytes or
Windows UTF-16, all 17 basic metadata observations and independent hidden state.
Available values and every failure state/domain/code/diagnostic remain distinct.
No pointer, native owner, locale conversion or display-name reconstruction is
serialized. A trusted live index context supplies the source namespace and
expected nonzero generation. Cached IDs remain observations, not acquisition
proof or mutation permission.

The versioned little-endian envelope checks length, schema and CRC32C before
variable decoding. Every field's type, native component boundary and resource
budget is checked. Corrupt/truncated/trailing data, wrong generation, invalid
scalar/enum values and exhausted limits return no prefix. Explicit enum maps,
a complete aggregate binding and independent wire images guard schema changes.
The storage owner still needs private ownership and authentication/privacy policy:
CRC32C is accidental-corruption detection only.

All 74 Files suites pass on macOS, Linux and Windows: 586, 591 and 588 cases.
Fourteen codec cases also pass in focused sanitizers on all three hosts. They
cover RFC 3720 CRC vectors, fixed 81-byte failure and 240-byte full-metadata
images, all 253 valid single-byte POSIX components, all 2048 unpaired UTF-16
units, signed/unsigned extremes, individual metadata failures, malformed fields
with recomputed checksums, every truncation and single-bit mutation of the
failure image, context mismatch, unqualified IDs and exact independent limits.
The initial positive-name corpus accidentally included the navigation component
`.`; the codec refused it. The corpus was corrected to contain valid components;
navigation/separator/NUL rejection remains part of the native-name contract.

See [the index-record evidence](evidence/ck-files-index-record-20261024.json)
for source/SDK fingerprints, complete-suite and focused sanitizer runs, output
paths/hashes, timing and cleanup. This milestone adds no filesystem writes or
index-backed view. The private workspace/backing store, aggregate disk quotas,
external sorting and page/model integration remain required work, together with
the remaining capability/preservation and cross-platform collision rules.
WP-01 remains in progress; WP-02–08 remain unstarted.

## 2026-10-24 — bounded worker-side index record store

The directory-index codec is committed as `cacc5f7`. WP-01 now also has an
append/seal/read store over an injected exclusive scratch-file boundary. It
keeps counters and one bounded record instead of a per-row RAM offset table.
Contiguous row IDs and scoped opaque addresses prevent accidental cross-store
reads. Sealing checks exact scratch size and gates reads; it does not prove
enumeration completion or native object authority.

A noncopyable shared ledger accounts for logical wire bytes and adopted files
across live stores, including future sort runs. Per-store row/byte limits and
record limits are independent. Preflight rejection performs no data writes.
Short positive I/O advances exactly; zero progress, excessive counts, native
failures, corruption, exceptions and cancellation revoke the complete store.
Attempted write reservations remain charged after partial failure until the
backend is destroyed, before quota release. Disk allocation and physical free
space remain separate from this logical quota.

All 75 Files suites pass on macOS, Linux and Windows: 603, 608 and 605 cases.
The 17 new store cases pass under ASan+UBSan on macOS/Linux and ASan on Windows.
Review caught cancellation arriving during the final I/O call; post-call
checks and a regression case now prevent publishing that result. Final-source
full and focused runs include the correction. Tests cover shared exhaustion,
short/partial I/O, all native failure states, scoped addresses, CRC/truncation
and valid alternate-row corruption, quota release order, exceptions and lifecycle.

See [the store evidence](evidence/ck-files-index-store-20261024.json) for all
183 exported source fingerprints, exact build/test commands, SDK fingerprints,
output hashes, timings and cleanup. Diagnostic builds and documentation scratch
were removed; current verified normal test builds remain. No GUI app was launched.

The new tests use a fault-controllable byte backend. Native private scratch-file
creation, privacy/permission qualification, external sorting and its address
format, indexed pane integration and large-directory performance remain pending.
The source read-only adapter gains no mutation methods. WP-01 remains in progress;
WP-02–08 remain unstarted.

## 2026-10-24 — private native index byte files

WP-01 now implements the native byte backend for the bounded record store.
Scratch creation takes an explicit native root, creates a new exclusive object
relative to a held directory and never reopens its name. POSIX roots require
current-user ownership and private mode on an admitted filesystem; macOS also
checks ownership enforcement and extended ACL state. POSIX files are unlinked
before any data is written or an owner published, with close-on-exec descriptors.
Windows requires persistent ACLs, installs an explicit protected user/LocalSystem
DACL at creation, verifies it independently, and holds an exclusive synchronous
delete-on-close handle. Neither source mutation methods nor durability claims
are added.

The new native tests cover binary seek/overwrite/EOF, overflow rejection,
lossless record-store integration, non-ASCII root names and POSIX literal
backslashes, independent owners and cleanup, final symlink/junction refusal,
root relocation or native rename refusal, POSIX private mode/unlinked descriptors/
close-on-exec, macOS ACL/ownership-disabled refusal and Windows inherited Everyone
access that must not reach the file. All 76 Files suites pass on macOS, Linux and
Windows: 612, 615 and 612 cases. Focused fresh nine-unit sanitizers pass 9 macOS,
7 Linux and 7 Windows native cases (ASan+UBSan on macOS/Linux, ASan on Windows).

S8T has ownership enforcement disabled, so native private storage correctly
refuses it. Builds/caches remain on S8T; positive private-file fixtures use the
explicit ownership-enabled PRO-BLADE temporary root. Both roots are permitted by
the standing policy and remain empty after tests. The mount setting is unchanged.
The shared fixture helper accepts an explicitly configured root; its existing
callers retain their previous default. Windows work remains entirely on V:.

Initial Mac compilation needed the public sys/random.h declaration. The first
native ACL test then caught acl_get_fd_np's overloaded ENOENT for an absent ACL.
The backend now uses successful fstatx_np/filesec property presence and support
checks, following the existing inventory contract, rather than treating failed
queries as absent ACLs. The empty crash fixture was audited and removed. Final
full and sanitizer runs include this correction and all required positive markers.

See [the native index-file evidence](evidence/ck-files-index-file-20261024.json)
for all 187 source fingerprints, exact commands, SDK/output hashes, timing,
privacy markers and cleanup. APFS/ext4/ReFS are the positive native storage
coverage here; admitted additional filesystems still need matrix qualification.
Diagnostic builds and documentation scratch were removed; current verified normal
test builds remain. No application was launched or visual result claimed.

Workspace creation, pre-adoption file-count coordination, external sorting and
address persistence, indexed panes, native exhaustion/crash testing and broader
filesystem qualification remain required. Destination preservation/collision
capabilities remain unfinished too. WP-01 remains in progress; WP-02–08 remain
unstarted.

## 2026-10-24 — factory-only reservation and pinned index workspaces

WP-01 now reserves a shared file slot before an injected factory creates any
managed record-store file. The reservation remains held during creation, empty
size qualification and failure/exception cleanup, then transfers exactly once
to the completed store. Invalid context/limits, an already requested stop or
exhausted quota never invoke creation. Cancellation during creation/qualification
publishes no owner. File closure precedes quota release, and the live store
retains its factory through file closure and construction unwind.

The unreleased pre-created-file adoption entry point had only test consumers;
it has been removed and all store consumers now use create_new(factory,...).
This makes pre-creation reservation mandatory at the managed store boundary.
Deterministic cases cover all native failure states, malformed/null/nonempty
replies, creation/size exceptions and cancellation, close order, shared factories
and reentrant attempts while the outer factory call holds the last slot.

Native workspaces pin an explicitly selected existing root once. Every creation
revalidates privacy through that held directory, and binds to the effective user
at open (UID or a bounded copied Windows SID). POSIX creation remains tied to the
original root after pathname relocation/replacement and refuses changed private
mode until restored. Windows refuses root rename while pinned and allows it
after retirement; a root ACL changed after opening still cannot broaden the
new file's protected DACL. Factories neither create nor delete the supplied root.

All 76 Files suites pass on macOS, Linux and Windows: 623, 626 and 622 cases.
Fresh ten-unit focused sanitizers pass 37, 35 and 34 cases respectively: 25 store
cases plus 12/10/9 native cases. ASan+UBSan run on macOS/Linux (Linux leak detector
enabled); Windows uses ASan. Required native privacy/identity/lifetime markers
are present in both full and focused logs. A different-account impersonation
transition is not claimed as native coverage and remains matrix work.

Windows SSH initially exposed an empty PATH, preventing normal PowerShell/SFTP
lookup before any build started. Absolute installed tool paths and an explicit
bounded process environment restored validation; sources, builds, temporary data,
module analysis cache and logs remain on V:. Registry, SSH service configuration
and global environment were left untouched. The evidence records observations
and the documented command-shell limit supporting the diagnosis.

See [the workspace evidence](evidence/ck-files-index-workspace-20261024.json) for
187 source fingerprints, exact commands, SDK/output hashes, timing, native
markers, Windows bootstrap and cleanup. Diagnostic builds and documentation
scratch were removed; both Mac fixture roots and remote scratch roots are empty.
Current verified normal test builds remain. No GUI application was launched.

Host directory creation, raw sort-file quota integration, external sorting and
address persistence, indexed pane integration and remaining destination
preservation/collision capabilities are still required. Factories retain a root
handle outside the byte-file slot count; host controllers must bound instances
and retire their final owners on workers. WP-01 remains in progress; WP-02–08
remain unstarted.

## 2026-10-24 — bounded sequential index scans

WP-01 now scans sealed index records without retaining every row address in RAM.
Opaque same-store cursors contain only counters and a namespace, not native
owners. Bounded pages preserve records and issue checked same-store addresses;
copying a cursor supports deterministic retry. The codec's shared framing query
validates the 12-byte schema/declared bound before allocation, while complete
codec validation and contiguous row-ID checks precede emission.

Page row and encoded-byte limits are independent. A too-large first record
returns an explicit page-budget failure without revoking the healthy store, so
the same cursor can be retried with an adequate bounded limit. Later oversized
records finish the current page without skipping/truncation. Successful logical
data transfers are bounded by the page wire budget plus one lookahead header.
End requires exact stored row/offset exhaustion and a fresh native size check.
I/O, corruption, cancellation and exceptions return no page prefix and revoke the
store; previous addresses cannot keep reading through a failed generation.

Nine new scan cases cover repeatability, moved/foreign cursors, exact limits,
lookahead, retry, post-prefix corruption and cancellation, row substitution,
all failure states, truncated/trailing data and exceptions. Three framing cases
check minimum/maximum/overflow/schema boundaries and that framing alone does
not establish integrity. All 76 Files suites pass on macOS, Linux and Windows:
636, 639 and 635 cases. Fresh eleven-unit sanitizer runs pass 64, 62 and 61
focused cases (codec17, store34, native13/11/10), with ASan+UBSan on macOS/Linux
and ASan on Windows. Linux leak detection remains enabled.

A native synthetic-cache case writes and validates 100,000 records on every
host, retaining one bounded page and no full address table. Each run stores
27,888,895 bytes and scans 1,724 pages, with maxima of 59 rows and 16,347 encoded
bytes per page. Normal append/scan times were 511/327 ms on APFS, 297/354 ms on
ext4 and 771/918 ms on ReFS. Sanitizer measurements are recorded separately.
These are cache measurements with OS caching, not real-directory enumeration,
external sorting, physical-I/O/RSS bounds or full-app responsiveness qualification.

See [the scan evidence](evidence/ck-files-index-scan-20261024.json) for 187 source
fingerprints, exact commands, SDK/output hashes, scale metrics, timing and
cleanup. Both Mac fixture roots and remote scratch roots are empty. Diagnostic
builds and documentation scratch were removed; current verified normal builds
remain. Windows project work stays on V: with explicit installed tools and a
bounded process environment. No GUI app was launched or visual result claimed.

Real-directory disk-index collection, duplicate/source-change validation,
external sorting and address persistence, indexed pane integration, destination
preservation/collision rules and broader native qualification remain required.
Scan End describes the private cache rather than source enumeration, a filesystem
snapshot or native operation authority. WP-01 remains in progress; WP-02–08 remain
unstarted.


## 2026-10-24 — bounded external index sorting

WP-01 now has a worker-driven external merge sorter over sealed private indexes.
It shares the pane comparator for name/kind/size/modified ordering, folder priority
and unavailable values, adding original row IDs as a deterministic final tie.
Sorted projections retain every cached observation and original row identity,
including across re-sorts; physical addresses remain specific to each cache.

Bounded generation blocks and two-page merges use a fixed 64-level run table.
All runs share the source factory and quota ledger. No RAM fallback or partial
projection is published on failure. Cancellation/abandonment retires staging and
keeps healthy input usable; actual source corruption still revokes the source.
Native calls remain blocking. Final row and encoded-byte counts must match.
Ledger peaks are measured when reservations occur, including between advances.

Twelve sorter unit cases cover all 16 order combinations with fixed expected
permutations, exact encoded payload preservation, re-sorting, cancellation at each
advance, quotas and failure/exception paths. All 77 Files suites pass: 649 cases
on macOS, 652 on Linux and 648 on Windows. Fresh fourteen-unit sanitizer runs pass
77/75/74 focused cases, with ASan+UBSan on Mac/Linux and ASan on Windows.

A native 100,000-record reverse-order synthetic sort validates every result on
APFS/ext4/ReFS. Each uses 28,000,000 source bytes and 2,031 advances, with peaks of
seven file slots and 84,000,000 logical bytes. Normal sort times were 12,214 ms on
Mac, 4,143 ms on Linux and 11,886 ms on Windows; these are cache benchmarks, not
real-directory enumeration, UI latency or physical-memory qualification.

See [the sorter evidence](evidence/ck-files-index-sort-20261024.json) for 192
source fingerprints, commands, timing, scale metrics, output hashes and cleanup.
UI-boundary, ckVision-only and strict documentation checks pass. Diagnostic builds
and documentation scratch were removed after recording hashes; fixture roots are
empty and current verified normal builds remain. Windows work stays on V:.
No GUI application was launched.

Real-directory indexed collection, duplicate/source-change validation, disk rank
and ID lookup, indexed pane integration, destination preservation/collision rules
and broader native qualification remain required. WP-01 remains in progress;
WP-02–08 remain unstarted.


## 2026-10-24 — real-directory collection into a validated private index

WP-01 now streams native directory listings into the private index and external
sorter. An explicit cursor adapter namespace binds cached native identities even
when the directory's own ID is unavailable; default injected cursors report
Unsupported rather than inventing a namespace. Native POSIX/Windows and simulated
adapters expose their actual token, independently of native ID query success.

The collector appends bounded native batches with original ordinals, preserves
per-entry metadata/hidden-state observations, and requires native End before
sealing and sorting. A bounded name-ordered pass detects duplicate native names
across pages without a directory-sized set. Only the completed validated result
exposes the sealed canonical index, pane request, namespace and held directory.
Cancellation, quotas, enumeration/scratch errors and exceptions retire staging;
no partial listing or RAM fallback is published.

Root metadata is inspected before enumeration and after duplicate validation.
Observed identity/time/kind/namespace changes and Gone/Changed inspections reject
publication. Unavailable observations remain explicit. Matching fields do not
prove a snapshot, unchanged child contents or absence of concurrent changes.
The completed result remains worker-owned and still requires future pane
generation acceptance and fresh native acquisition before any operation.

Fifteen new unit cases cover scope binding/uncertainty, duplicate names,
cancellation at every advance, native read cancellation, root changes, foreign
IDs, metadata errors, quotas/errors/exceptions, empty input, result lifetimes,
encoding mismatch and invalid input. Two native cases index 259 real entries
with bounded pages, including an untraversed directory and final symlink/junction,
and reject a directory timestamp changed after enumeration.

All 78 Files suites pass: 666 cases on macOS, 669 on Linux and 665 on Windows.
Fresh sanitizer builds compile 58 core/platform/host/test units and pass 94/92/91
focused cases, using ASan+UBSan on Mac/Linux and ASan on Windows. The prior
100,000-record synthetic scan/sort cases still pass; the 259-real-entry case is
integration evidence, not the planned large-directory RSS/latency qualification.

See [the collector evidence](evidence/ck-files-index-listing-20261024.json) for
195 source fingerprints, commands, timing, output hashes, exact native markers
and cleanup. UI-boundary, ckVision-only and strict documentation checks pass.
Diagnostic builds and documentation scratch were removed after recording hashes;
fixture roots are empty and current verified normal builds remain. Windows work
stays on V:. No GUI app was launched.

Disk rank/ID addressing, indexed pane providers and generation/selection/acquisition
integration, remaining destination preservation/collision capabilities and broader
native qualification remain required. WP-01 remains in progress; WP-02–08 remain
unstarted.


## 2026-10-24 — disk position and original-row lookup

WP-01 now builds a private bidirectional disk lookup for a sealed complete index.
It maps zero-based positions to source records and scoped original row IDs back
to positions. Each query checks two matching fixed-width entries, including
schema, generation, role, checksum and source bounds. Bounded source pages start
at the requested position, without scanning earlier rows or keeping every
address in RAM. Cached data and row keys remain observations, not native authority.

The full 128*N logical-byte and file reservation precedes native creation.
Initialization explicitly writes at most 64 KiB per advance; later advances
check one bounded source page and both lookup entries per row. Initialized disk
ID slots detect duplicates and establish a complete original-ID permutation.
Empty input checks source End without another file. Failure/cancellation and
exceptions publish no partial lookup; healthy source caches survive projection
cancellation. Issued owners retain storage and source lifetime after builder
abandonment and must be retired on their worker.

Fifteen unit cases cover deterministic reordered positions, exact payloads,
scoped keys, quotas before creation, initialization, partial/invalid I/O,
corruption and valid-checksum field substitution, cancellation at each build
advance and during reads, unavailable native states, exceptions, source failures,
duplicate original IDs and owner retirement. The injected file fixture now
preserves existing bytes on overwrite, matching the native write-at contract.

The native 100,000-record sorted-cache test now builds a 12,800,000-byte lookup
in 588 advances. It checks nonsequential pages, 233 returned rows, reverse ID
queries and End/cleanup. Normal build times for this lookup were 818 ms on Mac,
523 ms on Linux and 1,747 ms on Windows; these are private-cache measurements,
not real-directory throughput, process RSS or application latency qualification.
The existing 259-real-entry collector integration remains exercised separately.

All 79 Files suites pass: 681 cases on macOS, 684 on Linux and 680 on Windows.
Fresh 60-unit sanitizer builds pass 109/107/106 focused cases, using ASan+UBSan
on Mac/Linux and ASan on Windows. UI-boundary, ckVision-only and strict docs
checks pass. See [the lookup evidence](evidence/ck-files-index-lookup-20261024.json)
for 199 source fingerprints, commands, timings, output hashes and native markers.
Diagnostic builds and documentation scratch were removed after hashing; fixture
roots are empty. Current verified normal builds and SDK remain for active work.
Windows work stays on V:. No GUI application was launched.

Indexed pane providers, generation/filter/selection/acquisition integration,
remaining destination preservation/collision capabilities and broader native
qualification remain required. WP-01 remains in progress; WP-02–08 remain unstarted.


## 2026-10-24 — generation-bound indexed pane preparation and pages

WP-01 now prepares ordered indexed datasets and publishes bounded display pages
through worker-owned pane state. A private completed result binds collection,
sort order, disk lookup, request, original row namespace and held directory.
Reorders retain original row keys and focus; new listings use fresh namespaces.
No directory-sized pane row/address vector is materialized.

Navigation/reorder requests and stamps reject stale, foreign, wrong-order and
wrong-predecessor results. Each reorder binds the exact current dataset revision,
so an older preparation for the same rows cannot discard newer continuity
evidence. Available directory identity/time evidence survives later denied or
unknown queries. The held directory is checked again before completion; matching
observations remain weaker than a snapshot or source-operation authority.

Page and focus requests check freshness before and after bounded I/O. Old replies
and old failures cannot replace newer request status. Failed/cancelled navigation
retains the previous dataset. Capture/focus cancellation discards its payload
without revoking healthy data; current I/O exceptions revoke before propagation.
The initialized focus candidate is committed only after successful validation;
GCC14 strict warnings remain enabled.

Immutable display pages contain escaped names, scoped IDs, metadata/error
observations, order, focus and pending/error status. They contain no native name,
cursor, lookup, store or model owner and remain readable after worker retirement,
with revoked stamps preventing later actions. Independent row/wire/text bounds
charge every retained diagnostic through the shared existing pane-page helper.
Budget failures publish no prefix, truncation or RAM fallback.

Fourteen new unit cases cover publication, stale/foreign/order/revision gates,
failed/cancelled navigation, focus across reorders, escaping and budgets,
cancellation and I/O failures, denied continuity evidence, resource retirement,
invalid inputs, empty state and request changes during I/O. The native 259-entry
case now collects, prepares, publishes and captures an 11-row page, then retires
native owners while keeping the readable revoked page. This is backend/native
integration, not a rendered Files window or responsiveness qualification.

All 80 Files suites pass: 695 cases on macOS, 698 on Linux and 694 on Windows.
Fresh 63-unit sanitizer builds pass 131/129/128 focused cases, including eight
existing pane-page cases, with ASan+UBSan on Mac/Linux and ASan on Windows.
UI-boundary, ckVision-only and strict docs checks pass. See
[the indexed pane evidence](evidence/ck-files-indexed-pane-20261024.json) for 204
source fingerprints, commands, timings, native markers, output hashes and cleanup.
Initial diagnostic builds were retired before final reruns; final sanitizer/docs
scratch was removed after hashing. Fixture roots are empty. Current verified
normal builds and SDK remain; Windows project work stays on V:. No GUI app was
launched or visual result claimed.

Indexed filtering, marking/selection inventories and native acquisition remain
required, together with remaining destination preservation/collision capabilities
and broader native qualification. Those commands are absent from this backend
API rather than partially accepted. WP-01 remains in progress; WP-02–08 remain
unstarted.


## 2026-10-24 — indexed hidden and text filtering

WP-01 now applies the full pane view through disk-backed filtering: sort order,
hidden visibility, literal escaped-display substring and ASCII case sensitivity.
In-memory and indexed views share a bounded linear-time KMP matcher. Patterns
are limited to 4,096 bytes before copying; matching checks cancellation within
long names. Unknown/denied hidden states remain visible rather than becoming
an invented hidden=false/true observation.

The lookup retains all original records and checked reverse pairs. Included
positions fill its front; excluded positions fill its tail. A valid excluded
key returns Available(nullopt), distinct from I/O failure or a foreign key.
Visible/original counts stay separate, including when nothing matches. Filtered
pages use bounded per-visible-row reads and cannot pass their logical cursors
as source-store scans. Every original ID still participates in the permutation
proof; no directory-sized RAM mask or address array is introduced.

Prepared/pending results bind every view field and the existing exact predecessor
revision. Failed/cancelled changes preserve the previous view. A hidden focus key
is retained with no visible position or selected display row; explicit hidden
focus targets reject, and clearing the filter restores the original position.
Pages charge copied filter text alongside all metadata/name diagnostics.

Four matcher, four lookup and three pane cases cover binary substring oracles,
long overlapping patterns, hidden uncertainty, included/excluded reverse lookup,
all-excluded End, checksum/reverse-pair corruption, cancellation, filter flags,
text budgets and focus restoration. Existing pane/lookup cases remain regression
coverage. The 100,000-record synthetic cache case builds 12,800,000 lookup bytes
in 588 advances with 85,714 visible rows and checks 175 nonsequential page rows.
The 259-real-entry pipeline applies an ASCII-folded filter yielding 111 matches,
checks an 11-row page and preserves original/native identity namespaces.

All 81 Files suites pass: 706 cases on macOS, 709 on Linux and 705 on Windows.
Fresh 65-unit sanitizer builds pass 142/140/139 focused cases, using ASan+UBSan
on Mac/Linux and ASan on Windows. UI-boundary, ckVision-only and strict docs
checks pass. See [the filtering evidence](evidence/ck-files-indexed-filter-20261024.json)
for 207 source fingerprints, commands, timings, native markers, output hashes and
cleanup. Diagnostic builds/docs scratch were removed after hashing; fixture
roots are empty and current verified normal builds/SDK remain. Windows project
work stays on V:. No GUI application or rendered result was claimed.

Disk-backed marking/selection inventories, native acquisition and remaining
destination preservation/collision capabilities and broader qualification remain
required. The real-directory integration and synthetic cache measurements do not
establish full-app latency/RSS or 100,000-real-entry acceptance. WP-01 remains in
progress; WP-02–08 remain unstarted.

## 2026-10-24 — transactional indexed marks and hidden-selection review

WP-01 now supports disk-backed single-row, inclusive visible-range,
mark/unmark-visible, invert-visible and explicit clear-all commands. Original
row IDs survive sorting/filtering. Bulk commands preserve excluded marks;
hidden selections expose exact total/visible/hidden counts and block operation
candidates until reviewed. Pending changes also block candidates and focus.

Each immutable snapshot uses 576 checked bytes per 4,096 original IDs, with
schema, generation, block/row/valid-bit counts, reserved bytes, CRC32C, exact
native size and zero tail-padding checks. New files reserve their full shared
quota before creation. Worker advances retain one bitmap block, with bounded
row work. Publication binds the pending stamp, exact dataset revision and prior
mark revision, performs no I/O, and cannot apply superseded results. Failed or
cancelled jobs discard staged files and preserve the published snapshot.
Corrupt marks invalidate selection instead of inventing zero counts; explicit
clear-all recovers marks without reading the damaged file.

View preparation recounts marks against its new disk lookup before final native
directory-continuity validation. Its certificate retains only a revision/count,
avoiding ownership of obsolete mark files. Reorder publication rejects a missing
or wrong mark snapshot. Bounded owner-free pages carry mark flags, copied counts,
pending state and charged diagnostics. They reject the whole page on mark I/O
failure and discard replies superseded during reads.

Fourteen new deterministic cases exercise command semantics, hidden counts,
range endpoints, invalid input/quota, cancellation boundaries, write/read errors,
short and zero I/O, corruption and CRC-valid tail padding, factory exceptions,
stale replies, recount cancellation, popcount inconsistency during changes and
recounts, and the 4,096/4,097 boundary in 4,101 records.
The real-directory pipeline marks 259 entries, filters to 111 visible/148 hidden,
inverts visible marks, marks an 11-row range, then clears and retires all native
and cache owners while copied pages remain readable.

All 81 Files suites pass: 720 cases on macOS, 723 on Linux and 719 on Windows.
Fresh 67-unit sanitizer builds pass 156/154/153 focused cases, with ASan+UBSan
on Mac/Linux and ASan on Windows. The final regression also verifies that an
aggregate popcount mismatch invalidates the snapshot itself during both changes
and view recounts. See [the marking evidence](evidence/ck-files-indexed-marks-20261024.json)
for exact source/unit fingerprints, commands, timings, native markers and cleanup.

Indexed native source acquisition/selection inventories, remaining destination
preservation/collision capabilities and broader qualification remain required.
The native/core cases do not establish a rendered Files UI, full-app latency/RSS
or 100,000-real-entry directory acceptance. WP-01 remains in progress; WP-02–08
remain unstarted. No production copy/move/delete API is introduced.

## 2026-10-24 — indexed selection acquisition into fresh held bindings

WP-01 now prepares bounded native source batches from the current indexed
cursor or visible marks. Empty, pending, invalidated and hidden-mark selections
reject before I/O. The complete selection must fit the explicit held-entry
budget (default 1,024, hard maximum 4,096), with independent native-name,
diagnostic and source-read limits. No prefix, truncation or dropped/reopened pin
can substitute for the requested selection.

Bitmap scanning retains one block and bounds original-row work per advance.
Selected IDs resolve through checked lookup and one-row source reads, then open
one native child per acquire advance using MetadataOnly/no-follow access.
Parent source namespace, qualified identities, kinds and observable directory
changes are checked around acquisition. Unknown/denied identities cannot be
confirmed away. Native failures, including required identity/kind field errors,
remain explicit when cancellation arrives concurrently.

Each child has a fresh held family and binding-ID namespace. Discovery IDs and
metadata remain separate observations; even a matching cached ID does not prove
continuity with an unheld discovery row. Every completed batch requires source
review and grants no copy/move/delete authority. Exceptions, failures,
cancellation and stale requests discard all staged children. Mark-count or
selected-but-excluded contradictions invalidate the snapshot and stamp, with
explicit clear recovery available through a fresh pane stamp.

Completed worker batches retain held children and bounded metadata without
pinning index/bitmap files. Review delivery checks the exact current stamp,
dataset/mark revision, selection origin and count. The native 259-entry pipeline
reveals the hidden selection, acquires all 159 selected objects including the
link/junction without following it, verifies fresh binding IDs and mandatory
review, and retires every index file while acquired children remain readable.

Twelve deterministic cases cover fresh IDs and matching/replaced cached IDs,
hidden/pending/count limits, private-prefix failure, each-boundary cancellation,
stale native replies, parent/child identity uncertainty, native-name/wire/text
budgets, CRC-valid count/exclusion contradictions, directory changes,
exceptions and lossless names. The failure-priority case separately exercises
whole-metadata, required-identity and required-kind denial during cancellation.

All 81 Files suites pass: 732 cases on macOS, 735 on Linux and 731 on Windows.
Fresh 68-unit sanitizer builds pass 168/166/165 focused cases, with ASan+UBSan
on Mac/Linux and ASan on Windows. Final reviewed checks include required-field
failure priority during cancellation. See [the source acquisition evidence](evidence/ck-files-indexed-sources-20261024.json)
for 214 source fingerprints, actual compiler unit sets, commands, timings,
native markers, output hashes and cleanup.

Connecting full native inventories to these held indexed bindings, destination
preservation/collision capabilities and broader qualification remain required.
The held-entry bound is per batch; application queue/result retention must also
be bounded. Larger operation batching belongs to its engine and review policy.
No rendered Files UI, full-app latency/RSS, filesystem snapshot or
100,000-real-entry directory qualification is claimed. WP-01 remains in progress;
WP-02–08 remain unstarted.

## 2026-10-24 — object inventories anchored to acquired indexed sources

WP-01 now creates `ObjectInventoryJob` through the exact retained `HeldChild`,
without opening the basename again, changing access or creating an independent
matching-ID binding. The job/result retain that source and parent; retirement
of a batch or index does not detach the inventory from its original object.
The legacy independently-owned entry constructor remains available.

Requested access is explicit through `observe_with_access` and indexed source
limits. MetadataOnly remains the default. Invalid flags reject before opening;
qualified refresh preserves the requested mask, and denial does not trigger a
downgrade/reopen workaround. Queries may use existing native rights; additional
rights must be requested at acquisition. Unsupported/Denied query states remain
visible, including full audit-security coverage when it was not requested.

Inventory results expose their exact held anchor and compare its snapshot IDs
with both fresh inventory observations. Unknown/denied identities never become
Same, and a known difference is not hidden by another unavailable observation.
This comparison does not qualify independent payload owners, unchanged kinds,
contents or basename membership. Query owners still require their own checks;
the batch's source review and operation-specific validation remain mandatory.

Real layout failures are committed as field observations before a concurrent
cancellation yield, so resume does not silently repeat them. Only an explicit
interrupted layout query retries. Native open/basic failures also remain visible
during cancellation, including the old-handle observation in a refresh.

Eight new inventory/held-access cases and one indexed-access case exercise exact
anchor retention through replacement, no implicit upgrade, identity uncertainty/
differences, mismatched query owners, pause/resume, limits, exceptions, abandonment,
explicit access preservation/refusal, invalid flags and native failure priority.
The native indexed pipeline adds an explicit-access regular-file binding, then
inventories the original object after renaming/replacing its basename. It checks
the binary xattr on Mac/Linux or alternate-stream catalog on Windows, preserves
the original identity/size, and keeps the inventory readable after index/cache
retirement.

All 81 Files suites pass: 741 cases on macOS, 744 on Linux and 740 on Windows.
Fresh 69-unit sanitizer builds, now including the 33-case object-inventory suite,
pass 202/200/199 focused cases with ASan+UBSan on Mac/Linux and ASan on Windows.
See [the held inventory evidence](evidence/ck-files-held-inventory-20261024.json)
for 214 source fingerprints, actual compiler inputs, native feature markers,
timings, output hashes and cleanup.

Destination preservation/collision capabilities, remaining composition and broader
filesystem qualification remain required. These are bounded query observations,
not complete preservation coverage, a filesystem snapshot, metadata-loss consent
or copy/move/delete authority. Global worker/result retention still needs bounds
in the application. WP-01 remains in progress; WP-02–08 remain unstarted.

## 2026-10-24 — qualified destination directory observations

WP-01 now derives `DestinationDirectoryAssessment` from an exact completed native
directory inventory. It performs no additional I/O, probe, pathname reopening or
write. Twelve native feature advertisements, read-only mount observations and
recorded AddFile/AddDirectory access checks remain distinct. Filesystem labels
never supply inferred capabilities. Advertised/NotAdvertised describe native
reports, not preservation guarantees or permission to execute an operation.

The assessment qualifies directory identity/kind, query owners, native family,
identity scheme and feature domain. Contradictory available location fields,
mount/case/inode flags or directory timestamps reject context. Raw native feature
refusals remain visible even when a feature mapping is absent. Creation checks
validate the requested native rights and owner while preserving access failures;
read-only state remains separately visible. Neither a successful access query nor
a non-read-only mount settles names, collisions, flags, quota, inheritance,
providers or metadata preservation.

All newly retained diagnostics have an explicit budget. Exhaustion publishes no
partial report. Const getters allocate nothing; the result retains its exact
inventory and native directory until worker retirement. Nine deterministic cases
cover native mappings, qualification contradictions, error priority, access masks,
read-only state, budgets and invalid requests. The existing native 259-entry
pipeline now checks all twelve derived advertisements and both access observations
against actual native queries on APFS, ext4 and ReFS.

All 82 normal Files suites pass: 750 cases on macOS, 753 on Linux and 749 on
Windows. Fresh 71-unit sanitizer builds pass 211/209/208 focused cases with
ASan+UBSan on Mac/Linux and ASan on Windows. See [the destination assessment
evidence](evidence/ck-files-destination-assessment-20261024.json) for 217 source
fingerprints, compiler inputs, native markers, commands, timings, output hashes
and cleanup. All diagnostic builds were retired after process and fixture checks.
WP-01 remains in progress; WP-02–08 remain unstarted. Full preservation/collision
capabilities, operation-specific validation, scoped loss consent and broader
filesystem qualification remain required. No Files UI or copy/move/delete API
is introduced by this assessment.

## 2026-10-24 — WP-01 acceptance

WP-01 is complete after a requirement-by-requirement review of the native model,
inventory services, destination inspection, names/links/collision evidence and
pane selection semantics. The review checks the 217 current exported inputs
against the successful three-host snapshot and matches every registered suite
to its executed cases. All 82 suites pass per host: 750/753/749 cases on
macOS/Linux/Windows. The prior sanitizer results remain applicable to unchanged
source bytes: 211/209/208 focused cases.

The audit found one declared case excluded from normal configurations: the Linux
native casefold gate requires its own enabled filesystem. A fresh strict-warning
64-unit ASan+UBSan build against current sources passed all 45 focused cases,
including required native casefold activation and lookup. This closes coverage
of all 790 declared cases across the host/configuration union. The temporary ext4
image was unmounted, its loop attachment audited and diagnostic workspace removed.
The 239.856-second compile is consistent with the nearby 304.193-second 71-unit
Linux sanitizer build; actual current compiler inputs and native test markers
establish freshness independently of elapsed time.

See the [acceptance review](ck-files-wp01-acceptance.md) and
[acceptance evidence](evidence/ck-files-wp01-acceptance-20261024.json). Native
inspection completion does not prove preservation or authorize mutation. Unknown
name policies and feature mappings remain explicit; broader filesystem coverage
remains part of WP-07. No first-release requirement was waived.

WP-02 is next: the real read-only Files application and its asynchronous services,
navigation/completion, bounded views, properties/preview and interactive tests.
Large indexed-view refresh still needs application-level qualified selection or
explicit invalidation behavior. Recursive preservation planning and scoped consent
remain WP-03; copy/move/delete/recovery and release acceptance remain WP-04–08.

## 2026-10-24 — WP-02 bounded pane presentation

WP-02 has started with `FilesPaneGridModel`, a UI-thread implementation of the
public ckVision `CellGridModel` API. It displays either sealed bounded-pane or
indexed-pane pages while keeping native resources on the worker. The viewport
is bounded, uncached rows show Loading, and row targets require an actual cached
row plus a current revocable stamp. Positions never become native targets.

The grid presents escaped names, worker-supplied marks, exact unsigned byte sizes,
object kind and UTC modification time. Narrow widths progressively retain the
most useful columns. Wheel scrolling retains its origin across viewport reads
and page delivery; following worker focus is explicit. Stale pages remain
readable but cannot produce row targets. Provider calls perform no native I/O or
service scheduling, and range-mark semantics remain for controller integration.

Eleven tests cover escaped/control names, unknown metadata, uncached navigation,
wheel/cursor independence, stale/null pages, geometry and integer limits, UTC
calendar boundaries, every object kind, explicit worker focus, marks, empty views,
native indexed-cache retirement and two real ckVision grid views inside the suite
shell/window/splitter. The headless frame is checked for actual name/header text.
All 83 normal Files suites pass on Mac/Linux/Windows (761/764/760 cases).
Fresh matching sanitizer builds compile 63 product units plus an independently
installed instrumented ckVision 1.4.0 SDK (124 SDK units on Mac/Linux, 128 on
Windows). All eleven pane cases pass under ASan+UBSan on Mac/Linux and ASan on
Windows. The initial Mac run against an uninstrumented SDK reported a container
error in mixed instrumented/uninstrumented code; it is excluded from accepted
evidence. That report did not recur with the unchanged product sources and
fully instrumented SDK. Temporary Windows harness
path/PATH issues were corrected before its accepted run. No finding was suppressed.

See [the pane-grid evidence](evidence/ck-files-pane-grid-20261024.json) for 227
source/config fingerprints, actual compiler unit sets/flags, SDK install hashes,
normal and sanitizer results, timings, failure investigation and output cleanup.
Diagnostic SDK/build trees were retired; verified normal builds remain available.

The full Files application, async service/controller, bounded request/result
queues, navigation/completion, properties/preview, histories/bookmarks, large-view
refresh and interactive terminal acceptance remain WP-02 work. No production
Files executable or mutation command is introduced by this component.

## 2026-10-24 — WP-02 asynchronous pane worker

`FilesPaneService` now runs native discovery, indexing and display-page capture on
one serialized worker per pane. Latest navigation/page slots and one unread reply
bound the transport. Exact opaque tickets reject foreign and stale work; a new
navigation cancels/coalesces older requests without joining the worker. Wakeups
are coalesced until the receiver takes the reply.

Navigation stages a separate pane and its entire requested first viewport before
committing. Failure, cancellation or page-resource limits preserve the previous
pane and active address. The requested address stays separately available for an
error message. Native errors take priority over concurrent cancellation, and
short wire-budget reads become explicit limit failures rather than partial UI
success. All native handles, scratch files and final owner retirement stay on
the worker. Shutdown requests remain nonblocking while an OS call is blocked;
joining belongs outside UI dispatch.

Seventeen new tests cover real native source/cache integration, exact worker
thread ownership, blocked-call supersession (100 queued navigations), page-read
coalescing, retained old views, native-error/cancellation priority, foreign
service/stamp/range rejection, coalesced wakeups, stop/join, allocation/exception
poisoning, failed notification, workspace refusal and path/page/text/wire/error
limits. The new wire-budget regression requires a complete requested viewport.

All 83 Files suites pass: 778 cases on Mac, 781 on Linux and 777 on Windows.
Fresh matching sanitizer configurations compile 65 product units and the installed
instrumented ckVision 1.4.0 SDK (124/124/128 SDK units). All 28 pane cases pass
under ASan+UBSan on Mac/Linux and ASan on Windows. Separate UI-free 62-unit
ThreadSanitizer builds pass the 17 service cases three times on both Mac/Linux
(51 executions per host). This checks the service, not a complete application.

See [the pane-service evidence](evidence/ck-files-pane-service-20261024.json) for
230 current source/config fingerprints, exact compiler sets/flags, SDK/output
hashes, timing, barrier-controlled concurrency results and diagnostic cleanup.
Earlier preliminary runs predate the wire-budget regression and are excluded
from the final qualification. All superseded diagnostic builds and SDKs are
retired; the current verified normal builds remain available for active work.

WP-02 remains in progress. The two-pane application controller, bounded UI
publication, focus/mark/view commands, navigation/completion, properties/preview,
histories/bookmarks, qualified refresh and interactive terminal acceptance are
still required. No production Files executable or mutation command is enabled.

## 2026-10-24 — WP-02 two-pane application controller

`FilesApp` composes the suite shell, real splitter and two independent pane
services. Each pane keeps one bounded display page. Worker notifications share
one coalesced, payload-free UI post; exact navigation/request tickets and model
stamps gate publication. A watchdog runs only while reads are pending. Paint and
provider callbacks perform no native I/O or worker joins.

The application displays active and requested locations separately, preserves
the previous listing on failed navigation, requests uncached viewport pages and
keeps the other pane usable while a read is blocked. Narrow layouts show the
active pane with an explicit switch command; both pane models survive resizing.
The initial divider is established from the actual window geometry and later
resizes preserve its position. All five suite themes are exercised.

Quit and window close use the same asynchronous shutdown decision. While reads
are running, Enter chooses Stay and Escape dismisses without exiting. The
explicit exit choice requests worker stop and returns to the host; the host
joins outside dispatch after controller callbacks have been withdrawn. Weak
posts survive controller retirement safely, and the host Quit handler is
restored after desktop teardown.

Nine application tests exercise native source/cache integration, all themes,
narrow/wide resizing, independent panes, keyboard switching and uncached paging,
failed and rapidly superseded navigation, coalesced/late worker notifications,
blocked-read cancellation and explicit shutdown decisions. All 83 Files suites
pass: 787 cases on Mac, 790 on Linux and 786 on Windows. The keymap tests pass
on all three hosts. Fresh matched sanitizer builds compile 71 product units
and installed instrumented ckVision 1.4.0 SDKs (124/124/128 SDK units); all 37
UI/service cases pass under ASan+UBSan on Mac/Linux and ASan on Windows.

See [the controller evidence](evidence/ck-files-app-controller-20261024.json)
for 258 source/config fingerprints, exact compiler sets and sanitizer flags,
SDK/output hashes, timing, failure investigation and diagnostic cleanup. No
actual application window has been launched for this component milestone;
headless view tests do not establish interactive terminal acceptance.

WP-02 remains open. Navigation input/completion and child traversal, qualified
refresh with stable selection, marking/view commands, properties/preview,
histories/bookmarks, mouse acceptance and a production executable with actual
interactive terminal validation are still required. No mutation command is
enabled.

## 2026-10-24 — WP-02 acknowledged pane-state commands

The pane service now accepts focus, mark and view-change commands on its owning
worker. These commands use one acknowledged slot: another change or page read
receives Busy while work or an unread result is outstanding. Navigation may
supersede an accepted change; accepted mark commands are never silently replaced
by page reads or other mark commands. Exact navigation tickets and current model
stamps gate execution, and row IDs retain their original dataset scope.

Marking uses the core's copy-on-write disk bitmap jobs. Reorder/filter changes
reuse the retained indexed dataset and preserve row identities, focus and marks;
they do not rescan a path. Hidden marks remain explicit and block operation
candidates until reviewed or cleared. Cancellation of pending preparation keeps
the previous committed state, while native failures retain their exact error
even when cancellation races them.

Replies distinguish NotApplied, Applied and Unknown outcomes independently of
page delivery. A committed change whose viewport exceeds a display budget
remains Applied and can be read back with a smaller page. A worker fault during
acknowledgment reports Unknown, never an invented rollback. Failed commands can
carry a current recovery page of the retained state without becoming apparent
successes. These are local pane-state operations; they do not mutate source
files.

Eleven additional service tests cover retained focus/row IDs and marks through
reorder, filtering and hidden marks, clear/invert/range marking, stale and foreign
targets, exclusive acknowledgments, blocked cancellation and supersession,
input bounds, native-error precedence, display limits after commitment and
notification failure. All 83 Files suites pass: 798 cases on Mac, 801 on Linux
and 797 on Windows. All 48 UI/service cases pass under matched ASan+UBSan on
Mac/Linux and ASan on Windows, using 71 product units and instrumented installed
ckVision 1.4.0 SDKs. UI-free ThreadSanitizer builds run all 28 service cases three
times on each Mac/Linux host (84 case executions per host), without findings.

See [the pane-command evidence](evidence/ck-files-pane-changes-20261024.json)
for source/compiler/SDK/output fingerprints, exact test sets, timing and cleanup.
Diagnostic builds and fixture data have been retired; verified normal builds
remain available for active work. No GUI application was launched at this
component milestone.

WP-02 remains open. The application still needs to bind these worker commands
to its focus/mark/view controls, as well as implement the remaining navigation,
refresh, properties/preview, history/bookmark and interactive acceptance work.

## 2026-10-24 — WP-02 pane mark and view controls

The application now exposes mark toggling, visible mark/unmark/invert, explicit
range start and mark/unmark through cursor, and clearing all marks including
hidden ones. Insert toggles the current row; the Marks menu and status line use
the same commands. Range endpoints carry scoped row IDs, including when their
display pages are no longer cached. Marks and hidden-mark review counts update
only after worker acknowledgment.

View options provides typed sort, descending order, folders-first and hidden
settings plus a literal escaped-name substring filter and explicit ASCII case
matching. Its nonblocking ckVision descriptor dialog validates the filter bound,
retains usable buttons after narrow resizing, and refuses results from an
obsolete navigation context. Cancellation leaves the current view unchanged;
Quit dismisses pending options before following the normal close protocol.

Before reordering or filtering, the controller fetches an off-screen uncached
cursor row when necessary, acknowledges its scoped row ID on the worker, then
submits one bounded queued view intent. An already acknowledged worker focus
does not require reconstructing an uncached row ID. Cursor
movement pauses in that pane until the acknowledged change finishes; the other
pane remains usable. Cancelling between the focus acknowledgment and view
submission withdraws the queued view. Commands are scoped to the pane window
and are unavailable from unrelated inputs or dialogs.

Applied, not-applied and unknown acknowledgments remain distinct from page
delivery. Recovery pages do not erase change failures. A committed mark update
that exceeds a display budget remains explicit and can recover after resizing;
an unknown acknowledgment disables further changes in that pane while the other
pane remains usable. No source filesystem mutation is enabled.

Twelve additional application tests cover real menu/keyboard/pointer routing,
range marking across uncached pages, hidden marks, all themes, focus retention
through reorder, dialog validation/resizing/cancellation/obsolete contexts,
acknowledgment failures, off-screen uncached cursor resolution and the
inter-stage cancellation race. All 83 Files suites pass: 810 cases on macOS,
813 on Linux and 809 on Windows, plus the keymap suite on each platform.
All 60 UI/service cases pass matched ASan+UBSan on macOS/Linux and ASan on
Windows, with all 71 product units and pinned ckVision 1.4.0 SDK units
(124/124/128 respectively) instrumented.

See [the pane-control evidence](evidence/ck-files-pane-controls-20261024.json)
for final source, compiler, SDK and output fingerprints, exact tests, timings
and cleanup. Diagnostic builds and fixture data have been retired; normal
builds remain for active work. No GUI application was launched.

WP-02 remains open for navigation/completion and child traversal, qualified
refresh with stable selection, properties/preview, histories/bookmarks, a
production executable and actual interactive terminal acceptance.


## 2026-10-24 — WP-02 indexed child-directory binding

The indexed pane now opens one explicit visible row as an independently owning
child enumeration cursor on the worker. It reads the exact native component
from the bounded disk index, checks the held parent, and retains a fresh child
handle while opening the no-follow enumeration cursor relative to that parent.
Live identities and directory kinds must agree; the retained child and parent
are checked again before delivery. Marks and cursor position cannot retarget
this request. Stale, excluded, foreign, pending, link/reparse and unqualified
rows refuse without a partial result.

Matching cached IDs are only a contradiction check: the listing did not hold
those children and cannot prove discovery continuity or exclude earlier ID
reuse. The new result binds the current live directory; it neither authorizes
mutation nor promises snapshot, provider or mount-boundary semantics. Native
owners stay on the worker, including failure/cancellation retirement. Source
names remain exact POSIX bytes or UTF-16 units. Index I/O failure revokes issued
pane stamps, and explicit wire/name/diagnostic limits never truncate a target.

Nine new core tests cover target/state validation, mark independence, fresh and
retained-object identity contradictions, parent changes/scope uncertainty,
cancellation with native-error precedence, exact raw-byte/UTF-16 names,
reentrant supersession, bounds and index I/O failure/exception retirement.
One new native test verifies no-follow refusal and an independently owned
cursor that continues enumerating the original directory after its old name
is reused for a replacement. All 83 Files suites pass: 820 cases on macOS,
823 on Linux and 819 on Windows, plus keymap on each platform. Matched
ASan+UBSan macOS/Linux and ASan Windows runs pass all 130/128/127 cases:
60 UI/service, 53 indexed-pane and 17/15/14 platform-specific native-index cases.
All 73 product units and installed instrumented ckVision 1.4.0 SDK units
(124/124/128) are verified against the final source fingerprints.

See [the child-directory binding evidence](evidence/ck-files-directory-row-20261024.json)
for exact cases, compiled inputs, SDK/output hashes, timings and cleanup.
Diagnostic builds and fixture data have been retired; normal builds remain
for active work. Both architecture gates and strict documentation build pass.

This is the worker-domain foundation for child navigation. Submission and
acknowledgment through the pane service, exact address/history presentation and
application commands remain to be integrated. WP-02 remains open, including
completion, qualified refresh, properties/preview, histories/bookmarks and
production executable/interactive acceptance. No GUI application was launched.


## 2026-10-24 — WP-02 child navigation service

The pane service now accepts an exclusive child-directory request tied to an
exact navigation ticket, current pane stamp and explicit visible row ID.
The worker binds the live directory relative to its held parent, then retains
that same cursor through listing, sort/index preparation and first-page capture.
Only a complete renderable candidate commits. Marks cannot retarget a child
request; failure/cancellation retains the previous pane, marks and route.
Absolute navigation supersedes blocked child work without publishing an old
result. Native errors take precedence over concurrent cancellation, and stop
remains nonblocking while a native call is blocked.

Location replies now carry an exact absolute root plus native child components.
This keeps POSIX bytes and Windows literal components separate from flat path
interpretation. Presentation stays escaped and owner-free; it never becomes an
operation target or proof of continuing pathname membership. Byte/depth limits
are explicit, and child-route data is supplied only after resolving the scoped
row. Existing application location labels consume this structured value.

Twelve new service/location tests cover exact native route values, nested entry,
marks and failure retention, exclusivity, stale contexts, first-page and route
limits, replacement races, cancellation/error precedence, supersession, faults,
blocked stopping and worker-thread native-owner retirement. A Windows run
exposed an acknowledgment race: old pane stamps were revoked only after the
new reply could be consumed. Old/staged pane retirement now finishes outside
the commit mutex before publication; a gated regression proves that a reply
cannot escape while old native-owner retirement is blocked. A failed initial
navigation also cannot cause a child request to retry native backend startup:
the origin is now checked first, and the new regression requires zero additional
workspace/source queries. The existing native
backend test also enters a real child directory. All 83 Files suites pass:
832 cases on macOS, 835 on Linux and 831 on Windows, plus keymap on each.
Matched ASan+UBSan macOS/Linux and ASan Windows runs pass 142/140/139 cases:
72 UI/service, 53 indexed-pane and 17/15/14 platform-specific native-index cases.
All 73 product units and installed instrumented ckVision 1.4.0 SDK units
(124/124/128) match the final fingerprints. UI-free TSan runs all 40 service
cases three times on each macOS/Linux host (120 executions each), without findings.

See [the child-navigation service evidence](evidence/ck-files-child-service-20261024.json)
for exact inputs, compiler/SDK/output identities, tests, timings and cleanup.
The first Windows instrumented native stress run exceeded its 180-second
harness guard during the unchanged 100000-row external sort. Its process exit,
empty orphan and diagnostic retirement were verified; full fresh qualification
uses a 600-second native-suite guard without skipping cases or weakening data,
wire, storage or advance bounds. The final native suite completes in about
213 seconds. Pre-fix service runs are excluded from final qualification.
Diagnostic builds and fixture data have been retired; current normal builds
remain for active work. Both architecture gates and strict documentation pass.

The application still needs its child-entry command, navigation input/completion,
qualified refresh, properties/preview, histories/bookmarks and production
executable/interactive acceptance. WP-02 remains open. No source filesystem
mutation or GUI application launch is introduced by this milestone.


## 2026-10-24 — WP-02 child-entry application controls

Enter folder now shares one scoped suite command across the Location menu,
status line, current keymap and native grid double-click. Keyboard activation
passes through the suite registry, so remapping/unbinding takes effect and F2
does not implicitly enter a folder. File-list Home/End now move to first/last
records. Cached directory kind controls enablement; only a stamped row ID is
submitted, and the worker still performs every native identity/kind check.
No file is automatically executed and no displayed name is parsed as a target.

Opening freezes movement only in that pane. Previous route/listing/marks/cursor
and range anchor stay visible until acknowledgment; the other pane, rendering,
resize and cancellation remain usable. Success adopts the worker route/focus,
clears old marks/range and the name filter, and retains sort/visibility policy.
Failure/cancellation preserves the previous dataset. Late cancellation explicitly
reports an already opened location. Absolute navigation supersedes blocked entry;
unknown notification outcome blocks just the affected pane. The controller now
exposes immutable last-acknowledged route data and withdraws activation callbacks
before teardown.

Eleven new application cases cover all themes, menu/keyboard/double-click
routing, runtime keymap changes, F2, modal/unrelated contexts, failed-entry
retention, blocked cancellation, completed-before-cancel ordering, narrow layout,
supersession, notification failure, uncached Home/End and exact native names.
The real filesystem tests enter escaped POSIX names and a Windows literal
trailing-dot/space directory through the UI command without flattening its route.
All 83 Files suites pass on macOS/Linux/Windows (843/846/842 cases), plus
the keymap suite. Matched ASan+UBSan on macOS/Linux and ASan on Windows pass
153/151/150 cases: 83 UI/service, 53 indexed-pane and 17/15/14 native-index.
Qualification compiles all 73 product units and 124/124/128 pinned SDK units
with instrumentation. Diagnostic builds and fixtures have been retired; current
normal builds remain for active work. The unchanged service/native protocol
retains the prior milestone’s TSan evidence; this does not qualify the changed
UI controller/grid or new test helper as a whole. Both architecture checks pass.
See [qualification evidence](evidence/ck-files-child-ui-20261024.json).

WP-02 remains open for navigation input/completion, parent/home/root navigation,
qualified refresh, properties/preview, histories/bookmarks and production
executable/interactive acceptance. No source filesystem mutation or production
GUI application launch is introduced by this component milestone.
