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
$dashboardZh = "$([char]0x547D)$([char]0x4EE4)$([char]0x7126)$([char]0x70B9)"

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
  $hasDashboardMarker = ($stdout -match "ScopeFS" -or $stdout -match [regex]::Escape($dashboardZh) -or $stdout -match "command focus")
  if (!$hasDashboardMarker -or !$hasInodeMarker -or !$hasTraceMarker) {
    throw "interactive width $width did not render expected UI markers"
  }
}

Remove-Item Env:\SCOPEFS_WIDTH -ErrorAction SilentlyContinue
Write-Host "UI width smoke checks passed."
