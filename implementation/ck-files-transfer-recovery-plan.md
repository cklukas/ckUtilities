# Files transfer recovery — backend prerequisites and acceptance

Date: 2026-11-14. Shared backend requirements; no new capability accepted.

This specification extends [Files WPs](ck-files-plan.md) and
[Commander parity safety packages](ck-files-parity-plan.md). Backend work belongs
in ck-core-fs-lib and benefits all consumers. B-00 audits the installed public
API and native capabilities; B-01 qualifies Mac publication before enabling it.
See the [backend audit](../../ck-core-fs-lib/docs/macos-publication-audit.md)
for current findings and evidence. No separate product UI is specified here.

2026-11-14 checkpoint: B-00 has a source/API matrix and focused installed-C
evidence against backend `82264fe`. B-01 has eight native conformance checks,
including counterexamples to clone-reopen ownership and private-stage rename.
Strong Darwin publication remains unavailable; no positive copy/move/restart
gate is closed. The audit specifies the next preflight and publication-receipt
work instead of silently weakening existing guarantees.

The implemented development increment adds `ckfe_preflight_*` (family version 1), a
read-only worker assessment, and `SameObjectPublicationReceipt` in the backend.
The legacy regular-copy batch reports the Mac blocker before journal creation.
Copy/archive engines require the typed receipt; output observations cannot
construct it. See the [contract and remaining integration gaps](../../ck-core-fs-lib/docs/transfer-preflight-and-publication-receipts.md).
Focused validation passed 7 Mac checks, 37 Linux checks and installed C
consumer/audit checks; see the [evidence](../../ck-core-fs-lib/qualification/preflight-publication-receipts-20261114.json).
The subsequent V2 preservation increment adds preliminary source-policy
eligibility and explicit exclusions, with destination replay still Unknown and
achieved fields always zero. Admission covers regular/tree batches, cross-volume
move batches, reviewed trees, verified-prefix retries, native sync and named
resolution before their new journals/ledgers or row effects. Legacy C wrappers
also defer journal-root permission changes until admission passes. Full metadata
inventory, target replay, space/stability preflight, native Darwin publication
and interruption/repeat recovery remain open. A completed preflight
is never execution authority. No GUI code belongs in this shared project.
The [preservation increment evidence](../../ck-core-fs-lib/qualification/preservation-preflight-20261114.json)
records 13 selected Mac checks, installed-consumer/audit checks, 48 passing Linux
CTests and the separate 796-check tree pass after its debug CTest timeout.
These bounded results do not close B-01, B-03 or B-06.

## Interruption and repeated-operation contract

### The acceptance scenario

Start with source `Photos/{A,B,C}` and a destination containing an unrelated
`Photos/D`. Interrupt a move after A is durably published and, if qualified,
retired from the source, while B is staged and C is untouched. Restart the app
and either resume the recorded job or submit the same source/destination again.

Required result: A, B and C reach the reviewed destination; D is preserved.
Until each required destination commit is proven, its only valid source must
remain recoverable. A changed same-name file must survive as an unresolved
conflict or retained version. Repeating recovery must not delete A, overwrite
D, duplicate an already committed action, or remove a new source object.
If automatic completion cannot be established, retain data and explain exactly
which items require review. Missing source entries are never deletion intent.

### Resume, retry, and a new transfer are different

1. **Resume recorded transfer:** retain the original job ID, sealed inventory,
   reviewed decisions and lineage. Rebind locations and obtain fresh native
   evidence before deciding what is already complete or can safely continue.
   New source children remain outside the original scope and can be reviewed
   as a separate transfer. Changed evidence invalidates affected decisions.
2. **Retry unresolved items:** create a new reviewed attempt linked to the old
   job, after reconciliation. Protect prior committed destinations. An ambiguous
   row cannot be reclassified as an ordinary pending copy merely to retry it.
3. **Submit the same transfer again:** detect possible overlapping unfinished
   jobs using backend identities and lineage, not display paths alone. Present
   Resume, Review unresolved items, or Cancel. Hold conflicting execution until
   reconciliation; a fresh request cannot bypass an unresolved old intent.
