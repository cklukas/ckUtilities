# CkTools — AI Design Doc (`docs/ai-design.md`)

**Status:** design/specification for future implementation.
**Scope:** make **local, offline AI** a **first-class feature** of CkTools (no add-on package).
**Principles:** zero network by default, C++ first, reproducible builds, minimal external dependencies, pluggable backends.

---

## 1) Goals & Non-Goals

### Goals

* **Offline by default:** all inference on the user’s machine; no calls to online services.
* **Own the surface & workflow:** C++ APIs we control; backends are replaceable.
* **Small + fast:** work on typical developer laptops/desktops (CPU okay, GPU optional).
* **UX that teaches the CLI:** TUIs show the equivalent commands, keep user in control.
* **Composable CLIs:** every AI feature also exists as a CLI tool for scripting/CI.

### Non-Goals (for now)

* Training or fine-tuning pipelines.
* Multi-tenant servers, telemetry, or phoning home.
* Bundling large model weights inside distro packages.

---

## 2) High-Level Architecture

```
+---------------------------------------------------------------+
|                        cktools (TUI apps)                     |
|  ckfind  ckdiff  cktext  ckdu  ckrescue   ckchat  ckqna  ...  |
+-------------------------|------------|------------------------+
                          |            |
                    libckai_core   libckai_embed (+ index)
                          |            |
               +----------+            +---------------+
               |                                          
        Backend adapters                               Vector index
     (one or more compiled-in)                      (pluggable engine)
               |
   +-----------+-----------------------------------+
   |    llama.cpp runtime (primary; chat+embed)    |
   |  (optional) whisper.cpp ASR (speech-to-text)  |
   |  (future) alt backends via the same adapter   |
   +-----------------------------------------------+
```

* **`libckai_core`**: prompt formatting, sampling/caching, streaming tokens, stop rules, resource caps, logging. Single, stable API for TUIs/CLIs.
* **`libckai_embed`**: embedding API (can use the LLM backend’s embedding mode) + chunking, batching, dedup.
* **Vector index**: pluggable (start with a simple flat index; allow HNSW/FAISS later).
* **No daemon required**. Everything links in-process. (A tiny Unix-socket RPC can be added later if isolation is desired.)

---

## 3) Code Layout (monorepo tie-in)

```
cktools/
├─ lib/ckai_core/            # C++ API and backend glue
├─ lib/ckai_embed/
├─ third_party/              # vendored backends under lockstep versions
│  ├─ llama.cpp/             # as subproject (frozen at a known commit)
│  └─ whisper.cpp/           # optional
├─ src/tools/
│  ├─ ckchat/                # local chat (TUI + CLI)
│  ├─ ckqna/                 # question-answer over local docs (RAG)
│  ├─ ckembed/               # embed documents
│  └─ ckindex/               # build/search vector indexes
├─ include/ck/ai/            # public headers for internal use
├─ docs/ai-design.md         # this file
└─ configs/
   └─ ckai.example.toml
```

CMake options:

* `-DCKAI_BACKEND_LLAMA=ON` (default)
* `-DCKAI_BACKEND_WHISPER=OFF` (opt-in)
* `-DCKAI_INDEX_HNSW=OFF` (start with flat; enable later)
* `-DCKAI_BUILD_TOOLS=ON` (ckchat/ckqna/ckembed/ckindex)

---

## 4) Models & File Layout

No weights in packages. Users place or fetch models to:

```
~/.local/share/cktools/models/
  llm/       # chat/instruct models in GGUF (e.g., 7B-8B quantized)
  embed/     # embedding models (GGUF if supported by backend)
  asr/       # whisper ggml/gguf models (optional)
```

Helper command:

```
ckmodel list         # show local models
ckmodel add <name>   # move/copy/link a model into the layout
ckmodel verify       # hash/size check; record metadata
```

Config file (`~/.config/cktools/ckai.toml`):

```toml
[llm]
model = "~/.local/share/cktools/models/llm/MyModel-Q4_K_M.gguf"
threads = 6
context_tokens = 4096
gpu_layers = 0        # 0=CPU; >0 if GPU available
seed = 0              # 0 = auto

[embed]
model = "~/.local/share/cktools/models/embed/MyEmbedModel.gguf"
dim = 384
batch = 64

[limits]
max_prompt_tokens = 8000
max_output_tokens = 512
ram_soft_limit_mb = 4096

[privacy]
allow_network = false
```

