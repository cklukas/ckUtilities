---
name: ckvision-guide-screenshots
description: Add or refresh ckUtilities tool-guide screenshots using the real ckVision application UI and the deterministic documentation capture pipeline. Use when a tool guide needs a current screenshot; do not use for illustrative or AI-generated artwork.
metadata:
  short-description: Generate verified ckVision guide images
---

# ckVision guide screenshots

Create screenshots that show the current ckUtilities UI, not mockups. Each
tool guide in `docs/tools/` should embed its own SVG from
`docs/generated/screenshots/`.

## Capture workflow

1. Inspect the existing capture adapter at
   `tools/docgen/capture_ckutilities_screenshots.cpp` and its build wiring in
   `tools/docgen/CMakeLists.txt`. Add the tool's capture there rather than
   hand-authoring SVG.
2. Construct the real application with ckVision's `HeadlessTerminal` and
   render its display through ckVision's `frame_svg.cpp` helper. Drive a useful
   screen state: for example a populated result list, settings table, or an
   editor with document content.
3. Keep capture fixtures deterministic. Use explicit in-memory data and fixed
   values for clocks, paths, options, prompts, models, and responses. Never
   read a developer's configuration, home directory, network state, or live
   filesystem to produce a documentation image.
4. When a new app participates, add only the CMake include directories and
   targets needed by the capture executable. The capture tooling is opt-in and
   must not become a product or package dependency.
5. Regenerate the SVGs with `tools/docgen/generate_screenshots.sh`, providing
   a clean installed ckVision SDK through `CKTOOLS_CKVISION_PREFIX` and the
   matching clean source checkout through `CKTOOLS_CKVISION_SOURCE_DIR`.
   The script enforces the currently pinned ckVision revision.
6. Visually inspect the changed SVG after rendering it if needed, then embed it
   close to the introduction of the corresponding `docs/tools/<tool>.md` guide
   with concise, accurate alt text.
7. Regenerate a second time and confirm the screenshot directory is unchanged.
   The CI screenshot gate uses the same rule; leave the working tree clean
   after the final regeneration.

## Project constraints

- Capture real ckVision views only. Do not substitute generated artwork,
  manual SVG drawings, terminal text mockups, or screenshots from an outdated
  Turbo Vision version.
- Use the current pinned ckVision source revision. Do not replace it with an
  older public snapshot to make a capture compile, and do not alter a dirty
  local ckVision checkout.
- Preserve the existing CI consistency check for
  `docs/generated/screenshots/`. Any intentional visual change must be
  regenerated and committed with its guide and capture-adapter changes.

## Verification

Run the generator successfully and use `git diff -- docs/generated/screenshots`
to review intentional changes. For a change destined for GitHub, ensure the CI
jobs regenerate the same files on both macOS and Linux before declaring the
documentation update complete.
