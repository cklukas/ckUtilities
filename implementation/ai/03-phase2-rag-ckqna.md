# Phase 2 — RAG Pipeline & `ckqna`

**Goal:** Implement retrieval-augmented generation using the local index, ship the `ckqna` tool, and connect AI helpers into `cktext`. This phase delivers end-to-end offline Q&A on local docs.

## Prerequisites

* Phase 1 completed and merged.
* Local indexes can be built with `ckindex` and queried via the CLI.
* Familiarize yourself with `docs/ai-design.md` sections 6–8 and 13 (Phase 2).

## Implementation Steps

1. **Prompt templates & formatting**
   1. Add prompt template helpers under `lib/ckai_core/prompts/` to render the context-grounded templates described in the design doc. Include system messages enforcing “answer only from context.”
   2. Provide unit tests verifying template output for sample inputs (multiple context chunks, citations, empty results).

2. **RAG orchestrator**
   1. Implement a new component (e.g., `ck::ai::RagSession`) in `libckai_core` that wires together chunk retrieval, prompt building, and streaming generation.
   2. Support configurable retrieval parameters (top-k, score threshold) sourced from `ckai.toml`.
   3. Enforce safeguards: if retrieval returns low scores, emit “I don’t know” per design guidance.
   4. Add integration-style unit tests using tiny synthetic indexes to ensure deterministic behavior with seeded models (use mock backend stubs if full llama.cpp is too heavy for unit tests).

3. **`ckqna` CLI/TUI**
   1. Scaffold `src/tools/ckqna/` with CLI flags for selecting an index, specifying retrieval limits, and toggling JSON output.
   2. Implement TUI components showing streaming answers with citation list; integrate with shared UI widgets (status line, cancel hotkey `Esc`).
   3. Provide integration tests that query a sample index and assert that citations include file + line metadata.

4. **`cktext` AI pane**
   1. Add an `F4` (or next available key) AI help pane in `cktext` per design doc §7.
   2. Hook the pane into `libckai_core` so it can:
      * Rewrite selected text.
      * Generate outlines from titles.
   3. Respect offline rules—only use the local model specified in config. Ensure the UI exposes the equivalent CLI commands (`ckchat`, `ckembed`, `ckindex`) for transparency.
   4. Add unit/UI tests where possible (e.g., verifying command strings) and update manual test scripts.

5. **Documentation & samples**
   1. Extend `docs/tools/ckqna.md` (create if missing) with usage examples, JSON schema, and offline setup instructions.
   2. Update `docs/ai-design.md` section 6 with concrete RAG workflow diagrams/screenshots if available.
   3. Document how `cktext` exposes AI features in its manual page.

6. **Build, packaging, CI**
   1. Wire `ckqna` into the build via `src/tools/CMakeLists.txt` and ensure install rules copy any new assets.
   2. Update `packaging/CPackConfig.cmake` to include `ckqna` binaries and mark them in package descriptions.
   3. Extend CI workflows to run new integration tests and capture coverage for RAG components (update `coverage` preset expectations).

## Validation Checklist

* `cmake --preset dev`
* `cmake --build build/dev -t ckqna cktext`
* `ctest --test-dir build/dev --output-on-failure -L ckai`
* `./build/dev/src/tools/ckqna/ckqna --index test-data/test.ckv --question "What is ckfind?"`
* `cmake --build build/pkg -t package`

## Deliverables

* Prompt templating utilities and RAG orchestrator in `libckai_core`.
* Functional `ckqna` CLI/TUI with streaming answers and citations.
* `cktext` AI pane that reuses backend libraries and surfaces equivalent CLI commands.
* Updated docs, packaging, and CI covering the new workflows.
