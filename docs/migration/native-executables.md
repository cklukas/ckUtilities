# Native ckVision executables

This is the current dual-stack cutover map. Each native executable links only
the ckVision presentation stack and its framework-independent application
dependencies; it does not mix ckVision with Turbo Vision in the same process.

| Product | Native executable | Current native scope | Remaining scope before cutover |
|---|---|---|---|
| Utilities launcher | `ck-utilities-ckvision` | launcher, calendar, ASCII table, calculator, color selector, bounded diagnostics | failure/repeat-window interaction coverage and release acceptance |
| JSON view | `ck-json-view-ckvision` | open, tree navigation, find, copy, expansion | large/provider-backed tree measurement and final cutover |
| Find | `ck-find-ckvision` | guided specification form, command preview, save/load, background non-destructive execution, and cancellation | destructive action confirmation and parity acceptance |
| Disk usage | `ck-du-ckvision` | background cancellable snapshot scan, tree, selected-directory table | file lists, cloud actions, parity acceptance |
| Config | `ck-config-ckvision` | injected option registry inspect/edit/reset/save/reload | keymap capture/conflicts, import/export, cross-app reload |
| Edit | `ck-edit-ckvision` | native document open/save/save-as | Markdown transactions/profiles, conflict and close decisions |
| Chat | `ck-chat-ckvision` | FlowView transcript, prompt, streaming/cancel/copy, injected worker response service | Markdown, models/prompts, richer progress |

The exact package and integration-candidate state are recorded in
[the baseline record](ckvision-baseline.md) and
[the traceability ledger](traceability.md). This document does not authorize
removing the legacy runtime: that occurs only when each remaining journey has
accepted evidence under WP-10.
