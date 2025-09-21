# Phase 1 — Embeddings & Flat Index

**Goal:** Add embeddings support (`Llm::embed`), introduce `libckai_embed`, and provide CLI utilities (`ckembed`, `ckindex`) with a simple on-disk vector index. This enables offline RAG pipelines in later phases.

## Prerequisites

* Phase 0 is complete and merged.
* The llama.cpp backend builds and can stream dummy tokens via `ckchat`.
* Review `docs/ai-design.md` sections 5.2–6 and 8.

## Implementation Steps

1. **Extend llama backend bindings**
   1. Enhance the backend adapter in `lib/ckai_core` to call llama.cpp embedding functions. Make the implementation conditional on the backend supporting embeddings; emit a descriptive runtime error otherwise.
   2. Update `ck::ai::Llm::embed` to return `std::vector<float>` (or a thin wrapper) rather than placeholder strings. Ensure the dimension is discoverable at runtime.
   3. Add unit tests covering: embedding dimension detection, deterministic embeddings with fixed seeds, and error handling when the backend is unavailable.

2. **Introduce `libckai_embed`**
   1. Create `lib/ckai_embed/` with headers under `include/ck/ai/` for chunking helpers, batching, cosine similarity, and deduplication utilities per `docs/ai-design.md §5.2`.
   2. Implement pure functions for:
      * Text chunking with overlap (configurable size/stride).
      * Hash-based deduplication of embeddings.
      * Cosine similarity calculation.
   3. Provide unit tests in `tests/unit/ckai_embed/` for all helper functions (use synthetic vectors so tests run fast offline).

3. **Flat vector index**
   1. Implement the `ck::ai::Index` class with a naive L2/cosine search over in-memory vectors backed by an on-disk `.ckv` file.
   2. Store metadata (document id, offsets) alongside vectors in a simple binary format; document the format in `docs/ai-design.md` or a new developer note if needed.
   3. Add serialization tests to ensure indexes survive save/load cycles.

4. **Command-line tools**
   1. Scaffold `ckembed` and `ckindex` in `src/tools/` using `add_ck_tool`.
   2. `ckembed`: read files/stdin, chunk via `libckai_embed`, call `Llm::embed`, and write `.vec` files (include metadata JSON/YAML for provenance). Provide `--json` output to print embeddings inline for scripting.
   3. `ckindex`: load `.vec` files, add them to the flat index, and support both index build (`--build`) and query (`--query`) modes. Return top-k hits with similarity scores.
   4. Wire CLIs to respect `~/.config/cktools/ckai.toml` for model paths and batch sizes.
   5. Add integration tests simulating small corpora using temporary files. Keep fixtures tiny so they run quickly.

5. **Docs & examples**
   1. Update `docs/tools/` (create new files if needed) with usage instructions for `ckembed` and `ckindex`, including the offline workflow for preparing models.
   2. Add a brief how-to section in `docs/ai-design.md` or a new doc describing the `.ckv` file format and recommended chunk sizes.

6. **Build, packaging, CI updates**
   1. Extend CMake targets, install rules, and CPack configuration to include the new tools and libraries.
   2. Update GitHub Actions workflows so `ckembed`/`ckindex` are built and tested in all jobs.
   3. Ensure packages install vector index headers and binaries; update `packaging/deb` and `packaging/rpm` metadata if additional runtime dependencies (e.g., `libstdc++` requirements) arise. Keep dependencies minimal.

## Validation Checklist

Run locally:

* `cmake --preset dev`
* `cmake --build build/dev -t ckembed ckindex`
* `ctest --test-dir build/dev --output-on-failure -L ckai`
* `cmake --build build/pkg -t package`

Additionally, run a manual smoke test:

* `./build/dev/src/tools/ckembed/ckembed --model ~/.local/share/cktools/models/embed/<your-model>.gguf sample.txt`
* `./build/dev/src/tools/ckindex/ckindex --build sample.ckv sample.vec`

## Deliverables

* Functional `Llm::embed` implementation with unit tests.
* New `libckai_embed` helpers and tests.
* Working `ckembed` and `ckindex` CLIs with docs.
* Packaging/CI updates ensuring the new tools ship in `.deb`, `.rpm`, and tarball outputs.
