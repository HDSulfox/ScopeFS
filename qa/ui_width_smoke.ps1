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
trace show 4
map refcount
theme blue
scope
exit
"@

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
  if ($stdout -notmatch "ScopeFS" -or $stdout -notmatch "inode" -or $stdout -notmatch "Trace") {
    throw "interactive width $width did not render expected UI markers"
  }
}

Remove-Item Env:\SCOPEFS_WIDTH -ErrorAction SilentlyContinue
Write-Host "UI width smoke checks passed."
