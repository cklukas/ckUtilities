# ck-edit Markdown commands

`ck-edit` applies Markdown changes in the native ckVision editor. Each command
commits one document transaction, so an edit can be undone or redone as a single
action. Markdown-specific commands are available only for Markdown documents;
the editor explains an unavailable command in its status area rather than
changing another file type.

## Document commands

| Command | Behavior |
| --- | --- |
| Normalize Markdown whitespace | Applies the conservative Markdown normalizer. It preserves fenced and indented code, intentional hard breaks, and the document's newline style. |
| Reflow Markdown paragraph | Reflows the selected or current ordinary paragraph to 80 UTF-8 code points. Code, headings, quotes, lists, tables, references, Setext headings, and hard-break lines are left unchanged. |
| Toggle word wrap | Changes only the editor viewport; it does not alter source bytes or logical cursor positions. |
| Find, Find next, Replace, Replace all | Uses the native literal-search dialogs. Case and whole-word choices are retained, and Replace all is committed atomically. |

## Inline commands

Select text before applying inline formatting. The editor toggles the selected
Markdown span and keeps the change in a single undoable transaction.

| Command | Markdown form |
| --- | --- |
| Bold | `**text**` |
| Italic | `*text*` |
| Strikethrough | `~~text~~` |
| Inline code | Backtick fences sized safely for the selected content. |
| Insert or remove link | Inserts `[label](destination)` through a native destination dialog, or removes a selected complete link. Parenthesized destinations are supported. |
| Insert or remove image | Inserts `![alt text](destination)` through the same style of dialog, or removes a selected complete image. |
| Insert footnote | Prompts for a unique identifier, adds the reference and a blank-line-separated definition in one transaction, then positions the cursor in the definition body. |

## Block and list commands

Headings, quotes, task state, list style, and list indentation operate on the
selected lines or the line at the cursor. List commands preserve ordinary list
content and task markers where that is meaningful; indentation changes only
Markdown list items and leaves indented code alone.

| Command | Behavior |
| --- | --- |
| Heading 1–6 | Toggles the requested ATX heading level. |
| Toggle task | Toggles the Markdown task state. |
| Toggle quote | Adds or removes one quote level while retaining blank-line continuity. |
| Toggle bullet list | Converts selected content to a bullet list or removes that list form. |
| Toggle ordered list | Converts selected content to a sequential ordered list or removes that list form. |
| Indent list / Outdent list | Moves list markers by two spaces while retaining valid parent-list context. |

When Enter is pressed at the end of a list item, `ck-edit` continues the list:
bullet markers and indentation are preserved, ordered markers advance, task
items restart unchecked, and an empty item exits the list without trailing
whitespace.

## Table commands

Table commands operate only on ordinary pipe tables at the cursor (or on a
range wholly contained in one table). They never change fenced or indented
code, malformed tables, or partial table selections. Every successful command
replaces the complete table in one undoable document transaction and places
the cursor in the affected cell.

| Command | Behavior |
| --- | --- |
| Insert table | Prompts for 1–64 columns and 0–256 body rows, then inserts a header, separator, and requested blank rows. |
| Add table row | Inserts a blank body row after the current row; from a header or separator, it inserts the first body row. |
| Delete table row | Removes the current body row. |
| Add table column | Inserts a blank column after the active column and names its header `Column N`. |
| Delete table column | Removes the active column, except that a one-column table is left unchanged. |

Existing cell text, escaped pipe characters, separator alignment markers, and
the document's newline style are retained when a table is structurally edited.

## File safety

Open, Save, and Save As are native file workflows. If a file changes externally
while the document has unsaved edits, `ck-edit` preserves both versions and
requires an explicit Save As, reload/discard, or continue-editing choice.

The former Turbo Vision command reference is intentionally not retained here:
it listed obsolete commands and implementation-specific behavior. Native
coverage lives in `tests/unit/ck_edit` and `src/vision/edit/tests`.
