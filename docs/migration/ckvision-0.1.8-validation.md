# ckVision 0.1.8 validation

Validated on macOS arm64 on 2026-09-29. Upstream latest release and HEAD were both `338d950e7473c5fdd4f88ea6b37d9f842543ac15` (v0.1.8).

The suite previously pinned v0.1.3. CI, release, documentation, and the CMake compatibility record now use v0.1.8; package discovery requires 0.1.8 or newer. Dialog calls use the explicit modal APIs, and Markdown Enter handling uses the edit-request API.

## Build evidence

All builds used Ninja, Apple Clang, Release, and a fresh installed SDK. No comparable recent build-duration record was available; logs confirm 105 SDK build steps and 201 initial suite steps, with subsequent incremental steps after API corrections. No old build cache was reused.

| Build | Start UTC | End UTC | Elapsed | Result |
| --- | --- | --- | --- | --- |
| SDK clean | 2026-09-29T19:16:22+00:00 | 2026-09-29T19:16:39+00:00 | 16.82s | 0 |
| Suite clean | 2026-09-29T19:17:00+00:00 | 2026-09-29T19:17:54+00:00 | 54.12s | 1 |
| Suite API migration | 2026-09-29T19:18:39+00:00 | 2026-09-29T19:18:47+00:00 | 7.83s | 1 |
| Suite final | 2026-09-29T19:19:50+00:00 | 2026-09-29T19:20:00+00:00 | 10.79s | 0 |

Source manifest SHA-256 (sorted repository paths and content hashes under src/, include/, lib/): `7473b3b3878f06a28483c2a95d28933928614bfc0439218971bf15414cf85297`. Source bytes remained unchanged through validation.

Current verified installed output: `/Volumes/PRO-BLADE/tmp/ckutilities-ckvision-update-20260929/utilities-build/ckvision-install-check`. No persistent app instance was launched.

## Verification

- CTest: 97/97 passed; includes Markdown list continuation and undo, editor, dialogs, launcher, JSON, Find, Disk Usage, Config, Chat, and AI core tests.
- Independent installed ckVision package consumer: passed.
- Staged native commands, cutover checks, and extracted release archive: passed.
- Real-PTY terminal-profile verification: passed.
- Documentation regenerated twice with identical SVG hashes; changed Editor, Disk Usage, and JSON images visually inspected.
- git diff --check: passed.
- Linux and GitHub Actions checks were not run locally.

## Installed executable fingerprints

| Executable | Modification time UTC | SHA-256 |
| --- | --- | --- |
| ck-chat | 2026-09-29T19:20:19+00:00 | `0068276c865017e21bede0d8ee999e6782735978ddbfe171ca9c366cda51ef37` |
| ck-config | 2026-09-29T19:20:19+00:00 | `5b2ddae1c34ce89e938b211b3461b884dbd27f93f07241319cd963958f024784` |
| ck-du | 2026-09-29T19:20:19+00:00 | `7ad1e849a4917929d4ba136d462984ece91073337c35a2215b7a147b21c7d066` |
| ck-edit | 2026-09-29T19:20:19+00:00 | `5bdc2616377ccde1e58c504b356c93b111259ead364626669c715775a0e7afdb` |
| ck-find | 2026-09-29T19:20:19+00:00 | `ad5781901afaf2488a1e2ebadbdfec9ebdb9b7558be97982e3efcab4902e6d08` |
| ck-json-view | 2026-09-29T19:20:19+00:00 | `3263bf573e63b52e46fd3e481090e64d113c71f9b15fb36b932db6e781110ce0` |
| ck-utilities | 2026-09-29T19:20:19+00:00 | `0adb17abd0ae0ddab5431c63b9c66146c8bc465edb748f5e78bdeaca0444a275` |

## Bundled asset fingerprints

Installed bytes match the current source bytes.

- `README.md`: `f8eac9fb3708907de38e2a0ee4161778d66222bc6a214ee8d6d13eb75ef7d359`
- `LICENSE`: `3972dc9744f6499f0f9b2dbf76696f2ae7ad8af9b23dde66d6af86c9dfb36986`
- `configs/ckai.example.toml`: `e16a65a55056eaeb2b10ae6b7f37b2313d60d03bc74743cda8c94130a5d5f9fd`
