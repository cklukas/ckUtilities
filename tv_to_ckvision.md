# Work Package: ckUtilities ckVision architecture and release roadmap

**Status:** active product branch `master` is ckVision-only.  All configured
executables, tests, installers, archives, and release workflows consume the
installed `ckvision::cvision` SDK.

## Mandate

Deliver an exceptional terminal utility suite through real-world ckVision
adoption. The suite must improve ckVision only through independently useful,
well-specified framework capabilities with public contracts and acceptance
coverage.

## Non-negotiable architecture

- The seven product commands are `ck-utilities`, `ck-json-view`, `ck-find`,
  `ck-du`, `ck-config`, `ck-edit`, and `ck-chat`.
- Every CMake configuration requires an installed `ckvision::cvision` package.
  The source tree never fetches, embeds, or discovers ckVision from an adjacent
  checkout.
- Product packages contain only the native suite, framework-neutral cores, and
  their declared runtime dependencies. The install/archive verification rejects
  non-native UI artifacts and build-only test payloads.
- CI validates a checksummed, platform-specific SDK archive before it runs
  CMake. It has no fallback framework or unpinned SDK input.
- Source-level architecture checks reject non-native UI markers from all active
  product files. The protected `src/tools` area remains excluded from that
  scan only because it temporarily hosts framework-neutral domain cores used by
  the native applications.

## Current evidence

On 2026-08-30 the native suite passed a clean macOS Release rehearsal against
the installed candidate SDK:

- independent package consumer;
- staged product and archive verification;
- 97 CTest cases;
- all seven `--help` and launcher-child checks; and
- real-PTY smoke under `xterm-256color`, `xterm`, and `vt100`.

The Find sandbox policy and Disk Usage iCloud policy are implemented with
deterministic acceptance coverage. See
[action policies](docs/migration/action-policies.md).

## Package delivery

GitHub Actions has separate native package jobs:

| Platform | Release output | Preconditions |
| --- | --- | --- |
| Linux | `.deb`, `.rpm`, `.tar.gz` | `CKVISION_SDK_LINUX_URL` and matching SHA-256 repository variables |
| macOS | `.tar.gz` and a generated Homebrew formula | `CKVISION_SDK_MACOS_URL` and matching SHA-256 repository variables; a tap containing `ckvision` before formula publication |
| Windows | not published | accepted native terminal backend, native installer, package checks, and WinGet installation/uninstallation evidence |

The formula is deliberately attached to a release instead of pushed to an
unspecified tap. Publishing it requires an explicit user-owned tap and a
matching `ckvision` formula.

## Remaining work packages

### WP-R1 — immutable SDK distribution

Publish platform-specific ckVision SDK archives from an accepted upstream
revision. Record immutable URLs, SHA-256 digests, revision identity, compiler,
architecture, and sanitizer compatibility. Configure the four repository
variables used by CI, then run the normal, package-consumer, archive, and PTY
jobs from GitHub Actions.

### WP-R2 — Linux release acceptance

Run the full native release rehearsal on supported Debian and RPM-family
environments. Install each generated package in a disposable environment and
repeat product/launcher smoke. Record external runtime-library dependencies and
package metadata evidence.

### WP-R3 — Homebrew distribution

Create a user-owned tap containing both `ckvision` and `ck-utilities` formulae.
Validate source builds without undeclared network fetches, audit the formula,
and test installation on supported Intel and Apple Silicon runners. Only then
enable automated tap updates with a narrowly scoped token.

### WP-R4 — Windows and WinGet

Accept ckVision’s native Windows terminal backend, add the native product and
installer checks, prove interactive/silent installation and uninstallation in a
clean Windows environment, validate the generated manifest, and submit the
manifest review. No Windows package is advertised before those gates pass.

### WP-R5 — protected-core extraction and physical cleanup

Move the remaining framework-neutral JSON, disk-usage, search, Markdown, and
chat-option cores out of `src/tools` without changing their contracts. After
the native suite builds from their new locations, delete the protected
historical implementation with a separately approved file list.

### WP-R6 — product journey completion

Complete the remaining interactive and real-model acceptance evidence. Keep
deterministic tests model-free; record any authorised local model identity,
platform, and structured outcome separately.

## ckVision improvement protocol

1. Describe a concrete application need in framework-neutral language.
2. Demonstrate that the need has at least two plausible adopters or is a clear
   general framework responsibility.
3. Design a small public contract with ownership, threading, cancellation,
   error, and teardown semantics.
4. Add focused ckVision unit/rendering coverage and an adopting-application
   scenario.
5. Land and version the SDK change before depending on it in a release build.

No application convenience wrapper becomes a ckVision API without this proof.
