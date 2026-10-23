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
