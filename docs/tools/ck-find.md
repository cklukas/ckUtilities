# ck-find — Search specification designer

## SYNOPSIS

```
ck-find [--help]
```

## DESCRIPTION

`ck-find` is the new search planner for CK Utilities. The current build
focuses on the Turbo Vision user interface so we can refine the workflow
before wiring in the underlying file system engine. Launch the tool to
open a desktop with a **File → New Search…** command that displays a
multi-stage dialog for composing a search specification.

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
include/exclude glob patterns. Save and load buttons are already present
to demonstrate where preset management will live, although persistence
is not yet wired up.

When you accept the form, `ck-find` stores the specification in memory
and confirms that execution and saving will ship in later milestones.
This iteration is all about getting the ergonomics right.

## STATUS

Search execution, persistence, and directory browsing are intentionally
stubbed. Expect the next milestones to add storage for saved specs and a
command runner that translates the UI into real `find` invocations.

## SEE ALSO

`find(1)`, `fd(1)`
