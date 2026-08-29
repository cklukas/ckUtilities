# ckVision migration traceability ledger

This is the living WP-0 record linking each legacy product area to a
framework-neutral acceptance suite and its migration work package. It records
what users must be able to do, not how Turbo Vision implemented it.

## Application inventory

| ID | Product area | Core acceptance journeys | Owner WP | Status |
|---|---|---|---|---|
| CKU-SHELL | shared shell | command invocation, focus, menu/status/help, theme, resize, quit | WP-2 | initial native shell implemented; headless smoke test passing |
| CKU-JSON | `ck-json-view` | open/close, navigate, expand level, find, copy | WP-3 | native pilot implemented; headless scenario passing |
| CKU-LAUNCH | `ck-utilities` | launch tool, multi-window utilities, calculator, ASCII, color, event diagnostics | WP-4 | initial native launcher conversion implemented: calendar, ASCII table, calculator, color selector, and bounded application diagnostics all run as native Desktop surfaces |
| CKU-FIND | `ck-find` | compose, validate, persist, preview, and execute a search specification | WP-5 | native guided form, JSON-backed saved-search adapter, joinable injected execution service, UI-thread completion, cancellation, and explicit Yes/No/Cancel-confirmed deletion of matching regular files or symbolic links implemented. Directory deletion and custom command execution remain withheld; the latter needs a separately sandboxed policy |
| CKU-DU | `ck-du` | scan, navigate, sort, inspect, cancel, and execute cloud actions | WP-6 | native TreeView/table, injected joinable directory scanning, file-list Table, progress, UI-thread delivery, and cancellation implemented. The selected-directory cloud boundary now offers a cancellable macOS Foundation iCloud download request and an explicit-confirmation local-copy eviction request; request acceptance, failure/retry, and unsupported-platform behavior are surfaced honestly. Recursive/provider policy remains |
| CKU-CONFIG | `ck-config` | inspect/edit/reset/import/export configuration and key bindings | WP-7 | native injected-registry inspector/editor with typed edit/reset and injected JSON-default save/reload/import/export implemented. Its shortcut table now captures typed normalized chords, reports occupied bindings before an explicit replacement, and persists by stable command key; each native executable reloads shared `ckv.*` bindings at startup. A suite-wide catalog for editing another application's commands, schemes, and import rollback remains |
| CKU-EDIT | `ck-edit` | edit, format, search, save, resolve conflict, and close Markdown documents | WP-8 | native injected-file editor with open/save/save-as, explicit dirty Save/Discard/Cancel close, application-owned Markdown syntax profile, and a conservative Markdown-whitespace normalization command committed as one revision-bound document transaction. It preserves fenced/indented code and intentional hard breaks. A save conflict preserves the external file and requires an explicit Save As, reload/discard, or continue-editing choice; broader transformations remain |
| CKU-CHAT | `ck-chat` | edit/send/cancel prompts, stream/copy/export responses, manage models/prompts | WP-9 | native FlowView transcript, prompt dialog, injected streaming response service, cancellation, worker adapter, UI-thread lifetime guard, shared Markdown-to-FlowView adaptation, and injected transcript export implemented. Prompt selection/create/edit/default restoration/confirmed custom deletion and model selection/deactivation/confirmed local deletion use framework-neutral adapters over `ckai_core`, with active prompt/model IDs and completed prior turns propagated into response requests. The same boundary offers catalog-backed background download, cancellation, cached UI reads, rate-limited posted progress/completion, and worker-owned active-model loading plus cancellable chunk generation; long-session/runtime performance evidence remains |
| CKU-RELEASE | packaging | build, test, install, package, and run every executable | WP-10 | inventory captured |

Every row will gain links to domain tests, deterministic headless scenarios,
interactive platform tests, and release evidence before its work package can
close.

## Initial ckVision gap ledger

| Gap ID | Neutral requirement | First validating slice | State |
|---|---|---|---|
| CV-GAP-001 | A result controller needs to reveal and select a stable node in a materialized hierarchy without synthesizing input. Large changing hierarchies may additionally need provider-backed incremental refresh. | CKU-JSON, CKU-DU | `TreeView::reveal_and_select` implemented in ckVision candidate `d54f65f`; provider-backed scale work remains open |
| CV-GAP-002 | A long streaming rich-text document may need active-block updates, bottom anchoring, selection/copy, and bounded relayout. | CKU-CHAT | investigate in WP-9 |
| CV-GAP-003 | Runtime keymap editing needs a focused control that captures a normalized chord without allowing that chord to dispatch as a command. Conflict diagnostics and persistence remain application policy. | CKU-CONFIG | `KeyChordCapture` implemented in ckVision candidate `0bef919`; ckUtilities owns stable-key persistence, conflict confirmation, and shared-binding reload |
| CV-GAP-004 | The initial color-selection workflow uses ckVision's existing declarative radio-form dialog; bounded application diagnostics use its injected diagnostics observer. No framework enhancement is currently justified. | CKU-LAUNCH | resolved for the initial launcher slice; reassess only if a reusable palette control has independent adopters |
| CV-GAP-005 | Declarative forms need a labeled multi-line text field that retains ordinary Memo editing, returns a complete text value, and receives accept-time validation/error styling. | CKU-CHAT | `FieldKind::Memo`, `memo_rows`, typed result wiring, and dedicated Memo invalid roles implemented in ckVision candidate `351592d` |

An entry becomes a ckVision implementation only after it passes the promotion
test in [the migration roadmap](../../tv_to_ckvision.md#82-promotion-test).
