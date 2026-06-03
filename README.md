# ScopeFS

ScopeFS is a C++17, UNIX-like teaching file system that runs entirely from the terminal. It models multi-user sessions, directories, files, inode/block state, tracepoints, transaction journal recovery, crash injection, Copy-on-Write snapshots, block heatmaps, fsck, and a database-like identity group permission system.

The project is intentionally terminal-first. Interactive mode renders a Unicode/ANSI workspace, while `--script` mode is deterministic and suitable for automated course checks.

## Build

```powershell
cmake -S . -B build -G "MinGW Makefiles"
cmake --build build -j 4
```

The MinGW build links libgcc/libstdc++ statically so the executable is portable across Windows terminals. Runtime files are kept under the project directory:

- `.scopefs/scopefs.volume`
- `.scopefs/scopefs.journal`
- `.scopefs/scopefs.trace.jsonl`

## Quick Demo

```powershell
Get-Content qa\demo.scope | .\build\scopefs.exe --script
```

Interactive mode enables Windows UTF-8 and ANSI Virtual Terminal support at startup, then clears the screen and renders a dark Unicode workbench. Windows Terminal is recommended. If an older console still shows garbled block characters, run `chcp 65001` once before launching.

Default users after `format`:

- `root` / `root`
- `admin` / `admin`
- `usr1` ... `usr8`, password equals username

## Command Surface

Session and setup:

```text
format
login <user> [password]
logout
whoami
help
exit
```

Directories and files:

```text
mkdir <path>
rmdir <path>
chdir <path>
dir [path]
create <path> [mode]
open <path> <r|w|rw|append|truncate>
read <fd> [size]
write <fd> <data>
close <fd>
delete <path>
rm <path>
truncate <path|fd> <size>
```

Observation and replay:

```text
trace on
trace off
trace show [n]
trace save <file>
trace replay <file>
trace step
trace clear
scope
scope inode
scope block
scope journal
scope open
scope tree
map
map blocks
map inode
map journal
map refcount
map owner
```

Journal, recovery, snapshots, and permissions:

```text
crash now
crash after <event_type>
crash before <event_type>
crash at <seq>
crash clear
fsck
fsck --repair
snapshot create <name>
snapshot list
snapshot show <name>
snapshot diff <a> <b>
snapshot rollback <name>
snapshot delete <name>
clone <src> <dst>
class create <class_name>
class grant <class_name> to <user_or_class> [with grant option] [constraints]
class revoke <class_name> from <user_or_class>
class list
class tree
chmod <path> <mode>
chown <path> <user>
chclass <path> <class_name>
acl show <path>
acl grant <path> <user_or_class> <rights> [constraints]
acl revoke <path> <user_or_class> <rights>
```

Crash injection event names include:

```text
journal.begin
journal.record
journal.commit
journal.checkpoint
command.dispatch
```

## Architecture

All shell commands enter through `FileSystemKernel`. The kernel owns the shared model and routes every sensitive operation through the same path resolver, permission checker, transaction journal, and trace sink.

The persistent volume is a serialized teaching model, not a byte-for-byte POSIX disk image. It still preserves the required kernel concepts:

- superblock with checksum and clean/dirty/recovering mount states
- inode table with generation, owner, class, mode, block map, open count, delayed deletion
- directory entries stored as directory file data
- block metadata with refcount, checksum, last writer txid, owner inode, and flags
- physical-style journal records containing before/after transaction images
- crash recovery that ignores uncommitted transactions and redoes committed transactions
- Copy-on-Write inode/block sharing for snapshots and clones
- tracepoint ring buffer and JSONL export/replay

## QA Scripts

Run all scripted checks:

```powershell
powershell -ExecutionPolicy Bypass -File qa\run_all.ps1
```

Run the interactive UI width smoke checks:

```powershell
powershell -ExecutionPolicy Bypass -File qa\ui_width_smoke.ps1
```

The scripts cover normal operations, permissions, COW snapshots, and crash recovery around commit boundaries.

## Notes

ScopeFS is built for operating-system course demonstration. It does not attempt full Linux/POSIX compatibility, real device drivers, networking, or high-performance scheduling. The goal is a clear, observable, replayable file-system kernel model with a strong terminal presentation.
