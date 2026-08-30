# Native ckVision executables

This is the current dual-stack cutover map. Each native executable links only
the ckVision presentation stack and its framework-independent application
dependencies; it does not mix ckVision with Turbo Vision in the same process.

| Product | Native executable | Current native scope | Remaining scope before cutover |
|---|---|---|---|
| Utilities launcher | `ck-utilities-ckvision` | launcher, calendar, ASCII table, calculator, color selector, bounded diagnostics, staged missing-tool failure, and repeat/close/quit/resize/input window lifecycle | release acceptance |
| JSON view | `ck-json-view-ckvision` | open, stable-ID provider-backed tree navigation, find, copy, expansion, and teardown-safe shell removal | application-scale refresh/memory measurement and final cutover |
| Find | `ck-find-ckvision` | guided specification form, command preview, save/load, background execution/cancellation, and confirmed deletion of matching regular files or symbolic links | sandboxed custom-command policy and parity acceptance |
| Disk usage | `ck-du-ckvision` | background cancellable snapshot scan, stable-ID provider-backed tree, selected-directory and file-list tables; selected-directory iCloud download and confirmation-gated local-copy eviction via a cancellable macOS adapter | recursive/provider policy, application-scale refresh/memory, parity acceptance |
| Config | `ck-config-ckvision` | injected option registry inspect/edit/reset/save/reload/import/export with transactional failed-import rollback, plus a keyboard-only normalized shortcut editor with explicit collision replacement, suite-wide target-application command catalog, and built-in-default/personal shortcut scheme selection | final acceptance |
| Edit | `ck-edit-ckvision` | native document open/save/save-as, dirty Save/Discard/Cancel close, Markdown syntax profile, transactional whitespace normalization, revision undo/redo and viewport-only word wrap, bold/italic/strikethrough/delimiter-safe inline-code selection toggles, heading 1–6 toggles that protect code, task-list state, block-quote level, basic bullet/ordered-list toggles, conservative nested-list indent/outdent and smart-list continuation on Enter, destination-prompted link/image insertion/removal plus atomic footnote reference/definition insertion, native literal find/replace dialogs with atomic replace-all, conservative 80-code-point paragraph reflow, Unicode selection restoration, malformed-input preservation, large-document resize acceptance, and explicit external-save-conflict recovery | workflow-depth and final acceptance |
| Chat | `ck-chat-ckvision` | FlowView transcript, Markdown adaptation, streaming/cancel/copy/export, conversation-context propagation, prompt and model lifecycle, cancellable background downloads with rate-limited UI-thread progress, and worker-owned active-model loading/generation backed by `ckai_core`; the live transcript bounds rendering to its latest 160 messages while retaining complete export/context, and response chunks render promptly then coalesce through ckVision's checked active-block replacement with incremental realized-tail reflow | real-model runtime evidence, parity acceptance |

The exact package and integration-candidate state are recorded in
[the baseline record](ckvision-baseline.md) and
[the traceability ledger](traceability.md). This document does not authorize
removing the legacy runtime: that occurs only when each remaining journey has
accepted evidence under WP-10.

Every native controller places its application command registrations in a
`SuiteCommandScope`. This withdraws controller-capturing callbacks before the
controller's dialogs, shell, and borrowed view models are destroyed, so the
longer-lived application registry cannot dispatch into a stale controller.

`CKTOOLS_CKVISION_CUTOVER=ON` is the release-layout rehearsal: it composes
only these native products and framework-neutral cores, publishes the standard
product names, and excludes the legacy UI runtime from configuration and
installation. It remains opt-in while the selected ckVision candidate awaits
trunk acceptance and a CI-consumable installed package.

With `CKTOOLS_VERIFY_CKVISION_INSTALL=ON`, the
`verify_ckvision_cutover` target also rejects legacy runtime linkage, installed
legacy artifacts, and legacy references in the product's public headers.
