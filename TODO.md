# ck-find Notebook UI Redesign – Implementation Plan

This plan replaces the “scrollable drawers” concept with a tabbed experience built around `TNotebook`. Each tab keeps the surface area manageable while still exposing every advanced filter.

---

## 0. Prerequisites
- [x] **Tabs widget availability** – Turbo Vision (magiblot) does *not* ship `TNotebook`/`TTabCollection`. We need to implement a lightweight tab control (e.g., `TTabSwitcher` + `TGroup` stack) inside ck-find before proceeding.
- [x] Helper wrappers – Existing `ck/ui` headers cover clocks/status only; add a `TabPageView` base (focus management + accelerators) to reuse across notebook pages.
- [x] Reserve command IDs / hotkeys in `src/common/hotkeys/default_schemes.cpp` for tab switching (Alt+1…Alt+6) and the preview toggle once the tab control API is ready.

---

## 1. Dialog Shell & Notebook

### 1.1: `SearchNotebookDialog`
- [x] Introduce `SearchNotebookDialog` deriving from `TDialog` backed by the new `ck::ui::TabControl`.
- [x] Mount the tab control so it fills the client area above a bottom button row.
- [x] Place bottom-row buttons (`Preview…`, `Search`, `Cancel`); add Reset once behaviour is defined.
- [x] Provide a reusable helper for creating tab pages (e.g., `createTabPage` or similar) to simplify future tabs.

### 1.2: Tab Lineup (initial titles)
1. **Quick Start**
2. **Content & Names**
3. **Dates & Sizes**
4. **Types & Ownership**
5. **Traversal**
6. **Actions**

*(Tab titles must be short for Turbo Vision’s tab strip; we can tweak wording later.)*

- [x] Mount initial tabs (Quick, Content, Dates, Types, Traversal, Actions) using the custom tab control and command bindings.

### 1.3: Navigation
- [x] Map Ctrl+Tab / Ctrl+Shift+Tab to cycle the notebook (handled by the custom tab control).
- [x] Add status-line entries (Alt+1…Alt+6) to jump directly to tabs via new commands (`cmTabQuickStart`, etc.).
- [x] When switching pages, call a tab-specific `setInitialFocus()` to move cursor to the primary field (Quick Start now selects the name field on activation).

---

## 2. Tab Content Breakdown

### 2.1: Quick Start
- [ ] Controls:
  - Start location `TInputLine` + “Browse…” `TButton` (reuse browse handler).
  - Checkboxes: Include subfolders, Include hidden items.
  - Search text `TInputLine` with `TComboBox` selector (“Search contents”, “Search names”, “Both”).
  - Include patterns / Exclude patterns `TInputLine`s.
  - Quick type filter cluster (`TCluster`): All files / Documents / Images / Audio / Archives / Custom → hitting “Custom” activates Type tab.
  - Search name field (optional label for saved specs).
- [ ] Shortcuts: Alt+L (location), Alt+S (search text), Alt+I (include patterns), Alt+E (exclude patterns), Alt+Y (type filter).
- [ ] Buttons: “Go to advanced filters…” opens Content & Names tab (use `parent->selectPage(index)`).

### 2.2: Content & Names Tab
- [ ] Section A – Text Matching:
  - Radio buttons for mode (Contains, Whole word, Regular expression).
  - Checkboxes: Match case, Search file contents, Search file names, Allow multiple terms, Treat binary as text.
- [ ] Section B – Name/Path filters:
  - For each matcher (`-name`, `-iname`, `-path`, `-ipath`, `-regex`, `-iregex`, `-lname`, `-ilname`), pair a checkbox and `TInputLine`.
  - Buttons: “Copy text query”, “Clear all”.
- [ ] Section C – Prune:
  - Checkbox “Skip matching folders (prune)”.
  - Radio to choose prune test (name/iname/path/ipath/regex/iregex).
  - Input for prune pattern.
- [ ] Section D – Extensions & Detectors:
  - Checkbox “Filter by file extensions” + input (comma separated).
  - Checkbox “Use detector tags” + input.

### 2.3: Dates & Sizes Tab
- [ ] Layout: Two columns (left: dates, right: sizes).
- [ ] Dates:
  - Preset `TRadioButtons`: Any time, Past day, Past week, etc., Custom range.
  - Checkboxes: Modified, Created, Accessed.
  - `TInputLine`s for Custom From / To (enabled when preset is Custom).
  - Advanced area toggle button (“Advanced expressions…”) opens either inline panel or temporary modal showing `-mtime`, `-mmin`, `-newer` fields (reuse existing code).
