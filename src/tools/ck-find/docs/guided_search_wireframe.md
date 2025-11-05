# Guided Search Wireframes

These ASCII layouts capture the single-column guided experience implemented in the revamped `ck-find`. The diagrams assume an 84×36 Turbo Vision dialog.

```
+--------------------------------------------------------------------------------+
|                               Guided Search                                    |
| Saved…  Save…  Presets…  Recipes…                                              |
|                                                                                |
| Location                                                                       |
| Start in: [................................................] [ Browse… ]        |
| Options: [x] Include subfolders  [ ] Include hidden  [ ] Follow symlinks        |
|          [ ] Stay on filesystem                                                |
|                                                                                |
| What                                                                           |
| Look for: [..............................................................]     |
| Scope:    (o) Contents & names  ( ) Contents only  ( ) Names only               |
| Match:    (o) Contains text     ( ) Whole words    ( ) Regular expression       |
| Extras:   [ ] Match case  [ ] Allow multiple terms  [ ] Treat binary as text    |
| Include patterns: [.....................................................]      |
| Exclude patterns: [.....................................................]      |
| [ Fine-tune text… ] [ Fine-tune names… ]                                       |
|                                                                                |
| Filters                                                                        |
| File types:                                                                    |
|    (o) All files                                                               |
|    ( ) Documents                                                               |
|    ( ) Images                                                                  |
|    ( ) Audio                                                                   |
|    ( ) Archives                                                                |
|    ( ) Code                                                                    |
|    ( ) Custom                                                                  |
| Summary: Includes: pdf, doc, docx, txt, md, rtf                                |
|                                                                                |
| Edited within:                                                                 |
|    (o) Any time           ( ) Last 24 hours                                    |
|    ( ) Last 7 days        ( ) Last 30 days                                     |
|    ( ) Last 6 months      ( ) Past year                                        |
|    ( ) Custom range  From [....]  To [....]                                    |
|                                                                                |
| Size filters:                                                                  |
|    (o) Any size           ( ) Larger than…   Value [....]                      |
|    ( ) Smaller than…      ( ) Between…      Value [....] to [....]             |
|    ( ) Exactly…           ( ) Empty only                                      |
|                                                                                |
| [ Permission checks… ] [ Traversal… ]                                          |
| [ Fine-tune file types… ] [ Fine-tune dates… ] [ Fine-tune size… ]             |
|                                                                                |
| Actions                                                                        |
| [x] Preview matches   [x] List matching paths   [ ] Delete matches             |
| [ ] Run command   Command: [..........................................]        |
| [ Fine-tune actions… ]  [ Preview command ]  [ Search ]  [ Cancel ]            |
+--------------------------------------------------------------------------------+
```

Popular searches and expert recipes open dedicated pickers via the toolbar buttons. Saved searches reuse the same flow: choose “Saved…” to load one, or “Save…” to capture the current configuration with a friendly name.

### Expert Recipes Sheet (overlay)

```
+------------------------------------------------------------------+
| Expert Recipes                                                   |
|                                                                  |
| • Changed in last deploy                                         |
| • Root-owned & group writable                                   |
| • New symlinks outside project                                  |
| • Empty directories cleanup                                     |
|                                                                  |
| Selecting a recipe loads human-readable defaults; refine them    |
| before running.                                                  |
|                                                                  |
|                              [ Cancel ]  [ Run recipe ]          |
+------------------------------------------------------------------+
```

### Option Mapping

| Legacy bucket                          | Guided grouping | Notes                                                      |
| -------------------------------------- | --------------- | ---------------------------------------------------------- |
| Start path, recursion, symlinks, stay- | Location        | Friendly phrasing “Include subfolders”, “Follow links”.    |
| Text search flags                      | What            | Exposed as “Look for” scope with advanced modal.           |
| Name/path tests                        | What            | Hidden behind “Fine-tune names…” button.                   |
| Time/size filters                      | Filters         | Natural language presets with optional custom entries.     |
| Type filters                           | Filters         | Curated collections + custom modal.                        |
| Permission/ownership                   | Filters         | Context button labelled “Permission checks…”.              |
| Traversal, prune, depth, files-from    | Filters         | Secondary “Traversal controls…” dialog.                    |
| Actions/exec/printing                  | Actions         | Simplified toggles, advanced modal for exec/fprint/etc.    |
| Spec save/load                         | Toolbar         | “Saved…” and “Save…” buttons launch dedicated flows.       |

### Copy Guidelines

- Prefer verbs that explain the outcome (`Fine-tune`, `Browse`, `Run recipe`).
- Toggle labels describe the effect (“Include hidden”, “Stay on filesystem”) instead of flag names.
- Status summaries use natural language (“Includes: pdf, doc…”, “Archives older than six months”).
- Preset descriptions stay under 60 characters so they fit alongside the list box.
