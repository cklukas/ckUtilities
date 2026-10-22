# ckVision 1.3.0 validation

Validated on macOS arm64 (Apple Clang 21, CMake 4.4.4) on 2026-10-22.
The latest ckVision release was v1.3.0, commit
`57b379d0dddd6a3b517b614a4eb2d1a1c99b474b`, identical in the public GitHub
repository and the development checkout.

The suite previously pinned v0.1.8. CI, release, the Homebrew formula
generator, documentation captures, and the CMake compatibility record now use
v1.3.0, and package discovery requires 1.3.0 or newer. No source change was
needed: the suite compiled against the new SDK unchanged.

One build-system change was needed, unrelated to ckVision. The top-level
project now enables C. With CMake 4.4 and the Homebrew `llama`/`ggml`
packages, whose imported targets carry `c_std_90`, C was otherwise first
enabled inside `lib/ckai_core`. The generate step then failed with "No known
features for C compiler".

## Build evidence

The SDK was built in Release with examples, tests, and fuzzers off, using
`scripts/build_ckvision_sdk.sh`. The suite was built from the `dev` preset
(Debug, Ninja) against that installed SDK, with documentation captures and
all four ckVision verification options enabled.

## Verification

- CTest (Debug): 97/97 passed, ctest exit 0.
- Independent installed ckVision package consumer (`verify_ckvision_package`): passed.
- Staged native commands, cutover checks, and extracted release archive
  (`verify_ckvision_rehearsal`): passed.
- Real-PTY terminal-profile verification (`verify_ckvision_terminal`): passed.
- Documentation screenshots regenerated with `tools/docgen/generate_screenshots.sh`.
  Only `ck-json-view.svg` changed: the sample document names the framework
  version, which now reads 1.3.0 instead of 0.1.8.
- Homebrew formula resource: SHA-256 of the GitHub archive of the pinned
  commit recomputed as
  `f9d54bc52541919c6d5700d32801432e0b258dc3c49f99a1b458caf952509508`.
- `git diff --check`: passed.
- Linux, sanitizer, and GitHub Actions checks were not run locally.

## Staged executable fingerprints

From the Debug build's staged install (`ckvision-install-check/bin`).

| Executable | SHA-256 |
| --- | --- |
| ck-chat | `05fbb01c8f76f50019f3671071d1bf9bc45d0b356f60810f0e6a4a4d2db6000a` |
| ck-config | `c9e2615a3dcc7c542e439c093e524431356b0d90bbc841ca87af57864e6795ff` |
| ck-du | `7d9840dcac98deb33e9da72b371a6e69e5350f7df08e911e0ed9d792cc34a4c2` |
| ck-edit | `9ecba8e7604d885258d5bce189a66cd4865363f7ca237368991da92c0d96b304` |
| ck-find | `f2e9ed6b1c0009522f06c301ecb40cc44d1f7928ad91bc43b3fde91eda6e08e2` |
| ck-json-view | `d2d21bee8e3c7af4893c0314de8c09f58a42fcd5175ada5652f95b9619aa91ae` |
| ck-utilities | `a644102514ffe59d50dab0458ec8caf4402501039fa6f0b9130d1419e4a3a071` |
