---
layout: home
title: CK Utilities
description: Focused terminal applications for finding files, inspecting JSON, understanding disk usage, editing Markdown, managing settings, and chatting with local AI models.
hero:
  name: CK Utilities
  text: Everyday tools for the terminal, with a real interface.
  tagline: >
    Focused applications in the spirit of the classic utility
    suites, built on ckVision: a menu bar, movable windows, mouse support,
    and a footer that always shows the keys you can use right now.
  image:
    src: images/hero.png
    alt: A pixel-art evening desk with a beige PC running a disk utility, a keyboard, floppy disks, a magnifying glass, a mug, and a desk lamp
  actions:
    - text: Install
      link: installation.md
      theme: brand
    - text: Explore the tools
      link: tools/ck-utilities.md
    - text: GitHub
      link: https://github.com/cklukas/ckUtilities
features:
  - title: JSON View
    icon: "{ }"
    details: Browse JSON as an expandable tree; search, reload, and copy values.
    link: tools/json-view.md
  - title: Find
    icon: "⌕"
    details: Build guided file searches with filters, previews, cancellation, and deliberate result actions.
    link: tools/ck-find.md
  - title: Disk Usage
    icon: "▤"
    details: Scan folders in the background and browse directory sizes and file lists.
    link: tools/ck-du.md
  - title: Edit
    icon: "#"
    details: Edit Markdown with search, undo and redo, formatting, links, footnotes, and tables.
    link: tools/ck-edit.md
  - title: Chat
    icon: "…"
    details: Chat with local GGUF models; manage prompts and models, cancel responses, and export conversations.
    link: tools/ck-chat.md
  - title: Todo
    icon: "☑"
    details: Organize tasks on boards with lanes, notes, due dates, and a durable archive.
    link: tools/ck-todo.md
  - title: System Monitor
    icon: "▁▃▆"
    details: Watch CPU, memory, GPU, and loaded Ollama models here and on SSH hosts.
    link: tools/ck-sysmon.md
  - title: Config
    icon: "⚙"
    details: Edit Disk Usage and Chat settings, and manage keyboard shortcuts.
    link: tools/ck-config.md
---

## One launcher, eight tools

Start `ck-utilities` to pick a tool from the launcher, or run any tool
directly: `ck-json-view`, `ck-find`, `ck-du`, `ck-edit`, `ck-chat`,
`ck-todo`, `ck-sysmon`, and `ck-config`. Every tool shares the same menu
bar, window handling, help, and key footer, so what you learn in one works in all of them.

![The CK Utilities launcher](generated/screenshots/ck-utilities-launcher.svg)

## Start here

1. [Install CK Utilities](installation.md) on macOS or Linux.
2. Open the [launcher](tools/ck-utilities.md) and try a tool.
3. Set up [Chat with a local model](tools/ck-chat.md) if you want AI
   assistance that never leaves your computer.

## Find an answer

| Task | Guide |
|---|---|
| Download, install, or upgrade | [Installation](installation.md) |
| Inspect a JSON file | [JSON View](tools/json-view.md) |
| Search for files by name, content, date, or size | [Find](tools/ck-find.md) |
| See what is using disk space | [Disk Usage](tools/ck-du.md) |
| Write and format Markdown | [Edit](tools/ck-edit.md) and [Markdown commands](markdown-commands.md) |
| Talk to a local model | [Chat](tools/ck-chat.md) |
| Keep track of tasks | [Todo](tools/ck-todo.md) |
| Watch CPU, memory, and GPU load | [System Monitor](tools/ck-sysmon.md) |
| Change settings and shortcuts | [Config](tools/ck-config.md) |
| Build and test the source | [Building from source](building.md) |

## Platforms

CK Utilities runs on macOS and Linux. A Windows version is not available yet.

CK Utilities is free software, provided as-is under the
[GNU General Public License, version 3](https://github.com/cklukas/ckUtilities/blob/master/LICENSE),
without user support or warranty.