---

## 5) Public APIs (internal headers)

### 5.1 `ck::ai::Llm` (simplified)

```cpp
struct GenerationConfig {
  int   max_tokens = 512;
  float temperature = 0.7f;
  float top_p = 0.9f;
  int   top_k = 40;
  int   seed = 0;
  std::vector<std::string> stop;
};

struct Chunk { std::string text; bool is_last = false; };

class Llm {
public:
  static std::unique_ptr<Llm> open(const std::string& model_path, const RuntimeConfig&);
  void set_system_prompt(std::string sys);
  // streaming; callback can be lambda capturing TUI widget
  void generate(const std::string& prompt, const GenerationConfig&, std::function<void(Chunk)> on_token);
  std::string embed(const std::string& text);     // returns base64 or binary vector
  size_t token_count(const std::string& text) const;
};
```

### 5.2 `ck::ai::Index` (flat first)

```cpp
class Index {
public:
  static std::unique_ptr<Index> open(const std::string& path, int dim);
  void add(std::string doc_id, std::span<const float> vec);
  std::vector<std::pair<std::string,float>> topk(std::span<const float> q, int k) const;
  void save();
};
```

---

## 6) RAG (Retrieve-Augment-Generate) Pipeline

1. **Chunk** input docs (Markdown/man/info) with overlap; record source anchors.
2. **Embed** chunks via `Llm::embed`.
3. **Index** vectors using `Index` (flat L2/cosine initially).
4. **Answer**: for a question, embed query → retrieve top-k chunks → build a grounded prompt:

```
[System] You answer strictly from the provided CONTEXT. If unknown, say “I don’t know.”
[Context]
- doc1:line 120-160: ....
- doc2: ...
[Question]
<user question>
```

5. **Stream** tokens to TUI; show citations (doc/section) per chunk used.

**Sources for RAG (initial):**

* `docs/tools/*.md` (authoritative manuals)
* Generated man pages
* Optional: user-selected directories (project READMEs, source trees)

---

## 7) Integrations into Existing TUIs

* **ckfind**

  * *Explain match*: select a result → “Why did this match?” (show `find` predicates that matched).
  * *NL → predicates*: write “recent C++ files bigger than 1MB” → propose `-name '*.cpp' -size +1M -mtime -7`.
* **ckdiff**

  * *Explain hunk*: natural language summary of changes.
  * *Commit message draft*: generate conventional commit subject/body (user edits).
* **cktext**

  * *Rewrite*: improve/shorten/expand selected text; style presets (concise/formal).
  * *Outline from title*: scaffold headings/TOC; insert at cursor.
  * *(optional)* Voice note → text (whisper backend).
* **ckdu**

  * *Safe-delete hints*: highlight likely caches/logs; **never auto-delete** (only tag/describe).
* **ckrescue**

  * *Plan explainer*: generate human-readable plan for imaging/verification, with explicit commands.

**UX rules**

* Always preview the **equivalent CLI** an AI suggestion would run.
* Destructive actions require explicit, non-AI confirmation flows.
* Show token/latency stats in status line; `Esc` cancels generation.

---

## 8) CLI Tools (ship with suite)

* `ckchat` — local chat (stdin/stdout & TUI).
* `ckqna` — grounded Q\&A over local indexes.
* `ckembed` — embed files/stdin to vectors (writes `.vec` files).
* `ckindex` — build/search vector indexes from `.vec` files.
* `ckmodel` — model folder helper (add/list/verify).

Each supports `--json` for automation and `--seed` for reproducibility.

---

## 9) Build & Packaging

* AI is **part of the regular build**.
* `third_party/llama.cpp` vendored as a CMake subproject; compiled static.
* Optional backends toggled via CMake options (see §3).
* Packages (`.deb`/`.rpm`) include binaries and headers; **not** model weights.
* Post-install message points to `ckmodel add` instructions.

---

## 10) Performance & Resource Strategy

