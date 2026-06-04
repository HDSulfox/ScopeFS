$ErrorActionPreference = "Stop"

$exe = Join-Path $PSScriptRoot "..\build\scopefs.exe"
if (!(Test-Path $exe)) {
  throw "scopefs.exe not found. Build first with: cmake --build build -j 4"
}

$env:SCOPEFS_LOCK_TIMEOUT_MS = "600"

function Run-ScopeText([string]$Text, [int[]]$AllowedExit = @(0)) {
  $psi = [System.Diagnostics.ProcessStartInfo]::new()
  $psi.FileName = (Resolve-Path $exe).Path
  $psi.Arguments = "--script"
  $psi.UseShellExecute = $false
  $psi.RedirectStandardInput = $true
  $psi.RedirectStandardOutput = $true
  $psi.RedirectStandardError = $true
  $proc = [System.Diagnostics.Process]::Start($psi)
  $proc.StandardInput.Write($Text)
  $proc.StandardInput.Close()
  $stdout = $proc.StandardOutput.ReadToEnd()
  $stderr = $proc.StandardError.ReadToEnd()
  $proc.WaitForExit()
  if ($AllowedExit -notcontains $proc.ExitCode) {
    Write-Host $stdout
    Write-Host $stderr
    throw "scopefs exited with $($proc.ExitCode)"
  }
  return $stdout + $stderr
}

function Start-Scope() {
  $psi = [System.Diagnostics.ProcessStartInfo]::new()
  $psi.FileName = (Resolve-Path $exe).Path
  $psi.Arguments = "--script"
  $psi.UseShellExecute = $false
  $psi.RedirectStandardInput = $true
  $psi.RedirectStandardOutput = $true
  $psi.RedirectStandardError = $true
  return [System.Diagnostics.Process]::Start($psi)
}

function Send-Scope($proc, [string]$Text) {
  $proc.StandardInput.Write($Text)
  $proc.StandardInput.Flush()
}

function Finish-Scope($proc, [int[]]$AllowedExit = @(0)) {
  try { $proc.StandardInput.Close() } catch {}
  $stdout = $proc.StandardOutput.ReadToEnd()
  $stderr = $proc.StandardError.ReadToEnd()
  $proc.WaitForExit()
  if ($AllowedExit -notcontains $proc.ExitCode) {
    Write-Host $stdout
    Write-Host $stderr
    throw "scopefs long-running process exited with $($proc.ExitCode)"
  }
  return $stdout + $stderr
}

Write-Host "== concurrent different-file tx serialization =="
$deleted = Run-ScopeText @"
format
login root root
exit
"@ | Out-Null

$p1 = Start-Scope
$p2 = Start-Scope
Send-Scope $p1 @"
login root root
create /root/concurrent_a 0644
open /root/concurrent_a rw
write 3 AAAAA
close 3
exit
"@
Send-Scope $p2 @"
login root root
create /root/concurrent_b 0644
open /root/concurrent_b rw
write 3 BBBBB
close 3
exit
"@
$out1 = Finish-Scope $p1
$out2 = Finish-Scope $p2
$final = Run-ScopeText @"
login root root
dir /root
fsck
exit
"@
if ($final -notmatch "concurrent_a" -or $final -notmatch "concurrent_b" -or $final -notmatch "fsck: clean") {
  Write-Host $out1
  Write-Host $out2
  Write-Host $final
  throw "concurrent different-file write did not converge"
}

Write-Host "== shared read lock trace coverage =="
$tracePath = ".scopefs/read_lock_trace.jsonl"
Run-ScopeText @"
format
login root root
dir /
trace save $tracePath
exit
"@ | Out-Null
if (!(Test-Path $tracePath) -or (Get-Content $tracePath -Raw) -notmatch '"type":"coord\.lock\.acquire","object":"read"') {
  throw "shared read lock trace event was not recorded"
}

Write-Host "== same-file open has no explicit file lock conflict =="
Run-ScopeText @"
format
login root root
create /root/free 0644
exit
"@ | Out-Null
$unlockedHolder = Start-Scope
Send-Scope $unlockedHolder @"
login root root
open /root/free w
sleep 1500
close 3
exit
"@
Start-Sleep -Milliseconds 300
$unlocked = Run-ScopeText @"
login root root
open /root/free w
close 3
exit
"@
if ($unlocked -match "E_LOCK_BUSY" -or $unlocked -notmatch "fd 3 -> /root/free") {
  Write-Host $unlocked
  throw "unlocked open unexpectedly conflicted"
}
Finish-Scope $unlockedHolder | Out-Null

Write-Host "== open delete delayed reclaim without file locks =="
Run-ScopeText @"
format
login root root
create /root/delay 0644
open /root/delay rw
write 3 still-readable
close 3
open /root/delay r
delete /root/delay
dir /root
read 4 64
close 4
fsck
exit
"@
if ($deleted -match "\sdelay\s") {
  Write-Host $deleted
  throw "deleted directory entry stayed visible"
}
if ($deleted -notmatch "still-readable") {
  Write-Host $deleted
  throw "open reader lost delayed inode data"
}
if ($deleted -notmatch "fsck: clean") { throw "fsck failed after delayed reclaim" }

Write-Host "== snapshot COW across terminals =="
Run-ScopeText @"
format
login root root
create /root/snapfile 0644
open /root/snapfile rw
write 3 base
close 3
snapshot create baseline
exit
"@ | Out-Null
Run-ScopeText @"
login root root
open /root/snapfile append
write 3 -branch
close 3
snapshot create after_branch
exit
"@ | Out-Null
$diff = Run-ScopeText @"
login root root
snapshot diff baseline after_branch
fsck
exit
"@
if ($diff -notmatch "~ /root/snapfile" -or $diff -notmatch "fsck: clean") {
  Write-Host $diff
  throw "snapshot COW cross-terminal diff failed"
}

Write-Host "== broadcast crash signal =="
Run-ScopeText @"
format
login root root
exit
"@ | Out-Null
$victim = Start-Scope
Send-Scope $victim @"
login root root
sleep 2500
"@
Start-Sleep -Milliseconds 500
$crasher = Run-ScopeText @"
login root root
crash now
"@ @(88)
$victimOut = Finish-Scope $victim @(88)
if ($crasher -notmatch "E_CRASH" -or $victimOut -notmatch "E_CRASH") {
  Write-Host $crasher
  Write-Host $victimOut
  throw "broadcast crash did not terminate all terminals"
}
$recovered = Run-ScopeText @"
login root root
fsck
trace 80
exit
"@
if ($recovered -notmatch "fsck: clean") { throw "recovery after broadcast crash failed" }
if ($recovered -match "coord\.signal\." -or $recovered -match "crash\.point" -or $recovered -match "crash\.inject") {
  Write-Host $recovered
  throw "demo crash injection events leaked into trace"
}

Remove-Item Env:\SCOPEFS_LOCK_TIMEOUT_MS -ErrorAction SilentlyContinue
Write-Host "Concurrent coordination QA passed."