4. **Recover a Finder/foreign partial move or a job with no trustworthy journal:**
   offer conservative comparison and copy/merge of the two observed trees.
   Preserve destination-only entries and all uncertain conflicts. Do not infer
   original scope, prior successful source removal, or authority to delete.
   No journal means no claim to resume that original transaction.

Names, bookmarks, matching hashes and serialized file IDs locate or describe
objects; they are not independent authority to replace, retire or clean them.
After restart, native workers must acquire new authority under an explicit
provider contract. A private journal alone cannot recreate a lost live handle.

### Required recovery decisions

| Observed boundary | Safe next behavior |
| --- | --- |
| Planned, no effect | Revalidate scope and decisions before starting |
| Partial private staging | Reuse only under qualified fresh ownership and verified-prefix rules; otherwise preserve it and offer a separately safe recopy |
| Verified staging, no publication receipt | Revalidate staged bytes, metadata, source and destination; publication remains a separate authorized action |
| Publication intent, effect or receipt ambiguous | Inspect actual identities and retained versions; do not blindly replay publication |
| Destination committed, source present | Reverify destination and current source stability before any retirement; otherwise retain source |
| Source-removal intent, no completion receipt | Inspect source capture and destination; never infer successful deletion from absence at the old path |
| Completed or retained-source job submitted again | Revalidate outcome, return a no-op or a bounded reviewed continuation; never silently restart destructive work |
| Missing volume, corrupt/legacy journal, substituted object or denied access | Preserve all candidates, block affected mutations, provide read-only diagnosis and reconnect/reselect guidance |

Persist a versioned manifest with job/attempt lineage, native names, volume and
parent observations, original item scope, preservation policy, conflict receipts,
per-stream verification evidence, staging/retention receipts and sequenced
intent/effect states. Bound records and scanning; record intent before mutation.
Do not log file contents, credentials or raw sensitive metadata values.
Retain unresolved journals. A journal-store disk-full/flush failure prevents
further destructive steps. File checkpoints are mandatory for v1; mid-file
prefix resume is optional and must not block safe file-level restart.


## Backend packages

All B packages belong in `ck-core-fs-lib`, with consumer integration in Files
and other independently qualified frontends. Reuse existing implementations
after auditing their exact contracts; these are gap-closing packages, not
instructions to rewrite them.

| Package | Deliverables and acceptance | Dependencies / existing WP |
| --- | --- | --- |
| B-00 Capability and ABI audit | Installed C consumer matrix for planning, consent, native names, jobs, recovery, retention and errors. Separate implemented, native-qualified and exposed states. Version/lifetime/cancellation rules; unknown API/capability disables mutation. Freeze provider and threat assumptions, including concurrent same-user processes. | WP-00/03; CP-00 |
| B-01 Darwin publication | Prove publication of intended file/tree through held parents, no-clobber behavior, created-object identity, metadata, namespace durability and ambiguous outcomes. Evaluate `fclonefileat` as a candidate with distinct clone identity/ACL/space semantics; no assumption that rename checks prove held-object publication. Native positive and substitution/fault tests required. | B-00; WP-04; CP-04 |
| B-02 Retained replacement | Preserve old file/package content before replacement; no remove-then-rename. Journal retained identity and location, recover each crash interval, implement reviewed restore and separate explicit disposal. Retention quota exhaustion blocks replacement rather than discarding recovery data. | B-01; WP-03/04; CP-03 |
| B-03 Durable recovery and repeat reconciliation | Implement the contract above with fresh authority, per-file resume, repeated invocation, journal migration, unavailable volumes and unfinished-job overlap. Recovery itself is journaled and crash-safe. Expose inspect, reconcile, retry and supported cleanup as separate capabilities. | B-01/02; WP-04; CP-03 |
| B-04 Source retirement | Prove current content AND promised metadata stability, safe source capture, destination validity and bottom-up removal without new children. Test writers, mmap/hard-link aliases and changed trees. A snapshot proves copied content, not that the live original stayed unchanged. Retain originals if removal cannot be proved; no generic “force” bypass. | B-01/03; WP-05; CP-01 |
| B-05 Shared ownership and consumer integration | Coordinate overlapping consumer jobs and crash-abandoned claims without treating an advisory lock as exclusion of external writers. Export required versioned C capabilities and structured outcomes. No path-based I/O fallback in any frontend. Qualify installed C and existing C++ consumers together. | B-00–04; WP-06; CP-03 |
| B-06 Native reliability gate | Execute R-01–R-12 below on declared Mac/provider pairs with independent byte, metadata and retained-version inspection. Run matching sanitizers, journal/manifest fuzzing and actual installed-consumer restart acceptance. Record process-crash and power-loss evidence separately. | B-01–05; WP-07/08; CP-04 |

