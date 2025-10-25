# CK Utilities

**Status:** concept / exploration

![ck-chat conversation interface](images/Screenshot%202025-10-25%20at%2023.34.31.png)
![ck-chat model loading](images/Screenshot%202025-10-25%20at%2023.34.40.png)
![ck-cjat result](images/Screenshot%202025-10-25%20at%2023.36.05.png)

**Status:** concept / exploration.

Current prototypes:

* `ck-chat` — chat interface backed by the ck-ai runtime stubs.
* `ck-json-view` — JSON tree viewer built with Turbo Vision.
* `ck-du` — disk usage explorer with tree views, file listings, unit and sort controls.
* `ck-find` — staged search-specification builder with simple and advanced filters.
CK Utilities aims to bring a set of everyday power utilities to a **Turbo Vision** text UI, so they’re easier to discover and safer to use—while staying fast and script-friendly.
Target platform: **Linux text-mode terminals**.
Tech stack: **C++20 (or newer)** + **Turbo Vision**.

---

## Why

Why not?

---

## Building CK Utilities

### Prerequisites

- **Compiler:** GCC ≥ 12 or Clang ≥ 15 (C++20 or newer)
- **Build tools:** CMake ≥ 3.25, Ninja (recommended)
- **Dependencies:** Turbo Vision (fetched automatically), GoogleTest (fetched automatically)

### Quick Start

Clone the repository and enter the directory:

```bash
git clone https://github.com/cklukas/ckUtilities.git
cd ckUtilities
```

#### Configure the build (choose a preset):

```bash
cmake --preset dev         # Debug build (recommended for development)
cmake --preset release     # Optimized build
```

#### Build all tools:

```bash
cmake --build build/dev
```

#### Build and run a single tool (example: ck-find):

```bash
cmake --build build/dev -t ckfind
./build/dev/bin/ck-find --help
```

#### Run tests:

```bash
ctest --test-dir build/dev --output-on-failure
```

Unit tests exercise the shared libraries and each tool's core logic (JSON tree building, Markdown analysis, disk-usage math, configuration registries, and the ck-ai runtime stubs). The suite uses GoogleTest and is enabled automatically when `BUILD_TESTING` is on, so running `ctest` after a build executes the new checks.

For the ck-ai tooling, copy `configs/ckai.example.toml` to `~/.config/cktools/ckai.toml` and adjust the model path once you have a local GGUF file.

### Hotkey Schemes

All CK Utilities applications draw their shortcuts from a shared hotkey registry. Use the `--hotkeys <scheme>` flag (available on every tool) to override the scheme for a single launch. The built-in schemes are `linux`, `mac`, `windows`, and a user editable `custom` profile.

To change the default scheme globally, set the `CK_HOTKEY_SCHEME` environment variable before launching any tool:

```bash
export CK_HOTKEY_SCHEME=mac
./build/dev/bin/ck-utilities
```

Any customisations saved through `ck-config` are stored alongside the rest of the user configuration data.

#### Install (to staging directory):

```bash
cmake --build build/release -t install
```

For more details, see `COMPILE.md`.
