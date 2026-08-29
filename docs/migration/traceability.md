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
| CKU-FIND | `ck-find` | compose, validate, persist, preview, and execute a search specification | WP-5 | native guided form, JSON-backed saved-search adapter, joinable injected execution service, UI-thread completion, and cancellation implemented; destructive action confirmation remains |
| CKU-DU | `ck-du` | scan, navigate, sort, inspect, cancel, and execute cloud actions | WP-6 | native TreeView/table, injected joinable directory scanning, progress, UI-thread snapshot delivery, and cancellation implemented; file lists and cloud actions remain |
| CKU-CONFIG | `ck-config` | inspect/edit/reset/import/export configuration and key bindings | WP-7 | native injected-registry inspector/editor with typed edit/reset and injected JSON-default save/reload/import/export implemented; keymap capture/conflicts and cross-app reload remain |
| CKU-EDIT | `ck-edit` | edit, format, search, save, resolve conflict, and close Markdown documents | WP-8 | native injected-file editor with open/save/save-as and explicit dirty Save/Discard/Cancel close implemented; Markdown transformations, profiles, and richer external-conflict resolution remain |
| CKU-CHAT | `ck-chat` | edit/send/cancel prompts, stream/copy responses, manage models/prompts | WP-9 | native FlowView transcript, prompt dialog, injected streaming response service, cancellation, worker adapter, UI-thread lifetime guard, and shared Markdown-to-FlowView adaptation implemented; models, prompts, and richer progress remain |
| CKU-RELEASE | packaging | build, test, install, package, and run every executable | WP-10 | inventory captured |

Every row will gain links to domain tests, deterministic headless scenarios,
interactive platform tests, and release evidence before its work package can
close.

## Initial ckVision gap ledger

| Gap ID | Neutral requirement | First validating slice | State |
|---|---|---|---|
| CV-GAP-001 | A result controller needs to reveal and select a stable node in a materialized hierarchy without synthesizing input. Large changing hierarchies may additionally need provider-backed incremental refresh. | CKU-JSON, CKU-DU | `TreeView::reveal_and_select` implemented in ckVision candidate `d54f65f`; provider-backed scale work remains open |
| CV-GAP-002 | A long streaming rich-text document may need active-block updates, bottom anchoring, selection/copy, and bounded relayout. | CKU-CHAT | investigate in WP-9 |
| CV-GAP-003 | Runtime keymap editing may need normalized chord capture, conflict diagnostics, and persistence based on stable command names. | CKU-CONFIG | investigate in WP-2/WP-7 |
| CV-GAP-004 | The initial color-selection workflow uses ckVision's existing declarative radio-form dialog; bounded application diagnostics use its injected diagnostics observer. No framework enhancement is currently justified. | CKU-LAUNCH | resolved for the initial launcher slice; reassess only if a reusable palette control has independent adopters |

An entry becomes a ckVision implementation only after it passes the promotion
test in [the migration roadmap](../../tv_to_ckvision.md#82-promotion-test).
