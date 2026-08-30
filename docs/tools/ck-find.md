# ck-find

`ck-find` is the native ckVision search workflow. It provides one guided,
scrolling form for a named search, start location, text matching, include and
exclude patterns, traversal policy, file types, date range, size range,
permission audit, and safe result actions. The generated command plan is
available for preview, while the search itself runs through a cancellable
background service and returns results to the interface.

## Usage

```bash
ck-find
```

Use `ck-find --help` for the command-line synopsis. Search creation, loading,
saving, previewing, running, and cancellation are available from the native
menus and status line.

## Safe actions

Deleting matches requires an explicit confirmation and is limited to matching
regular files and symbolic links; directories are never removed. Existing
saved custom-command settings are retained for compatibility, but custom
commands cannot be edited or executed by the native workflow until a separate
sandbox policy has been accepted.
