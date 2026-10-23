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
