# CkTools — Build & Testing Guide (`compile.md`)

> **Status:** early-spec for developers. This document defines the repo layout, build tooling, test strategy, docs generation, and packaging flow for **CkTools** (Turbo Vision TUIs for Linux, in **C++20+**). It enables **fast per-tool iteration** *and* an **all-tools build** with man/Texinfo docs and distro packages.

---

## Goals

* **Fast inner loop:** build/run/tests for a single tool in seconds.
* **Monorepo orchestration:** build **all** tools + shared libs + docs with one command.
* **Reproducible packages:** produce `.deb` and `.rpm` locally and in CI.
* **Shared code:** factor common logic into internal libraries without slowing developers down.

---

## Toolchain & dependencies

* **Compiler:** GCC ≥ 12 or Clang ≥ 15 (C++20 or newer).
* **Build:** CMake ≥ 3.25 + Ninja (preferred).
* **Framework:** Turbo Vision (fetched automatically or from system).
* **Testing:** GoogleTest (via CMake’s FetchContent) + `ctest`.
* **Analysis (optional):** clang-tidy, cppcheck, ASan/UBSan/TSan.
* **Docs:** Pandoc (Markdown → man & Texinfo), `texinfo` (for `install-info`).
* **Packaging:** CPack (generates `.deb` / `.rpm`), `dpkg-dev` & `rpm-build` optional.
* **Nice-to-haves:** `ccache`, `lcov/gcovr` (coverage).

> Linux only (text-mode terminals). macOS/Windows may build, but are not targets right now.

---

## Repository layout

```
cktools/
├─ CMakeLists.txt                # top-level orchestrator
├─ cmake/
│  ├─ AddCkTool.cmake            # helper to declare a new tool
│  ├─ FetchTurboVision.cmake     # pin/fetch TV
│  └─ VersionFromGit.cmake       # version = git describe
├─ CMakePresets.json             # dev/release/asan/coverage/pkg presets
├─ include/ck/                   # public headers for internal libs
├─ lib/
│  ├─ ckai_core/                 # ck-ai runtime scaffolding
│  ├─ ckcore/                    # CLI args, logging, config, common utils
│  ├─ ckfs/                      # filesystem helpers, safe ops, temp dirs
│  └─ ckui/                      # Turbo Vision wrappers/widgets (dialogs, lists)
├─ third_party/
│  └─ llama.cpp/                 # populated on demand via FetchContent (pinned)
├─ src/tools/
│  ├─ ck-chat/                   # ck-ai chat prototype
│  ├─ ck-config/
│  ├─ ck-du/
│  ├─ ck-edit/
│  ├─ ck-utilities/
│  └─ json-view/
├─ tests/
│  ├─ unit/                      # gtest targets per lib/tool
│  └─ integration/               # black-box runs of binaries with fixtures
├─ docs/
│  ├─ tools/                     # Markdown: one .md per tool (authoritative)
│  ├─ man/                       # generated *.1 (ignored by VCS)
│  └─ texinfo/                   # generated cktools.info + sources (ignored)
├─ packaging/
│  ├─ CPackConfig.cmake          # shared cpack config
│  ├─ deb/                       # deb metadata templates
│  └─ rpm/                       # rpm metadata templates
└─ scripts/
   ├─ new_tool.sh                # scaffold a new tool
   ├─ dev.sh                     # helpers (format, tidy, quick-build)
   └─ run-integration.sh
```

---

## Configure & build

### One-time

```bash
git clone https://example.com/cktools.git
cd cktools
# Optional: enable ccache globally
export CCACHE_DIR=~/.cache/ccache
```

### Configure (presets)

```bash
cmake --preset dev         # Debug + ASan off + ccache + Ninja
cmake --preset asan        # Debug + Address/UB sanitizers
cmake --preset release     # -O2/3, LTO if available
cmake --preset coverage    # Debug + coverage flags
cmake --preset pkg         # Release + packaging metadata
```

> Presets set install prefix to `/usr` for packaging; local `install` defaults to `<build>/stage` to avoid root.

The ck-ai configuration template lives at `configs/ckai.example.toml`; install it alongside the binaries or copy it to `~/.config/cktools/ckai.toml` when testing locally.

### Fast per-tool loop

Build only one tool:

```bash
cmake --build build/dev -t ckfind
./build/dev/src/tools/ckfind/ckfind --help
ctest --test-dir build/dev -R ckfind    # unit+integration tests tagged with tool name
```

Build everything:

```bash
cmake --build build/dev
ctest --test-dir build/dev --output-on-failure
```

Install to staging dir (for packaging / sandbox tests):

```bash
cmake --build build/release -t install
```

---

