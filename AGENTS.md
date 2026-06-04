# Repository Guidelines

## Project Structure & Module Organization

ScopeFS is a C++17 terminal-first teaching file system. Public headers live in `include/scopefs/`; implementation files live in `src/`. Keep core behavior behind `FileSystemKernel` in `src/kernel.cpp` and route commands through `src/shell.cpp`. Terminal rendering is isolated in `src/ui.cpp` and `src/terminal.cpp`; do not mix visual formatting into storage or kernel logic. Structured trace emission lives in `src/trace.cpp`, and multi-terminal sessions, read/transaction locks, crash signals, and epoch files live in `src/coordinator.cpp`.

Key runtime and support directories:
- `qa/`: PowerShell and `.scope` scripted checks. These are external QA assets, not core project code.
- `docs/`: design notes and demo documentation.
- `.scopefs/`: generated volume, journal, trace JSONL, epoch, signal, lock, read-lock, and session files. Do not commit it.
- `build/`: local CMake build output. Do not commit it.

## Build, Test, and Development Commands

Build on Windows/MinGW:

```powershell
cmake -S . -B build -G "MinGW Makefiles"
cmake --build build -j 4
```

Run a deterministic scripted demo:

```powershell
Get-Content qa\demo.scope | .\build\scopefs.exe --script
```

Run full QA:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File qa\run_all.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File qa\ui_width_smoke.ps1
```

Use interactive mode with `.\build\scopefs.exe`; Windows Terminal with UTF-8 is recommended.

## Coding Style & Naming Conventions

Use C++17, 2-space indentation, and small module-local helpers. Prefer `PascalCase` for types (`FileSystemKernel`, `CommandResult`) and `camelCase` for functions/fields. Keep constants centralized in `include/scopefs/config.hpp`. All disk access must go through the block/coordinator layer, and all trace records must go through `TraceSink`. User-visible formatting should go through UI helpers. Preserve script-mode plain output and tabular columns when polishing TUI output, especially `dir`, `trace`, `scope`, and `map` output.

## Testing Guidelines

Add or update `.scope` scripts for command-level behavior and PowerShell wrappers for multi-process, crash, serialized-volume, or COW/refcount scenarios. Always run `qa\run_all.ps1` after kernel, journal, permission, snapshot, COW, timestamp, coordinator, or concurrency changes. Run `qa\cp_cow.ps1` for block sharing/refcount work, `qa\time_attrs.ps1` for inode timestamps or `dir` metadata, and `qa\concurrent_locks.ps1` for session coordination, crash broadcast, delayed deletion, or lock semantics. Run `qa\ui_width_smoke.ps1` after prompt, ANSI, layout, localization, theme/language, or map/trace visualization changes. Crash tests may intentionally exit with code `88`.

## Commit & Pull Request Guidelines

Recent history uses concise imperative commit messages, for example `Remove fd file locks and polish trace UI` or `Localize TUI filesystem terms`. Keep commits focused and mention the user-visible behavior changed. PRs should include a short summary, affected commands/modules, QA commands run, and screenshots or terminal captures for TUI changes.

## Agent-Specific Instructions

Do not stage generated `.scopefs/`, `build/`, or user-only local docs unless explicitly requested. Before changing semantics, inspect `README.md`, `docs/design.md`, `docs/ui_showcase.md`, and relevant QA scripts so behavior, demos, and tests stay aligned. Preserve current semantics that `cp` is the COW copy command, `clone` is not a user command, same-file opens do not create explicit file-lock conflicts, open deleted files remain readable until close, and crash/coord signal internals do not leak into normal trace demos. Keep trace JSONL fields stable (`seq`, `txid`, `parent`, `depth`, `ts`, `session`, `user`, `command`, `type`, `object`, `before`, `after`, `reason`, `status`) unless tests and docs are updated together.
