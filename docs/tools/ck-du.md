# ck-du — Disk Utilization Explorer

`ck-du` visualizes disk usage with Turbo Vision. Launch it with one or more directory paths to open each location in its own window:

```bash
ck-du ~/Downloads /var/log
```

## Features

- **Directory tree** that mimics `ncdu`: sizes, file counts, and nested directory counts are displayed for each entry.
- **Multiple windows**: pass paths on the command line or open new directories at runtime.
- **File listings**: press <kbd>F3</kbd> ("View Files") to list files in the selected directory, or <kbd>Shift</kbd>+<kbd>F3</kbd> for a recursive listing that includes subdirectories. File lists show size, owner, group, creation, and modification times.
- **Unit control**: choose Auto, Bytes, KB, MB, GB, TB, or Blocks from the Units menu. The active unit is marked and updates every open view immediately.
- **Sort modes**: switch between Unsorted, Name, Size, or Modified order from the Sort menu. The selection applies to every directory tree and file list window instantly.

## Keyboard & menu shortcuts

| Shortcut | Command | Notes |
| --- | --- | --- |
| <kbd>F2</kbd> | Open Directory | Type a path to open in a new window. |
| <kbd>F3</kbd> | View Files | Lists files directly inside the selected directory. |
| <kbd>Shift</kbd>+<kbd>F3</kbd> | View Files (Recursive) | Includes files from subdirectories. |
| <kbd>F1</kbd> | About | Shows version and developer. |
| <kbd>Alt</kbd>+<kbd>X</kbd> | Quit | Exit the application. |

Use the arrow keys to expand/collapse directories, and open multiple directories to compare usage side-by-side. Adjust units from the **Units** menu to switch between automatic scaling, fixed sizes, or 512-byte blocks. Change the ordering of directories and files from the **Sort** menu without reopening windows.