## Adding a new tool

New ck-ai tools follow the same build integration as the legacy TUIs. `ck-chat` ships in-tree as an example that links `ckai_core` and streams deterministic output for tests.

Use the scaffold:

```bash
./scripts/new_tool.sh ckfoo "One-line description"
```

This creates:

```
src/tools/ckfoo/{main.cpp,CMakeLists.txt}
tests/{unit,integration}/ckfoo/...
docs/tools/ckfoo.md
```

Inside `src/tools/ckfoo/CMakeLists.txt` the scaffold calls our helper:

```cmake
# Minimal example
add_ck_tool(
  NAME ckfoo
  SOURCES main.cpp
  LIBS ckcore ckfs ckui
  DESCRIPTION "One-line description"
)
```

`add_ck_tool` (from `cmake/AddCkTool.cmake`) wires:

* target `ckfoo` with `-std=c++20`, warnings, rpaths
* link to Turbo Vision and internal libs
* `--version` baked from `VersionFromGit.cmake`
* CTest labels `tool:ckfoo`
* doc build hooks (see below)

---

## Shared libraries

* **ckai_core:** ck-ai runtime scaffolding and deterministic stubs for the llama backend.
* **ckcore:** logging, argument parsing, subprocess helpers, config file loader, error handling.
* **ckfs:** path ops, safe file actions (dry-run & confirm flows), temp & locking.
* **ckui:** Turbo Vision wrappers (list/table widgets, dialogs, keymaps, status-line, help viewer).

> **Linking policy:** internal libs default to **static** to keep each binary self-contained; switchable to shared via `-DCK_SHARED=ON` if we later stabilize an ABI.

---

## Testing strategy

* **Unit tests (gtest):** cover ckcore/ckfs/ckui and pure logic inside tools.
  * `tests/unit/app_info` validates the launcher catalogue (`ck-utilities`).
  * `tests/unit/config` checks the shared configuration registry (`ck-config`).
  * `tests/unit/ck_du` exercises size/unit helpers and option registration.
  * `tests/unit/json_view` verifies JSON tree construction and formatting helpers.
  * `tests/unit/ck_edit` parses Markdown structures and inline spans.
  * `tests/unit/ckai_core` keeps the ck-ai stubs deterministic.
* **Integration tests:** run compiled binaries against fixtures; assert exit codes, stdout patterns, and side effects in a temp sandbox.
  * `tests/integration/ck-chat` calls the CLI and asserts the placeholder stream.
* **Sanitizers:** `asan` preset runs unit+integration under Address/UBSan.
* **Coverage:** `coverage` preset emits `*.info`; use `lcov`/`genhtml` or `gcovr`.

GoogleTest is fetched automatically when `BUILD_TESTING=ON` (default for the presets). Every executable uses `gtest_discover_tests` so new cases are picked up without editing `CTestTestfile.cmake`.

Examples:

```bash
ctest --test-dir build/asan -L unit
ctest --test-dir build/coverage -L integration
```

**Writing integration tests**

`tests/integration/ckfind/test_basic.sh` (Bash):

```bash
set -euo pipefail
BIN="$CK_BIN_DIR/ckfind"     # injected by CTest
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

mkdir -p "$TMP/a"; touch "$TMP/a/file.txt"
out=$("$BIN" --path "$TMP" --name '*.txt' --dry-run)
grep -q 'file.txt' <<<"$out"
```

Registered in CTest via a small macro that sets `CK_BIN_DIR` to the right build path.

---

## Documentation pipeline

**Authoritative source:** Markdown in `docs/tools/*.md` (one file per tool).
From each `.md` we generate:

* **man(1):** `docs/man/<tool>.1` (Pandoc)
* **Texinfo:** merged `docs/texinfo/cktools.texi` + `cktools.info`

CMake targets:

```bash
cmake --build build/dev -t docs      # all man + texinfo
cmake --build build/release -t install-docs
```

Conventions inside Markdown:

* First H1 is “Tool name — short synopsis”.
* Sections: SYNOPSIS, DESCRIPTION, OPTIONS, EXAMPLES, EXIT STATUS, SEE ALSO.
* We include a block showing the **equivalent CLI** for actions displayed in the TUI.

---

## Packaging (`.deb` / `.rpm`)

We use **CPack** with generator-specific settings. Version pulled from `git describe` or `PROJECT_VERSION`.

Build packages:

```bash
cmake --build build/pkg -t package
ls build/pkg/*.deb build/pkg/*.rpm
```

Install (local sandbox):

```bash
sudo dpkg -i build/pkg/cktools-*.deb      # Debian/Ubuntu
sudo rpm -Uvh build/pkg/cktools-*.rpm     # Fedora/RHEL/OpenSUSE
```

