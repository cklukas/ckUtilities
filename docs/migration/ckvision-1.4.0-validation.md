# ckVision 1.4.0 validation

Status: SDK migration validated, 2026-10-23. This completes WP-00 of the Files implementation plan in
`implementation/ck-files-plan.md`.

## Dependency provenance

Latest published release rechecked through GitHub: v1.4.0 (2026-10-22).
Annotated tag commit: `a791f103c3bc67f9fe398cf5c5010f7c460ae7ba`.
Source archive SHA-256:
`50ff61b347fd3a514a798c2879fac86613fee07d4271c6434e808606d4d91831`.
The independent source checkout resolves to the same commit. CI, release,
Homebrew and documentation capture pins move together; CMake now requires
ckVision 1.4.0 or newer. Historical validation documents remain unchanged.

## Workspaces and freshness

Mac task root: `/Volumes/S8T/tmp/ckutilities-files-20261023-01a165ae`.
All local subprocesses use its explicit `tmp` directory as `TMPDIR`. No prior
ckUtilities build tree or running product process was found in the current
workspace; unrelated project/ckVision task trees are not cleanup targets.
No GUI product has been launched for this change.

SDK: clean Release/Ninja build, Apple Clang 21, examples/tests/fuzzers disabled,
four parallel jobs, installed at `<task-root>/sdk`. The build compiled 128
steps, including the static library and SDK diagnostic tools. Start/end UTC:
2026-10-22 22:25:17.480 → 22:27:04.611; elapsed 107.131 seconds. These are
2026-10-23 00:25:17.480 → 00:27:04.611 Europe/Berlin. No comparable elapsed
baseline exists in the previous checked-in validation records; this run is the
first measured baseline for this configuration. Timing alone is not freshness
evidence; source revision/archive identity and compiled-input logs establish it.

The initial suite source snapshot shared with Windows/Linux has tree SHA-256
`40dc9b2bd75c0ee732e379615fa06102864b85feefafc4a0887a8b61615dd0bd`.
The tree digest is SHA-256 over sorted relative-path records, each containing
an eight-byte big-endian path length, native path bytes and SHA-256 file bytes.
It includes tracked and nonignored new files at snapshot time, including the
Files specification. Later documentation-only evidence edits are separate.
Exact commands, step timestamps, input fingerprints and logs are in the active
task workspace; concise final results will be retained here before cleanup.

## Gates

| Gate | Current result |
| --- | --- |
| macOS installed SDK build/install | Passed; revision and archive hash above |
| UI architecture boundaries | Passed (24 domain files; 352 active product files) |
| macOS Debug suite build, CTest, package consumer, archive and PTY | Passed; 116/116 tests |
| macOS Release/documentation capture validation | Passed; 116/116 tests, SDK consumer, archive and PTY |
| Windows Release build, CTest, ConPTY and ZIP | Passed; 115/115 tests and installed SDK consumer, under `V:\tmp\ckutilities-files-20261023-01a165ae` |
| Linux Release build, CTest, packages and PTY | Passed in Lima Debian VM; 115/115 tests |
| Documentation regeneration and repeatability | Passed; 50 screenshots/goldens identical on macOS/Linux and after a second generation |
| Sanitizers | Not yet run |
| GitHub Actions | Not dispatched |

The source specification `implementation/ck-files-contracts.md` records the
reviewed public SDK APIs and the product-owned discovery/mutation boundaries.
Files remains unimplemented and no mutation capability is enabled by this
dependency change.

## Initial suite runs

macOS Debug/Ninja, four build jobs: 2026-10-22 22:29:08.050 →
22:32:47.285 UTC (2026-10-23 00:29:08.050 → 00:32:47.285 Berlin),
219.236 seconds, 256 steps, output `<task-root>/build/bin`. CTest passed
116/116 in 5.67 seconds. The rehearsal passed (29.674 seconds), including
the independent SDK consumer, staged install/cutover, extracted archive and
another complete CTest run. The separate PTY gate passed (28.443 seconds).
The pinned llama.cpp and GoogleTest produce compiler deprecation/conversion
diagnostics with Apple Clang 21; the linker also reports duplicate static
library entries from existing target composition. These are recorded rather
than suppressed or represented as a zero-warning run.

Linux: Lima Debian 13 aarch64, GCC 14.2.0, Release/Ninja, four jobs, local guest
root `/home/klukas.guest/build/ckutilities-files-20261023-01a165ae`.
SDK build: 2026-10-22 22:30:36.214 → 22:33:33.324 UTC, 177.236 seconds.
Suite build: 22:34:22.765 → 22:36:36.054 UTC, 133.290 seconds, no compiler
warnings. CTest passed 115/115 (0.619 seconds); rehearsal passed (11.068 seconds),
including SDK consumer, install/cutover, archive and DEB/RPM layout verification.
Separate PTY gate passed (14.352 seconds). The real application documentation
captures and Todo/Sysmon golden generation completed successfully. Cross-host
byte comparison with the macOS captures passed for every generated file.

