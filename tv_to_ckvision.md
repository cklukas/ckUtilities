# Work Package: ckUtilities migration to ckVision

Status: Active — WP-0/WP-1, the initial WP-2 shell, and a native vertical slice for every application WP through WP-9 are implemented. Workflow-depth completion and WP-10 legacy removal remain in progress.
Owner: ckUtilities and ckVision maintainers  
Created: 2026-08-29  
Target branch: `master`  
Historical application baseline: `legacy_tv`

## Implementation record (2026-08-29)

- WP-0/WP-1: a clean installed ckVision SDK workflow, independent package
  consumer, UI-boundary scanner, baseline record, and traceability ledger are
  implemented on `master`.
- WP-2: `ck_vision_shell` provides a native Desktop, semantic themes,
  stable About/launcher commands, native help, menus, and status composition;
  its headless smoke test passes.
- WP-3: `ck-json-view-ckvision` is a separate native executable. It owns the
  parsed JSON document for the complete view lifetime, maps the domain tree
  into ckVision TreeNodes, uses injected filesystem/file-dialog services and
  the application clipboard, and implements open, close, copy,
  find/next/previous/end-search, and expand-to-level commands. Its headless
  scenario covers filesystem load, result reveal/selection, copy, expansion,
  full-frame rendering, UTF-8 search in a 64-level document, malformed reload
  preservation, explicit close-state cleanup, narrow-terminal recomposition,
  keyboard and mouse tree navigation, and a 2,048-entry search smoke with a
  visible-frame composition cap. Provider-backed large-tree measurement and
  the resulting performance decision remain open.
- The JSON search slice found and exercised a reusable TreeView gap.
  ckVision candidate `d54f65f0b190b836e04f4ffbc38fcbe08c4368cb` adds the
  documented, unit-tested `TreeView::reveal_and_select` API. It remains an
  integration candidate until it is landed on ckVision trunk; its provider-
  backed large-tree follow-up remains open.
- WP-4: `ck-utilities-ckvision` now provides a native multi-window tool
  browser. It uses registry commands and publishes a selected launch request
  to the POSIX composition root, which closes the terminal UI before running
  the child process. The built-in calendar, ASCII table, and calculator are
  native Desktop windows; the calculator's expression model is UI-independent
  and directly tested. The calendar reuses ckVision's CalendarView rather
  than retaining a suite copy. A bounded, read-only application-diagnostics
  window uses ckVision's injected diagnostics sink rather than creating a
  raw-event interception path. The color selector uses ckVision's declarative,
  validated radio-form dialog and presents the selected palette values without
  a suite-specific widget. The initial launcher conversion is complete; its
  workflow-depth, resizing, and input coverage will grow with release gates.
  Its headless scenario now exercises repeated native launcher windows, the
  standard close lifecycle, a full standard-quit window sweep, list selection
  through keyboard and mouse input, and terminal resize recomposition.
  The disposable installed-product verifier now temporarily removes a staged
  child executable and confirms that `ck-utilities --launch ck-json-view`
  fails with a clear diagnostic before it can start a child process.
- WP-5: `ck-find-ckvision` keeps the specification and command-plan generation
  in the framework-independent search core, exposes command-registry actions
  for a validated scrolling guided form and command preview, and now saves,
  loads, runs, and cancels searches through injected storage and execution
  services. The production execution adapter uses a joinable worker and the
  search core's explicit traversal-boundary cancellation probe; its callbacks
  are marshalled back to the UI thread through `Application::post()` behind a
  teardown lifetime gate. Interactive execution bounds rendered matches to 200
  while preserving the full match count. Confirmed deletion now runs only in
  the injected worker and only removes matching regular files or symbolic
  links; it never removes directories. Custom commands remain preview-only
  until a separately sandboxed execution policy is designed.
- WP-6: `ck-du-ckvision` maps application-owned directory snapshots into a
  native TreeView plus selected-directory Table. Its production composition
  root now starts immediately and delegates scanning to an injected joinable
  service; progress and completion cross back to the UI through
  `Application::post()` behind a teardown lifetime gate, and cancellation is
  propagated into the existing scan-core cancellation probe. Selected-directory
  file inspection now uses a separate injected, cancellable file-list service
  and native Table window, so aggregation and enumeration never compete for
  one implicit worker.
  A selected directory can now request a macOS iCloud download or an explicit
  confirmation-gated request to free local copies through a separate injected
  cloud service. The Foundation adapter only reports that macOS accepted the
  request (the provider may continue synchronizing); it runs off the UI thread,
  posts immutable progress/results through the existing lifetime gate, and
  exposes cancellation. Unsupported platforms report that fact rather than
  simulating a cloud change. Recursive cloud policy and other providers remain
  acceptance work.
- WP-7: `ck-config-ckvision` accepts an injected option registry and exposes
  a native provider-backed table with typed edit/reset commands keyed by the
  option's stable string name. Save and reload are now registry commands
  delegated to an injected persistence policy; the production composition
  root retains the established JSON defaults format without giving the UI a
  filesystem dependency. Import and export use that same injected boundary.
  The native keyboard-shortcut window now lists stable command identities,
  captures normalized `KeyChord` values without dispatching them, requires an
  explicit confirmation before replacing an occupied chord, and persists
  overrides by command key. It now includes a suite-wide catalog of every
  native executable's application-specific command metadata; that catalog is
  also the source from which the executables declare their title, category,
  and default chord. A binding is therefore stored under its target executable
  without importing or running that UI.
  Every native executable reloads shared `ckv.*` and its own application
  bindings at startup. Configuration import now snapshots and restores the
  registry if an injected persistence policy fails after mutation, so a failed
  import cannot partially change the active settings. The shortcut selector
  offers separate built-in-default and personal-binding schemes. The first
  edit activates the personal scheme; selecting defaults leaves those personal
  bindings intact but inactive. Keymap format 1 data upgrades on its next
  write. Final configuration acceptance remains.
