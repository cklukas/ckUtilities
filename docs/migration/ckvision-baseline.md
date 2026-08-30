# ckVision SDK baseline

ckUtilities consumes an installed `ckvision::cvision` CMake package. A source
checkout is not a build input.

## Current local baseline

| Field | Value |
| --- | --- |
| Foundation commit | `e40c9beee7b0be8450fd417aa0d55480418807bb` |
| Integration candidate | `c25a324aa1db9c24851085a6ff84f5ae47e9014b` |
| CMake target | `ckvision::cvision` |
| Client language level | C++20 |
| Local evidence | macOS Release, ASan/UBSan, package consumer, staged product, archive, and real-PTY gates |

The release pipeline is enabled only after a platform-specific SDK archive is
published from an accepted upstream revision. Record its immutable URL,
SHA-256, platform, architecture, compiler, and package version in the release
record before configuring repository variables.

## Local verification

```sh
cmake --preset dev \
  -DCMAKE_PREFIX_PATH=/path/to/ckvision-sdk \
  -DCKTOOLS_CKVISION_PREFIX=/path/to/ckvision-sdk \
  -DCKTOOLS_VERIFY_CKVISION_PACKAGE=ON \
  -DCKTOOLS_VERIFY_CKVISION_INSTALL=ON \
  -DCKTOOLS_VERIFY_CKVISION_ARCHIVE=ON \
  -DCKTOOLS_VERIFY_CKVISION_TERMINAL=ON
cmake --build build/dev --target verify_ckvision_rehearsal
cmake --build build/dev --target verify_ckvision_terminal
```

The rehearsal verifies the independent package consumer, all native product
commands, staged installation, extracted archive, and CTest suite. Run the
terminal target separately because it stages the same disposable install
prefix.

## CI input

GitHub Actions validates the SDK archive checksum before invoking CMake. Set
both URL and SHA-256 variables for every published platform:

- `CKVISION_SDK_LINUX_URL` / `CKVISION_SDK_LINUX_SHA256`
- `CKVISION_SDK_MACOS_URL` / `CKVISION_SDK_MACOS_SHA256`

Change this baseline only after the upstream SDK’s specification, tests,
documentation, release artifact, and provenance evidence have all been
accepted.
