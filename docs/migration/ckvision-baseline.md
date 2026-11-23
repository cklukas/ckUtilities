# ckVision SDK baseline

ckUtilities consumes an installed `ckvision::cvision` CMake package. A source
checkout is not a build input.

## Current local baseline

| Field | Value |
| --- | --- |
| ckVision version | `2.1.0` |
| Pinned source revision | [`3d018210de4f8ad724678aa97b0d0b0ddee69a43`](https://github.com/cklukas/ckVision/commit/3d018210de4f8ad724678aa97b0d0b0ddee69a43) |
| Published SDK | [ckVision 2.1.0](https://github.com/cklukas/ckVision/releases/tag/v2.1.0) |
| CMake target | `ckvision::cvision` |
| Client language level | C++20 |
| Previous SDK evidence | [1.4.2 integration record](../../implementation/evidence/ckvision-1.4.2-integration-20261102.json) and [1.4.1 integration record](../../implementation/evidence/ckvision-1.4.1-integration-20261102.json) |

Native qualification builds and installs an SDK from this revision on Linux,
macOS and Windows before configuring ckUtilities. Use the same revision to
reproduce the SDK input locally.

## Local verification

```sh
cmake -S /path/to/pinned-ckvision -B /path/to/ckvision-build \
  -DCMAKE_BUILD_TYPE=Release \
  -DCKVISION_BUILD_TESTING=OFF -DCKVISION_BUILD_EXAMPLES=OFF \
  -DCMAKE_INSTALL_PREFIX=/path/to/ckvision-sdk
cmake --build /path/to/ckvision-build --config Release --parallel 1
cmake --install /path/to/ckvision-build --config Release

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

The source revision is recorded in the shared GitHub qualification workflow,
the CMake package check and `scripts/ci/dependency-pins.json`.
Change it only after accepting the upstream specification, tests,
documentation, release artifact, and provenance evidence, then run the normal,
package-consumer, archive, and terminal checks from GitHub Actions.
