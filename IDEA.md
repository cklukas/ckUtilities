# CkTools: A modern TUI suite for Linux

**Pitch:** CkTools reimagines classic DOS-era utility suites and wraps the best GNU power tools behind clean, discoverable **Turbo Vision** text UIs (menus, dialogs, mouse support, keyboard-first). Think *PC Tools / Norton Utilities* vibes—reborn for today’s terminal: safer defaults, searchable help, and one-keystroke actions for everyday power tasks. Built on the actively maintained open-source Turbo Vision port (Unicode, cross-platform). ([GitHub][1])

---

## What we can (re)learn from the classics

Below is an overview of notable **PC Tools** programs and whether they still map to useful Linux tasks today.

> PC Tools bundled utilities such as **DiskFix**, **PC Backup**, **Mirror/UnDelete/Unformat**, **DriveMap**, **Desktop/Shell**, and later **Central Point Anti-Virus (CPAV)**; several components (Mirror/Undelete/Unformat, CPAV/VSafe) were even licensed into MS-DOS. ([vtda.org][2])

### PC Tools → usefulness today on Linux

| PC Tools program (orig.)       | What it did (then)                 | Useful today? | Linux angle / TUI idea                                                                 |
| ------------------------------ | ---------------------------------- | ------------: | -------------------------------------------------------------------------------------- |
| DiskFix                        | Check/repair disks, FAT structures |       **Yes** | Wrap `fsck`/SMART info in read-only first, with guided repair.                         |
| UnDelete / UnFormat            | Recover files / volumes            |       **Yes** | Front-end to `extundelete`, `photorec`/`testdisk` with previews & “what’s safe” hints. |
| Mirror                         | Saved dir info to aid Undelete     |     **Maybe** | A background indexer (inotify) to improve recovery hints.                              |
| PC Backup                      | Scheduled backups                  |       **Yes** | Drive a `tar`/`rsync`/`borg` profile runner with job history.                          |
| DriveMap                       | Visual disk map / usage            |       **Yes** | Interactive `du`/inode browser (think ncdu-like) with filters.                         |
| File Manager / Desktop / Shell | GUI-ish file ops inside DOS        |    **Partly** | We’ll keep file ops minimal (copy/move/view/edit); the shell is your terminal.         |
| Data Monitor / DProtect        | TSR guarding disk ops              |     **Maybe** | A “guarded mode” for risky commands: dry-run diffs, safety interlocks.                 |
| CPR (Recuperator)              | Recovery toolkit                   |       **Yes** | A curated recovery workspace (imaging via `ddrescue`, hash/verify, mount-readonly).    |
| CPAV / VSafe                   | Antivirus / TSR shield             |       **Low** | Niche on Linux; optional ClamAV front-end at most.                                     |
| PC Tools Editor (PCE)          | Text editor                        |       **Yes** | See *Markdown editor* below.                                                           |

Sources touching these components and bundles: manual & media listings for **PC Tools Deluxe** (v3–v9), CPAV background, and licensing into MS-DOS. ([vtda.org][2])

> Parallel inspiration: **Norton Utilities** shipped **Norton Disk Doctor (NDD)**, **Speed Disk**, **UnErase**, **FileFind** and more—great models for friendly diagnostics and recovery flows. ([Wikipedia][3])

---

## Proposed CkTools lineup (Turbo Vision TUIs)

Below are concrete tools to ship. Each wraps a solid GNU utility (from your long list) or creates a small, modern replacement where helpful.

