# ScopeFS Design Notes

## Kernel Boundary

`FileSystemKernel` is the only mutating entrypoint. Shell commands do not edit the model directly. Mutating commands follow this pattern:

1. Parse and resolve paths with the shared resolver.
2. Run traversal and authorization checks.
3. Start a transaction.
4. Apply COW path copying if the visible tree is shared with a snapshot.
5. Mutate inode, directory entry, block metadata, ACL, class, or snapshot state.
6. Emit tracepoints.
7. Commit and checkpoint through the journal.

This keeps path lookup, permission checks, trace output, and transaction boundaries observable and consistent.

## Transaction and Recovery Model

The implementation uses a whole-model teaching journal. Each transaction writes:

```text
BEGIN|txid|name
RECORD|txid|before|<hex serialized before image>
RECORD|txid|after|<hex serialized after image>
COMMIT|txid
CHECKPOINT|txid
```

The volume is checkpointed only after `COMMIT`. A crash before commit leaves the previous checkpoint visible. A crash after commit but before checkpoint is recovered by replaying the `after` image. This is intentionally simple, but it demonstrates the atomicity rules expected in the course project.

## Copy-on-Write

Snapshots retain the active root inode. Future writes call `ensureMutablePath`, which copies any shared root or shared path component. Copied directory inodes retain their children; copied files retain their data blocks. File writes then replace the writer's block map with newly allocated blocks and decrement old block reference counts. Shared old blocks remain visible through snapshots or `cp` copies.

## Permission Model

The authorization chain is:

1. root/admin bypass
2. traversal execute checks on every directory component
3. owner mode bits
4. file group mode bits through effective identity groups
5. other mode bits
6. ACL entries and constraints
7. default deny

Identity groups form a parent graph. Granting a group to another group means the target inherits the granted group. Revocation increments group generation and removes the direct grant; ACL and class-command trace events expose the change.

## Terminal Rendering

ScopeFS deliberately avoids GUI assumptions. Interactive mode prints a Unicode title and framed output, while all observability commands use script-compatible text:

- `dir` renders inode-rich tables.
- `scope tree` renders the visible directory tree with inode refs.
- `map refcount` renders block sharing with `·░▒█`.
- `trace [n]` renders recent tracepoints, and `trace <command>` renders only that command's kernel trace.
- `trace replay` reads JSONL and displays events without mutating the mounted file system.