- WP-8: `ck-edit-ckvision` uses ckVision's EditorDocument, EditorWindow, and
  injected file service for native open/save/save-as workflows. Dirty-window
  close now presents an explicit Save/Discard/Cancel decision and invokes the
  file controller's typed close contract before allowing teardown. The
  Markdown analyzer now builds as a framework-independent `ck_markdown_core`
  target and supplies an application-owned Markdown syntax profile through
  ckVision's documented profile registry. A conservative Markdown-whitespace
  normalization command now commits one revision-bound document transaction,
  preserving two-space hard breaks and fenced/indented code verbatim while
  removing accidental prose whitespace.
  A save conflict now preserves the externally changed file and gives the user
  an explicit Save As, reload/discard, or continue-editing choice. Broader
  Markdown transformations remain the next slice.
- WP-9: `ck-chat-ckvision` owns a native FlowView transcript and prompt
  workflow with registry commands for new/send/cancel/copy. It consumes an
  injected streaming response service; chunks and completion are marshalled to
  the UI thread behind a request-generation and teardown lifetime gate, while
  the production adapter loads the activated local model and generates on a
  joinable worker.
  Assistant Markdown is adapted by the shared framework-independent analyzer
  into styled FlowView content and link targets. Transcript export is delegated
  to an injected storage service. Native prompt selection, add/edit, default
  restoration, and confirmed custom-prompt deletion now use an injected
  `SystemPromptManager` adapter; the active prompt is carried in every response
  request. Downloaded-model selection, deactivation, and confirmed local
  deletion now use an injected `ModelManager` adapter, and the active model ID
  and completed prior turns are carried in every response request. The adapter now owns cancellable
  background downloads behind a cached catalog, so rate-limited typed progress
  and completion reach the UI without racing `ModelManager` or retaining view
  pointers. The production response adapter now opens the activated local
  `ckai_core` model on its own worker, streams cancellable generation back
  through the existing lifetime gate, and prevents model lifecycle changes
  while a response is active. The live rich transcript now renders at most the
  latest 160 messages (with an explicit retention notice) while export and
  model context retain the full conversation. The first response chunk renders
  promptly; subsequent small chunks are coalesced until 96 bytes or completion.
  ckVision candidate `b3b754fdc2231bf02284505ec12066d0f89b1d47` provides
  a checked `FlowView::replace_block` API with incremental realized-tail
  reflow, which the active response uses to avoid rebuilding prior
  rich-content blocks. The same candidate retains materialized `TreeView`
  visible rows until roots or expansion state changes, so an unchanged tree is
  not re-flattened on every redraw. Real-model runtime evidence remains before
  acceptance.
- All seven native executables build together against the installed ckVision
  candidate SDK. Their headless suite, JSON-domain, and architecture tests
  pass as one 19-test checkpoint. A separate installed-product gate builds the
  complete suite, stages it to a disposable prefix, and verifies that each
  native executable completes `--help`; the gate also protects the chat
  runtime's relative shared-library lookup. The legacy executables remain
  deliberately separate while the outstanding workflow entries in the
  traceability ledger are completed.
- `CKTOOLS_CKVISION_CUTOVER=ON` rehearses the release composition against the
  installed candidate: only the framework-neutral cores and seven native
  executables are configured, and the product binaries use their production
  names without configuring or installing the legacy UI runtime. The complete
  74-test cutover configuration, independent package consumer, and staged
  installed-product smoke gate pass. The `verify_ckvision_cutover` gate also
  rejects legacy product linkage, installed legacy artifacts, and legacy
  references in installed public headers. It remains opt-in until the
  candidate is accepted upstream and can be installed reproducibly in CI.
  A macOS ASan/UBSan cutover build against a package built with
  `CKVISION_SANITIZE=address,undefined` also passes all 74 tests. The
  sanitizer package propagates its required compile and link flags to CMake
  consumers; validation must use that supported ckVision option rather than a
  release SDK built with ad-hoc sanitizer flags. The clean detached candidate
  passed ckVision's 169-test normal and 169-test ASan/UBSan suites, and this
  cutover configuration's 74-test normal and ASan/UBSan suites, on 2026-08-30.

## 1. Mandate

Convert the complete ckUtilities application suite from Turbo Vision to
ckVision while using the migration as a demanding real-world proving ground
for ckVision itself.

The outcome is not a mechanical toolkit substitution and not an emulation
layer. It is a deliberate re-architecture of the suite around ckVision's
modern ownership, event, command, layout, rendering, service-injection, and
testing models. When the applications expose a general framework weakness,
the weakness is fixed in ckVision at the correct architectural layer, with a
specification, tests, documentation, examples, and performance evidence. The
applications must not accumulate local workarounds for missing framework
capabilities.

The quality target is exceptional software:

- clear dependency direction and explicit ownership;
- application/domain logic independent of the UI framework;
- deterministic, headless-first UI verification;
- complete keyboard and mouse behavior;
- Unicode-correct rendering and input;
- safe background work and cancellation;
- measurable performance on realistic data sets;
- warnings-clean, sanitizer-clean, portable C++20;
- no compatibility facade, hidden global state, or knowingly deferred design
  debt; and
- documentation that stays synchronized with the public behavior and API.

## 2. Located ckVision library and discovery snapshot

The ckVision checkout was located at:

```text
/Volumes/PRO-BLADE/git/ckvision
```

This path is a local discovery fact, not a build contract. Committed
ckUtilities build files must consume an installed or otherwise explicitly
pinned ckVision package through:

```cmake
find_package(ckvision CONFIG REQUIRED)
target_link_libraries(<target> PRIVATE ckvision::cvision)
```

At discovery time, ckVision was on `main` at commit `e40c9beee7b0` and its
worktree contained staged, unstaged, and untracked work. No ckUtilities build
or acceptance result may depend on that uncommitted state. WP-0 selects and
records a clean, reproducible ckVision baseline before migration code begins.

