# Building from source

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

GitHub Actions checks out and builds the immutable ckVision 1.3.0 revision
`57b379d0dddd6a3b517b614a4eb2d1a1c99b474b` on each Linux and macOS runner,
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
ckVision 1.3.0 source checkout:

```sh
CKTOOLS_CKVISION_PREFIX=/path/to/ckvision-sdk \
CKTOOLS_CKVISION_SOURCE_DIR=/path/to/ckvision \
tools/docgen/generate_screenshots.sh
```

CI and release verification regenerate these files and fail if the tracked
SVGs differ.

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
