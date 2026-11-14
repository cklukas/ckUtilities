# Bounded handles for Files transfers

The current worker holds every source object while collecting the evidence
for a transfer. Execution reacquires and revalidates the reviewed scope.
The interim 1,024-entry planning ceiling bounds the number of rows. The
process may reach its open-file limit sooner because each native handle
counts, including Linux metadata-only descriptors, traversal cursors,
and handles used by the rest of the process.

## Backend and Files integration

1. Separate inventory size from live native handle use. Traverse with a
   bounded directory stack and acquire leaf handles only to collect
   observations. Store exact native paths, parent identities, and qualified
   object, mount and change evidence in a worker-owned manifest. Bound
   memory, disk usage, path depth and cancellation latency independently.
2. Bind the approved manifest, row decisions, exact destination names and
   preservation policy to an immutable plan. Keep review paged. A displayed
   name, matching timestamp, or ordinary inode number alone supplies no
   authority after the corresponding handle has been released.
3. Reacquire a bounded group through qualified parents immediately before
   execution. Prove source and target identities and revalidate the
   metadata, conflict and stability predicates from review. A missing
   continuity proof or a changed object requires fresh review. Keep
   the resulting handles through staging, verification, publication and
   any qualified source retirement. Release them after the worker records
   the row outcome.
4. Preserve operation-wide topology and hard-link constraints in the
   manifest. Journal each applied group under the same plan and preserve
   committed rows across interruption or partial failure. Recovery must
   obtain fresh native authority before continuing any mutation.

## Acceptance

- Review and copy a tree far larger than the process handle allowance while
  measuring peak handles and keeping browse and cancel responsive.
- Replace files, parents and mount targets between inventory, approval
  and execution. Try identical names, sizes, timestamps and recycled IDs.
  Changed or unqualified rows must remain untouched.
- Exercise boundary cancellation, partial commits, hard-link aliases and
  restart with a mixed tree. Verify source and destination contents and
  preservation independently.
- Run the native cases on ordinary-user Windows, Linux with the default
  process descriptor limit, and macOS. A storage provider without a sound
  rebinding contract must keep the affected action disabled.