The selected ckVision baseline must be evaluated against its own authoritative
documents in this order:

1. `VISION.md`
2. `ARCHITECTURE.md`
3. `ROADMAP.md`
4. `DECISIONS.md`
5. the relevant `plans/` document
6. public client documentation and executable tests

ckVision is currently a pre-release library. Its existing surface is already
substantial: retained views, Desktop/Window management, menus, commands and
runtime keymaps, modal dialogs, layouts, themes, headless and POSIX terminals,
provider-backed lists and tables, a materialized/lazy-expansion TreeView,
FlowView, a text-editor core, file dialogs, calendar/clock controls, and a
large widget catalog. Its own status documents say that not every milestone
or platform gate is acceptance-complete. The migration therefore pins proven
capabilities rather than treating the presence of an API as proof of release
readiness.

## 3. Scope

### In scope

- All Turbo Vision based executables:
  - `ck-utilities`
  - `ck-json-view`
  - `ck-du`
  - `ck-find`
  - `ck-config`
  - `ck-edit`
  - `ck-chat`
- Shared UI, hotkey, command, clock/calendar, status-line, about, color, tab,
  and layout facilities under `src/common/` and `include/ck/`.
- CMake, dependency acquisition, CI, tests, packaging, documentation, and
  release metadata affected by the UI framework.
- Necessary enhancements in the separate ckVision repository when a gap meets
  the promotion criteria in section 8.
- Preservation or intentional improvement of all current user workflows.
- Removal of Turbo Vision, curses-only integration, local dependency patches,
  and framework-specific public types after the final cutover.

### Out of scope

- Source or API compatibility with Turbo Vision.
- A wrapper that reproduces `TApplication`, `TView`, numeric messages,
  `TRect`, `TDialog`, or other legacy concepts under different names.
- Pixel-for-pixel imitation where ckVision has a stronger, documented native
  interaction model.
- Moving ckUtilities-specific parsers, search logic, AI/model logic, cloud
  operations, configuration formats, or process launching into ckVision.
- Adding a general task framework, filesystem domain model, Markdown product,
  JSON model, or network stack to ckVision merely because one application
  needs it.
- Reading or deriving behavior from Turbo Vision's own source, fetched source,
  ports, or derivatives.

`cku-win-installer` does not currently depend on Turbo Vision. It remains in
scope for full-suite build and packaging verification, but it does not require
a UI rewrite unless its product requirements change.

## 4. Current migration surface

The repository currently fetches Turbo Vision through
`cmake/FetchTurboVision.cmake`, applies dependency patches through
`scripts/apply_patches.sh`, and links Turbo Vision/curses from the shared UI
layer and most tool targets. Turbo Vision types are also exposed by public
ckUtilities headers. A tracked-source scan found direct legacy UI constructs
across the shared hotkey/layout/UI code and every TUI executable.

| Area | Current responsibility | Migration pressure on ckVision |
|---|---|---|
| Shared `ck_ui` | application clock, calendar, status line, color selector, tabs, window menu | application shell, command presentation, common components, themes, dialogs, Desktop window actions |
| Shared `ck_hotkeys` | platform schemes, runtime lookup, editable custom bindings | namespaced commands, runtime keymaps, chord capture/display, import/export |
| `ck-json-view` | JSON outline, open/close, search, expand-to-level, copy | TreeView, file dialog, clipboard, selection/search presentation, large-tree behavior |
| `ck-utilities` | suite launcher, multi-window tools, ASCII table, calculator, calendar, color selector, event viewer | Desktop/window management, custom views, diagnostics, subprocess boundary |
| `ck-find` | tabbed guided search, complex filters, saved specifications, command preview/execution | TabControl/Wizard, declarative forms, validation, file/directory selection, command enablement |
| `ck-du` | directory tree, file/type tables, sorting and units, background scans, cloud operations | scalable trees/tables, stable identities, progress/cancellation, safe cross-thread posting |
| `ck-config` | application/options browser, hotkey editor, key capture, import/export/reset | PropertyInspector/forms, registry introspection, keymap persistence and conflict diagnostics |
| `ck-edit` | document/file editor plus Markdown analysis and transformations | EditorDocument, TextEditor, FileEditorController, profiles, transactions, close/conflict workflow |
| `ck-chat` | streaming transcript, prompt editor, model/prompt management, downloads, clipboard | FlowView, Memo, large incremental content, progress, cancellation, async lifetime safety |

The present numeric command scheme is also a migration target. Commands move
to ckVision's per-application registry with stable namespaced string keys. The
current scheme permits accidental numeric collisions; for example,
`CopyFullConversation` and `NoOp` are both declared as `1117`. The new design
must make such collisions impossible and must derive menus, status items,
availability, and key bindings from one command declaration.

## 5. Target architecture

ckUtilities adopts an inward-facing dependency model. Framework-dependent
code is deliberately thin and kept out of domain libraries.

```text
Executable composition root
  ├── platform adapters (terminal, filesystem, process, cloud, LLM)
  ├── application services/controllers/use cases
  └── ckVision presentation
        ├── app shell, views, dialogs, view models, command presentation
        └── ckvision::cvision

application services/controllers
  └── domain models and policies

domain models and policies
  └── C++ standard library and explicitly approved domain dependencies
```

### 5.1 Dependency rules

1. Domain libraries contain no `cvision` headers or UI types.
2. Application services know domain interfaces and values, not widgets.
3. Presentation code maps domain state to ckVision models and maps typed UI
   actions back to application use cases.
4. Platform access is behind explicit interfaces and is supplied at the
   composition root. Widgets do not read the environment, filesystem, clock,
   process table, or network directly.
5. Background workers never touch a view. They publish immutable results or
   typed progress through `Application::post()` and respect explicit
   cancellation/lifetime tokens.
