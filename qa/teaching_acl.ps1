$ErrorActionPreference = "Stop"

$exe = Join-Path $PSScriptRoot "..\build\scopefs.exe"
if (!(Test-Path $exe)) {
  throw "scopefs.exe not found. Build first with: cmake --build build -j 4"
}

function Run-ScopeText([string]$Text) {
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
  $output = $stdout + $stderr
  $output | Write-Host
  if ($proc.ExitCode -ne 0) {
    throw "scopefs exited with $($proc.ExitCode)"
  }
  return $output
}

function Assert-True([bool]$Condition, [string]$Message) {
  if (!$Condition) { throw $Message }
}

$branchMid = "$([char]0x251C)$([char]0x2500)"
$branchEnd = "$([char]0x2514)$([char]0x2500)"

Write-Host "== teaching identity defaults =="
$identity = Run-ScopeText @"
format
lang en
login usr1 usr1
whoami
logout
login usr5 usr5
whoami
exit
"@
Assert-True ($identity -match "role: teacher") "usr1 did not report teacher role"
Assert-True ($identity -match "courses: cs101") "usr1 did not report cs101 course"
Assert-True ($identity -match "role: student") "usr5 did not report student role"
Assert-True ($identity -match "groups: .*cs101_student") "usr5 did not inherit cs101_student"

Write-Host "== teaching course resource setup =="
$setup = Run-ScopeText @"
lang en
login root root
mkdir /course
mkdir /course/cs101
mkdir /course/cs102
create /course/cs101/material 0600
create /course/cs102/material 0600
chgroup /course/cs101 cs101
chgroup /course/cs101/material cs101
chgroup /course/cs102 cs102
chgroup /course/cs102/material cs102
chmod /course 0755
chmod /course/cs101 0750
chmod /course/cs102 0750
logout
login usr1 usr1
open /course/cs101/material rw
close 3
acl grant /course/cs101/material cs101_student r path_prefix=/course/cs101
acl grant /course/cs101/material usr3 g path_prefix=/course/cs101
exit
"@
Assert-True ($setup -match "fd 3 -> /course/cs101/material") "cs101 teacher could not manage course material"
Assert-True (($setup -split "acl granted").Count -ge 3) "teacher did not grant expected ACL entries"

Write-Host "== teaching course boundaries =="
$crossGrant = Run-ScopeText @"
lang en
login usr1 usr1
acl grant /course/cs101/material cs102_student r path_prefix=/course/cs101
exit
"@
Assert-True ($crossGrant -match "E_PERMISSION") "teacher granted ACL across course boundary"

$otherCourse = Run-ScopeText @"
lang en
login usr1 usr1
open /course/cs102/material r
exit
"@
Assert-True ($otherCourse -match "E_PERMISSION") "teacher accessed other course resource by default"

Write-Host "== assistant scoped ACL management =="
$assistant = Run-ScopeText @"
lang en
login usr3 usr3
acl grant /course/cs101/material usr6 r path_prefix=/course/cs101
exit
"@
Assert-True ($assistant -match "acl granted") "authorized assistant could not manage course ACL"

$otherAssistant = Run-ScopeText @"
lang en
login usr4 usr4
open /course/cs101/material r
exit
"@
Assert-True ($otherAssistant -match "E_PERMISSION") "assistant accessed unrelated course resource"

Write-Host "== student least privilege and isolation =="
$studentRead = Run-ScopeText @"
lang en
login usr5 usr5
open /course/cs101/material r
close 3
exit
"@
Assert-True ($studentRead -match "fd 3 -> /course/cs101/material") "student could not read authorized course material"

$studentWrite = Run-ScopeText @"
lang en
login usr5 usr5
open /course/cs101/material w
exit
"@
Assert-True ($studentWrite -match "E_PERMISSION") "student wrote course material"

$studentOtherCourse = Run-ScopeText @"
lang en
login usr7 usr7
open /course/cs101/material r
exit
"@
Assert-True ($studentOtherCourse -match "E_PERMISSION") "student accessed other course material"

$privateSetup = Run-ScopeText @"
lang en
login usr5 usr5
create private 0600
open private rw
write 3 private-submission
close 3
exit
"@
Assert-True ($privateSetup -match "wrote") "student private submission setup failed"

$peerPrivate = Run-ScopeText @"
lang en
login usr6 usr6
open /home/usr5/private r
exit
"@
Assert-True ($peerPrivate -match "E_PERMISSION") "student read peer private submission"

Write-Host "== administrator bypass and group tree =="
$admin = Run-ScopeText @"
lang en
login admin admin
open /home/usr5/private r
read 3 64
close 3
group tree
fsck
exit
"@
Assert-True ($admin -match "private-submission") "admin did not bypass ACL to read private submission"
Assert-True ($admin -match "group cs101") "group tree did not show cs101"
Assert-True ($admin.Contains("$branchMid group cs101_student") -or $admin.Contains("$branchEnd group cs101_student")) "group tree did not render branch connectors"
Assert-True ($admin -match "fsck: clean") "fsck was not clean after teaching ACL scenario"

Write-Host "Teaching ACL checks passed."
