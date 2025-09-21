# CkTools (working title)

**Status:** concept / exploration.
CkTools aims to bring a set of everyday power utilities to a **Turbo Vision** text UI, so they’re easier to discover and safer to use—while staying fast and script-friendly.
Target platform: **Linux text-mode terminals**.
Tech stack: **C++20 (or newer)** + **Turbo Vision**.

---

## Why

Many GNU tools are extremely powerful but tricky to memorize. CkTools wraps a focused subset behind clean, keyboard-first TUIs with sensible defaults and visible dry-runs.

---

## Planned tools (initial scope)

* **ckfind** — Visual query builder for `find`: filter by type/mtime/size/perm, test patterns live, preview actions (copy/move/delete/archive) with safe confirmation.
* **ckdiff** — Side-by-side diff & patch helper: browse files/dirs, stage/apply hunks, ignore rules, make backups automatically.
* **ckdu** — Interactive disk-usage explorer: tree view, sort & filter by size/inodes, reveal sparse files, jump-to-path actions.
* **cktext** — Minimal **Markdown** editor for quick notes and docs; optional export via system tools (e.g., to HTML/PDF) when available.
* **ckrescue** — Read-only front-end for disk imaging/recovery workflows (e.g., plan → image → verify), emphasizing logs and safety.

> The above list is **not final** and may change as we prototype. Tools not listed here are **out of scope** for this phase.

---

## Design principles

* **TUI-first, CLI-friendly:** every screen shows the equivalent command-line so users learn by doing.
* **Safety by default:** destructive actions are opt-in, with dry-run previews and clear warnings.
* **Portable & fast:** single static binaries where practical; minimal external deps.

---

## Roadmap (very early)

1. Prototype **ckfind** and **cktext**.
2. Add **ckdiff**, **ckdu**.
3. Ship **ckrescue** (read-only features first).

---

## License & contributions

TBD. Discussion, mockups, and feedback welcome while this is still an idea.
