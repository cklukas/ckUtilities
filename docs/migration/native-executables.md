# Native ckVision executables

This is the current dual-stack cutover map. Each native executable links only
the ckVision presentation stack and its framework-independent application
dependencies; it does not mix ckVision with Turbo Vision in the same process.

| Product | Native executable | Current native scope | Remaining scope before cutover |
|---|---|---|---|
| Utilities launcher | `ck-utilities-ckvision` | launcher, calendar, ASCII table, calculator, color selector, bounded diagnostics, staged missing-tool failure, and repeat/close/quit/resize/input window lifecycle | release acceptance |
| JSON view | `ck-json-view-ckvision` | open, stable-ID provider-backed tree navigation, find, copy, expansion, and teardown-safe shell removal; a 2,048-entry search and a 12,000-entry-to-two-node transactional reload remain within the visible-frame cap while stale provider indexes are released | final cutover |
| Find | `ck-find-ckvision` | guided specification form, command preview, save/load, background execution/cancellation, and confirmed deletion of matching regular files or symbolic links | sandboxed custom-command policy and parity acceptance |
| Disk usage | `ck-du-ckvision` | background cancellable snapshot scan, stable-ID provider-backed tree, selected-directory and file-list tables; selected-directory iCloud download and confirmation-gated local-copy eviction via a cancellable macOS adapter. A 2,048-entry-to-four-node refresh remains within the visible-frame cap while stale provider indexes are released | recursive/provider policy, parity acceptance |
| Config | `ck-config-ckvision` | injected option registry inspect/edit/reset/save/reload/import/export with transactional failed-import rollback, plus a keyboard-only normalized shortcut editor with explicit collision replacement, suite-wide target-application command catalog, and built-in-default/personal shortcut scheme selection | final acceptance |
| Edit | `ck-edit-ckvision` | native document open/save/save-as, dirty Save/Discard/Cancel close, Markdown syntax profile, transactional whitespace normalization, revision undo/redo and viewport-only word wrap, bold/italic/strikethrough/delimiter-safe inline-code selection toggles, heading 1–6 toggles that protect code, task-list state, block-quote level, basic bullet/ordered-list toggles, conservative nested-list indent/outdent and smart-list continuation on Enter, destination-prompted link/image insertion/removal plus atomic footnote reference/definition insertion, native literal find/replace dialogs with atomic replace-all, conservative 80-code-point paragraph reflow, Unicode selection restoration, malformed-input preservation, large-document resize acceptance, and explicit external-save-conflict recovery | workflow-depth and final acceptance |
| Chat | `ck-chat-ckvision` | FlowView transcript, profile-controlled Markdown links, streaming/cancel/copy/export, conversation-context propagation, profile-synchronized prompt and model lifecycle, structured response failures (including immediate unavailable-model rejection), cancellable background downloads with rate-limited UI-thread progress, and worker-owned active-model loading/generation backed by `ckai_core`; the live transcript bounds rendering to its latest 160 messages while retaining complete export/context, and response chunks render promptly then coalesce through ckVision's checked active-block replacement with incremental realized-tail reflow. An opt-in `CKTOOLS_REAL_MODEL` scenario exercises actual worker load, streaming, structured completion, and teardown when supplied a local GGUF | recorded real-model runtime evidence, parity acceptance |

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

A clean macOS Debug cutover against that installed candidate passes the
92-test suite, an independent Debug package consumer, staged executable smoke
tests, launcher-child discovery, and the legacy-negative install gate. This is
local configuration evidence only; the cross-platform release matrix and the
remaining product journeys are still required before cutover.

With `CKTOOLS_VERIFY_CKVISION_INSTALL=ON`, the
`verify_ckvision_cutover` target also rejects legacy runtime linkage, installed
legacy artifacts, and legacy references in the product's public headers.
With `CKTOOLS_VERIFY_CKVISION_ARCHIVE=ON` in the cutover configuration,
`verify_ckvision_archive` runs CPack, extracts the TGZ, repeats those
product-tree checks against the delivered archive, and rejects build-only
GTest/GMock payload.