If B-01 or B-04 proves impossible under the declared provider/threat model,
record a go/no-go decision. Do not lower the guarantee silently. A copy-only
release may be scoped explicitly after B-01–03/05/06 pass; a product advertised
as completing cross-volume moves requires B-04. Safe refusal alone never
qualifies positive copy, move, or resume support.


## Acceptance matrix

| ID | Scenario | Required observable result |
| --- | --- | --- |
| R-01 | Kill/relaunch before and after each file/tree intent, effect, receipt, flush and finalization | Reconcile actual effects; no missing only copy, false completion or blind destructive replay |
| R-02 | A transferred, B partial, C pending, destination-only D; resume and resubmit | Preserve the A/B/C/D union; qualified completion or exact retained/uncertain rows |
| R-03 | Repeat recovery three times; kill recovery itself at every mutation boundary | No extra publication, deletion or retention loss; stable lineage and convergent results |
| R-04 | Finder-created partial trees, absent/corrupt/legacy journal | Conservative merge/copy review; destination-only data protected; no invented historical authority |
| R-05 | Source edited through open writer, mmap or hard link; new children | Detect or refuse unsupported stability; retain source and unplanned children |
| R-06 | Replace names/parents, reuse identities, symlink substitution, same display path on different volume | No mutation of an unreviewed object; exact reconciliation diagnostic |
| R-07 | Replace old destination; crash in every retention/publication interval; restore over later edit | Old content remains recoverable; later edit protected; no whole-folder replacement |
| R-08 | Unplug/reconnect external APFS, denied/revoked access, sleep/wake | Preserve receipts/data, revalidate volume/object/access, resume only when qualified |
| R-09 | Destination/catalog/retention full; short I/O, flush/close failure, low memory | Stop destructive progression; preserve prior versions and sources; bounded diagnostics |
| R-10 | Resource fork/xattr/ACL/flags/time/sparse/hard-link/name fixtures and case/normalization conflicts | Independently verify promised preservation; explicit approved differences or refusal |
| R-11 | Two instances or distinct consumers submit overlapping jobs; rapid repeated clicks | One admitted owner per conflicting scope; no stale-plan execution after another job changes it |
| R-12 | At least 100,000 entries and one large multi-checkpoint file; cancel/restart repeatedly | Bounded memory/catalog use, responsive UI, correct counts and no unbounded rescan on UI thread |

Initial measurable targets, to ratify with hardware/filesystem details in
B-00: p95 local UI acknowledgement under 100 ms, progress delivery at most
10 Hz, paged lists of at most 500 rows, no unbounded pending event queue, and
under 512 MiB consumer resident memory for the 100,000-entry fixture. Report the
actual measurements and catalog/retention bytes. A blocked kernel call may
delay cancellation; the UI must remain responsive and report the wait honestly.
Measure copy/verify/recovery durations against a same-host baseline; do not
promise a speedup over Finder or APFS clones.

Use deterministic injection for exhaustive boundary coverage, separate real
process-kill/relaunch tests through the installed app, and controlled storage
fault tests for any power-loss claim. Inspect actual user-facing windows from
the verified single app instance. Preserve concise source/asset/build/test
receipts and clean task-owned obsolete builds under the standing workspace rules.

