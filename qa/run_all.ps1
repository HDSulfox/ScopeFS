$ErrorActionPreference = "Stop"

$exe = Join-Path $PSScriptRoot "..\build\scopefs.exe"
if (!(Test-Path $exe)) {
  throw "scopefs.exe not found. Build first with: cmake --build build -j 4"
}

function Run-ScopeScript($name, [int[]]$AllowedExit = @(0)) {
  Write-Host "== $name =="
  $script = Join-Path $PSScriptRoot $name
  $psi = [System.Diagnostics.ProcessStartInfo]::new()
  $psi.FileName = (Resolve-Path $exe).Path
  $psi.Arguments = "--script"
  $psi.UseShellExecute = $false
  $psi.RedirectStandardInput = $true
  $psi.RedirectStandardOutput = $true
  $psi.RedirectStandardError = $true
  $proc = [System.Diagnostics.Process]::Start($psi)
  $proc.StandardInput.Write((Get-Content $script -Raw))
  $proc.StandardInput.Close()
  $stdout = $proc.StandardOutput.ReadToEnd()
  $stderr = $proc.StandardError.ReadToEnd()
  $proc.WaitForExit()
  $code = $proc.ExitCode
  $output = ($stdout + $stderr)
  $output | Write-Host
  if ($AllowedExit -notcontains $code) {
    throw "$name exited with $code"
  }
  return $output
}

Run-ScopeScript "demo.scope" | Out-Null
Run-ScopeScript "permissions.scope" | Out-Null

Run-ScopeScript "crash_before_commit.scope" @(88) | Out-Null
$before = Run-ScopeScript "check_after_before.scope"
if ($before -match "should_not_exist") {
  throw "uncommitted mkdir became visible after recovery"
}

Run-ScopeScript "crash_after_commit.scope" @(88) | Out-Null
$after = Run-ScopeScript "check_after_after.scope"
if ($after -notmatch "should_exist") {
  throw "committed mkdir was not recovered"
}

Write-Host "All ScopeFS QA scripts passed."
