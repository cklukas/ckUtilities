# ck-du

`ck-du` is the native ckVision disk-usage explorer. It scans a directory in the
background and presents a navigable directory tree and file listings without
requiring `du` command pipelines.

## Usage

```text
ck-du [DIRECTORY]
```

`DIRECTORY` defaults to the current directory. Use `--help` (or `-h`) to show
the command-line usage.

## Native workflow

- **Scan:** rescan the requested directory, cancel an in-progress scan, or
  view files for the selected directory.
- The tree preserves a valid prior result while a rescan is underway; a
  cancelled or invalid replacement does not discard the visible snapshot.
- Saved Disk Usage options control the scan policy, including filtering,
  symlink handling, filesystem boundaries, and related reporting choices.
  Manage those options through `ck-config`.

## Cloud actions

The Cloud menu always reports what is available on the current platform. On
macOS, it can request an iCloud download for an eligible selected path and can
offer a confirmed request to free local copies. On other platforms, cloud
actions remain explicitly unavailable rather than claiming to change remote or
local cloud state.

Cloud requests are cancellable. A completed request is reported separately from
the provider's eventual storage state, so a successful request is not presented
as an immediate guarantee that a recursive cloud operation has completed.