6. View ownership follows ckVision's `std::unique_ptr` tree. Non-owning cached
   pointers are narrowly scoped and lifetime-safe; application models outlive
   views that borrow them.
7. Commands use names such as `ck.du.view_files` or `ck.edit.bold`. Menus,
   status lines, command palettes, enabled state, help, and keymaps reference
   the registry rather than duplicate command metadata.
8. `ck_ui` may evolve into a small suite-specific presentation library, but it
   must not mirror ckVision's API. It may hold suite policy and reusable
   compositions such as the branded app shell, shared command declarations,
   about content, and launcher integration.
9. Each executable links exactly one UI runtime. During migration, different
   executables may temporarily use different runtimes, but no executable may
   mix Turbo Vision and ckVision.
10. No committed build logic relies on the local sibling checkout path.

### 5.2 Preserve and strengthen existing core boundaries

Existing framework-independent components are retained and improved rather
than rewritten with the UI:

- `ck_json_view_core`
- `ck_du_core`
- `ck_options`
- `ckai_core`
- `ck_chat_options`
- `ck-find` search model/backend and guided-search policies
- Markdown parsing and Markdown-specific transformations
- launcher/process and cloud-operation policies

Before a UI slice begins, any legacy UI type that leaks into its domain API is
replaced with a domain value, result, callback, or injected interface. Large
single-file applications are decomposed along the architecture above; the
migration must not reproduce monolithic files with ckVision names substituted.

### 5.3 Product behavior, not implementation mimicry

`legacy_tv` is retained as a historical ckUtilities product baseline. It may
be used to inventory commands and to observe user-visible workflows. Each
workflow must be restated as framework-neutral acceptance scenarios before it
is implemented on `master`.

The old implementation is not evidence for a ckVision API or behavior. Any
ckVision enhancement is specified and accepted inside ckVision using its own
authority chain, published standards, documented interaction conventions,
headless scenarios, goldens, and executable evidence. No ckVision change may
claim correctness because it matches a prior framework or a private client
implementation.

## 6. Migration strategy

The program proceeds as architecture-first vertical slices. Every slice
delivers a runnable ckVision application with tests; it does not leave a
half-converted executable. The sequence intentionally increases stress on the
framework:

1. establish reproducible baselines and architecture gates;
2. build the shared native ckVision shell;
3. validate a compact tree-based application (`ck-json-view`);
4. validate a multi-window shell and custom views (`ck-utilities`);
5. validate complex forms and tabs (`ck-find`);
6. validate large data and background work (`ck-du`);
7. validate runtime command/keymap editing (`ck-config`);
8. validate document editing and file safety (`ck-edit`);
9. validate streaming rich content and long-running AI/model work (`ck-chat`);
10. remove the legacy stack and run release acceptance.

If an earlier slice reveals a general ckVision gap needed by later slices,
the gap is completed upstream before dependent application work continues.

## 7. Work packages

### WP-0 — Baseline, support policy, and traceability

Deliverables:

- Select a clean ckVision commit or released package and record its immutable
  identity in the dependency lock/build documentation.
- Produce a tracked-source inventory of Turbo Vision/Curses dependencies,
  legacy public headers, commands, dialogs, custom views, background tasks,
  and platform-specific paths.
- Record current unit/integration/build results on clean build trees.
- Define framework-neutral user journeys for each executable, including
  keyboard-only, mouse, resize, Unicode, error, cancellation, and quit flows.
- Capture deterministic reference fixtures where possible: domain results,
  command-state transitions, serialized configuration, and representative
  screen semantics. Visual baselines describe information hierarchy and
  interaction; they do not require legacy pixel identity.
- Decide and document supported release platforms. Recommended staged policy:
  macOS and Linux are required for the first ckVision suite release; Windows
  becomes a release gate when ckVision's ConPTY backend is accepted. No
  platform may be advertised before its actual gates pass.
- Establish a traceability ledger linking each journey to its domain tests,
  headless UI scenario, platform integration test, and migration WP.

Exit criteria:

- Baseline results are reproducible from clean checkouts.
- The ckVision dependency does not rely on a dirty worktree.
- Every legacy TUI executable has an agreed behavior inventory.
- Known behavior gaps and existing failures are distinguished from migration
  regressions.

### WP-1 — Build boundary and architectural seams

Deliverables:

- Add a package-based ckVision integration and a documented local SDK workflow
  using `CMAKE_PREFIX_PATH`.
- Introduce narrowly scoped build targets for domain, application, platform,
  presentation, and executable composition code.
- Add an architecture check that rejects Turbo Vision/ckVision headers in
  domain targets and rejects forbidden dependency direction.
- Add ckVision headless test support for application-level golden/scenario
  tests without creating a second view graph.
- Convert shared command declarations from integers to registry-owned,
  namespaced command keys and define deterministic keymap persistence mapping.
- Define a framework-neutral cancellation/progress result vocabulary shared by
  `ck-du`, `ck-edit`, and `ck-chat` application services without introducing a
  UI or thread framework into the domain layer.
- Ensure every temporarily dual-stack build keeps the frameworks in separate
  executables.

Exit criteria:

- A minimal independent ckVision consumer configures, builds, runs, installs,
  and passes a headless smoke test through the selected package.
- Domain targets compile without either UI framework.
- Numeric command collisions are structurally impossible in new code.
- No compatibility facade has been introduced.

### WP-2 — Shared native ckVision application shell

Deliverables:

- A suite composition helper that constructs the terminal, clock, clipboard,
  `ckv::ui::Application`, Desktop, theme, and common application shell.
- Shared command and presentation policy for quit, return-to-launcher, about,
  window list, tile/cascade, theme selection, help, and contextual status.
- Runtime-selectable Linux/macOS/Windows/custom keymap schemes mapped onto the
  ckVision command registry, with import/export owned by ckUtilities.
