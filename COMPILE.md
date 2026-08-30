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

GitHub Actions downloads a platform-specific ckVision SDK only when its URL and
SHA-256 are configured as repository variables:

- `CKVISION_SDK_LINUX_URL` / `CKVISION_SDK_LINUX_SHA256`
- `CKVISION_SDK_MACOS_URL` / `CKVISION_SDK_MACOS_SHA256`

Release jobs verify the SDK checksum before configuration and publish only the
native package outputs. A Homebrew formula is attached to each release; it
expects the same tap to provide `ckvision`. Windows packaging remains disabled
until ckVision’s native Windows terminal backend and installer acceptance are
complete.
