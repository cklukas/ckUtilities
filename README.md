# CK Utilities

**Status:** ckVision-native product and release validation

![ck-chat conversation interface](images/Screenshot%202025-10-25%20at%2023.34.31.png)
![ck-chat model loading](images/Screenshot%202025-10-25%20at%2023.34.40.png)
![ck-chat result](images/Screenshot%202025-10-25%20at%2023.36.05.png)

CK Utilities is a native ckVision suite of seven terminal products:
`ck-utilities`, `ck-json-view`, `ck-find`, `ck-du`, `ck-config`, `ck-edit`,
and `ck-chat`. Every build and package consumes an installed `ckvision::cvision`
CMake SDK; no product configuration has an alternative UI path.

Current local acceptance evidence covers a clean macOS Debug/Release build,
the configured release-and-sanitizer test suite, ASan/UBSan, an installed-SDK
consumer, staged executable and launcher checks, a verified release archive,
and real-PTY terminal lifecycle smoke across three profiles (also under
ASan/UBSan). Linux and Windows remain release acceptance work. See
[the migration roadmap](tv_to_ckvision.md) for the complete status and
constraints.

Target platforms: macOS and Linux terminal hosts; Windows is not advertised
until its ckVision backend has passed the required gates.

Tech stack: C++20 and an installed `ckvision::cvision` CMake package.

## Install a released build

Only ckVision-native release assets are supported. Choose the section for your
platform; do not mix assets from different releases or architectures.

### Linux

Download the `.deb` or `.rpm` asset attached to the matching GitHub release,
then install it with the native package manager:

```sh
# Debian, Ubuntu, and derivatives
sudo apt install ./ck-utilities_*.deb

# Fedora, RHEL, openSUSE, and other RPM-family distributions
sudo dnf install ./ck-utilities-*.rpm
```

The package installs `ck-utilities`, `ck-json-view`, `ck-find`, `ck-du`,
`ck-config`, `ck-edit`, and `ck-chat` on `PATH`. Confirm the install with:

```sh
ck-utilities --help
```

### macOS

Each release contains a native archive named
`ck-utilities-<version>-macos.tar.gz`. Extract it wherever you keep local
applications, then add its `bin` directory to your shell path:

```sh
tar -xzf ck-utilities-<version>-macos.tar.gz
export PATH="$PWD/ck-utilities-<version>-macos/bin:$PATH"
ck-utilities --help
```

Add the `export` line to your shell profile if you want it to persist.

A Homebrew formula is attached to each release but is not yet published to a
named public tap: it needs a companion `ckvision` formula in the same
user-owned tap. Once those formulae are published, installation will be:

```sh
brew tap <tap-providing-ckvision>
brew tap <tap-providing-ck-utilities>
brew install ck-utilities
```

Until then, use the verified archive above rather than an untracked local
formula copy.

### Windows

There is currently no supported Windows installer, WinGet manifest, or binary
release. This is intentional: Windows packaging will begin only after the
ckVision native Windows terminal backend and installer acceptance gates pass.
Use a supported macOS or Linux release, or follow the source-build instructions
below when Windows support is formally introduced.

## Native tool guides

- [CK Utilities launcher](docs/tools/ck-utilities.md)
- [JSON View](docs/tools/json-view.md)
- [Find](docs/tools/ck-find.md)
- [Disk Usage](docs/tools/ck-du.md)
- [Config](docs/tools/ck-config.md)
- [Edit](docs/tools/ck-edit.md)
- [Chat](docs/tools/ck-chat.md)

---

## Why

Why not?

---

## Building CK Utilities

### Prerequisites

- **Compiler:** GCC ≥ 12 or Clang ≥ 15 (C++20 or newer)
- **Build tools:** CMake ≥ 3.25, Ninja (recommended)
- **Framework:** an installed ckVision SDK matching
  [the recorded candidate](docs/migration/ckvision-baseline.md)
- **Test dependency:** GoogleTest (fetched only for test builds)

### Quick Start

Clone the repository and enter the directory:

```bash
git clone https://github.com/cklukas/ckUtilities.git
cd ckUtilities
```

#### Configure the native suite:

```bash
cmake -S . -B build/native \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCKTOOLS_CKVISION_PREFIX=/path/to/ckvision-sdk \
  -DCMAKE_PREFIX_PATH=/path/to/ckvision-sdk
```

The build consumes only the installed SDK; it does not use a neighbouring
ckVision source tree.

#### Build all tools:

```bash
cmake --build build/native
```

#### Build and run a single tool (example: ck-find):

```bash
cmake --build build/native -t ckfind_ckvision
./build/native/bin/ck-find --help
```

#### Run tests:

```bash
ctest --test-dir build/native --output-on-failure
```

The suite exercises each native presentation and the framework-neutral cores.
GoogleTest is build-only and is not included in the verified product archive.

For chat, copy `configs/ckai.example.toml` to `~/.config/cktools/ckai.toml`
and set a local GGUF model path. A missing or unusable real model is reported
to the chat transcript; it is never silently replaced by the deterministic
test stub.

### Release rehearsal

The staged-product, archive, and independent package-consumer gates must use
the same installed ckVision SDK:

```bash
cmake -S . -B build/native-release \
  -DCMAKE_BUILD_TYPE=Release \
  -DCKTOOLS_CKVISION_PREFIX=/path/to/ckvision-sdk \
  -DCMAKE_PREFIX_PATH=/path/to/ckvision-sdk \
  -DCKTOOLS_VERIFY_CKVISION_PACKAGE=ON \
  -DCKTOOLS_VERIFY_CKVISION_INSTALL=ON \
  -DCKTOOLS_VERIFY_CKVISION_ARCHIVE=ON
cmake --build build/native-release --target verify_ckvision_rehearsal
```

`verify_ckvision_rehearsal` runs the complete local CTest suite, an independent
installed-SDK consumer, the staged-product checks, and the archive
check. The latter runs CPack, extracts the delivered TGZ, verifies every
product executable and launcher path, rejects legacy runtime artifacts, and
rejects build-only GTest/GMock payload. The focused verification targets remain
available for diagnosis. This does not replace remaining cross-platform or
real-model acceptance.

On macOS or Linux, add `-DCKTOOLS_VERIFY_CKVISION_TERMINAL=ON` and run
`verify_ckvision_terminal` to smoke-test every staged executable in a real PTY
under `xterm-256color`, `xterm`, and `vt100`. The gate uses an empty disposable
configuration root, delivers a PTY resize and the standard Alt+X quit command,
then verifies clean exit and restoration of persistent terminal mode. It does
not replace interactive workflow or cross-platform acceptance.

### Keyboard shortcuts

Native applications load shared built-in shortcuts and any saved personal
bindings. Use `ck-config` to select the built-in-default or personal scheme and
to add, reset, or resolve conflicting bindings. The native executables do not
accept the historical `--hotkeys` flag or `CK_HOTKEY_SCHEME` environment
override.

#### Install (to staging directory):

```bash
cmake --install build/native-release --prefix /path/to/staging
```

For more details, see `COMPILE.md`.
