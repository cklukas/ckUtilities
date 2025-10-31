# ck-find — Search specification designer

## SYNOPSIS

```
ck-find [--help] [--list-specs] [--search NAME]
```

## DESCRIPTION

`ck-find` is the new search planner for CK Utilities. Launch the tool to
open a desktop with a **File → New Search…** command that displays a
multi-stage dialog for composing a search specification and executing it
with a native `find(1)` backend.

The main form keeps the essentials visible—specification name, starting
location, and optional text to look for—while grouping everything else
behind purpose-built option buttons. Each button opens a focused dialog
modelled after the corresponding `find(1)` feature set:

* **Text Options…** toggles match modes (contains, whole word, regular
  expression) and controls whether case, file contents, filenames, or
  binary data should be included.
* **Name & Path Tests…** wires in `-name`, `-path`, regex, and symlink
  tests plus a configurable `-prune` helper.
* **Time Tests…** layers presets like “Past 7 days” with direct access
  to `-mtime`, `-newer`, and related operators.
* **Size Filters…** captures ranges and exact `-size` expressions along
  with an `-empty` shortcut.
* **File Types…** lets you outline `-type`/`-xtype` letters and optional
  extension or detector hints.
* **Permissions & Ownership…** aggregates the `-perm`, readability
  helpers, and `-user`/`-group` family.
* **Traversal & Filesystem…** exposes symlink policy, depth limits,
  filesystem pruning, and inode/link filters.
* **Actions & Output…** collects the printing variants, `-delete`, and
  the `-exec`/`-ok` family alongside `-fprint*` destinations.

The dialog also includes check boxes for recursive traversal, hidden
files, symbolic links, and filesystem boundaries plus quick
include/exclude glob patterns. Saved specifications are stored under the
CK config directory, and the **File → Save/Search Spec…** commands now
persist and reload those presets. Choose **Load Search Spec…** to
rehydrate a preset into the notebook for further editing.

When you accept the form, `ck-find` builds a `find(1)` invocation that
mirrors the captured options. The current build still shows a textual
summary inside the UI, but the same specification can be executed from
the command line with `ck-find --search NAME`, which prints the matched
paths (after applying any content filters) to standard output.

## STATUS

The CLI runner executes saved specifications and lists them with
`--list-specs`. Future milestones will integrate richer previews inside
the Turbo Vision UI and extend detector-based filters.

## SEE ALSO

`find(1)`, `fd(1)`
