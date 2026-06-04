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

$demo = Run-ScopeScript "demo.scope"
if ($demo -match "E_") {
  throw "demo.scope emitted an unexpected error"
}
if ($demo -notmatch "/course/lab/report -> /course/lab/report\.copy") {
  throw "demo.scope did not exercise cp successfully"
}
if ($demo -notmatch "\+ /course/lab/report\.copy") {
  throw "demo.scope snapshot diff did not include copied report"
}
if ($demo -notmatch "name\s+root index node\s+version\s+transaction id\s+created") {
  throw "demo.scope did not list snapshots with the new snapshot command"
}
Run-ScopeScript "permissions.scope" | Out-Null

powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot "teaching_acl.ps1")
if ($LASTEXITCODE -ne 0) {
  throw "teaching_acl.ps1 failed with exit code $LASTEXITCODE"
}

powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot "cp_cow.ps1")
if ($LASTEXITCODE -ne 0) {
  throw "cp_cow.ps1 failed with exit code $LASTEXITCODE"
}

powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot "time_attrs.ps1")
if ($LASTEXITCODE -ne 0) {
  throw "time_attrs.ps1 failed with exit code $LASTEXITCODE"
}

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

powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot "concurrent_locks.ps1")
if ($LASTEXITCODE -ne 0) {
  throw "concurrent_locks.ps1 failed with exit code $LASTEXITCODE"
}

Write-Host "All ScopeFS QA scripts passed."
