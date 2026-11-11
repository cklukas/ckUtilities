# Building from source

ckUtilities is a ckVision-native C++20 suite. Every build requires an
installed `ckvision::cvision` CMake package; a neighbouring ckVision source
checkout is never used as a dependency.

The headless filesystem backend is a separate sibling project:
[`ck-core-fs-lib`](https://github.com/cklukas/ck-core-fs-lib). A normal local
checkout is expected at `../ck-core-fs-lib`; override it explicitly with
`-DCK_CORE_FS_LIB_SOURCE_DIR=/path/to/ck-core-fs-lib`. The ckVision frontend
stays in this repository and links the backend's `ck_files_core` and
`ck_files_platform` targets.

## Build

Provide the installed SDK prefix to CMake:

```sh
cmake --preset dev \
  -DCMAKE_PREFIX_PATH=/path/to/ckvision-sdk \
  -DCKTOOLS_CKVISION_PREFIX=/path/to/ckvision-sdk
cmake --build build/dev
ctest --test-dir build/dev --output-on-failure
```

The produced commands are `ck-utilities`, `ck-json-view`, `ck-find`, `ck-du`,
`ck-files`, `ck-config`, `ck-edit`, `ck-chat`, `ck-todo`, and `ck-sysmon`.

## Release rehearsal

Configure a clean Release test tree and a separate production package tree.
The rehearsal runs the complete Release CTest suite in the test tree, then
checks the staged and packaged production executables.

```sh
cmake --preset pkg -B build/pkg-test -DBUILD_TESTING:BOOL=ON \
  -DCMAKE_PREFIX_PATH=/path/to/ckvision-sdk \
  -DCKTOOLS_CKVISION_PREFIX=/path/to/ckvision-sdk
cmake --build build/pkg-test
cmake --preset pkg \
  -DCMAKE_PREFIX_PATH=/path/to/ckvision-sdk \
  -DCKTOOLS_CKVISION_PREFIX=/path/to/ckvision-sdk \
  -DCKTOOLS_REHEARSAL_TEST_BINARY_DIR="$PWD/build/pkg-test" \
  -DCKTOOLS_VERIFY_CKVISION_PACKAGE=ON \
  -DCKTOOLS_VERIFY_CKVISION_INSTALL=ON \
  -DCKTOOLS_VERIFY_CKVISION_ARCHIVE=ON \
  -DCKTOOLS_VERIFY_CKVISION_TERMINAL=ON
cmake --build build/pkg --target verify_ckvision_rehearsal
cmake --build build/pkg --target verify_ckvision_terminal
```

The two commands are intentionally separate because both stage the product
into the same disposable prefix. The production preset disables backend test
hooks. The archive verifier rejects UI-runtime and test-framework payloads.
If the build volume cannot enforce directory ownership, set
`CKTOOLS_TERMINAL_CONFIG_PARENT` to an existing private directory on an
ownership-enforcing filesystem. Each terminal check uses a fresh child directory.

## Packages

CPack creates `.deb`, `.rpm`, and `.tar.gz` assets on Linux, and a `.tar.gz`
archive on macOS:

```sh
cmake --build build/pkg --target package
```

GitHub Actions checks out and builds the immutable ckVision 1.5.0 revision
`0d81e2eb81d5ba2c2fd6420189b7498e829ebd9f` on each Linux and macOS runner,
then configures ckUtilities against that SDK installation.

Release jobs publish only the native package outputs. A Homebrew formula is
attached to each release and published in the `cklukas/ckutilities` Homebrew tap.
The release job compares the ckVision and backend resource archives with the
pinned checkouts, including their file contents and Git modes. The formula
builds and uses the verified ckVision SDK. Install with
`brew install cklukas/ckutilities/ck-utilities`.
Windows release builds use Visual Studio 2022 (MSVC), Ninja, and a matching
Release ckVision SDK with the static C runtime. The packaging script builds
the pinned SDK and suite, runs every registered CTest including ConPTY, then
builds the production ZIP in a fresh test-disabled tree and checks its
extracted executables. Use a fresh task directory under `V:\tmp`.

For the native action-scope test, supply the exact attached `CKFEA05SCOPE`
NTFS VHD and a dedicated workspace inside its mount. Keep both addresses under
`V:\tmp`, omit the trailing workspace separator, and keep the image reserved
through the run. The ordinary provider fixture remains a separate directory.
Set the repository variables
`CKTOOLS_WINDOWS_ACTION_SCOPE_ROOT` and `CKTOOLS_WINDOWS_ACTION_SCOPE_VHD` for
CI. The backend verifies the held volume against the image before the test
can modify its Recycle Bin.

```powershell
./scripts/windows/build-and-package.ps1 `
    -CkVisionSource $CkVisionSource `
    -CkCoreFsSource $CkCoreFsSource `
    -WorkRoot $WorkRoot `
    -ActionScopeRoot $ActionScopeRoot `
    -ActionScopeVhd $ActionScopeVhd
```

## Documentation screenshots

The screenshots in the README and on the documentation site are generated
SVGs, not hand-made illustrations. The capture executables run the real
launcher, JSON View, Find, Disk Usage, Config, Markdown editor, Chat, and Files
on ckVision's `HeadlessTerminal`, then use ckVision's SVG renderer to record the decoded
display. ck-todo's and ck-sysmon's golden frames, which their test suites
compare against, are written by the same run.

To regenerate them, point the script at the installed SDK and the matching
ckVision 1.5.0 source checkout:

```sh
CKTOOLS_CKVISION_PREFIX=/path/to/ckvision-sdk \
CKTOOLS_CKVISION_SOURCE_DIR=/path/to/ckvision \
CKTOOLS_DOCS_BUILD_DIR=/path/to/task-workspace/docs-build \
CKTOOLS_DOCS_JOBS=1 \
tools/docgen/generate_screenshots.sh
```

The capture build defaults to one job. Set `CKTOOLS_DOCS_JOBS` to the CPU
allocation for that run. On macOS, place the task workspace and `TMPDIR` beneath
`/Volumes/S8T/tmp` (or `/Volumes/PRO-BLADE/tmp`); on the Windows workstation use
`V:\tmp` and set `TEMP`/`TMP` there.

CI regenerates the screenshots and goldens and fails if any tracked file
differs, so review a regenerated golden as carefully as the code that changed
it.

## Strict targets, fuzzers, and benchmarks

ck-todo and ck-sysmon, with their tests and tools, build with every warning
as an error (`cmake/CkToolsStrict.cmake`; `-DCKTOOLS_WARNINGS_AS_ERRORS=OFF`
relaxes it for a newer toolchain). Their tests use ckVision's `cktest`, one
CTest entry per test file.

The libFuzzer targets for ck-todo's workspace codec and ck-sysmon's parsers
need an LLVM Clang (not Apple Clang) and are off by default:

```sh
cmake -S . -B build/fuzz -G Ninja -DCMAKE_CXX_COMPILER=clang++ \
  -DCKTOOLS_BUILD_FUZZERS=ON -DCMAKE_PREFIX_PATH=/path/to/ckvision-sdk
cmake --build build/fuzz --target cktools_fuzzers
ctest --test-dir build/fuzz -L fuzz-corpus
```

`todo_bench` prints ck-todo's timings; its `todo_benchmark_smoke` CTest entry
checks the deterministic work budgets only, never wall-clock speed.

## Site images

The documentation site's hero (`docs/images/hero.png`, 320x180 pixel art
stored at 4x) and its logo and favicon (`docs/images/logo.svg`) are
generated, deterministic artwork:

```sh
python3 tools/docgen/generate_brand_images.py
```

## Documentation site

The site is built by ckdocs 0.8.1 from `ckdocs.yml`; the
`pages` workflow publishes it to GitHub Pages after CI succeeds on `master`.
Preview and check it locally with:

```sh
ckdocs serve --root .
ckdocs check --root .
```
