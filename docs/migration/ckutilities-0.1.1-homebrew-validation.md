# CK Utilities 0.1.1 Homebrew validation

Validated on macOS arm64 on 2026-09-29.

- Source release: `v0.1.1` (`4e5558d`).
- Homebrew tap: `cklukas/ckutilities`, formula revision 1 (`8cdc3cd`).
- Install: `brew install cklukas/ckutilities/ck-utilities`.
- Verified current installation: `/opt/homebrew/Cellar/ck-utilities/0.1.1_1`.
- All seven installed commands passed `brew test`.
- `brew linkage --test` passed.
- Formula attachment matches the tested tap formula byte-for-byte.
- Installed README, license, and sample configuration match release source bytes.
- Previous local builds and superseded Homebrew keg removed after verification.

## Build evidence

Build start: 2026-09-29 19:32:26 +0000. End: 2026-09-29T19:33:14+00:00. Homebrew reports 51 seconds; the previous comparable build took 39 seconds. Logs show fresh compilation of the pinned ckVision SDK and suite using Release, Apple Clang, and Homebrew superenv.

Source manifest SHA-256: `7473b3b3878f06a28483c2a95d28933928614bfc0439218971bf15414cf85297`.

Detailed timestamp, asset, and executable fingerprints are attached as [homebrew-validation.json](https://github.com/cklukas/ckUtilities/releases/download/v0.1.1/homebrew-validation.json).

## Release checks

See the [release workflow](https://github.com/cklukas/ckUtilities/actions/runs/36619696446) and [branch CI](https://github.com/cklukas/ckUtilities/actions/runs/36619952842) for macOS/Linux build, test, SDK consumer, archive, and terminal checks.

The release workflow completed successfully on both platforms and published
the macOS archive, Linux archive, DEB, RPM, and Homebrew formula. The downloaded
macOS archive contains all seven commands and matches GitHub's SHA-256 digest:
`9ecefd11f05ec60f28a855f571a9a1b71d1fffe14786e4094df1b2014b6e03b0`.
