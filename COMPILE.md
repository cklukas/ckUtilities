# ckUtilities build, verification, and packaging

ckUtilities is a ckVision-native C++20 suite. Every build requires an
installed `ckvision::cvision` CMake package; a neighbouring ckVision source
checkout is never used as a dependency.

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
`ck-config`, `ck-edit`, and `ck-chat`.

## Release rehearsal

Run the package-consumer, staged-install, archive, and terminal gates from one
clean Release build:

```sh
cmake --preset pkg \
  -DCMAKE_PREFIX_PATH=/path/to/ckvision-sdk \
  -DCKTOOLS_CKVISION_PREFIX=/path/to/ckvision-sdk \
  -DCKTOOLS_VERIFY_CKVISION_PACKAGE=ON \
  -DCKTOOLS_VERIFY_CKVISION_INSTALL=ON \
  -DCKTOOLS_VERIFY_CKVISION_ARCHIVE=ON \
  -DCKTOOLS_VERIFY_CKVISION_TERMINAL=ON
cmake --build build/pkg --target verify_ckvision_rehearsal
cmake --build build/pkg --target verify_ckvision_terminal
```

The two commands are intentionally separate because both stage the product
into the same disposable prefix. The archive verifier rejects UI-runtime and
test-framework payloads.

## Packages

CPack creates `.deb`, `.rpm`, and `.tar.gz` assets on Linux, and a `.tar.gz`
archive on macOS:

```sh
cmake --build build/pkg --target package
```

GitHub Actions checks out and builds the immutable ckVision 0.1.8 revision
`338d950e7473c5fdd4f88ea6b37d9f842543ac15` on each Linux and macOS runner,
then configures ckUtilities exclusively against that installed SDK. No
repository variables or prebuilt framework archive are required.

Release jobs publish only the native package outputs. A Homebrew formula is
attached to each release and published in the `cklukas/ckutilities` Homebrew tap.
It builds the pinned ckVision SDK as a resource, so no separately installed
`ckvision` formula is required. Install with
`brew install cklukas/ckutilities/ck-utilities`.
Windows packaging is intentionally not part of the current release workflow.

## Documentation screenshots

The README images are generated SVGs, not hand-made illustrations. The
capture executable runs the real launcher, JSON View, and Markdown editor on
ckVision's `HeadlessTerminal`, then uses ckVision's SVG renderer to record the
decoded display.

To regenerate them, point the script at the installed SDK and the matching
ckVision 0.1.8 source checkout:

```sh
CKTOOLS_CKVISION_PREFIX=/path/to/ckvision-sdk \
CKTOOLS_CKVISION_SOURCE_DIR=/path/to/ckvision \
tools/docgen/generate_screenshots.sh
```

CI and release verification regenerate these files and fail if the tracked
SVGs differ.
