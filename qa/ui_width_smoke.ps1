$ErrorActionPreference = "Stop"

$exe = Join-Path $PSScriptRoot "..\build\scopefs.exe"
if (!(Test-Path $exe)) {
  throw "scopefs.exe not found. Build first with: cmake --build build -j 4"
}

$commands = @"
format
login root root
scope
dir /
trace 4
map refcount
theme blue
scope
exit
"@

$inodeZh = "$([char]0x7D22)$([char]0x5F15)$([char]0x8282)$([char]0x70B9)"
$traceZh = "$([char]0x8DDF)$([char]0x8E2A)"
$projectBriefZh = "$([char]0x9879)$([char]0x76EE)$([char]0x8BF4)$([char]0x660E)"
$courseDesignZh = "$([char]0x4E1C)$([char]0x5317)$([char]0x5927)$([char]0x5B66)$([char]0x64CD)$([char]0x4F5C)$([char]0x7CFB)$([char]0x7EDF)$([char]0x8BFE)$([char]0x7A0B)$([char]0x8BBE)$([char]0x8BA1)"

function Remove-Ansi([string]$Text) {
  $esc = [char]27
  return [regex]::Replace($Text, "$esc\[[0-9;?]*[ -/]*[@-~]", "")
}

function Get-DisplayWidth([string]$Text) {
  $width = 0
  for ($i = 0; $i -lt $Text.Length;) {
    $code = [char]::ConvertToUtf32($Text, $i)
    if ([char]::IsHighSurrogate($Text[$i])) { $i += 2 } else { ++$i }
    $wide = (($code -ge 0x2E80 -and $code -le 0xA4CF) -or
             ($code -ge 0xAC00 -and $code -le 0xD7A3) -or
             ($code -ge 0xF900 -and $code -le 0xFAFF) -or
             ($code -ge 0xFE10 -and $code -le 0xFE19) -or
             ($code -ge 0xFE30 -and $code -le 0xFE6F) -or
             ($code -ge 0xFF00 -and $code -le 0xFF60) -or
             ($code -ge 0xFFE0 -and $code -le 0xFFE6) -or
             ($code -ge 0x1F300 -and $code -le 0x1FAFF))
    if ($wide) { $width += 2 } else { ++$width }
  }
  return $width
}

function Assert-ProjectBoxAligned([string]$Output, [int]$Width) {
  $plain = Remove-Ansi $Output
  $lines = @($plain -split "\r?\n")
  $top = -1
  for ($i = 0; $i -lt $lines.Count; ++$i) {
    if ($lines[$i] -match "^\s*╭─+╮\s*$") {
      $top = $i
      break
    }
  }
  if ($top -lt 0) {
    throw "interactive width $Width did not render the project brief box"
  }

  $boxLines = @()
  for ($i = $top; $i -lt $lines.Count; ++$i) {
    $boxLines += $lines[$i].TrimEnd()
    if ($lines[$i] -match "^\s*╰─+╯\s*$") { break }
  }
  if ($boxLines.Count -lt 4 -or $boxLines[-1] -notmatch "^\s*╰─+╯\s*$") {
    throw "interactive width $Width project brief box was incomplete"
  }

  $expected = Get-DisplayWidth $boxLines[0]
  foreach ($line in $boxLines) {
    $actual = Get-DisplayWidth $line
    if ($actual -ne $expected) {
      throw "interactive width $Width project brief box misaligned: expected display width $expected got $actual for [$line]"
    }
  }
}

foreach ($width in @(88, 120, 160)) {
  Write-Host "== interactive width $width =="
  $env:SCOPEFS_WIDTH = "$width"
  $psi = [System.Diagnostics.ProcessStartInfo]::new()
  $psi.FileName = (Resolve-Path $exe).Path
  $psi.UseShellExecute = $false
  $psi.RedirectStandardInput = $true
  $psi.RedirectStandardOutput = $true
  $psi.RedirectStandardError = $true
  $proc = [System.Diagnostics.Process]::Start($psi)
  $proc.StandardInput.Write($commands)
  $proc.StandardInput.Close()
  $stdout = $proc.StandardOutput.ReadToEnd()
  $stderr = $proc.StandardError.ReadToEnd()
  $proc.WaitForExit()
  if ($proc.ExitCode -ne 0) {
    Write-Host $stdout
    Write-Host $stderr
    throw "interactive width $width failed with exit code $($proc.ExitCode)"
  }
  $hasInodeMarker = ($stdout -match [regex]::Escape($inodeZh) -or $stdout -match "index node" -or $stdout -match "inode")
  $hasTraceMarker = ($stdout -match [regex]::Escape($traceZh) -or $stdout -match "Trace" -or $stdout -match "trace")
  $hasDashboardMarker = ($stdout -match [regex]::Escape($projectBriefZh) -or $stdout -match [regex]::Escape($courseDesignZh) -or $stdout -match "Project Brief")
  if (!$hasDashboardMarker -or !$hasInodeMarker -or !$hasTraceMarker) {
    throw "interactive width $width did not render expected UI markers"
  }
  Assert-ProjectBoxAligned $stdout $width
}

Remove-Item Env:\SCOPEFS_WIDTH -ErrorAction SilentlyContinue
Write-Host "UI width smoke checks passed."
