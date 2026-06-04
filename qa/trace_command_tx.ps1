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

function Read-JsonLines([string]$Path) {
  $items = @()
  foreach ($line in [System.IO.File]::ReadAllLines($Path)) {
    if ([string]::IsNullOrWhiteSpace($line)) { continue }
    $items += ($line | ConvertFrom-Json)
  }
  return @($items)
}

function Assert-CommandRootOutsideTx([string]$Path, [string]$Command, [string]$Type) {
  $events = Read-JsonLines $Path | Where-Object { $_.command -eq $Command -and $_.type -eq $Type } | Sort-Object seq
  Assert-True ($events.Count -ge 2) "$Command did not produce both root and transactional $Type events"
  Assert-True ($events[0].txid -eq 0) "$Command root event for $Type was assigned txid $($events[0].txid)"
  Assert-True ($events[1].txid -gt 0) "$Command transactional event for $Type was missing txid"
  Assert-True ($events[1].seq -gt $events[0].seq) "$Command transactional $Type event did not occur after root event"
}

$tmp = Join-Path ([System.IO.Path]::GetTempPath()) ("scopefs-trace-" + [System.Guid]::NewGuid().ToString("N"))
[System.IO.Directory]::CreateDirectory($tmp) | Out-Null

try {
  Write-Host "== trace root events stay outside transactions =="
  $suffix = [System.Guid]::NewGuid().ToString("N").Substring(0, 8)
  $labPath = "/lab-$suffix"
  $filePath = "$labPath/file.txt"
  $groupName = "reviewers_$suffix"
  $snapshotName = "baseline_$suffix"
  $mkdirTrace = Join-Path $tmp "mkdir.jsonl"
  $createTrace = Join-Path $tmp "create.jsonl"
  $writeTrace = Join-Path $tmp "write.jsonl"
  $groupTrace = Join-Path $tmp "group.jsonl"
  $snapshotTrace = Join-Path $tmp "snapshot.jsonl"
$chmodTrace = Join-Path $tmp "chmod.jsonl"
$aclTrace = Join-Path $tmp "acl.jsonl"
$mkdirTraceArg = $mkdirTrace -replace "\\", "/"
$createTraceArg = $createTrace -replace "\\", "/"
$writeTraceArg = $writeTrace -replace "\\", "/"
$groupTraceArg = $groupTrace -replace "\\", "/"
  $snapshotTraceArg = $snapshotTrace -replace "\\", "/"
  $chmodTraceArg = $chmodTrace -replace "\\", "/"
  $aclTraceArg = $aclTrace -replace "\\", "/"

  Run-ScopeText @"
format
lang en
login root root
trace clear
mkdir $labPath
trace save $mkdirTraceArg
"@ | Out-Null

  Run-ScopeText @"
format
lang en
login root root
mkdir $labPath
trace clear
create $filePath 0644
trace save $createTraceArg
"@ | Out-Null

  Run-ScopeText @"
format
lang en
login root root
mkdir $labPath
create $filePath 0644
open $filePath rw
trace clear
write 3 hello
trace save $writeTraceArg
exit
"@ | Out-Null

  Run-ScopeText @"
format
lang en
login root root
trace clear
group create $groupName
trace save $groupTraceArg
"@ | Out-Null

  Run-ScopeText @"
format
lang en
login root root
trace clear
snapshot create $snapshotName
trace save $snapshotTraceArg
"@ | Out-Null

  Run-ScopeText @"
format
lang en
login root root
mkdir $labPath
create $filePath 0644
trace clear
chmod $filePath 0600
trace save $chmodTraceArg
"@ | Out-Null

  Run-ScopeText @"
format
lang en
login root root
mkdir $labPath
create $filePath 0644
trace clear
acl grant $filePath usr1 r path_prefix=$labPath
trace save $aclTraceArg
"@ | Out-Null

  Assert-CommandRootOutsideTx $mkdirTrace "mkdir $labPath" "dir.mkdir"
  Assert-CommandRootOutsideTx $createTrace "create $filePath 0644" "file.create"
  Assert-CommandRootOutsideTx $writeTrace "write 3 hello" "file.write"
  Assert-CommandRootOutsideTx $groupTrace "group create $groupName" "group.create"
  Assert-CommandRootOutsideTx $snapshotTrace "snapshot create $snapshotName" "snapshot.create"
  Assert-CommandRootOutsideTx $chmodTrace "chmod $filePath 0600" "inode.chmod"
  Assert-CommandRootOutsideTx $aclTrace "acl grant $filePath usr1 r path_prefix=$labPath" "acl.grant"

  Write-Host "Trace command transaction checks passed."
} finally {
  Remove-Item -LiteralPath $tmp -Recurse -Force -ErrorAction SilentlyContinue
}