| New tool (CkTools)             | Inspiration                     | Builds on (GNU)                                      | What it does (short)                                                                                                 |
| ------------------------------ | ------------------------------- | ---------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------- |
| **ckFind**                     | PC Tools *FileFind*, GNU *find* | `findutils`                                          | Visual query builder for `find` (type/mtime/size/perm), live result pane, action menu (open, copy, delete, archive). |
| **ckGrep**                     | NU *File Compare* vibe          | `grep`                                               | Regex sandbox + live matches, include/exclude globs, ripgrep-style UX (if present), saveable presets.                |
| **ckDiff**                     | NU *File Compare*               | `diffutils`                                          | Side-by-side diff/patch viewer, stage/apply hunks, ignore rules, colorized.                                          |
| **ckPatch**                    | PC Tools *CPR* spirit           | `patch`                                              | Safer patch apply with dry-run, conflict browser, backup management.                                                 |
| **ckTar**                      | PC Tools *Backup*/*Viewers*     | `tar`, `gzip`, `xorriso`                             | Archive browser/extractor, create/update, “what will be added” preview.                                              |
| **ckWget**                     | Download mgr                    | `wget`                                               | Queue/resume downloads, per-site throttles, checksum verify.                                                         |
| **ckKeys**                     | Security console                | `gnupg`                                              | Keyring manager: list/import/export/revocation, trust editing, sign/encrypt with templates.                          |
| **ckRescue**                   | PC Tools *CPR*                  | `ddrescue`                                           | Disk imaging & recovery dashboard: plan, image, map, verify, log.                                                    |
| **ckDU**                       | PC Tools *DriveMap*             | `coreutils` (`du`, `stat`)                           | Interactive disk usage explorer with tree, filters, inode & sparse file flags.                                       |
| **ckProc**                     | NU *System Info* flavor         | `procfs`, `ps`, `kill`                               | Process/browser with search, tree, niceness, signals; “why high CPU?” hints.                                         |
| **ckUnits**                    | Handy calc                      | `units`                                              | Unit conversions with favorites & history.                                                                           |
| **ckCalc**                     | Programmer’s calc               | `bc`, `dc`                                           | Arbitrary precision calc with memory registers & named vars.                                                         |
| **ckCalendar**                 | Power user calendar             | `gcal`                                               | Monthly/agenda views, reminders export to ics; not a full PIM.                                                       |
| **ckBatch**                    | Job runner                      | `parallel`                                           | Build/run parallel job sets, live progress, retry rules.                                                             |
| **ckRec**                      | Lightweight DB                  | `recutils`                                           | Browse/edit `.rec` databases with schema help, search, import/export.                                                |
| **ckDatamash**                 | Data crunching                  | `datamash`                                           | Wizard for group-by, stats, pivot on CSV/TSV, preview results safely.                                                |
| **ckGlobal**                   | Code nav                        | `global` (GNU GLOBAL)                                | Query tags db with filters, history, “jump back”, symbol x-ref panes.                                                |
| **ckBuild**                    | Build monitor                   | `make`                                               | Run targets with nice logs, target graph, “why rebuilt?” explanations.                                               |
| **ckSedLab**                   | Text transforms                 | `sed`                                                | Recipe builder: compose filters step-by-step with sample input, export script.                                       |
| **ckAwkLab**                   | Data transforms                 | `gawk`                                               | Interactive field/regex playground; scaffold awk one-liners/scripts.                                                 |
| **ckNet**                      | Net kit                         | `inetutils` (ping, traceroute, ftp, telnet), `whois` | Ping/traceroute dashboards, WHOIS lookup, quick FTP/Telnet sessions (readable logs).                                 |
| **ckMount** *(read-only)*      | Disk safety                     | `parted`/`fdisk`                                     | View partitions/filesystems SMART (if available), mount **read-only** by default.                                    |
| **ckText** *(Markdown editor)* | PC Tools *Editor*               | `nano`-class editor (custom)                         | Minimal Markdown editor with sidebar, link checker, export via Pandoc if present (HTML/PDF).                         |
| **ckSpell**                    | Writing aid                     | `aspell`                                             | Spellcheck with project dictionaries; integrate with **ckText**.                                                     |
| **ckTime**                     | Time kit                        | `time`, `date`, `tzdata`                             | Timezone conversions, run-time measurement presets, cron snippet helper.                                             |
| **ckInfo**                     | Docs/help                       | `info`, `help2man`                                   | Searchable GNU docs with “open at section” and copy-paste examples.                                                  |

> Notes
> • We lean on well-known GNU tools you listed (e.g., `findutils`, `diffutils`, `tar`, `wget`, `gnupg`, `ddrescue`, `units`, `gcal`, `parallel`, `recutils`, `datamash`, `global`, `make`, `gawk`, `sed`, `inetutils`, `aspell`, `info`).
> • The **Markdown editor** is a bespoke app, but uses system tools (Aspell, Pandoc) when available.
> • Anything destructive (partitioning, writes) defaults to **read-only** with explicit unlock flow.

---

## Naming & packaging

* Suite name: **CkTools**
* Executable prefix: `ck*`
* Examples: `ckfind`, `ckdiff`, `cktext`, `ckrescue`

---

## Why Turbo Vision now?

Turbo Vision’s modern ports add **Unicode** and **cross-platform** support, and its CUA widgets are still ideal for fast keyboard UIs—perfect for reducing `find`/`grep`/`sed` friction without dumbing anything down. ([GitHub][1])

---

## Suggested v0.1 roadmap

1. **ckFind**, **ckDiff**, **ckDU**, **ckText** (Markdown), **ckRescue** (read-only features first).
2. Add **ckGrep**, **ckTar**, **ckKeys**, **ckGlobal**.
3. Integrate help: `info(1)` snippets and copy-ready CLI equivalents in each dialog.

If you’d like, I can turn this into a GitHub README skeleton (with module layout and key screens) next.

[1]: https://github.com/magiblot/tvision?utm_source=chatgpt.com "magiblot/tvision: A modern port of Turbo Vision 2.0, the ..."
[2]: https://vtda.org/docs/computing/CentralPointSoftware/PC_Tools_V3_Manual.pdf?utm_source=chatgpt.com "PC Tools Version 3 Manual"
[3]: https://en.wikipedia.org/wiki/Norton_Utilities?utm_source=chatgpt.com "Norton Utilities - Wikipedia"
