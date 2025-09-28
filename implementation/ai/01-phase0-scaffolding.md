# Phase 0 — Scaffolding & Runtime Skeleton

**Goal:** Lay down the minimal runtime (`libckai_core`), vendor the primary backend (`llama.cpp`), and ship the `ckchat` prototype with configuration + build plumbing. This unlocks all later phases.

## Prerequisites

* Read `docs/ai-design.md` sections 1–5 and 9–12.
* Understand the build flow in `COMPILE.md` (CMake presets, library layout, packaging via CPack).
* Ensure you have local write access to the `third_party/` directory and can update CMake toolchain files.

## Implementation Steps

1. **Fetch llama.cpp automatically**
   1. Add a helper (e.g., `cmake/FetchLlama.cmake`) that declares a `FetchContent` block pointing at the pinned upstream commit.
   2. Update the top-level `CMakeLists.txt` to include the helper when `CKAI_BACKEND_LLAMA=ON` and ensure the upstream target `llama` is available to dependents.
   3. For reproducibility, document the pinned commit hash and keep optional patches under `third_party/patches/llama.cpp/` if they become necessary.

2. **Create core AI library skeleton**
   1. Under `lib/`, add a new directory `ckai_core/` with `CMakeLists.txt`, headers in `include/ck/ai/`, and a minimal `src/` directory.
   2. Implement placeholder classes matching the API sketches in `docs/ai-design.md §5.1`, but initially stub out methods with TODOs and deterministic return values.
   3. Expose a CMake target `ckai_core` that links to `llama_cpp` when `CKAI_BACKEND_LLAMA=ON`. When the backend is off, provide a compile-time error if code attempts to build AI tools.
   4. Register unit test targets under `tests/unit/ckai_core/` using GoogleTest; create a `CMakeLists.txt` that hooks into the existing test tree. Add simple tests that the stubs return expected placeholder values.

3. **Configuration plumbing**
   1. Introduce a new config loader in `ckcore` (or reuse existing config facilities) to read `~/.config/cktools/ckai.toml`. For Phase 0 this can parse only `llm.model`, `llm.threads`, and `limits.max_output_tokens`.
   2. Provide a sample config template at `configs/ckai.example.toml` as described in `docs/ai-design.md §4`.
   3. Ensure the config path is mentioned in README material or existing help output as appropriate.

4. **Prototype `ckchat` CLI/TUI**
   1. Scaffold `src/tools/ckchat/` using the existing helper (`add_ck_tool`). Implement a CLI front-end that wires the stubs in `ckai_core` to accept a prompt and echo a placeholder response.
   2. Implement a minimal TUI using shared widgets from `ckui` (status line, scrolling output) to stream tokens. For Phase 0, stream the stub response character-by-character with a timer to exercise the event loop.
   3. Add integration tests under `tests/integration/ckchat/` that run the CLI in non-interactive mode.

5. **Build system wiring**
   1. Update `cmake/AddCkTool.cmake` if needed so AI tools can declare dependencies on `ckai_core`.
   2. Add `ckchat` to the default build by including it in `src/tools/CMakeLists.txt`.
   3. Extend `CMakePresets.json` presets (`dev`, `asan`, `pkg`) to toggle `CKAI_BACKEND_LLAMA` and ensure builds fail fast if the backend is off but `ckchat` is requested.

6. **Packaging & CI**
   1. Update `packaging/CPackConfig.cmake` to include the new binaries (`ckchat`) and install paths for `configs/ckai.example.toml`.
   2. Ensure `install(TARGETS ...)` directives for `ckai_core` headers and libraries are in place so packages include them.
   3. Modify GitHub Actions workflows to build and test the new targets (update `cmake --build` invocations and `ctest` filters). Verify that sanitizer and packaging jobs cover `ckchat`.

## Validation Checklist

Run these commands locally before opening a PR:

* `cmake --preset dev`
* `cmake --build build/dev`
* `ctest --test-dir build/dev --output-on-failure`
* `cmake --build build/pkg -t package`

Confirm the resulting `.deb`/`.rpm` contain `ckchat`, `libckai_core` headers, and the sample config.

## Deliverables

* FetchContent helper for `third_party/llama.cpp` with pinned commit + documentation.
* New `lib/ckai_core` target and headers in `include/ck/ai/`.
* Stubbed `ckchat` CLI/TUI wired into the build.
* Sample AI config file and updated packaging/CI definitions.
* Passing builds and tests on the CI matrix.