Windows development initially started on `D:` before the user's `V:` rule was
established. Its exact owned process tree was terminated. All 5601 source/SDK
files, including Git metadata, were hash-verified after copying to `V:`; concise
aborted-build evidence was retained on `V:`, and the complete old task root
was removed. The validation restarted with clean build paths and explicit
`TEMP`, `TMP` and `TMPDIR` on `V:`. No D-path build cache is reused.

## Final Release evidence

The macOS capture script reconfigured the existing build tree to Release;
there is no retained superseded Debug build directory. The capture-target
Release build took 86.751 seconds (151 steps). A second generation took
9.010 seconds with identical results. The full Release build then compiled
the remaining 104 steps in 37.930 seconds, finishing at 2026-10-22
22:40:23.309 UTC (2026-10-23 00:40:23.309 Berlin). These are distinct target
sets, not comparable clean-build durations. The log verifies all required
inputs compiled or reused valid outputs. Final `ninja -n` reports no work.

Release rehearsal passed in 16.804 seconds, including 116/116 tests, installed
SDK consumer, install/cutover and archive checks. Separate Release PTY passed
in 32.204 seconds. `ck-todo --version` and `ck-sysmon --version` both report
`0.2.0 (ckVision 1.4.0)`. Executable hashes/timestamps were recorded after the
full Release build. The changed JSON screenshot was rendered and inspected;
only the framework version digit changes. No interactive visual-fix claim is
made from this documentation render.

Windows 11 build 26300 x64, MSVC 19.44.35228, Release/static CRT: clean V-drive
SDK and suite validation ran from 2026-10-22 22:37:32.068 to 22:42:32.522 UTC
(2026-10-23 00:37:32.068 to 00:42:32.522 Berlin), 300.454 seconds. The SDK
compiled 130 steps and the suite 234. All 115 tests passed, including the
ConPTY contract, and the extracted ZIP started all nine tools successfully.
Compiler conversion diagnostics from the existing dependencies/AI targets are
retained in the build log; this is not claimed as a zero-warning build.

The Windows script now also runs `verify_ckvision_package`. Its independent
consumer inherits the parent generator, optional platform/toolset and MSVC
runtime, preventing a Ninja/static-CRT SDK check from silently using another
generator or the dynamic CRT. The updated script passed end to end on the
existing verified tree (31.617 seconds including transfer): SDK/suite no-op
builds, consumer 1/1, suite 115/115, ConPTY and ZIP. Cache assertions verify
Ninja and `MultiThreaded` in the independent consumer. The revised consumer
gate also passed on macOS and Linux. macOS took 72.112 seconds for that gate:
the log shows a 40.1-second parent CMake regeneration, 10.7-second fresh external
configuration and two compilation/link steps, not skipped compilation. Host
load was 224 during the follow-up; the duration was investigated rather than
treated as freshness evidence.

Homebrew formula generation and Ruby syntax validation passed using fixture
release arguments; the SDK resource hash is the actual downloaded archive hash.
Shell syntax, architecture boundaries, strict documentation checks and
`git diff --check` passed. GitHub Actions and sanitizers were not run for this
SDK integration change. Files native operation qualification remains work
for subsequent packages; these suite checks do not establish preservation or
mutation safety for an unimplemented file manager.

## macOS Release fingerprints

Exact executable directory: `/Volumes/S8T/tmp/ckutilities-files-20261023-01a165ae/build/bin`.
SDK tracked-source tree SHA-256: `35a8e3dbdbe63c96d1dfffe459d3c15ea21c08e87d53cf98252e3f9a2cc5424b`.
Installed SDK `lib/libcvision.a`: `c481288ba9a83b812d2697fd926e69d8ccbf46bacdd603b1c7bb19101efce166`.

| Executable | SHA-256 |
| --- | --- |
| ck-chat | `21bf5b2e6b968b8ee2410a23af40cee04e7e525def9a76f4d1e3102bcb7c4b6b` |
| ck-config | `0d33d3deb446ad1e976df80d1091b9fcaea6b57c504da43953f454e12e4061b7` |
| ck-du | `facf8afe8bb46612090ac7b66a46060bf7224bd91728643058fdf06e3d3ab7cc` |
| ck-edit | `b4a5fd7ad9485ab95ede2af804454f519228669ddaf1d927c1971d27ac68981f` |
| ck-find | `f79d733d8034cf913708c9a112f13414b6d0de63ff19a5ce3bd45b5eb8629a31` |
| ck-json-view | `bc8059cb2d1070a425b700751cc71eb86b20e934821c73c2458d04d9e8bfaa63` |
| ck-sysmon | `16933c575ea3bc9b19d6fa1d0838f966be30b9717cee4426ec572e19f04b31be` |
| ck-todo | `194b79a0a37a32fc81cab4c85a683735563cbd894ed980dce3fe4fba68af1e0e` |
| ck-utilities | `d5b9acc939a1077f828d84f18e98eb336f45ea134d416b42b5e6675573ff7560` |
