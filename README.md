# CK Utilities

**Status:** concept / exploration.

Current prototypes:

* `ck-json-view` — JSON tree viewer built with Turbo Vision.
* `ck-du` — disk usage explorer with tree views, file listings, unit and sort controls.
CK Utilities aims to bring a set of everyday power utilities to a **Turbo Vision** text UI, so they’re easier to discover and safer to use—while staying fast and script-friendly.
Target platform: **Linux text-mode terminals**.
Tech stack: **C++20 (or newer)** + **Turbo Vision**.

---

## Why

Many GNU tools are extremely powerful but tricky to memorize. CkTools wraps a focused subset behind clean, keyboard-first TUIs with sensible defaults and visible dry-runs.

---

## Planned tools (initial scope)

* **ckfind** — Visual query builder for `find`: filter by type/mtime/size/perm, test patterns live, preview actions (copy/move/delete/archive) with safe confirmation.
* **ckdiff** — Side-by-side diff & patch helper: browse files/dirs, stage/apply hunks, ignore rules, make backups automatically.
* **ckdu** — Interactive disk-usage explorer: tree view, sort & filter by size/inodes, reveal sparse files, jump-to-path actions.
* **cktext** — Minimal **Markdown** editor for quick notes and docs; optional export via system tools (e.g., to HTML/PDF) when available.
* **ckrescue** — Read-only front-end for disk imaging/recovery workflows (e.g., plan → image → verify), emphasizing logs and safety.

> The above list is **not final** and may change as we prototype. Tools not listed here are **out of scope** for this phase.

---

## Design principles

* **TUI-first, CLI-friendly:** every screen shows the equivalent command-line so users learn by doing.
* **Safety by default:** destructive actions are opt-in, with dry-run previews and clear warnings.
* **Portable & fast:** single static binaries where practical; minimal external deps.

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

#### Build and run a single tool (example: ckfind):

```bash
cmake --build build/dev -t ckfind
./build/dev/src/tools/ckfind/ckfind --help
```

#### Run tests:

```bash
ctest --test-dir build/dev --output-on-failure
```

#### Install (to staging directory):

```bash
cmake --build build/release -t install
```

For more details, see `COMPILE.md`.