- [ ] Sizes:
  - Inputs for Minimum size and Maximum size (with unit hints).
  - Exact size expression field.
  - Checkboxes: Inclusive range, Include zero-byte, Treat directories as files, Use decimal units, Match empty entries.

### 2.4: Types & Ownership Tab
- [ ] Types:
  - Primary `TCheckBoxes` for file types (Regular files, Folders, Symlinks, Block devices, etc.) – map to `-type`.
  - Secondary `TCheckBoxes` for `-xtype`.
- [ ] Extensions/Detectors: duplicated from Content tab but optionally hidden unless user wants deeper control (or present summary plus “Edit…” button).
- [ ] Permissions:
  - Simple checkboxes: Readable, Writable, Executable.
  - `TRadioButtons` for permission mode (Exact / All bits / Any bit) and `TInputLine` for octal/symbolic value.
- [ ] Ownership:
  - Fields for Owner (name), UID, Group, GID.
  - Checkboxes: Orphaned owner (`-nouser`), Orphaned group (`-nogroup`).

### 2.5: Traversal Tab
- [ ] Symlink handling radio buttons: Physical, CLI only, Everywhere.
- [ ] Checkboxes: Depth-first, Stay on filesystem, Assume no leaf, Ignore readdir race, Day start.
- [ ] Numeric inputs: Max depth, Min depth.
- [ ] File inputs: Files-from (with browse), Samefile target.
- [ ] Text inputs: Filesystem type, Link count, Inode number.
- [ ] `TStaticText` footnote describing performance impact.

### 2.6: Actions Tab
- [ ] Output options:
  - Checkboxes: Print results, Print with NUL, Verbose list, Delete matches, Stop after first.
- [ ] Exec command:
  - Checkbox “Run command on matches”.
  - `TInputLine` for command template (with `{}` placeholder).
  - Radio group for command variant (`-exec`, `-execdir`, `-ok`, `-okdir`).
  - Checkbox “Group with + terminator”.
- [ ] File outputs:
  - Individual toggles for `-fprint`, `-fprint0`, `-fls`, `-printf`, `-fprintf`.
  - Each toggle reveals path and/or format input plus “append” checkbox.
- [ ] Warning text whenever destructive options enabled.

---

## 3. Supporting Infrastructure

### 3.1: Command IDs & Hotkeys
- [x] Add notebook-specific commands (`cmTabQuickStart` … `cmTabActions`, `cmTogglePreview`, `cmOpenAdvancedDates`).
- [x] Update status line to show tab accelerators; ensure they respect current hotkey scheme.

### 3.2: Shared State
- [ ] Replace `SearchDialogData` with `SearchNotebookState` containing fields grouped per tab.
- [ ] Each tab gets `populateFromState()` and `collectIntoState()` methods.
- [ ] On OK: translate state back into `SearchSpecification`.
- [ ] On cancel: discard state changes.
- [ ] When showing dialog with an existing spec, set notebook page to the highest-priority tab that has active filters (e.g., if time filters enabled, jump to Dates & Sizes, otherwise remain on Quick Start).

### 3.3: Legacy Dialog Bridge
- [ ] For parity, add “Use classic editor…” buttons in each tab that call existing modal `edit*` functions until we migrate validation logic.
- [ ] Once inline versions are feature-complete, mark the legacy dialogs for removal.

---

## 4. Testing & Polish
- [ ] Verify keyboard traversal: TAB order in each tab, Ctrl+Tab across tabs, Alt+shortcut activation.
- [ ] Ensure tab switching updates the preview summary (future feature) without losing unsaved entries.
- [ ] Build Expect script that navigates the notebook, toggles each major option, hits OK, and reopens to confirm state persists.
- [ ] Update `docs/tools/ck-find.md` with new screenshots and tab descriptions.

---

## 5. Cleanup Tasks
- [ ] After tabs ship, delete unused “drawer” scaffolding ideas and remove old modal source files once inlined logic is stable.
- [ ] Refresh README / help texts with new terminology (“Quick Start”, “Actions tab”, etc.).
- [ ] Review hotkey descriptions in `src/common/hotkeys/default_schemes.cpp` for updated labels.

---

Following these steps will deliver the tabbed UI: start with the notebook shell, populate each page using existing option structs, keep the legacy editors around for validation, then phase them out once the tabs are verified.***