- ckVision-native about, clock, calendar, tab, status, and window-management
  usage. Remove suite duplicates when ckVision already supplies a complete
  capability.
- A suite-specific theme expressed only through semantic ckVision roles, with
  dark, light, mono, and degraded-color verification.
- Standard deterministic shell fixtures at wide, 80x24, narrow, below-minimum,
  and recovery sizes.

Gap checkpoints:

- Verify that keymap enumeration, chord formatting, conflict detection,
  capture, and serialization are sufficient for `ck-config`.
- Verify that suite context help and dynamic status hints can be derived from
  command/focus metadata rather than manual status-line rebuilding.
- If either capability is generically incomplete, finish it in ckVision before
  adding suite-local infrastructure.

Exit criteria:

- A small shell demonstrates every shared policy through the real and
  headless terminals.
- Focus, menu, modal, resize, theme, quit, and launcher-return scenarios have
  deterministic tests.
- Shared code contains no legacy UI type.

### WP-3 — Pilot conversion: `ck-json-view`

Why first: its domain model and search state are already isolated, while its
UI exercises a useful vertical slice: file input, trees, selection, search,
clipboard, status, menus, and expand/collapse commands.

Deliverables:

- Map `ck_json_view_core::Node` through an explicit presentation model into a
  ckVision TreeView; do not put ckVision state in the JSON domain tree.
- Implement open/close, copy, find/next/previous/end-search, and expand-to-level
  commands through the registry.
- Use injected filesystem/file-dialog services and the application clipboard.
- Preserve stable selection through expand/collapse and search updates.
- Add malformed input, deeply nested input, large input, Unicode keys/values,
  narrow terminal, keyboard-only, and mouse scenarios.
- Remove Turbo Vision and Curses from this executable when its slice lands.

Likely ckVision gap to validate:

- TreeView's current 0.1 model is materialized, with lazy expansion but no
  provider-backed stable-identity tree model. The current candidate avoids
  re-flattening unchanged visible rows through a retained materialized cache;
  large JSON and later disk trees may still require a generic provider-backed
  tree with stable node IDs, incremental refresh, and selection/expansion
  preservation. If the measured scenarios need it, this becomes a ckVision
  work package rather than a ckUtilities shadow tree widget.

Exit criteria:

- All listed workflows pass against the same view graph under headless and
  interactive hosts.
- Large/deep input meets the WP-0 responsiveness and memory budgets.
- The executable and its public headers have no Turbo Vision dependency.

### WP-4 — Convert `ck-utilities` launcher and built-in tools

Deliverables:

- Rebuild the launcher as a ckVision multi-window application with a typed
  tool model and injected process-launch service.
- Port the ASCII table, calculator, calendar, color selector, and event viewer
  as native retained views or composed standard widgets.
- Use ckVision Desktop/window commands for new launcher windows, activation,
  close, list, tile, cascade, and focus restoration.
- Keep calculator and ASCII behavior in ckUtilities domain/presentation code;
  reuse ckVision calendar/clock capability rather than fork it.
- Add launcher failure, missing executable, repeated window, close/quit,
  resize, and keyboard/mouse interaction tests.

Likely ckVision gaps to validate:

- A generic color-selection control/dialog may be library-worthy if ckVision's
  theme/color APIs do not provide one.
- A read-only, bounded application event/diagnostic observer may be
  library-worthy if the event viewer cannot be implemented without overriding
  or duplicating dispatch. It must be optional, deterministic, safe against
  re-entrancy, and designed as diagnostics rather than a second event route.

Exit criteria:

- The launcher can start every installed tool and handles failures safely.
- Built-in windows are fully keyboard/mouse operable and resize correctly.
- Any promoted color/diagnostics capability is accepted in ckVision before the
  launcher depends on it.

### WP-5 — Convert `ck-find`

Deliverables:

- Keep search specifications, recipes, persistence, command preview, and the
  search backend framework-independent.
- Express the guided workflow with ckVision TabControl, Wizard/PagedStrip, or
  declarative dialogs according to measured usability; do not reproduce the
  old tab implementation.
- Build filters from typed view models and validators for name/path, text,
  time, size, type, permissions/ownership, traversal, and actions/output.
- Drive enabled state, preview visibility, load/save, execute, and navigation
  from one application controller and command registry.
- Use injected filesystem/process services and present errors as typed
  outcomes.
- Test saved-spec compatibility, validation focus, tab order, narrow layouts,
  long paths/patterns, cancellation, command quoting, and CLI result parity.

Gap checkpoints:

- Validate dialog descriptor coverage for composite fields and conditional
  visibility/enablement.
- Validate TabControl/Wizard focus restoration and responsive layout with this
  many fields.
- Promote only generic form/materialization improvements to ckVision; search
  semantics stay in ckUtilities.

Exit criteria:

- Every search-spec field round-trips through the UI and serialization.
- Generated execution plans match domain golden tests.
- No custom widget bypasses ckVision focus, validation, command, or layout
  mechanisms.

### WP-6 — Convert `ck-du`

Deliverables:

- Present directory data through stable application-owned identities and
  provider-backed views; never make a widget own filesystem traversal.
- Replace hand-drawn list headers with ckVision Table columns when the table
  interaction is a better semantic fit.
- Move scans, recursive aggregation, file-type summaries, and cloud operations
  behind application services with explicit cancellation and immutable/batched
  progress updates.
- Route worker results only through `Application::post()`; closing a window or
  quitting cancels and joins/finishes safely without callbacks into dead views.
- Preserve units, sorting, ignores, thresholds, link/filesystem options,
  copy-path, recursive views, and platform-specific cloud behavior. The
  initial native macOS slice provides selected-directory iCloud download and
  confirmation-gated local-copy eviction as cancellable injected requests;
  provider completion is not conflated with request acceptance.