* Default to small, quantized models (e.g., 4-bit) for interactive speed on CPU.
* Auto-detect CPU threads; cap by default (e.g., 50% of cores) to keep terminal responsive.
* GPU layers enabled only if compiled with GPU support and user opts in.
* Enforce soft limits: prompt size, output tokens, RAM limit; ask to override when exceeded.
* Cache:

  * Prompt→response summaries (short-term)
  * Embeddings for files via content hash
  * Index shards on disk

---

## 11) Privacy & Safety

* **No network** unless `allow_network=true` in config or `--net` passed.
* All prompts/responses are **local only**; no telemetry.
* RAG always cites sources; if retrieval confidence is low, we print “unknown.”

---

## 12) Testing

### Unit

* Tokenization counts, stopword handling.
* Sampler determinism with fixed seeds.
* Chunking/embedding invariants.

### Integration

* Golden tests with a tiny test model (or mock backend) to verify:

  * Streaming works (partial tokens emitted).
  * RAG inserts only allowed context and cites correctly.
  * CLI JSON mode outputs expected schema.

### Contract tests

* Backend adapter must pass the same test suite across builds.
* Resource-limit tests (graceful abort, clear error messages).

### CI presets

* `asan` for memory issues.
* `coverage` for `libckai_*` and CLI tools.
* Optional nightly perf smoke (tokens/sec baseline).

---

## 13) Step-by-Step Implementation Plan

**Phase 0 — Scaffolding (1–2 sprints)**

* Vendoring: add `third_party/llama.cpp` @ pinned commit.
* `libckai_core` minimal: load model, count tokens, stream generation.
* `ckchat` CLI + tiny TUI window (stream to memo widget).
* Config file parsing; reasonable defaults; seed + stop tokens.

**Phase 1 — Embeddings & Index (1 sprint)**

* `Llm::embed` via backend.
* `libckai_embed` (chunking; cosine similarity).
* `Index` flat implementation; `ckembed` & `ckindex` CLIs.
* Build small index from `docs/tools/*.md`.

**Phase 2 — RAG & ckqna (1 sprint)**

* Prompt templates with strict “answer from context.”
* `ckqna` over local index; show citations.
* Add `F4: AI` help pane to `cktext` (rewrite/outline).

**Phase 3 — Tool integrations (2+ sprints)**

* `ckfind`: NL→predicates, explain match.
* `ckdiff`: hunk explain + commit draft.
* `ckdu`: tagging-only hints.
* `ckrescue`: plan explainer.

**Phase 4 — Quality & Optional Backends**

* ASR via whisper (if enabled).
* Optional HNSW index for large corpora.
* Perf polish, model downloader helper, docs.

---

## 14) Developer Notes & Conventions

* All AI UI actions must be **cancelable** (Esc).
* Show a compact **stats line**: `tok/s`, `ctx used`, `temp`, `seed`.
* Log prompts (redacted for secrets) to a per-session debug log **only if** `CK_DEBUG=1`.
* Keep adapter boundary thin; avoid leaking backend types outside `libckai_core`.
* Prefer pure functions for chunking/embedding glue to simplify tests.

---

## 15) Open Questions (to revisit later)

* Ship a tiny sample model for tests, or mock backend only?
* Add translations for AI prompts/messages?
* Expose a minimal local RPC later for third-party plugins?

---

## 16) Appendix — Example Prompts

**Explain diff hunk**

```
[System] Summarize the following diff hunk in one sentence, imperative mood.
[Hunk]
@@ -12,6 +12,9 @@
- old line
+ new line
```

**NL → find predicates**

```
[System] Convert the user request into a GNU find command predicate only.
[Examples]
"files larger than 1 MiB modified in last 7 days" -> -type f -size +1M -mtime -7
"directories named build or dist" -> -type d \( -name build -o -name dist \)
[User]
<request>
```

---

## 17) Deliverables Checklist

* [ ] `lib/ckai_core` with tests
* [ ] `lib/ckai_embed` with tests
* [ ] `third_party/llama.cpp` wired in CMake
* [ ] `ckchat`, `ckembed`, `ckindex`, `ckqna` CLIs (+ minimal TUIs)
* [ ] RAG over `docs/tools/*.md`
* [ ] Integrations into `cktext`, `ckfind`, `ckdiff`
* [ ] Docs: user guide & admin notes (models, limits, privacy)
* [ ] CI: sanitizer & coverage gates
