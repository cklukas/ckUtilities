# ck-edit

`ck-edit` is the native ckVision Markdown editor. It supports safe file
editing, Markdown transformations, search and replacement, and configurable
keyboard shortcuts.

## Usage

```text
ck-edit [MARKDOWN_FILE]
```

Pass a Markdown file to open it at startup, or use **File → Open document**.
Use `--help` (or `-h`) to show the command-line usage.

## Native workflow

- **File:** open, save, save as, and close documents. Dirty documents require
  an explicit Save, Discard, or Cancel choice.
- **Edit:** undo, redo, toggle viewport-only word wrap, find, replace, and
  replace all.
- **Markdown:** normalize whitespace, reflow paragraphs, format selected text,
  manage headings, tasks, quotes, and lists, and insert or remove links,
  images, and footnotes. Create pipe tables and add or delete table rows and
  columns through one-step, undoable Markdown transactions.

If an open file changes outside the editor while it has local unsaved edits,
`ck-edit` preserves both versions and requires an explicit Save As,
reload/discard, or continue-editing decision.

See [Markdown command details](../markdown-commands.md) for the native
transformation behavior and safety rules.