- Test permission failures, disappearing files, symlink/hard-link policy,
  one-filesystem policy, cancellation races, huge directories, Unicode paths,
  and cloud-operation failure/retry.

Expected ckVision stress point:

- Provider-backed TreeView with stable IDs is likely required here even if the
  JSON pilot can tolerate materialization. Its acceptance must include lazy
  asynchronous child publication, model refresh, selection/expansion
  preservation, bounded visible-row work, and deterministic headless tests.

Do not add a thread pool or disk-scanning abstraction to ckVision merely to
solve this application. ckVision owns safe UI-thread ingress and scalable
generic views; ckUtilities owns scan scheduling and filesystem semantics.

Exit criteria:

- UI work remains bounded by visible/changed data during large scans.
- Cancellation and close/quit are sanitizer-clean under repeated race tests.
- Domain scan results remain covered independently from presentation.

### WP-7 — Convert `ck-config`

Deliverables:

- Present applications and settings using ListView/Table/PropertyInspector and
  typed editors selected from option metadata.
- Use the ckVision command registry as the authoritative source of command
  titles, categories, current/default chords, and conflicts.
- Implement key capture without terminal-specific numeric constants.
- Preserve scheme selection, custom binding edit/clear, reset, import/export,
  application option editing, string-list editing, and config-directory
  actions.
- Make persistence versioned and independent of ckVision's ephemeral numeric
  `CommandId`; persist stable command keys and normalized key chords.
- Add duplicate/conflicting chord, unknown future command, corrupt config,
  import rollback, defaults restore, and disabled/read-only option tests.

Gap checkpoints:

- If command/keymap introspection, normalized chord serialization, or capture
  diagnostics are incomplete, extend ckVision's generic registry/key model.
- If PropertyInspector cannot express a common typed editor cleanly, improve
  that generic composition rather than adding option-specific behavior to the
  framework.

Exit criteria:

- Shared framework and application-specific bindings changed in `ck-config`
  are observed by their target migrated executable after the defined restart
  boundary.
- Persistence is deterministic, forward-tolerant, and collision-safe.
- All app configuration behavior is testable without a terminal or real home
  directory.

### WP-8 — Convert `ck-edit`

Deliverables:

- Separate Markdown analysis/transformations from the legacy editor and expose
  them as deterministic operations over document text/selections.
- Build the UI on ckVision EditorDocument, TextEditor,
  FileEditorController/EditorWindow, editor status model, commands, and close
  protocol.
- Preserve headings, paragraphs, inline styles, code, quotes, lists/tasks,
  links/images/footnotes, tables, indentation, reflow/format, smart-list,
  wrap, find/replace, undo/redo, dirty state, and conflict-safe file actions.
- Supply a Markdown syntax profile from ckUtilities initially. Promote it to a
  ckVision standard profile only after it has a general, self-contained
  grammar contract and acceptance beyond ck-edit-specific formatting rules.
- Model multi-step formatting through document transactions so each user
  action is atomic and undoable.
- Replace polling/idle work with bounded editor work and explicit posted tasks
  where appropriate.
- Add Unicode grapheme, mixed newline, malformed UTF-8 policy, external file
  conflict, large document, wrap/resize, selection, undo, and crash-safe save
  tests.

Gap checkpoints:

- Validate that public document transactions and editor selection/status APIs
  are sufficient for every Markdown transformation.
- If syntax decorations, compound edits, or editor command contexts are
  awkward, improve the general ckVision editor API; never reach into private
  members or fork the editor widget.

Exit criteria:

- Markdown transformations pass framework-independent golden tests.
- File open/save/close behavior is atomic, conflict-aware, and recoverable.
- Interactive editing meets ckVision's steady-state allocation and latency
  expectations on representative documents.

### WP-9 — Convert `ck-chat`

Deliverables:

- Retain `ckai_core`, chat session, model management, prompt management, and
  options as application/domain services with no UI dependencies.
- Compose chat windows from ckVision Window, FlowView/TextView, Memo,
  ScrollViewport, Button, Progress, and declarative dialogs as appropriate.
- Convert parsed Markdown into a ckVision FlowDocument in a dedicated
  presentation adapter; ckVision does not become a Markdown parser.
- Stream inference and download/model progress through bounded, coalesced
  `Application::post()` updates. No worker holds a raw view/dialog pointer.
- Preserve prompt editing, send/cancel, model selection/activation/loading,
  model and prompt management, thinking/analysis visibility, links, copying,
  conversation export, and return-to-launcher.
- Consolidate or delete duplicate/unused model-manager dialog variants rather
  than carrying parallel implementations through the migration.
- Test token streaming, cancellation at every lifecycle point, window close
  during work, model load/download failure, long transcript, Unicode/Markdown,
  link activation, copy, offline configuration, and repeated open/close.

Expected ckVision stress points:

- FlowView supports checked block replacement and incrementally reflows a
  realized final block, which covers the active streaming response without
  reparsing or reflowing prior rich-content blocks. Viewport anchoring,
  stable link identity across broader edits, and text selection/copy remain
  separate generic capabilities to specify only when independently needed.
- Do not implement a ck-chat-only transcript widget that duplicates wrapping,
  scrolling, links, selection, or rich-text rendering. Any future FlowView
  work needs a generic contract and performance tests before adoption.
- Generic progress-dialog cancellation/lifetime behavior may also deserve a
  ckVision composition if it recurs across chat, editor, and disk usage.

Exit criteria:

- Long-running work remains responsive and lifetime-safe under sanitizer race
  scenarios.
- Streaming cost is bounded by changed/visible content rather than total
  transcript size.
- All chat/model/prompt workflows are accepted with fake deterministic
  services before real model integration tests run.

### WP-10 — Legacy removal, packaging, and release acceptance

Implementation state: a ckVision-only cutover configuration now exists and is
verified against the installed integration candidate. It intentionally remains
opt-in while that candidate awaits ckVision trunk acceptance; this avoids
making a dirty checkout or an unlanded commit a production dependency.

