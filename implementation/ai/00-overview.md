# AI Backend Implementation Plan — Overview

This directory contains the implementation playbooks for turning the concepts in `docs/ai-design.md`, `README.md`, `COMPILE.md`, and `IDEA.md` into an offline-first AI backend for CkTools.

## Guiding Principles

1. **Offline by default** — all inference runs locally; only allow manual model downloads.
2. **Minimal dependencies** — reuse existing build + packaging tooling and vendor only what we must (e.g., `llama.cpp`).
3. **C++-first integration** — expose AI capabilities through `libckai_*` libraries, consistent with our current monorepo layout and CMake workflows.
4. **Reproducible & testable** — every phase must add tests and CI hooks as described in `COMPILE.md`.
5. **Package-ready** — update CPack + distro specs alongside new binaries so `.deb`, `.rpm`, and tarball builds keep working.

## Phase Structure

Each phase has its own markdown file in this folder. Follow them sequentially:

1. [Phase 0 — Scaffolding](01-phase0-scaffolding.md)
2. [Phase 1 — Embeddings & Index](02-phase1-embeddings-index.md)
3. [Phase 2 — RAG & ckqna](03-phase2-rag-ckqna.md)
4. [Phase 3 — Tool Integrations](04-phase3-tool-integrations.md)
5. [Phase 4 — Quality & Optional Backends](05-phase4-quality-extensions.md)

Each document contains:

* **Prerequisites** — what must be ready before you start.
* **Implementation steps** — ordered tasks with file paths, build targets, and code integration notes.
* **Validation checklist** — commands to run (build, tests, packaging) per `COMPILE.md`.
* **Deliverables** — artifacts or code expected in the PR for that phase.

> ⚠️ Do not skip ahead. Later phases assume the interfaces, configs, and packaging from earlier steps are in place.
