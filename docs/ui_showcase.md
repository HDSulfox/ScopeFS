# ScopeFS UI Showcase

The interactive UI now uses a dedicated C++17 ANSI renderer instead of fixed-width ad hoc strings.

## Visual System

- Theme: carbon black background, warm amber primary, ice blue secondary, green commit/pass, red deny/crash, magenta snapshot/COW.
- Layout: terminal width is measured at render time; content is centered up to an ideal width and truncated by display width.
- Modes: `--script` remains plain text for automated tests; interactive mode renders dashboard, cards, timelines, graphs, and heatmaps.

## Showcase Commands

Run this in an interactive terminal to see the high-density visual pass:

```powershell
Get-Content qa\ui_showcase.scope | .\build\scopefs.exe
```

Run it in deterministic script mode for output comparison:

```powershell
Get-Content qa\ui_showcase.scope | .\build\scopefs.exe --script
```

Width stress examples:

```powershell
$env:SCOPEFS_WIDTH=88;  Get-Content qa\ui_showcase.scope | .\build\scopefs.exe
$env:SCOPEFS_WIDTH=120; Get-Content qa\ui_showcase.scope | .\build\scopefs.exe
$env:SCOPEFS_WIDTH=160; Get-Content qa\ui_showcase.scope | .\build\scopefs.exe
Remove-Item Env:\SCOPEFS_WIDTH
```

## What To Inspect

- Startup dashboard: centered logo, command focus panel, and three status cards.
- `dir`: semantic directory cards with inode/generation/refcount and block mini bars.
- `scope`: dashboard cards instead of a single tiny table.
- `trace [n]`: event timeline with journal/path/auth/snapshot colors.
- `map refcount`: colored heatmap with legend and shared-block hotspots.
- `snapshot diff`: git-like diff with COW marker.
- `acl show` and `group tree`: grant graph style output.
