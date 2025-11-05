# ck-find Modernization Plan

## Product Vision
- Deliver a friendly Turbo Vision experience that borrows workflows from modern desktop search tools (Spotlight, Alfred, ripgrep front-ends) while keeping power-user depth discoverable, not mandatory.
- Focus on a single-page “Guided Search” that covers 90% of cases, with clear language, sensible defaults, and contextual help.
- Relegate expert-only expressions to an optional “Expert Recipes” sheet that shows prebuilt, human-readable templates (no raw `find` flags).

## Core Use Cases To Support
- Locate documents by name or recent edits (`“proposal in last week”`).
- Find media assets by type, size, and creation window.
- Audit large, stale files for cleanup in a chosen folder tree.
- Track permission/ownership issues for deployment directories.
- Run saved searches (“Smart Searches”) quickly without reconfiguration.

## High-Level Roadmap
1. **Reset UX Baseline**
   - Remove mechanical replication of `find` flags from the primary dialog.
   - Introduce a single-column layout with grouped sections: Location, What, Filters, Actions.
   - Replace checkboxes of jargon with phrasing like “Include hidden system files”.
2. **Guided Filters**
   - Convert time/size inputs into preset dropdowns with optional “custom” toggles; surface natural language descriptions.
   - Bundle file types into named collections (Documents, Images, Audio, Archives, Code) with curated extensions + detector tags.
   - Merge traversal, permission, and action choices into concise toggles with tooltip-like helper text.
3. **Preset & Recipe System**
   - Create “Popular searches” list (Recent documents, Large videos, Duplicates by name) that loads friendly defaults.
   - Allow saving searches with thumbnail descriptions; show them in the Quick tab.
   - Provide “Expert recipes” showing examples (e.g., “Changed in last deploy”) with edit & run options, still masking raw flags.
4. **Progressive Disclosure**
   - Hide advanced tweaks behind context buttons (e.g., “Fine-tune date range…” opens simplified modal with natural language labels).
   - Offer inline explanations for why a filter matters (status bar or help pane).
5. **Visual Clean-Up**
   - Standardize spacing, alignments, and color palette to avoid “Frankenstein” visual style.
   - Ensure consistent shadow/padding treatments for all dialogs and buttons.
   - Add an optional dark/light theme toggle that respects Turbo Vision constraints.
6. **Developer Support**
   - Introduce UI story fixtures to preview filtered states quickly.
   - Document design language + copy style guidelines alongside component usage tips.
   - Add regression tests for preset serialization to avoid reintroducing cryptic flag names.

## Immediate Next Steps
1. Draft wireframes (ASCII/Turbo mockups) for the new Guided Search screen and optional recipe sheet.
2. Audit existing option structures and map each to the new UX buckets (Location, What, Filters, Actions).
3. Produce copy deck with user-centric labels/tooltips; review with stakeholders.
4. Begin implementing layout scaffold: reorganize `SearchNotebookDialog` into new grouped sections.
5. Prototype preset management (load/save) with minimal UI to validate data model changes.

## Risks & Mitigations
- **Over-reduction of power features** → Keep expert recipes and per-filter “Advanced…” modals, but phrase them clearly.
- **Turbo Vision layout limits** → Build reusable components (section headers, inline help) to maintain consistency.
- **Scope creep from legacy parity** → Freeze direct `find` flag exposure to backend only; front-end stays human-readable.

## Step-by-step Actions
1. Inventory every existing filter/action in the current dialog and map it to the new UX buckets (Location, What, Filters, Actions).
2. Draft ASCII mockups for the Guided Search layout and the Expert Recipes sheet, validating spacing within Turbo Vision constraints.
3. Write user-facing copy for each section header, toggle, and helper tip; review with stakeholders or product notes.
4. Refactor `SearchNotebookDialog` scaffolding to create the new grouped sections without populating controls yet (layout shell only).
5. Implement curated preset collections (Documents, Images, Audio, Archives, Code) and wire them into the type filter data model.
6. Replace the legacy time/size modals with the natural-language versions defined in the plan, keeping advanced overrides behind “Fine-tune…” buttons.
7. Build the preset management flow: list built-in presets, allow saving/loading user presets, ensure serialization hides raw flags.
8. Add an Expert Recipes panel that presents human-readable scenarios and loads the matching configuration when selected.
9. Pass through the UI visual cleanup sweep: normalize padding, ensure consistent shadow behavior, and verify alignment across tabs/modals.
10. Document the new design language and add developer notes/tests so regressions back to raw flag labels are caught automatically.
