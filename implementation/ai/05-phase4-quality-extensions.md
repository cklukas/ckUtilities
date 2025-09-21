# Phase 4 — Quality, Performance, Optional Backends

**Goal:** Polish the AI stack for production: add optional backends (e.g., Whisper ASR, alternative indexes), improve performance/telemetry, and finalize docs + tooling for releases.

## Prerequisites

* Phases 0–3 complete and stable in main.
* CI pipelines green for multiple consecutive commits.
* Product requirements for optional features agreed upon (confirm with lead before starting).

## Implementation Steps

1. **Optional Whisper backend (if enabled)**
   1. Vendor `whisper.cpp` under `third_party/` similar to llama.cpp (pinned commit, documented patches).
   2. Add CMake option `CKAI_BACKEND_WHISPER` (default `OFF`). When enabled, build a static library `whisper_cpp` and expose ASR APIs under `libckai_core`.
   3. Implement a thin wrapper `ck::ai::AsrSession` with streaming transcription. Ensure it reuses config + logging infrastructure.
   4. Ship a CLI (`cktranscribe` or integrate into `ckchat` if approved) with tests and docs.

2. **Advanced index options**
   1. Add optional HNSW or FAISS-like index support behind `CKAI_INDEX_HNSW`. Keep the default flat index unchanged to avoid new dependencies.
   2. Implement adapter interfaces so index choice is runtime-configurable via `ckai.toml`.
   3. Provide benchmarking scripts (under `scripts/ai/`) to compare recall/latency with sample data.

3. **Performance tuning**
   1. Implement caching layers described in `docs/ai-design.md §10` (prompt summaries, embedding cache, on-disk index shards).
   2. Add instrumentation hooks to log token/sec, context usage, and cache hits when `CK_DEBUG=1`.
   3. Provide automated microbenchmarks or perf smoke tests (nightly job) to prevent regressions.

4. **Robustness & safety**
   1. Expand test suites to cover resource limits (max tokens, RAM soft limits). Simulate limit breaches and ensure graceful errors.
   2. Fuzz config parsing for `ckai.toml` to harden against malformed input.
   3. Add localized messages or translation hooks if the team decides to support multiple languages.

5. **Documentation & release readiness**
   1. Produce comprehensive admin + user guides detailing model management (`ckmodel`), offline operation, and troubleshooting. Update `docs/ai-design.md` appendices accordingly.
   2. Ensure README and `COMPILE.md` mention the new AI tooling, optional backends, and required build flags.
   3. Add upgrade notes / migration steps for existing installations (e.g., config schema changes).

6. **Packaging & CI hardening**
   1. Update CPack scripts to install optional binaries only when the corresponding CMake options are enabled; verify packages remain self-contained without model weights.
   2. Extend GitHub Actions matrix to run optional backend builds periodically (nightly or on-demand) to ensure they stay healthy.
   3. Generate SBOMs or dependency manifests if required by release process.

## Validation Checklist

* `cmake --preset dev -DCKAI_BACKEND_WHISPER=ON -DCKAI_INDEX_HNSW=ON` (when testing optional features)
* `cmake --build build/dev`
* `ctest --test-dir build/dev --output-on-failure`
* `cmake --build build/pkg -t package`
* Run performance smoke tests: `scripts/ai/run-perf-smoke.sh`

## Deliverables

* Optional backend integrations guarded by build flags with documentation.
* Performance/caching improvements with metrics exposed under debug mode.
* Hardened tests, packaging, and CI that keep the AI stack stable for releases.