Deliverables:

- Remove `cmake/FetchTurboVision.cmake`, Turbo Vision target wiring, Curses
  integration that exists only for Turbo Vision, and `scripts/apply_patches.sh`
  once no target needs them.
- Delete obsolete shared wrappers, `Uses_T*` macros, `T*` public types, numeric
  event/message plumbing, and dead duplicate UI implementations.
- Add a negative source/build gate for `tvision`, `TurboVision`, `Uses_T`,
  legacy UI type families, and removed patch hooks, with narrow documented
  exceptions only for historical prose if retained.
- Update README, COMPILE, IDEA/TODO/PROBLEMS, package descriptions, release
  metadata, CI, screenshots, help, and user configuration migration notes.
- Verify install/package consumers against an installed ckVision package, not
  its source tree.
- Run complete clean Release, Debug, sanitizer, platform, terminal-profile,
  packaging, and provenance acceptance.

Exit criteria:

- All seven TUI executables link `ckvision::cvision` and none link Turbo Vision
  or its Curses support.
- `cku-win-installer` and every non-UI target still build and test.
- Clean builds contain no legacy dependency source or generated patch step.
- The release support statement matches tested reality.
- The traceability ledger has no unaccepted user journey.

## 8. ckVision improvement protocol

Real-world adoption is a first-class source of ckVision requirements, but it
does not weaken ckVision's architecture or acceptance rules.

### 8.1 Gap ledger

Every suspected framework gap receives an entry with:

- stable ID, for example `CV-GAP-001`;
- adopting application and user scenario;
- neutral problem statement, without a requested legacy-shaped API;
- scale, Unicode, input, platform, lifecycle, and performance constraints;
- current ckVision capability and measured failure/awkwardness;
- classification: framework defect, missing generic capability, API
  ergonomics, documentation/example deficiency, or application concern;
- upstream work package/decision/test links; and
- the ckVision commit/package in which it is accepted.

### 8.2 Promotion test

A gap belongs in ckVision only when all of the following are true:

1. It is a reusable TUI/framework capability, not ckUtilities domain policy.
2. It fits ckVision's VISION and layer model without a dependency inversion.
3. Its API can be named and specified without Turbo Vision concepts or private
   ckUtilities types.
4. It preserves explicit ownership, no global mutable state, deterministic
   behavior, and zero mandatory dependencies.
5. It has a coherent behavior for keyboard, mouse, Unicode, resizing,
   degradation, errors, and lifecycle as applicable.
6. It can be demonstrated by a self-contained ckVision example or gallery
   scenario and accepted by ckVision-local tests.

If these conditions fail, the behavior stays in ckUtilities behind an
application interface. This is not permission for a workaround: the client
design must still be clean and native.

### 8.3 Upstream sequence

1. Reproduce the need with the smallest framework-neutral ckVision scenario.
2. Update the relevant ckVision authority document first; record an
   architectural decision when the public surface, dependency rule, or design
   contract changes.
3. Define unit, headless, golden, integration, hostile-input, and performance
   acceptance before or with the implementation.
4. Implement at the lowest correct ckVision layer and update all existing
   callers; pre-1.0 API cleanup is preferred over compatibility overloads.
5. Add or update public documentation and an executable example/gallery case.
6. Run ckVision's mandatory build, CTest, fuzz, sanitizer, benchmark,
   documentation, `provenance_check`, and
   `provenance_checker_self_test` gates as applicable.
7. Land the complete change in ckVision according to its trunk-only practice.
8. Consume a clean, recorded ckVision commit/package in ckUtilities and remove
   any temporary experiment.
9. Run both the generic ckVision acceptance and the adopting application
   scenario. Neither substitutes for the other.

ckVision planning, documentation, and tests for promoted features must remain
self-contained. They may factually name ckUtilities as an adopter, but they
must not depend on this repository's private plans or internals for their
specification or acceptance.

### 8.4 Initial gap hypotheses

These are investigation targets, not pre-approved APIs:

| Candidate | Driven by | Evidence to collect |
|---|---|---|
| Provider-backed TreeView with stable node IDs | JSON and disk trees | memory, refresh cost, selection/expansion stability, async lazy-load semantics |
| FlowView viewport anchoring, selection/copy, and virtualized history | chat transcript | long-session memory, copy/selection and link behavior |
| Generic color-selection control/dialog | launcher and theme tools | reusable color model, truecolor/degraded palette behavior, keyboard/mouse UX |
| Safe event diagnostics observer | launcher event viewer and troubleshooting | no dispatch interference/re-entrancy, bounded recording, deterministic replay text |
| Complete keymap capture/introspection/serialization | config editor and every app | normalized chords, conflicts, unknown commands, scheme switching, portability |
| General progress/cancellation dialog composition | disk, editor, chat | modal/modeless lifecycle, close/quit races, posted progress coalescing |
| Editor extension points for semantic transformations/decorations | Markdown editor | atomic transactions, selection preservation, incremental styling cost |
| More expressive typed dialog/property descriptors | find/config/model dialogs | reusable field kinds, validation/focus, conditional presentation, responsive layout |

## 9. Verification strategy

### 9.1 Test pyramid

1. **Domain unit tests** — parsers, scanners, search plans, configuration,
   Markdown transformations, chat/model state, and launcher policy without UI.
2. **Application/controller tests** — command availability, state transitions,
   cancellation, error mapping, persistence, and view-model publication with
   fake services.
3. **Headless UI scenarios** — the production view graph driven by scripted
   key, text, paste, mouse, resize, timer, and posted-work events.
4. **Golden/semantic frame tests** — normal, wide, narrow, minimum, recovered,
   dark, light, mono, reduced color, Unicode, focus, modal, and error states.
5. **Platform integration tests** — real filesystem/process/clipboard/cloud/LLM
   adapters in bounded fixtures.
