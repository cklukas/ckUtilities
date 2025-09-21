# Phase 3 — Tool Integrations

**Goal:** Embed AI helpers into existing CkTools TUIs (`ckfind`, `ckdiff`, `ckdu`, `ckrescue`) while keeping workflows offline, auditable, and cancelable. Deliver polished UX with clear CLI equivalents.

## Prerequisites

* Phase 2 completed and verified.
* Shared AI libraries (`ckai_core`, `ckai_embed`) are stable, documented, and shipped in packages.
* Familiarity with the UX rules in `docs/ai-design.md` §7 and the TUI architecture described in `README.md` and `COMPILE.md`.

## Implementation Steps

1. **Common UI glue**
   1. Add reusable Turbo Vision widgets to `lib/ckui` for displaying AI suggestions, streaming output, and CLI command previews.
   2. Provide APIs for canceling in-flight generations (`Esc`) and showing stats lines (`tok/s`, context usage, temperature, seed).
   3. Write unit/UI tests verifying that widgets surface CLI equivalents and respond to cancel events.

2. **`ckfind` enhancements**
   1. Implement “Explain match” action: capture the selected result, call a new prompt template in `ckai_core`, and render the explanation with references to the matched predicates.
   2. Implement NL→predicate suggestion: collect user input, run through the model, and output the proposed `find` command. Require explicit confirmation before applying.
   3. Update integration tests to cover both features in headless mode (simulate input via existing test harnesses).

3. **`ckdiff` helpers**
   1. Add a panel/button to summarize the current diff hunk, using the prompt from `docs/ai-design.md §16`.
   2. Implement commit message drafting: pre-fill subject/body, but keep them editable and require user confirmation before staging/applying.
   3. Ensure tests verify deterministic output for fixed seeds and that commands are logged with CLI equivalents.

4. **`ckdu` safe-delete hints**
   1. Provide a passive sidebar listing suggested cache/log directories based on AI analysis. Never auto-delete; only tag entries.
   2. Integrate with the stats widget to show inference cost and allow cancellation.
   3. Add tests confirming suggestions are informational-only (no delete actions emitted).

5. **`ckrescue` plan explainer**
   1. When a rescue plan is generated, call into AI to produce a human-readable summary of each step (image, verify, logs).
   2. Display the summary alongside the exact commands (from the existing plan generator) to reinforce transparency.
   3. Provide integration tests using canned rescue plans to ensure explanations mention all critical steps.

6. **Accessibility & safety review**
   1. Audit keybindings, dialogs, and confirmations to ensure AI actions remain opt-in and reversible.
   2. Update manuals (`docs/tools/*.md`) to describe new AI panes and reference offline configuration.
   3. Document fallback behavior when AI is disabled (e.g., show tooltips pointing users to `ckmodel add`).

7. **Build, packaging, CI**
   1. Confirm no new external runtime dependencies were introduced.
   2. Update automated UI/integration tests and ensure they run in GitHub Actions within existing timeouts.
   3. Refresh screenshots or asciicasts if documentation relies on them (store assets under `docs/` per existing conventions).

## Validation Checklist

* `cmake --preset dev`
* `cmake --build build/dev -t ckfind ckdiff ckdu ckrescue`
* `ctest --test-dir build/dev --output-on-failure -L ai-integration`
* Manual smoke test each TUI to verify cancelability and CLI previews.
* `cmake --build build/pkg -t package`

## Deliverables

* Shared AI UI widgets with cancel/stats support.
* Integrated AI features in `ckfind`, `ckdiff`, `ckdu`, and `ckrescue` that respect offline + safety constraints.
* Updated documentation and tests demonstrating the new workflows.
* Passing CI builds including the expanded integration suites.
