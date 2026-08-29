# ckVision migration traceability ledger

This is the living WP-0 record linking each legacy product area to a
framework-neutral acceptance suite and its migration work package. It records
what users must be able to do, not how Turbo Vision implemented it.

## Application inventory

| ID | Product area | Core acceptance journeys | Owner WP | Status |
|---|---|---|---|---|
| CKU-SHELL | shared shell | command invocation, focus, menu/status/help, theme, resize, quit | WP-2 | inventory captured |
| CKU-JSON | `ck-json-view` | open/close, navigate, expand level, find, copy | WP-3 | inventory captured |
| CKU-LAUNCH | `ck-utilities` | launch tool, multi-window utilities, calculator, ASCII, color, event diagnostics | WP-4 | inventory captured |
| CKU-FIND | `ck-find` | compose, validate, persist, preview, and execute a search specification | WP-5 | inventory captured |
| CKU-DU | `ck-du` | scan, navigate, sort, inspect, cancel, and execute cloud actions | WP-6 | inventory captured |
| CKU-CONFIG | `ck-config` | inspect/edit/reset/import/export configuration and key bindings | WP-7 | inventory captured |
| CKU-EDIT | `ck-edit` | edit, format, search, save, resolve conflict, and close Markdown documents | WP-8 | inventory captured |
| CKU-CHAT | `ck-chat` | edit/send/cancel prompts, stream/copy responses, manage models/prompts | WP-9 | inventory captured |
| CKU-RELEASE | packaging | build, test, install, package, and run every executable | WP-10 | inventory captured |

Every row will gain links to domain tests, deterministic headless scenarios,
interactive platform tests, and release evidence before its work package can
close.

## Initial ckVision gap ledger

| Gap ID | Neutral requirement | First validating slice | State |
|---|---|---|---|
| CV-GAP-001 | A large changing hierarchy may need stable node identities, lazy publication, and selection/expansion preservation without materializing the entire tree. | CKU-JSON, CKU-DU | investigate in WP-3 |
| CV-GAP-002 | A long streaming rich-text document may need active-block updates, bottom anchoring, selection/copy, and bounded relayout. | CKU-CHAT | investigate in WP-9 |
| CV-GAP-003 | Runtime keymap editing may need normalized chord capture, conflict diagnostics, and persistence based on stable command names. | CKU-CONFIG | investigate in WP-2/WP-7 |
| CV-GAP-004 | Reusable color selection and bounded event diagnostics may be useful generic controls. | CKU-LAUNCH | investigate in WP-4 |

An entry becomes a ckVision implementation only after it passes the promotion
test in [the migration roadmap](../../tv_to_ckvision.md#82-promotion-test).