6. **PTY/terminal-profile tests** — terminal restoration, resize, input
   protocols, suspend/resume, paste, mouse, and graceful capability reduction.
7. **Install/package tests** — consume installed headers/library/CMake package
   and exercise installed executables.

### 9.2 Quality gates for every slice

- C++20, warnings as errors on all declared compilers.
- Unit, scenario, golden, and integration tests green.
- ASan and UBSan green; TSan or equivalent concurrency evidence for code with
  worker threads.
- Sanitizer clients link the matching sanitizer-enabled ckVision package
  (`CKVISION_SANITIZE`), so instrumentation is consistent across the static
  library boundary.
- No leaked thread, terminal mode, file descriptor, process, callback, or view.
- No UI access from a background thread.
- No wall-clock, environment, filesystem, or locale access below the injected
  platform/application boundary.
- Keyboard-only and mouse paths complete; focus is always visible and
  restorable.
- All displayed untrusted text is safe and Unicode width behavior is covered.
- Public behavior/API changes documented.
- Architecture/include-direction and legacy-dependency negative checks green.
- Any ckVision change passes ckVision's stronger local definition of done.

### 9.3 Performance acceptance

WP-0 records representative workloads and fixes budgets before implementation.
At minimum, measure:

- initial and incremental rendering;
- menu/focus/dialog interaction latency;
- resize storms and theme switching;
- deep/large JSON trees;
- large directory scans and continuously updating tables;
- complex `ck-find` forms and saved-spec loading;
- large Markdown documents and local edits;
- long chat transcripts and token streaming; and
- cancellation/quit under load.

Adopt ckVision's deterministic damage/allocation counters wherever available.
Interactive event-to-present p99 and full-theme-switch results must remain
within ckVision's accepted framework budgets, and application updates must not
introduce work proportional to the total model when only a visible/changed
slice is required. Any exception requires measured evidence and an explicit
decision, not an undocumented threshold increase.

## 10. Cross-repository integration policy

- ckVision and ckUtilities remain independently buildable repositories.
- ckUtilities records the exact ckVision revision/package used for CI and
  releases.
- Local development may stage an SDK from the sibling checkout, but no source
  tree path is embedded in committed targets or installed packages.
- A ckVision change is complete before ckUtilities raises its pin.
- A pin update includes the ckVision change summary, affected gap IDs, and
  both repositories' verification results.
- ckUtilities never carries patches against fetched ckVision source.
- If a ckVision regression blocks the suite, fix and accept it upstream; do
  not fork or monkey-patch the library in the consumer build.
- Public ckUtilities releases identify the ckVision version/commit they were
  built and tested with.

## 11. Risk register

| Risk | Consequence | Mitigation |
|---|---|---|
| ckVision pre-release API churn | repeated client rewrites | pin clean revisions; promote complete vertical gaps; update deliberately, not continuously |
| Dirty local ckVision checkout | irreproducible builds and false capability assumptions | stage only clean SDK/package artifacts; record commit identity |
| Legacy UI/domain entanglement | ckVision-shaped monoliths and unsafe lifetimes | extract framework-neutral controllers/models before each UI slice |
| Compatibility-layer temptation | permanent duplicated architecture | ban legacy API mirrors and mixed-framework executables; review include direction |
| Large materialized trees/transcripts | latency and memory regressions | early JSON pilot, provider-backed tree investigation, streaming benchmarks |
| Background worker/view coupling | use-after-free, hangs, shutdown races | typed result queues, `Application::post()`, cancellation ownership, race tests/sanitizers |
| Scope creep in ckVision | application framework/domain leakage | enforce promotion test and ckVision authority chain |
| Cross-platform readiness mismatch | unsupported release claims | explicit staged support policy and platform-specific exit gates |
| Behavioral drift hidden by visual rewrite | lost workflows or shortcuts | framework-neutral journey ledger and command/state tests |
| Two-repository coordination | temporary local patches and unclear blame | gap IDs, upstream-first completion, immutable pins, dual verification |
| Provenance violation | invalid ckVision contribution | clean-room rule, neutral specs, mandatory provenance review/checkers |

## 12. Program-level definition of done

The conversion program is complete only when all of the following are true:

1. All seven TUI applications run on ckVision and preserve or intentionally
   improve every accepted user journey.
2. No production target, public header, build script, package, or CI job
   depends on Turbo Vision or its Curses integration.
3. Domain/application/platform/presentation boundaries are enforced and
   documented; no large legacy monolith was merely transliterated.
4. Commands, menus, status, help, and runtime keymaps use one collision-safe
   registry model.
5. Background operations are cancellable, lifetime-safe, responsive, and
   tested through deterministic service fakes and sanitizer scenarios.
6. Large data, editor, and streaming workloads satisfy recorded memory,
   allocation, damage, and latency budgets.
7. Every promoted ckVision enhancement is native to its architecture, fully
   accepted in ckVision, documented, demonstrated, and consumed from a clean
   pinned revision/package.
8. The suite passes clean builds, all tests, required sanitizers, supported
   platform/terminal matrices, install/package smoke tests, and negative
   legacy-dependency checks.
9. User configuration migration, keymap behavior, support policy, help, and
   release documentation are complete and accurate.
10. `legacy_tv` is needed only as historical context; production development
    and releases proceed from the ckVision architecture on `master`.

## 13. Immediate next actions

1. Approve this mandate and work-package sequence.
2. Clean or separately preserve the existing ckVision worktree, then select a
   clean baseline commit/package without discarding unrelated work.
3. Execute WP-0's behavior and dependency inventory and create the traceability
   and gap ledgers.
4. Decide the first-release platform gate and the ckVision package/pinning
   mechanism.
5. Implement WP-1's independent package consumer and architecture checks.
6. Build the WP-2 shell and begin the `ck-json-view` pilot.
