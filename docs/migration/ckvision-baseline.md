# ckVision SDK baseline

ckUtilities consumes an installed `ckvision::cvision` CMake package. A source
checkout is not a build input.

## Current local baseline

| Field | Value |
| --- | --- |
| ckVision version | `1.3.0` |
| Pinned source revision | `57b379d0dddd6a3b517b614a4eb2d1a1c99b474b` |
| CMake target | `ckvision::cvision` |
| Client language level | C++20 |
| Local evidence | See [1.3.0 validation record](ckvision-1.3.0-validation.md) for current checks |

CI checks out this exact public revision and builds an installed SDK on every
Linux and macOS runner before it configures ckUtilities. This keeps the
application’s dependency boundary identical to a user build while eliminating
manual SDK-archive configuration.

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

The source revision is recorded directly in both GitHub Actions workflows.
Change it only after accepting the upstream specification, tests,
documentation, release artifact, and provenance evidence, then run the normal,
package-consumer, archive, and terminal checks from GitHub Actions.
