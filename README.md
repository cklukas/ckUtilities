# CK Utilities

**Status:** active ckVision migration and release rehearsal

![ck-chat conversation interface](images/Screenshot%202025-10-25%20at%2023.34.31.png)
![ck-chat model loading](images/Screenshot%202025-10-25%20at%2023.34.40.png)
![ck-chat result](images/Screenshot%202025-10-25%20at%2023.36.05.png)

CK Utilities is converting its seven terminal products to native ckVision
applications: `ck-utilities`, `ck-json-view`, `ck-find`, `ck-du`,
`ck-config`, `ck-edit`, and `ck-chat`. The conversion is currently an opt-in
release rehearsal while the selected ckVision candidate is accepted upstream.
The historical implementation remains deliberately separate and is not
modified by the rehearsal.

Current local acceptance evidence covers a clean macOS Debug/Release cutover,
92 tests, ASan/UBSan, an installed-SDK consumer, staged executable and launcher
checks, a verified release archive, and real-PTY terminal smoke across three
profiles (also under ASan/UBSan). Linux and Windows remain release acceptance
work. See [the migration roadmap](tv_to_ckvision.md) for the complete status
and constraints.

Target platforms: macOS and Linux terminal hosts; Windows is not advertised
until its ckVision backend has passed the required gates.

Tech stack: C++20 and an installed `ckvision::cvision` CMake package.

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

#### Configure a native ckVision cutover rehearsal:

```bash
cmake -S . -B build/cutover \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCKTOOLS_CKVISION_CUTOVER=ON \
  -DCKTOOLS_CKVISION_PREFIX=/path/to/ckvision-sdk \
  -DCMAKE_PREFIX_PATH=/path/to/ckvision-sdk
```

The cutover build consumes only the installed SDK; it does not use a
neighbouring ckVision source tree or the legacy patch helper.

#### Build all tools:

```bash
cmake --build build/cutover
```

#### Build and run a single tool (example: ck-find):

```bash
cmake --build build/cutover -t ckfind_ckvision
./build/cutover/bin/ck-find --help
```

#### Run tests:

```bash
ctest --test-dir build/cutover --output-on-failure
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
cmake -S . -B build/cutover-release \
  -DCMAKE_BUILD_TYPE=Release \
  -DCKTOOLS_CKVISION_CUTOVER=ON \
  -DCKTOOLS_CKVISION_PREFIX=/path/to/ckvision-sdk \
  -DCMAKE_PREFIX_PATH=/path/to/ckvision-sdk \
  -DCKTOOLS_VERIFY_CKVISION_PACKAGE=ON \
  -DCKTOOLS_VERIFY_CKVISION_INSTALL=ON \
  -DCKTOOLS_VERIFY_CKVISION_ARCHIVE=ON
cmake --build build/cutover-release --target verify_ckvision_rehearsal
```

`verify_ckvision_rehearsal` runs the complete local CTest suite, an independent
installed-SDK consumer, the staged product/cutover checks, and the archive
check. The latter runs CPack, extracts the delivered TGZ, verifies every
product executable and launcher path, rejects legacy runtime artifacts, and
rejects build-only GTest/GMock payload. The focused verification targets remain
available for diagnosis. This does not replace remaining cross-platform or
real-model acceptance.

On macOS or Linux, add `-DCKTOOLS_VERIFY_CKVISION_TERMINAL=ON` and run
`verify_ckvision_terminal` to smoke-test every staged executable in a real PTY
under `xterm-256color`, `xterm`, and `vt100`. The gate uses an empty disposable
configuration root and sends the standard Alt+X quit command after startup; it
does not replace interactive workflow or cross-platform acceptance.

### Hotkey Schemes

All CK Utilities applications draw their shortcuts from a shared hotkey registry. Use the `--hotkeys <scheme>` flag (available on every tool) to override the scheme for a single launch. The built-in schemes are `linux`, `mac`, `windows`, and a user editable `custom` profile.

To change the default scheme globally, set the `CK_HOTKEY_SCHEME` environment variable before launching any tool:

```bash
export CK_HOTKEY_SCHEME=mac
./build/cutover/bin/ck-utilities
```

Any customisations saved through `ck-config` are stored alongside the rest of the user configuration data.

#### Install (to staging directory):

```bash
cmake --install build/cutover-release --prefix /path/to/staging
```

For more details, see `COMPILE.md`.