Packaging metadata (maintainer, license, deps like `libtvision`, `pandoc`, `texinfo`) are set in `packaging/CPackConfig.cmake`. Per-distro dependency names are mapped there (or we vendor Turbo Vision as a static dep to reduce system requirements).

---

### Windows installer & winget manifests

We ship a lightweight Windows installer that simply copies the staged payload into `%ProgramFiles%/ck-utilities`. The installer binary (`cku-win-installer`) is built alongside the rest of the tools. To assemble the distributable ZIP on Windows:

```powershell
cmake --preset pkg
cmake --build build/pkg --config Release
$staging = "$PWD/build/pkg/windows-installer"
cmake --install build/pkg --config Release --prefix "$staging/payload"
Copy-Item build/pkg/bin/cku-win-installer.exe -Destination $staging
Compress-Archive -Path "$staging/*" -DestinationPath "build/pkg/ck-utilities-<version>-windows.zip"
```

The GitHub release workflow performs the same steps and then generates Windows Package Manager manifests via `scripts/generate_winget_manifest.py`. You can run the script locally to produce manifests for a custom source:

```bash
python scripts/generate_winget_manifest.py \
  --output build/pkg/winget-manifests \
  --package-identifier ckUtilities.ck-utilities \
  --package-name ck-utilities \
  --publisher "ckUtilities Project" \
  --version 0.1.0 \
  --installer-url "https://example.invalid/ck-utilities-0.1.0-windows.zip" \
  --installer-sha256 012345... \
  --silent-switch "--quiet --force"
```

The generated directory hierarchy (`<publisher>/<package>/<version>/...`) can be published in a separate GitHub repository and added with `winget source add` once it is hosted.

---

## Continuous Integration (outline)

**GitHub Actions** matrix: `ubuntu-22.04` and `fedora-latest`.

Jobs:

1. **build-and-test** (dev preset): configure → build → `ctest`
2. **sanitizers** (asan preset): build → `ctest`
3. **packaging** (pkg preset): `cpack` → upload `.deb`/`.rpm` artifacts
4. **lint**: `clang-format --dry-run -Werror`, `clang-tidy` (warnings not fatal initially)

On git tag `vX.Y.Z`: run packaging and attach artifacts to a Release.

---

## Coding standards & quality

* **C++20** or newer, UTF-8 source files.
* **Warnings-as-errors** in CI for `lib/` and gradually for tools.
* **clang-format** enforced; `scripts/dev.sh format`.
* **clang-tidy** runs on PRs; exceptions must be documented.
* **No exceptions** thrown across tool boundaries; use `expected<T,Error>` style or status codes.
* **Consistent TUI UX:** keybindings, status-line hints, F1 help, ESC to back out.

---

## Developer quick recipes

**Build only `cktext` in Release and run it:**

```bash
cmake --preset release
cmake --build build/release -t cktext
./build/release/src/tools/cktext/cktext
```

**Generate docs and view man page locally:**

```bash
cmake --build build/dev -t docs
man ./docs/man/cktext.1
```

**Create a new tool skeleton:**

```bash
./scripts/new_tool.sh cknet "Network dashboard"
cmake --build build/dev -t cknet
```

**Make a package:**

```bash
cmake --preset pkg
cmake --build build/pkg -t package
```

---

## Notes on Turbo Vision integration

* `ckui` centralizes Turbo Vision setup (application, desktop, event loop, theme).
* Shared widgets (file picker, filter builder, log pane, diff viewer) live in `ckui/`.
* Each tool implements a thin `App` class and composes shared widgets.

---

## Roadmap for the build system

* v0: static internal libs, per-tool binaries, docs via Pandoc, packages via CPack.
* v1: optional shared `libck*` with proper SONAME/versioning.
* v1: split debug symbols (`.ddeb` / `-debuginfo`) in packages.
* v1: translation support (GNU gettext) and manpage localisation.

---

### FAQ

**Q: Can I use Make instead of Ninja?**
A: Yes—CMake will honor your generator. Ninja is just faster.

**Q: Do I need Pandoc for day-one?**
A: Docs targets are optional; builds won’t fail if `PANDOC` is missing (we gate with `find_program`). Packaging will skip man/info unless docs are present.

**Q: How do tools find external commands (e.g., `find`, `diff`)?**
A: `ckcore::which()` resolves them at runtime; we surface clear errors and show the distro package name when missing.

---

That’s the whole developer-facing spec. If you want, I can also drop in the initial `CMakePresets.json`, `AddCkTool.cmake`, and a minimal `ckfind` skeleton so your team can start coding immediately.
